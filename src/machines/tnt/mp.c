// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mp.c
// The Apple/DayStar two-way processor card of the Power Macintosh
// 9500/180MP: a second 604 on the 60x bus, brought up by system software
// after the uniprocessor ROM has booted CPU 0.  The card adds no device —
// its whole surface is four register behaviours and one CPU-lifecycle rule:
//
//   Hammerhead +$90 ArbConfig  bit $02 "TwoCPU" — the only way to detect
//                              the card (a strap; hammerhead.c reads it from
//                              the board descriptor);
//   Hammerhead +$B0 WhoAmI     $10 to the primary, $08 to the secondary —
//                              its value depends on which core issued the
//                              load (tnt_mp_whoami);
//   Hammerhead +$C0 IntReg     bit $80 "SecInt", ACTIVE LOW: clearing it
//                              raises the secondary's interrupt, setting it
//                              drops it (tnt_mp_intreg);
//   $F2800000                  the start vector, written into Bandit 1's
//                              config-address latch (tnt_mp_mailbox);
//   GC +$19000 (Ethernet PROM) the secondary's access to the PROM chip
//                              select interrupts the primary, on Grand
//                              Central line 30 (tnt_mp_eprom_access).
//
// Sources: OSF/Apple MkLinux MPPlugIn.h (the register map, WhoAmI); Linux
// arch/powerpc/platforms/powermac/smp.c (the psurge kick sequence); Apple,
// "Apple Network Server Hardware Developer Notes" §4.2 (the Ethernet-PROM
// chip select as the secondary-to-primary interrupt); and BeOS 5's
// kernel_mac, whose start_other_cpus/interrupt_cpu follow exactly this
// protocol (start vector to $F2800000, IntReg AND $7FFFFFFF to kick, wait
// for the secondary to OR the bit back as its acknowledge).
//
// The CPU-1 lifecycle.  The secondary is PARKED from reset.  A write to
// IntReg with SecInt clear, while it is parked, CALLS it at the mailbox
// address: hard-reset state (translation off), with the link register
// pointing at a parking spin, so a `blr` from the entry code parks it again.
// While it RUNS, SecInt is simply its external-interrupt level.  Every
// consumer agrees with that contract:
//   - Open Firmware, seeing TwoCPU, writes the mailbox with a three-word
//     snippet (`mfspr r0,pvr; stw r0,12(r1); blr`) and rings — the
//     secondary stores its PVR and returns to its parking place, which
//     is therefore the card's, not a loop in RAM;
//   - BeOS writes its entry ($000568AC on 5.0.3) and rings by ANDing IntReg
//     with $7FFFFFFF — over the $00 Open Firmware left there, so the kick is
//     the WRITE, not an edge — then waits for the new core to OR SecInt
//     back as its acknowledge;
//   - Linux (smp_psurge_kick_cpu) writes the entry and pulses SecInt, and
//     afterwards rewrites the mailbox to $100 "so if we get another intr we
//     won't try to startup again" — later interrupts reach a RUNNING core.
//
// Execution model (the "peer helper" of the multi-CPU design): CPU 0 owns
// emulated time and runs the scheduler's sprints; CPU 1 runs in BURSTS from
// a periodic scheduler event once started.  Each burst gives CPU 1 as many
// instructions as CPU 0 retired since the previous burst — the same clock
// and the same CPI model — so both cores advance at the same rate, CPU 1
// trailing by at most one quantum.  Around the burst the global fast path
// is swapped to CPU 1's MMU context (ppc_mmu_activate) and the sprint-time
// I/O penalty channel is closed, so nothing CPU 1 does is charged to a CPU 0
// sprint.  Serialized bursts give the guest a sequentially consistent
// memory history (stronger than the 60x bus's), so the only coherence rules
// that need modelling are the reservation and tlbie broadcast: a core's
// lwarx reservation is dropped whenever the other core has run in between
// (the conservative "any peer store clears" rule; the architecture allows a
// reservation to be lost for any reason), and tlbie invalidates every
// core's translation caches (ppc_mmu.c).  A parked CPU 1 costs nothing: its
// burst event is not armed until the doorbell rings.

#include "tnt.h"

#include "log.h"
#include "machine.h"
#include "machine_parts.h"
#include "memory.h"
#include "object.h"
#include "ppc.h"
#include "scheduler.h"
#include "system.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mp");

// Burst quantum in CPU cycles.  4096 cycles is ~23 us at 180 MHz: the
// bound on any cross-CPU observation latency (lock hand-off, IPI round
// trip), far inside every timeout the guests use.
#define MP_QUANTUM_CYCLES 4096u

// A burst never runs more than this many instructions (a long single
// sprint of CPU 0 — e.g. after a debugger pause — is not replayed whole).
#define MP_BURST_MAX 65536u

static void mp_burst_event(void *source, uint64_t data);

static inline tnt_mp_t *mp_of(config_t *cfg) {
    return &tnt_st(cfg)->mp;
}

bool tnt_mp_present(config_t *cfg) {
    return tnt_board(cfg)->mp_cpus > 1;
}

// The secondary's interrupt line is SecInt, active low.
static void mp_update_cpu1_irq(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (!st->cpu1)
        return;
    bool asserted = !(st->hh.reg[0xC0u >> 4] & 0x80000000u);
    ppc_set_ext_irq(st->cpu1, asserted && st->mp.running);
}

// The parking spin: the first self-branch (`b .`, $48000000) in the top
// megabyte of the ROM — the reset/exception region, present in every TNT
// image.  CPU 1 returns into it and spins until the burst ends, where the
// glue sees it and parks the core.  $FFF00100 (the reset vector) if none.
static uint32_t mp_find_park_pc(config_t *cfg) {
    const uint8_t *rom = memory_rom_bytes(cfg->mem_map);
    uint32_t size = memory_rom_size(cfg->mem_map);
    if (rom && size >= 0x100000u) {
        uint32_t base = 0u - size; // the ROM ends at $FFFFFFFF
        for (uint32_t off = size - 0x100000u; off + 4 <= size; off += 4)
            if (rom[off] == 0x48 && rom[off + 1] == 0 && rom[off + 2] == 0 && rom[off + 3] == 0)
                return base + off;
    }
    return 0xFFF00100u;
}

static void mp_arm(config_t *cfg) {
    tnt_mp_t *mp = mp_of(cfg);
    remove_event(cfg->scheduler, mp_burst_event, cfg);
    mp->last_instr = scheduler_instr_count(cfg->scheduler);
    scheduler_new_cpu_event(cfg->scheduler, mp_burst_event, cfg, 0, MP_QUANTUM_CYCLES, 0, true);
}

// One CPU 1 burst: as many instructions as CPU 0 retired since the last.
static void mp_burst_event(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    tnt_state_t *st = tnt_st(cfg);
    tnt_mp_t *mp = &st->mp;
    if (!st->cpu1 || !mp->running)
        return;
    uint64_t now = scheduler_instr_count(cfg->scheduler);
    uint64_t owed = now - mp->last_instr;
    mp->last_instr = now;
    if (owed == 0)
        return;
    uint32_t budget = owed > MP_BURST_MAX ? MP_BURST_MAX : (uint32_t)owed;
    uint32_t planned = budget;

    // Enter CPU 1: its translation context, no sprint penalty channel, and
    // a bus-error counter of its own.
    uint32_t *saved_bus_err = g_bus_error_instr_ptr;
    uint32_t saved_cpi = g_io_cpi_x256;
    uint32_t *saved_burn = g_sprint_burndown_ptr;
    g_io_cpi_x256 = 0;
    g_sprint_burndown_ptr = NULL;
    mp->in_cpu1 = 1;
    ppc_mmu_activate(st->cpu1);
    ppc_clear_reservation(st->cpu1); // CPU 0 ran since this core last did
    ppc_run(st->cpu1, &budget);
    ppc_mmu_activate(cfg->ppc);
    ppc_clear_reservation(cfg->ppc); // and CPU 1 has now run since CPU 0 did
    mp->in_cpu1 = 0;
    g_sprint_burndown_ptr = saved_burn;
    g_io_cpi_x256 = saved_cpi;
    g_bus_error_instr_ptr = saved_bus_err;
    // ppc_run leaves *instructions at 0; it ran the whole plan unless a
    // fault ended it early, which is rare enough to count as the plan.
    mp->cpu1_instr += planned;
    mp->bursts++;
    // Returned into the parking spin: parked until the next call.
    if (ppc_get_pc(st->cpu1) == mp->park_pc) {
        mp->running = 0;
        remove_event(cfg->scheduler, mp_burst_event, cfg);
        mp_update_cpu1_irq(cfg);
        LOG(1, "CPU 1 parked (returned to $%08X)", mp->park_pc);
    }
}

uint64_t tnt_mp_cpu1_instr(config_t *cfg) {
    return mp_of(cfg)->cpu1_instr;
}

// --- register behaviours -------------------------------------------------

uint8_t tnt_mp_whoami(config_t *cfg) {
    return mp_of(cfg)->in_cpu1 ? 0x08u : 0x10u;
}

bool tnt_mp_is_cpu1(config_t *cfg) {
    return tnt_st(cfg)->mp.in_cpu1 != 0;
}

void tnt_mp_mailbox(config_t *cfg, uint32_t le_latch) {
    if (!tnt_mp_present(cfg))
        return;
    // The guest stores the physical entry address big-endian into a port
    // that latches little-endian; the start logic takes the bus value.
    mp_of(cfg)->mailbox = __builtin_bswap32(le_latch);
}

// IntReg lane 0 was written (the register already holds the new value).
void tnt_mp_intreg(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_mp_t *mp = &st->mp;
    if (!tnt_mp_present(cfg) || !st->cpu1)
        return;
    bool asserted = !(st->hh.reg[0xC0u >> 4] & 0x80000000u);
    if (asserted && !mp->running) {
        // The start doorbell: call the parked secondary at the mailbox.
        if (!mp->park_pc)
            mp->park_pc = mp_find_park_pc(cfg);
        mp->running = 1;
        mp->calls++;
        mp->entry = mp->mailbox;
        ppc_start_at(st->cpu1, mp->mailbox, mp->park_pc);
        LOG(1, "CPU 1 called at $%08X (call %u, returns to $%08X)", mp->mailbox, mp->calls, mp->park_pc);
        mp_arm(cfg);
        cpu_reschedule(cfg->scheduler);
        return; // the doorbell itself is not an interrupt to the new core
    }
    LOG(3, "SecInt %s by CPU %d", asserted ? "asserted" : "released", mp->in_cpu1 ? 1 : 0);
    mp_update_cpu1_irq(cfg);
}

// An access to the Ethernet PROM's chip select BY THE SECONDARY interrupts
// the primary.  The card decodes it on the secondary's bus requests only:
// the primary reads the same chip select for the station address (Mac OS's
// MACE driver, BeOS's bootstrap comparing it with OF's local-mac-address),
// and if that rang too the bootstrap — whose interrupt poll treats a
// pending line 30 as an IPI and has no handler to clear it before the
// kernel loads — would spin forever.  Every sender is the secondary
// (Linux smp_psurge_message_pass to CPU 0, BeOS interrupt_cpu(0)), and
// Apple's wording is "the programmatic way for a second processor to
// interrupt the first processor".
void tnt_mp_eprom_access(config_t *cfg) {
    if (!tnt_mp_present(cfg) || !mp_of(cfg)->in_cpu1)
        return;
    LOG(3, "PriInt (Ethernet-PROM access by CPU 1)");
    tnt_gc_pulse_event(cfg, TNT_INT_IPI);
}

// --- machine.mp: the card as the shell sees it -----------------------------

static tnt_mp_t *mp_obj(struct object *self) {
    return mp_of((config_t *)object_data(self));
}

static DEF_GETTER(mp_attr_running) {
    return val_bool(mp_obj(self)->running != 0);
}
static DEF_GETTER(mp_attr_calls) {
    return val_uint(4, mp_obj(self)->calls);
}
static DEF_GETTER(mp_attr_mailbox) {
    value_t v = val_uint(4, mp_obj(self)->mailbox);
    v.flags |= VAL_HEX;
    return v;
}
static DEF_GETTER(mp_attr_entry) {
    value_t v = val_uint(4, mp_obj(self)->entry);
    v.flags |= VAL_HEX;
    return v;
}
static DEF_GETTER(mp_attr_park_pc) {
    value_t v = val_uint(4, mp_obj(self)->park_pc);
    v.flags |= VAL_HEX;
    return v;
}
static DEF_GETTER(mp_attr_bursts) {
    return val_uint(8, mp_obj(self)->bursts);
}
static DEF_GETTER(mp_attr_cpu1_instr) {
    return val_uint(8, mp_obj(self)->cpu1_instr);
}

static const member_t mp_members[] = {
    {.kind = M_ATTR,
     .name = "running",
     .doc = "CPU 1 is executing (called at the mailbox and not parked again)",
     .attr = {.type = V_BOOL, .get = mp_attr_running, .set = NULL}   },
    {.kind = M_ATTR,
     .name = "calls",
     .doc = "Times the start doorbell called the parked CPU 1",
     .attr = {.type = V_UINT, .get = mp_attr_calls, .set = NULL}     },
    {.kind = M_ATTR,
     .name = "mailbox",
     .doc = "The $F2800000 latch now (Bandit 1's config address: PCI accesses overwrite it)",
     .attr = {.type = V_UINT, .get = mp_attr_mailbox, .set = NULL}   },
    {.kind = M_ATTR,
     .name = "entry",
     .doc = "The start vector the last call used",
     .attr = {.type = V_UINT, .get = mp_attr_entry, .set = NULL}     },
    {.kind = M_ATTR,
     .name = "park_pc",
     .doc = "The parking spin CPU 1 returns into",
     .attr = {.type = V_UINT, .get = mp_attr_park_pc, .set = NULL}   },
    {.kind = M_ATTR,
     .name = "bursts",
     .doc = "CPU 1 bursts run",
     .attr = {.type = V_UINT, .get = mp_attr_bursts, .set = NULL}    },
    {.kind = M_ATTR,
     .name = "cpu1_instr",
     .doc = "Instructions CPU 1 has retired (= machine.cpu1.instr_count)",
     .attr = {.type = V_UINT, .get = mp_attr_cpu1_instr, .set = NULL}},
};

static const class_desc_t mp_class = {
    .name = "mp",
    .doc = "The dual-processor card: CPU 1's lifecycle and doorbells",
    .members = mp_members,
    .n_members = sizeof(mp_members) / sizeof(mp_members[0]),
};

// --- lifecycle -----------------------------------------------------------

static uint64_t mp_cpu1_instr_count(void *ctx) {
    return tnt_mp_cpu1_instr((config_t *)ctx);
}

static void part_save_mp(void *obj, checkpoint_t *cp) {
    config_t *cfg = (config_t *)obj;
    tnt_state_t *st = tnt_st(cfg);
    system_write_checkpoint_data(cp, &st->mp, sizeof(st->mp));
    ppc_checkpoint(st->cpu1, cp);
}

int tnt_mp_init(config_t *cfg, checkpoint_t *cp, uint32_t tick_hz) {
    tnt_state_t *st = tnt_st(cfg);
    if (!tnt_mp_present(cfg))
        return 0;
    machine_part_begin(cfg, cp, "cpu1");
    if (cp)
        system_read_checkpoint_data(cp, &st->mp, sizeof(st->mp));
    st->mp.in_cpu1 = 0;
    st->cpu1 = ppc_init_peer(cp, cfg->machine->cpu_model, "cpu1");
    if (!st->cpu1) {
        LOG(0, "Error: cannot construct the second processor");
        return -1;
    }
    machine_part(cfg, cp, "cpu1", part_save_mp, cfg);
    ppc_set_pir(st->cpu1, 1);
    ppc_set_instr_counter(st->cpu1, mp_cpu1_instr_count, cfg);
    if (tnt_board(cfg)->pvr)
        ppc_set_identity(st->cpu1, tnt_board(cfg)->pvr, 0);
    ppc_bind_time_named(st->cpu1, cfg->scheduler, "cpu1", cfg->machine->freq, tick_hz);
    scheduler_new_event_type(cfg->scheduler, "cpu1", cfg, "burst", mp_burst_event);
    if (cp)
        mp_update_cpu1_irq(cfg);
    st->mp_object = object_new(&mp_class, cfg, "mp");
    if (st->mp_object) {
        object_set_label(st->mp_object, "MP card");
        object_set_order(st->mp_object, 12);
        object_attach(machine_object(), st->mp_object);
    }
    return 0;
}

// Bus reset: the secondary goes back to its parked state.
void tnt_mp_reset(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (!st->cpu1)
        return;
    remove_event(cfg->scheduler, mp_burst_event, cfg);
    memset(&st->mp, 0, sizeof(st->mp));
    ppc_reset(st->cpu1);
    ppc_set_ext_irq(st->cpu1, false);
}

void tnt_mp_teardown(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (st && st->mp_object) {
        object_detach(st->mp_object);
        object_delete(st->mp_object);
        st->mp_object = NULL;
    }
    if (!st || !st->cpu1)
        return;
    if (cfg->scheduler)
        remove_event(cfg->scheduler, mp_burst_event, cfg);
    ppc_delete(st->cpu1);
    st->cpu1 = NULL;
}
