// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// oss.c
// Macintosh IIfx Operating System Support interrupt controller.

#include "oss.h"

#include "system.h"

#include <stdlib.h>
#include <string.h>

// Number of software-visible OSS interrupt sources.
#define OSS_NUM_SOURCES 15

// OSS register offsets (canonical IIfx hardware layout).
#define OSS_LEVEL_FIRST 0x000
#define OSS_LEVEL_LAST  0x00E
#define OSS_INT_STAT    0x202
#define OSS_ROM_CTRL    0x204
#define OSS_COUNTER_CTL 0x205
#define OSS_INPUT_STAT  0x206
#define OSS_60HZ_ACK    0x207
#define OSS_COUNTER     0x208

// OSS source numbers used by the IIfx ROM.
#define OSS_SRC_60HZ 10

// Concrete OSS state hidden behind the public handle.
struct oss {
    uint8_t level[OSS_NUM_SOURCES];
    uint16_t pending;
    uint8_t rom_ctrl;
    uint8_t counter_ctl;
    uint64_t counter;

    memory_interface_t memory_interface;
    oss_irq_fn irq_cb;
    oss_control_fn control_cb;
    void *cb_context;
};

// Notifies the owning machine that CPU IPL may need recomputing.
static void oss_notify(oss_t *oss) {
    if (oss && oss->irq_cb)
        oss->irq_cb(oss->cb_context);
}

// Reads one big-endian byte from a 32-bit register value.
static uint8_t be32_byte(uint32_t value, unsigned index) {
    return (uint8_t)(value >> ((3u - (index & 3u)) * 8u));
}

// Clears pending bits selected by a byte write to the long status register.
static void clear_status_byte(oss_t *oss, uint32_t addr, uint8_t value) {
    unsigned lane = addr & 3u;
    uint32_t mask = (uint32_t)value << ((3u - lane) * 8u);
    uint16_t old_pending = oss->pending;
    oss->pending &= (uint16_t)~mask;
    if (oss->pending != old_pending)
        oss_notify(oss);
}

// Reads one OSS byte register.
static uint8_t oss_read_uint8(void *device, uint32_t addr) {
    oss_t *oss = (oss_t *)device;
    uint32_t offset = addr & 0x1fff;

    if (offset <= OSS_LEVEL_LAST)
        return oss->level[offset] & 7u;

    if (offset >= 0x200 && offset <= 0x203)
        return be32_byte((uint32_t)oss->pending, offset - 0x200);

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
        uint64_t value = oss->counter;
        if ((oss->counter_ctl & 1u) == 0)
            oss->counter++;
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

    if (offset >= 0x200 && offset <= 0x203) {
        clear_status_byte(oss, offset - 0x200, value);
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
oss_t *oss_init(oss_irq_fn irq_cb, oss_control_fn control_cb, void *context, checkpoint_t *checkpoint) {
    oss_t *oss = calloc(1, sizeof(*oss));
    if (!oss)
        return NULL;

    oss->irq_cb = irq_cb;
    oss->control_cb = control_cb;
    oss->cb_context = context;
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
    // See IIfx-ROM.asm §16c (table-population) and §16f (phase $92)
    // for the full discussion.
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
        system_read_checkpoint_data(checkpoint, oss->level, sizeof(oss->level));
        system_read_checkpoint_data(checkpoint, &oss->pending, sizeof(oss->pending));
        system_read_checkpoint_data(checkpoint, &oss->rom_ctrl, sizeof(oss->rom_ctrl));
        system_read_checkpoint_data(checkpoint, &oss->counter_ctl, sizeof(oss->counter_ctl));
        system_read_checkpoint_data(checkpoint, &oss->counter, sizeof(oss->counter));
    }

    return oss;
}

// Frees an OSS instance.
void oss_delete(oss_t *oss) {
    free(oss);
}

// Saves OSS plain state to a checkpoint.
void oss_checkpoint(oss_t *oss, checkpoint_t *checkpoint) {
    if (!oss || !checkpoint)
        return;
    system_write_checkpoint_data(checkpoint, oss->level, sizeof(oss->level));
    system_write_checkpoint_data(checkpoint, &oss->pending, sizeof(oss->pending));
    system_write_checkpoint_data(checkpoint, &oss->rom_ctrl, sizeof(oss->rom_ctrl));
    system_write_checkpoint_data(checkpoint, &oss->counter_ctl, sizeof(oss->counter_ctl));
    system_write_checkpoint_data(checkpoint, &oss->counter, sizeof(oss->counter));
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
