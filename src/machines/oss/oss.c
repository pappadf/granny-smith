// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// oss.c
// Macintosh IIfx Operating System Support interrupt controller.

#include "oss.h"

#include "irq_controller.h"
#include "log.h"
#include "machine.h"
#include "object.h"
#include "regfile.h"
#include "scheduler.h"
#include "system.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

// Number of software-visible OSS interrupt sources.
#define OSS_NUM_SOURCES 15

// OSS register offsets (canonical IIfx hardware layout).
#define OSS_LEVEL_FIRST 0x000
#define OSS_LEVEL_LAST  0x00E
// Interrupt-status longword.  The four byte lanes live at $200-$203; OSS_INT_STAT
// is lane 2, the one the ROM's byte accesses use.  Both decode sites open-coded
// the range and the define was never referenced.
#define OSS_INT_STAT_BASE 0x200
#define OSS_INT_STAT      0x202
#define OSS_ROM_CTRL      0x204
#define OSS_COUNTER_CTL   0x205
#define OSS_INPUT_STAT    0x206
#define OSS_60HZ_ACK      0x207
#define OSS_COUNTER       0x208

// OSS source numbers used by the IIfx ROM.
#define OSS_SRC_60HZ 10

// Concrete OSS state hidden behind the public handle.
struct oss {
    uint8_t level[OSS_NUM_SOURCES];
    uint16_t pending;
    uint8_t rom_ctrl;
    uint8_t counter_ctl;
    // Free-running counter, derived from emulated time rather than stored as
    // a running value: `counter_base` is its value at `counter_base_ns`, and
    // a read adds the elapsed ticks.  Writing the control register rebases
    // both, so start/stop is exact.  See oss_counter_value().
    uint64_t counter_base;
    uint64_t counter_base_ns;

    memory_interface_t memory_interface;
    oss_irq_fn irq_cb;
    oss_control_fn control_cb;
    void *cb_context;
    struct scheduler *scheduler; // counter time base; not checkpointed
    struct object *object; // machine.oss (F-26); after the blob, never saved
};

// Notifies the owning machine that CPU IPL may need recomputing.
static void oss_notify(oss_t *oss) {
    if (oss && oss->irq_cb)
        oss->irq_cb(oss->cb_context);
}

// Reads one big-endian byte from a 32-bit register value.
// Clears pending bits selected by a byte write to the long status register.
// Write-1-to-clear one byte lane of the 32-bit interrupt-status word.  `lane`
// is a lane index 0-3 (lane 0 is the MSB on this bus), not an address -- the
// parameter used to be named `addr` while every caller passed an index, which
// is how an off-by-$200 gets introduced later.
static void clear_status_byte(oss_t *oss, uint32_t lane, uint8_t value) {
    lane &= 3u;
    uint32_t mask = (uint32_t)value << ((3u - lane) * 8u);
    uint16_t old_pending = oss->pending;
    oss->pending &= (uint16_t)~mask;
    if (oss->pending != old_pending)
        oss_notify(oss);
}

// Current value of the free-running counter.
//
// The counter advances with EMULATED TIME.  It used to be incremented once per
// byte read -- so reading it as eight byte accesses advanced it eight times, a
// 32-bit read four times, and its rate was a function of the guest's own
// access pattern rather than of time (05-chipsets-irq F-14).  Every other
// timer in the tree is scheduler-derived: VIA T1/T2, the RBV and DAFB Swatch,
// the PSC's sndPhase/UTSC, the PPC decrementer.  The OSS was the outlier.
//
// RATE IS UNATTESTED.  Neither the F19 theory-of-operation volumes nor
// docs/machines/oss/iifx.md states what clock drives it, so this follows the
// precedent the finding names -- psc_utsc(), which is scheduler_time_ns()/1000
// -- and ticks at 1 MHz.  Nothing in the corpus reads the counter at all
// (measured across iifx-mactest, iifx-marathon and iifx-install-76: zero
// reads), so no behaviour depends on the choice today; a source that settles
// the real rate should change the divisor here and nothing else.
//
// Control bit 0 stops the count.  A read is now side-effect-free, which also
// means a memory.peek of $208-$20F no longer perturbs guest-visible state.
static uint64_t oss_counter_value(const oss_t *oss) {
    if (oss->counter_ctl & 1u)
        return oss->counter_base; // stopped: frozen where it was rebased
    uint64_t now_ns = (uint64_t)scheduler_time_ns(oss->scheduler);
    uint64_t elapsed_us = (now_ns - oss->counter_base_ns) / 1000u;
    return oss->counter_base + elapsed_us;
}

// Reads one OSS byte register.
static uint8_t oss_read_uint8(void *device, uint32_t addr) {
    oss_t *oss = (oss_t *)device;
    uint32_t offset = addr & 0x1fff;

    if (offset <= OSS_LEVEL_LAST)
        return oss->level[offset] & 7u;

    if (offset >= OSS_INT_STAT_BASE && offset <= OSS_INT_STAT_BASE + 3)
        return be_lane8((uint32_t)oss->pending, offset - OSS_INT_STAT_BASE);

    if (offset == OSS_ROM_CTRL)
        return oss->rom_ctrl;
    if (offset == OSS_COUNTER_CTL)
        return oss->counter_ctl;
    if (offset == OSS_INPUT_STAT)
        return 0;
    if (offset == OSS_60HZ_ACK) {
        oss_set_source(oss, OSS_SRC_60HZ, false);
        return 0;
    }
    if (offset >= OSS_COUNTER && offset < OSS_COUNTER + 8) {
        uint64_t value = oss_counter_value(oss);
        return (uint8_t)(value >> ((7u - ((offset - OSS_COUNTER) & 7u)) * 8u));
    }

    return 0;
}

// Reads one OSS word register.
static uint16_t oss_read_uint16(void *device, uint32_t addr) {
    uint16_t hi = oss_read_uint8(device, addr);
    uint16_t lo = oss_read_uint8(device, addr + 1);
    return (uint16_t)((hi << 8) | lo);
}

// Reads one OSS long register.
static uint32_t oss_read_uint32(void *device, uint32_t addr) {
    uint32_t b0 = oss_read_uint8(device, addr);
    uint32_t b1 = oss_read_uint8(device, addr + 1);
    uint32_t b2 = oss_read_uint8(device, addr + 2);
    uint32_t b3 = oss_read_uint8(device, addr + 3);
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

// Writes one OSS byte register.
static void oss_write_uint8(void *device, uint32_t addr, uint8_t value) {
    oss_t *oss = (oss_t *)device;
    uint32_t offset = addr & 0x1fff;

    if (offset <= OSS_LEVEL_LAST) {
        oss->level[offset] = value & 7u;
        // OSS self-test quirk for the 60Hz source (offset $0A = level[10]):
        // writing a non-zero priority pulses the source pending bit;
        // writing 0 clears it.  Phase $92 of the IIfx ROM POST
        // (at $4084306E) exercises this — it walks D6 from 6 down to 1,
        // writing each value to OSS+$0A and expecting the corresponding
        // autovector to fire from a clean state.  See the OSS section in
        // src/machines/iifx.c for the rationale.
        // Other level[] registers correspond to real hardware sources
        // (SCC, SCSI, VIA1, IOPs, NuBus...) that have their own pending-
        // bit drivers — only level[10] has this self-test behaviour.
        if (offset == OSS_SRC_60HZ) {
            if ((value & 7u) != 0) {
                // Non-zero priority write: pulse source 10 pending.
                oss_set_source(oss, OSS_SRC_60HZ, true);
            } else {
                // Write of 0 (disable): clear source 10 pending.
                oss_set_source(oss, OSS_SRC_60HZ, false);
            }
            // oss_set_source already calls oss_notify on a change.
            return;
        }
        oss_notify(oss);
        return;
    }

    if (offset >= OSS_INT_STAT_BASE && offset <= OSS_INT_STAT_BASE + 3) {
        clear_status_byte(oss, offset - OSS_INT_STAT_BASE, value);
        return;
    }

    if (offset == OSS_ROM_CTRL) {
        oss->rom_ctrl = value;
        if (oss->control_cb)
            oss->control_cb(oss->cb_context, value);
        oss_notify(oss);
        return;
    }
    if (offset == OSS_COUNTER_CTL) {
        // Rebase across the transition so neither starting nor stopping the
        // counter loses or invents ticks.
        oss->counter_base = oss_counter_value(oss);
        oss->counter_base_ns = (uint64_t)scheduler_time_ns(oss->scheduler);
        oss->counter_ctl = value;
        return;
    }
    if (offset == OSS_60HZ_ACK) {
        oss_set_source(oss, OSS_SRC_60HZ, false);
        return;
    }
}

// Writes one OSS word register.
static void oss_write_uint16(void *device, uint32_t addr, uint16_t value) {
    oss_write_uint8(device, addr, (uint8_t)(value >> 8));
    oss_write_uint8(device, addr + 1, (uint8_t)value);
}

// Writes one OSS long register.
static void oss_write_uint32(void *device, uint32_t addr, uint32_t value) {
    oss_write_uint8(device, addr, (uint8_t)(value >> 24));
    oss_write_uint8(device, addr + 1, (uint8_t)(value >> 16));
    oss_write_uint8(device, addr + 2, (uint8_t)(value >> 8));
    oss_write_uint8(device, addr + 3, (uint8_t)value);
}

// Creates an OSS instance with ROM-like default source priorities.
// === Object node: machine.oss (05-chipsets-irq F-26) ========================
//
// The IIfx has no VIA2 and no RBV; the OSS *is* its interrupt controller, so
// before this node an IRQ storm on a IIfx was not inspectable at all.  The
// chip-specific half matters here more than anywhere: the OSS has no mask
// register, and "disabled" means a source whose level register reads 0, so
// `source_levels` is the thing an investigation actually needs.

static uint32_t oss_obj_pending(void *ctx) {
    return oss_pending((const oss_t *)ctx);
}

// The OSS has no enable mask.  A source is enabled exactly when its level
// register is non-zero -- "Writing 0 disables that source" (IIfx note),
// which is also why level[10] starts at 0 above.
static uint32_t oss_obj_enabled(void *ctx) {
    const oss_t *oss = (const oss_t *)ctx;
    uint32_t mask = 0;
    for (int i = 0; i < OSS_NUM_SOURCES; i++) {
        if (oss->level[i] & 7u)
            mask |= 1u << i;
    }
    return mask;
}

static int oss_obj_ipl(void *ctx) {
    return (int)oss_highest_ipl((const oss_t *)ctx);
}

static int oss_obj_level_count(void *ctx) {
    (void)ctx;
    return 7; // IPL 1..7
}

static uint32_t oss_obj_level(void *ctx, int index) {
    const oss_t *oss = (const oss_t *)ctx;
    uint32_t mask = 0;
    for (int i = 0; i < OSS_NUM_SOURCES; i++) {
        if ((oss->level[i] & 7u) == (uint8_t)(index + 1))
            mask |= 1u << i;
    }
    return mask;
}

static const irq_controller_ops_t oss_irq_ops = {
    .chip = "OSS",
    .pending = oss_obj_pending,
    .enabled = oss_obj_enabled,
    .ipl = oss_obj_ipl,
    .level_count = oss_obj_level_count,
    .level = oss_obj_level,
    .level_base = 1,
};

// The per-source programmed level, in source order -- the OSS's own view,
// and the inverse of `levels`.  Reading both together is how you tell a
// source that is shouting from a source that was programmed to the wrong
// priority.
static value_t oss_attr_source_levels(struct object *self, const member_t *m) {
    (void)m;
    const oss_t *oss = (const oss_t *)object_data(self);
    value_t *items = (value_t *)calloc(OSS_NUM_SOURCES, sizeof(value_t));
    if (!items)
        return val_err("oss.source_levels: out of memory");
    for (int i = 0; i < OSS_NUM_SOURCES; i++)
        items[i] = val_uint(1, oss->level[i] & 7u);
    return val_list(items, OSS_NUM_SOURCES);
}

static value_t oss_attr_rom_ctrl(struct object *self, const member_t *m) {
    (void)m;
    value_t v = val_uint(1, ((const oss_t *)object_data(self))->rom_ctrl);
    v.flags |= VAL_HEX;
    return v;
}

static value_t oss_attr_counter_ctl(struct object *self, const member_t *m) {
    (void)m;
    value_t v = val_uint(1, ((const oss_t *)object_data(self))->counter_ctl);
    v.flags |= VAL_HEX;
    return v;
}

static value_t oss_attr_counter(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, oss_counter_value((const oss_t *)object_data(self)));
}

static const member_t oss_members[] = {
    IRQ_CONTROLLER_MEMBERS(&oss_irq_ops){
                                         .kind = M_ATTR,
                                         .name = "source_levels",
                                         .doc = "Programmed CPU level per OSS source, source order (0 = disabled)",
                                         .flags = VAL_RO,
                                         .attr = {.type = V_LIST, .presentation_flags = VAL_VOLATILE, .get = oss_attr_source_levels, .set = NULL}},
    {.kind = M_ATTR,
                                         .name = "rom_ctrl",
                                         .doc = "ROM control register ($204)",
                                         .flags = VAL_RO,
                                         .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = oss_attr_rom_ctrl, .set = NULL}          },
    {.kind = M_ATTR,
                                         .name = "counter_ctl",
                                         .doc = "Free-running counter control ($20C)",
                                         .flags = VAL_RO,
                                         .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = oss_attr_counter_ctl, .set = NULL}       },
    {.kind = M_ATTR,
                                         .name = "counter",
                                         .doc = "Free-running counter, derived from emulated time",
                                         .flags = VAL_RO,
                                         .attr = {.type = V_UINT, .presentation_flags = VAL_VOLATILE, .get = oss_attr_counter, .set = NULL}      },
};

static const class_desc_t oss_class = {
    .name = "irq_controller",
    .members = oss_members,
    .n_members = sizeof(oss_members) / sizeof(oss_members[0]),
};

static void oss_attach_object(oss_t *oss) {
    oss->object = object_new(&oss_class, oss, "oss");
    if (!oss->object)
        return;
    object_set_order(oss->object, 45); // between the VIAs (40) and the RTC (60)
    object_attach(machine_object(), oss->object);
}

oss_t *oss_init(oss_irq_fn irq_cb, oss_control_fn control_cb, void *context, struct scheduler *scheduler,
                checkpoint_t *checkpoint) {
    oss_t *oss = calloc(1, sizeof(*oss));
    if (!oss)
        return NULL;

    oss->irq_cb = irq_cb;
    oss->control_cb = control_cb;
    oss->cb_context = context;
    oss->scheduler = scheduler;
    oss->rom_ctrl = 0x0d;

    // Default level[] state.  These specific non-zero values are what
    // test #$11 (called from §16b at $40841282) expects to find when
    // it reads OSS level registers — empirically validated by live
    // trace: with these defaults, test #$11 leaves a properly-formed
    // `(ptr, size, $FFFFFFFF, ...)` table at $FFFFEC..$FFFFFC that
    // §16c walks correctly to find its sentinel.  Changing any of
    // these alters test #$11's RAM-write side effects and breaks
    // §16c.
    //
    // SPECIAL CASE: level[10] = 0 (NOT 1).  This is the OSS source 10
    // (60Hz) priority.  At hardware reset, all level registers are 0
    // (per IIfx note: "Writing 0 disables that source").  Phase $92
    // of POST ($4084306E) saves the current level[10] on entry and
    // restores it on exit; if level[10] starts at a non-zero value
    // (e.g. 1), the restore-write triggers our pulse-on-write quirk
    // (see oss_write_uint8 below), firing a spurious source-10 IRQ
    // at the end of phase $92 that vectors through whatever the OS
    // installed at the level-1 autovector — usually wrong.  With
    // level[10] = 0 default, the save/restore is a no-op and phase
    // $92 exits cleanly.
    //
    // This mirrors the IIfx ROM's POST table-population and phase-$92
    // exit.
    for (int i = 0; i <= 5; i++)
        oss->level[i] = 2;
    oss->level[6] = 1;
    oss->level[7] = 4;
    oss->level[8] = 2;
    oss->level[9] = 2;
    oss->level[10] = 0; // see SPECIAL CASE comment above
    oss->level[11] = 1;
    oss->level[12] = 3;
    oss->level[13] = 1;
    oss->level[14] = 7;

    oss->memory_interface = (memory_interface_t){
        .read_uint8 = oss_read_uint8,
        .read_uint16 = oss_read_uint16,
        .read_uint32 = oss_read_uint32,
        .write_uint8 = oss_write_uint8,
        .write_uint16 = oss_write_uint16,
        .write_uint32 = oss_write_uint32,
    };

    if (checkpoint) {
        // Mirrors oss_checkpoint: one blob, so the two halves cannot drift.
        system_read_checkpoint_data(checkpoint, oss, offsetof(oss_t, memory_interface));
    }

    oss_attach_object(oss);
    return oss;
}

// Frees an OSS instance.
void oss_delete(oss_t *oss) {
    if (oss && oss->object) {
        object_detach(oss->object);
        object_delete(oss->object);
    }
    free(oss);
}

// Saves OSS plain state to a checkpoint.
// One blob of everything before the first pointer, the idiom via.c, rbv.c,
// psc.c, new_age.c and civic.c already use and swim3.h:51-53 documents.
// This was six per-field calls whose order had to be kept in step BY HAND
// with six more in oss_init -- the idiom 05-chipsets-irq F-37 calls the most
// error-prone of the three in the tree, and the one F-08's missing field
// lived in.  A field added to the struct prefix is now carried automatically
// instead of being silently dropped.
void oss_checkpoint(oss_t *oss, checkpoint_t *checkpoint) {
    if (!oss || !checkpoint)
        return;
    system_write_checkpoint_data(checkpoint, oss, offsetof(oss_t, memory_interface));
}

// Returns the OSS memory interface.
const memory_interface_t *oss_get_memory_interface(oss_t *oss) {
    return oss ? &oss->memory_interface : NULL;
}

// Sets or clears one OSS source.
void oss_set_source(oss_t *oss, int source, bool active) {
    if (!oss || source < 0 || source >= OSS_NUM_SOURCES)
        return;
    uint16_t old_pending = oss->pending;
    uint16_t bit = (uint16_t)(1u << source);
    if (active)
        oss->pending |= bit;
    else
        oss->pending &= (uint16_t)~bit;
    if (oss->pending != old_pending)
        oss_notify(oss);
}

// Sets or clears a group of OSS sources.
void oss_set_source_mask(oss_t *oss, uint16_t mask, bool active) {
    if (!oss)
        return;
    uint16_t old_pending = oss->pending;
    if (active)
        oss->pending |= (uint16_t)(mask & 0x7fffu);
    else
        oss->pending &= (uint16_t) ~(mask & 0x7fffu);
    if (oss->pending != old_pending)
        oss_notify(oss);
}

// Returns the pending-source bit mask.
uint16_t oss_pending(const oss_t *oss) {
    return oss ? oss->pending : 0;
}

// Returns the programmed interrupt level for one source.
uint8_t oss_level(const oss_t *oss, int source) {
    if (!oss || source < 0 || source >= OSS_NUM_SOURCES)
        return 0;
    return oss->level[source] & 7u;
}

// Computes the highest active interrupt priority.
uint8_t oss_highest_ipl(const oss_t *oss) {
    if (!oss)
        return 0;
    uint8_t ipl = 0;
    for (int i = 0; i < OSS_NUM_SOURCES; i++) {
        if ((oss->pending & (1u << i)) && oss->level[i] > ipl)
            ipl = oss->level[i];
    }
    return ipl;
}
