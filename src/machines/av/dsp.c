// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dsp.c
// The AV family's DSP3210 — see dsp.h.  Execution model per
// proposal-heterogeneous-multi-cpu.md §3: the DSP runs in burst events on
// the one scheduler queue; `ratio_x256` converts elapsed main-CPU cycles
// into a DSP instruction budget (aux_freq / (4 CKI × main_freq), carry
// kept exact), a 4096-cycle quantum re-arms while the core is runnable,
// and an idle core (held in reset, or parked in waiti with nothing
// pending) costs zero events until a kick.
//
// Board wiring (dsp3210.md §8 + dsp3210-plaintalk findings):
//   * bus hooks — guest-physical through the bus resolver (the PSC-DMA
//     pattern; the CPU MMU is deliberately not in the path); the host
//     decoder never maps the on-chip $5003xxxx window (the core decodes
//     it internally); accesses into ROM/NuBus space ($40000000+) fault →
//     DSP bus-error vector 1, which Apple's 'evt' handlers turn into an
//     'xbus' crash dump.
//   * dspOverRun → reset lifecycle: $83 holds (state clear), $01 releases
//     (fetch from external physical 0 — the 7-word bootstrap), $81
//     re-holds.  pdspResetEn is an arm interlock, not power management
//     (rtm-rom-host-side.md §4).
//   * DSP→host doorbell: the kernel's per-message BIO0 toggle latches PSC
//     L5 bit 0 (dsp-kernel-messages.md §1); the RTM's DSPhndlr acks L5IR
//     itself.

#include "dsp.h"

#include "av.h"
#include "psc.h"

#include "dsp3210.h"
#include "dsp3210_disasm.h"

#include "log.h"
#include "machine_profile.h"
#include "mmu.h"
#include "object.h"
#include "scheduler.h"
#include "system.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("dsp3210");

// Burst quantum in main-CPU cycles (~102 us at 40 MHz): three orders of
// magnitude of margin against the tightest boot-protocol deadline.
#define AV_DSP_QUANTUM 4096

struct av_dsp {
    // --- plain data (checkpointed up to `cfg`; the core blob follows) ---
    uint8_t reset_held; // dspOverRun bit 0: 1 = held in reset
    uint8_t started; // the RTM released the DSP at least once
    uint8_t burst_armed; // burst event scheduled
    uint8_t overrun_bits; // last dspOverRun latch value seen
    uint32_t ratio_x256; // DSP instructions per main cycle, x256
    uint32_t carry_x256; // sub-instruction budget remainder
    uint64_t last_burst_cycles; // cycle stamp of the previous burst

    // --- pointers (not checkpointed) ---
    config_t *cfg;
    dsp3210_t *core; // heap-allocated (embeds the 64 KB on-chip window)
    struct object *object; // the machine.dsp node
};

extern const class_desc_t av_dsp_class;

static void av_dsp_burst_event(void *source, uint64_t data);

static inline av_psc_t *dsp_psc(av_dsp_t *d) {
    av_state_t *st = (av_state_t *)d->cfg->machine_context;
    return st ? st->psc : NULL;
}

// ============================================================
// Bus hooks: guest-physical, never the CPU MMU
// ============================================================

// The DSP's IO alias: $F0000000-$F07FFFFF maps the host IO window
// $50800000-$50FFFFFF (the driver's 'phas' selector hands the kernel the
// sndPhase register as $F073120C = PSC $50F3120C; the sound team's output
// task phase-syncs against it every frame).
static inline int dsp_io_alias(uint32_t addr, uint32_t *host) {
    if ((addr & 0xFF800000u) == 0xF0000000u) {
        *host = 0x50800000u | (addr & 0x7FFFFFu);
        return 1;
    }
    return 0;
}

// $50040000 is the top of the kernel's on-chip hmem heap (kernel data
// +$1A8/+$1F8) and nothing legitimate ever addresses at or above it:
// all sound/message FIFO rings live in host RAM (DSPFIFO records carry
// absolute host bases), so accesses landing here are runaway pointers
// and take the normal >= $40000000 bus fault ('xbus' in the guest).

static uint32_t av_dsp_bus_read(void *ctx, uint32_t addr, int size, int *fault) {
    av_dsp_t *d = (av_dsp_t *)ctx;
    uint32_t host;
    if (dsp_io_alias(addr, &host)) {
        addr = host;
    } else if (addr >= 0x40000000u) {
        // ROM/NuBus space is not a DSP target — fault, and let the guest's
        // own 'xbus' handler report it (free negative-path fidelity).
        LOG(1, "bus read FAULT addr=%08X size=%d pc=%08X", addr, size, d && d->core ? d->core->cur_insn : 0);
        *fault = 1;
        return 0;
    }
    if (size == 1)
        return mmu_read_physical_uint8(g_mmu, addr);
    if (size == 2)
        return mmu_read_physical_uint16(g_mmu, addr);
    return mmu_read_physical_uint32(g_mmu, addr);
}

static void av_dsp_bus_write(void *ctx, uint32_t addr, uint32_t val, int size, int *fault) {
    av_dsp_t *d = (av_dsp_t *)ctx;
    uint32_t host;
    if (dsp_io_alias(addr, &host)) {
        addr = host;
    } else if (addr >= 0x40000000u) {
        LOG(1, "bus write FAULT addr=%08X val=%08X size=%d pc=%08X", addr, val, size,
            d && d->core ? d->core->cur_insn : 0);
        *fault = 1;
        return;
    }
    if (size == 1)
        mmu_write_physical_uint8(g_mmu, addr, (uint8_t)val);
    else if (size == 2)
        mmu_write_physical_uint16(g_mmu, addr, (uint16_t)val);
    else
        mmu_write_physical_uint32(g_mmu, addr, val);
}

// ============================================================
// DSP→host doorbell: BIO0 output transition → PSC L5 bit 0
// ============================================================

static void av_dsp_bio_transition(void *ctx, uint8_t old_pins, uint8_t new_pins) {
    av_dsp_t *d = (av_dsp_t *)ctx;
    av_psc_t *psc = dsp_psc(d);
    if (psc && ((old_pins ^ new_pins) & 1)) {
        LOG(3, "BIO0 %d->%d: L5 bit 0 latched (doorbell)", old_pins & 1, new_pins & 1);
        av_psc_level_latch(psc, AV_PSC_L5, 0);
    }
}

// ============================================================
// Burst execution
// ============================================================

// Arm (or leave armed) the burst event `cycles` main-CPU cycles from now.
static void av_dsp_arm(av_dsp_t *d, uint64_t cycles) {
    if (d->burst_armed)
        return;
    d->burst_armed = 1;
    scheduler_new_cpu_event(d->cfg->scheduler, &av_dsp_burst_event, d, 0, cycles, 0);
}

static void av_dsp_burst_event(void *source, uint64_t data) {
    (void)data;
    av_dsp_t *d = (av_dsp_t *)source;
    uint64_t now = scheduler_cpu_cycles(d->cfg->scheduler);
    d->burst_armed = 0;

    if (d->reset_held || !d->started) {
        d->last_burst_cycles = now;
        return; // parked until the reset latch releases
    }

    // Budget = elapsed main cycles × ratio, carry kept exact so the
    // long-run instruction rate matches the DSP clock.
    uint64_t budget_x256 = (now - d->last_burst_cycles) * d->ratio_x256 + d->carry_x256;
    uint32_t budget = (uint32_t)(budget_x256 >> 8);
    d->carry_x256 = (uint32_t)(budget_x256 & 0xFF);
    d->last_burst_cycles = now;

    if (budget)
        dsp3210_run(d->core, &budget);

    // Unspent budget means the core went idle mid-burst: drop it (idle
    // time retires nothing) and park unless something is pending.
    if (!dsp3210_is_idle(d->core))
        av_dsp_arm(d, AV_DSP_QUANTUM);
}

// ============================================================
// Board inputs
// ============================================================

void av_dsp_overrun_write(av_dsp_t *d, uint8_t bits, uint8_t written) {
    bool touches_reset = (written & 0x01) != 0; // the write addressed bit 0
    bool held = (bits & 1) != 0;
    d->overrun_bits = bits;
    if (!touches_reset)
        return; // pdspResetEn/pdspFrameOvr housekeeping only
    if (held) {
        if (!d->reset_held) {
            // $83/$81: hold in reset — state clears per the chip reset
            // spec; the burst event parks itself at its next firing.
            LOG(1, "reset asserted (dspOverRun=$%02X)", bits);
            d->reset_held = 1;
            dsp3210_reset(d->core, 0);
        }
    } else if (d->reset_held || !d->started) {
        // Bit-0 clear write: StartProcessorRoutine's release.  The first
        // one releases the power-on hardware reset (the latch already
        // read 0); execution starts at external physical 0 — the 7-word
        // bootstrap.
        LOG(1, "reset released — DSP running (bootstrap at physical 0)");
        d->reset_held = 0;
        dsp3210_reset(d->core, 0); // processor mode (straps 0)
        d->started = 1;
        d->carry_x256 = 0;
        d->last_burst_cycles = scheduler_cpu_cycles(d->cfg->scheduler);
        av_dsp_arm(d, 1);
        cpu_reschedule();
    }
}

void av_dsp_overrun_hook(void *ctx, uint8_t bits, uint8_t written) {
    av_dsp_overrun_write((av_dsp_t *)ctx, bits, written);
}

void av_dsp_irq(av_dsp_t *d, int vector) {
    if (!d || !d->started || d->reset_held)
        return;
    dsp3210_request_interrupt(d->core, vector);
    // Wake a parked core promptly: burst at the next cycle, and end the
    // main sprint at the next instruction boundary (the IRQ trick).
    av_dsp_arm(d, 1);
    cpu_reschedule();
}

// Frame tick: a short active-low pulse on EXT1 (IR1N), width in core time
// (instruction-slots).  It must outlast the kernel's 2-slot `if (ir1s)
// goto self` spin period (so a spin never misses a pulse) but expire
// before the calibration gadget's FIRST spin check runs (~9 slots after
// the tick: waiti latent + dispatch + handler prologue) — otherwise both
// of the gadget's spins fall through inside one pulse and the measured
// frame period (kernel data+$1D0, the device's total GPB) collapses to a
// few ticks, and the RTM rejects every sound task with $F5C8.
#define AV_DSP_EXT1_PULSE_SLOTS 4

void av_dsp_ext1_tick(av_dsp_t *d) {
    if (!d || !d->started || d->reset_held)
        return;
    dsp3210_ext_pulse(d->core, DSP3210_VEC_EXT1, AV_DSP_EXT1_PULSE_SLOTS);
    av_dsp_arm(d, 1);
    cpu_reschedule();
}

bool av_dsp_ext1_pending(av_dsp_t *d) {
    // "Previous tick never consumed": the request is still latched AND the
    // kernel is asking for frame interrupts.  In timer-driven steady state
    // EXT1 is masked (emr=$0200) and latched-but-ignored edges are normal.
    return d && (d->core->pending & (1u << DSP3210_VEC_EXT1)) != 0 && (d->core->emr & (1u << DSP3210_VEC_EXT1)) != 0;
}

bool av_dsp_running(av_dsp_t *d) {
    return d && d->started && !d->reset_held;
}

// ============================================================
// machine.dsp — the object node
// ============================================================

static inline av_dsp_t *dsp_self(struct object *self) {
    return (av_dsp_t *)object_data(self);
}

static const char *av_dsp_state_name(av_dsp_t *d) {
    if (!d->started || d->reset_held)
        return "reset";
    if (d->core->halted || d->core->level == DSP3210_LVL_DERROR)
        return "crashed";
    if (dsp3210_is_idle(d->core))
        return "idle";
    return "running";
}

static value_t dsp_attr_state(struct object *self, const member_t *m) {
    (void)m;
    av_dsp_t *d = dsp_self(self);
    return val_str(d ? av_dsp_state_name(d) : "reset");
}

static value_t dsp_attr_pc(struct object *self, const member_t *m) {
    (void)m;
    av_dsp_t *d = dsp_self(self);
    value_t v = val_uint(4, d ? d->core->pc : 0);
    v.flags |= VAL_HEX;
    return v;
}

static value_t dsp_attr_ps(struct object *self, const member_t *m) {
    (void)m;
    av_dsp_t *d = dsp_self(self);
    value_t v = val_uint(2, d ? d->core->ps : 0);
    v.flags |= VAL_HEX;
    return v;
}

static value_t dsp_attr_emr(struct object *self, const member_t *m) {
    (void)m;
    av_dsp_t *d = dsp_self(self);
    value_t v = val_uint(2, d ? d->core->emr : 0);
    v.flags |= VAL_HEX;
    return v;
}

static value_t dsp_attr_pcw(struct object *self, const member_t *m) {
    (void)m;
    av_dsp_t *d = dsp_self(self);
    value_t v = val_uint(2, d ? d->core->pcw : 0);
    v.flags |= VAL_HEX;
    return v;
}

static value_t dsp_attr_sp(struct object *self, const member_t *m) {
    (void)m;
    av_dsp_t *d = dsp_self(self);
    value_t v = val_uint(4, d ? d->core->r[21] : 0);
    v.flags |= VAL_HEX;
    return v;
}

static value_t dsp_attr_evtp(struct object *self, const member_t *m) {
    (void)m;
    av_dsp_t *d = dsp_self(self);
    value_t v = val_uint(4, d ? d->core->r[22] : 0);
    v.flags |= VAL_HEX;
    return v;
}

static value_t dsp_attr_instr_count(struct object *self, const member_t *m) {
    (void)m;
    av_dsp_t *d = dsp_self(self);
    return val_uint(8, d ? d->core->icount : 0);
}

static value_t dsp_method_step(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    av_dsp_t *d = dsp_self(self);
    if (!d)
        return val_err("dsp not available");
    if (!d->started || d->reset_held)
        return val_err("dsp is held in reset");
    uint64_t n = argc >= 1 ? argv[0].u : 1;
    uint64_t done = 0;
    while (done < n) {
        int st = dsp3210_step(d->core);
        if (st != DSP3210_STEP_OK)
            break;
        done++;
    }
    return val_uint(8, done);
}

// Read one instruction word through the core's own bus view: on-chip
// window through the core map, external space guest-physical.
static int dsp_peek_word(av_dsp_t *d, uint32_t addr, uint32_t *out) {
    uint32_t base = (d->core->pcw & (1u << 10)) ? 0u : 0x50030000u;
    if (addr - base < 0x10000u)
        return dsp3210_peek(d->core, addr, 4, out);
    if (addr >= 0x40000000u)
        return -1;
    *out = mmu_read_physical_uint32(g_mmu, addr & ~3u);
    return 0;
}

static value_t dsp_method_disasm(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    av_dsp_t *d = dsp_self(self);
    if (!d)
        return val_err("dsp not available");
    uint32_t addr = argc >= 1 ? (uint32_t)argv[0].u : d->core->pc;
    uint32_t count = argc >= 2 ? (uint32_t)argv[1].u : 16;
    if (count > 256)
        count = 256;
    size_t cap = (size_t)count * 160 + 1;
    char *buf = malloc(cap);
    if (!buf)
        return val_err("out of memory");
    size_t len = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = (addr & ~3u) + 4 * i;
        uint32_t w;
        if (dsp_peek_word(d, a, &w)) {
            len += (size_t)snprintf(buf + len, cap - len, "%08x: <bus error>\n", a);
            break;
        }
        dsp3210_insn ins;
        dsp3210_disassemble(w, a, &ins);
        len += (size_t)snprintf(buf + len, cap - len, "%08x: %08x  %s\n", a, w, ins.text);
        if (len + 160 >= cap)
            break;
    }
    value_t v = val_str(buf);
    free(buf);
    return v;
}

static const arg_decl_t dsp_step_args[] = {
    {.name = "count",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "instructions to execute (default 1)"},
};

static const arg_decl_t dsp_disasm_args[] = {
    {.name = "addr",
     .kind = V_UINT,
     .presentation_flags = VAL_HEX,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "start address (default: current pc)"},
    {.name = "count",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "instructions (default 16, max 256)"},
};

static const member_t av_dsp_members[] = {
    {.kind = M_ATTR,
     .name = "state",
     .doc = "reset | running | idle | crashed",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = dsp_attr_state, .set = NULL}},
    {.kind = M_ATTR,
     .name = "pc",
     .doc = "Program counter (next instruction)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = dsp_attr_pc, .set = NULL}},
    {.kind = M_ATTR,
     .name = "ps",
     .doc = "Processor status flags",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = dsp_attr_ps, .set = NULL}},
    {.kind = M_ATTR,
     .name = "emr",
     .doc = "Exception mask register",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = dsp_attr_emr, .set = NULL}},
    {.kind = M_ATTR,
     .name = "pcw",
     .doc = "Processor control word",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = dsp_attr_pcw, .set = NULL}},
    {.kind = M_ATTR,
     .name = "sp",
     .doc = "Stack pointer (r21)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = dsp_attr_sp, .set = NULL}},
    {.kind = M_ATTR,
     .name = "evtp",
     .doc = "Exception vector table pointer (r22)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = dsp_attr_evtp, .set = NULL}},
    {.kind = M_ATTR,
     .name = "instr_count",
     .doc = "DSP instructions executed since power-on",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = dsp_attr_instr_count, .set = NULL}},
    {.kind = M_METHOD,
     .name = "step",
     .doc = "Execute up to N instructions; returns the number executed",
     .method = {.args = dsp_step_args, .nargs = 1, .result = V_UINT, .fn = dsp_method_step}},
    {.kind = M_METHOD,
     .name = "disasm",
     .doc = "Disassemble N instructions through the DSP's own bus view",
     .method = {.args = dsp_disasm_args, .nargs = 2, .result = V_STRING, .fn = dsp_method_disasm}},
};

const class_desc_t av_dsp_class = {
    .name = "dsp",
    .members = av_dsp_members,
    .n_members = sizeof(av_dsp_members) / sizeof(av_dsp_members[0]),
};

// ============================================================
// Lifecycle
// ============================================================

av_dsp_t *av_dsp_init(config_t *cfg, checkpoint_t *cp) {
    av_dsp_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->core = calloc(1, sizeof(*d->core));
    if (!d->core) {
        free(d);
        return NULL;
    }
    d->cfg = cfg;

    // DSP clock from the profile's aux_cpus entry (66.6667 MHz Cyclone /
    // 55.5 MHz Tempest); instructions take 4 CKI.
    uint32_t dsp_freq = 66666667;
    if (cfg->machine->aux_cpus && cfg->machine->aux_cpus[0].name)
        dsp_freq = cfg->machine->aux_cpus[0].freq;
    d->ratio_x256 = (uint32_t)(((uint64_t)dsp_freq * 256 + 2 * cfg->machine->freq) / (4ull * cfg->machine->freq));

    dsp3210_init(d->core, NULL, 0);
    dsp3210_reset(d->core, 0); // processor-mode straps

    if (cp) {
        size_t data_size = offsetof(av_dsp_t, cfg);
        system_read_checkpoint_data(cp, d, data_size);
        system_read_checkpoint_data(cp, d->core, DSP3210_CHECKPOINT_SIZE);
    }

    // Bus + doorbell wiring survives restore (pointers re-planted here).
    d->core->read_fn = av_dsp_bus_read;
    d->core->write_fn = av_dsp_bus_write;
    d->core->hook_ctx = d;
    d->core->bio_fn = av_dsp_bio_transition;
    d->core->bio_ctx = d;

    scheduler_new_event_type(cfg->scheduler, "dsp", d, "burst", &av_dsp_burst_event);

    d->object = object_new(&av_dsp_class, d, "dsp");
    if (d->object) {
        object_set_label(d->object, "DSP3210");
        object_set_order(d->object, 15);
        object_attach(machine_object(), d->object);
    }

    LOG(1, "DSP3210 init (%u Hz, ratio_x256=%u)", dsp_freq, d->ratio_x256);
    return d;
}

void av_dsp_delete(av_dsp_t *d) {
    if (!d)
        return;
    if (d->object) {
        object_detach(d->object);
        object_delete(d->object);
    }
    if (d->cfg && d->cfg->scheduler)
        remove_event(d->cfg->scheduler, &av_dsp_burst_event, d);
    free(d->core);
    free(d);
}

void av_dsp_checkpoint(av_dsp_t *d, checkpoint_t *cp) {
    if (!d || !cp)
        return;
    size_t data_size = offsetof(av_dsp_t, cfg);
    system_write_checkpoint_data(cp, d, data_size);
    system_write_checkpoint_data(cp, d->core, DSP3210_CHECKPOINT_SIZE);
}
