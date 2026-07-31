// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mace.c
// MACE register stub + Apple address PROM — see mace.h.
//
// Registers latch and read back at their $10 stride, with the handful of
// architected read-only values the driver checks: CHIPID ($40/$41 for the
// Am79C940 rev), the read-to-clear interrupt register (always 0 — nothing
// can interrupt without a datapath), the empty FIFO frame counts, and the
// BIU config.  No transmit or receive path exists, so the driver's
// loopback self-tests fail and `.ENET` does not load — the documented
// no-Ethernet contract (IMPLEMENTATION.md §7).

#include "mace.h"

#include "av.h"

#include "cpu.h"
#include "log.h"
#include "system.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mace");

#define AV_MACE_REGS 32 // $000-$1F0 on a $10 stride

// Register indices (offset >> 4; mace.md §2).
#define MACE_IR       8 // interrupt register, read-to-clear
#define MACE_IMR      9 // interrupt mask (1 = masked)
#define MACE_PR       10 // poll register
#define MACE_BIUCC    12 // bus interface unit config
#define MACE_FIFOCC   13 // FIFO config
#define MACE_PLSCC    15 // PLS config
#define MACE_CHIPIDLO 24 // chip ID low byte
#define MACE_CHIPIDHI 25 // chip ID high byte

// Am79C940 revision the Curio integrates (mace.md §2 chip-ID register).
#define MACE_CHIPID 0x0940

// The Apple address PROM (mace.md §4): the station address bytes read
// backwards from $51 and bit-reversed by the driver's NormAddr; the XOR of
// all eight bytes must equal $FF.  Locally-administered 02:00:00:09:07:02
// -> bit-reversed 40 00 00 90 E0 40, with byte 7 chosen for the checksum.
static const uint8_t av_mace_prom[8] = {0x40, 0x00, 0x00, 0x90, 0xE0, 0x40, 0x00, 0xCF};

struct av_mace {
    // --- plain data (checkpointed up to the first pointer field) ---
    uint8_t regs[AV_MACE_REGS];

    // --- pointers (not checkpointed) ---
    config_t *cfg;
};

static inline av_mace_t *mace_of(config_t *cfg) {
    return ((av_state_t *)cfg->machine_context)->mace;
}

uint8_t av_mace_read(config_t *cfg, uint32_t addr) {
    av_mace_t *m = mace_of(cfg);
    uint32_t reg = ((addr & 0x1FFu) >> 4) % AV_MACE_REGS;
    switch (reg) {
    case MACE_IR: {
        // Read-to-clear; nothing can interrupt with no datapath.
        uint8_t v = m->regs[MACE_IR];
        m->regs[MACE_IR] = 0;
        return v;
    }
    case MACE_PR:
        return 0; // no transmit/receive requests pending
    case MACE_CHIPIDLO:
        return (uint8_t)MACE_CHIPID;
    case MACE_CHIPIDHI:
        return (uint8_t)(MACE_CHIPID >> 8);
    default:
        return m->regs[reg];
    }
}

void av_mace_write(config_t *cfg, uint32_t addr, uint8_t value) {
    av_mace_t *m = mace_of(cfg);
    uint32_t reg = ((addr & 0x1FFu) >> 4) % AV_MACE_REGS;
    if (reg == MACE_IR || reg == MACE_PR || reg == MACE_CHIPIDLO || reg == MACE_CHIPIDHI)
        return; // read-only
    m->regs[reg] = value;
    LOG(3, "reg %u = $%02X (pc=%08X)", reg, value, cpu_get_pc(cfg->cpu));
}

uint8_t av_mace_prom_read(config_t *cfg, uint32_t addr) {
    (void)cfg;
    // One byte per 16-byte group, at offset $x1 of each.
    if ((addr & 0xFu) != 1)
        return 0xFF;
    return av_mace_prom[(addr >> 4) & 7];
}

void av_mace_prom_write(config_t *cfg, uint32_t addr, uint8_t value) {
    LOG(2, "PROM write $%X = $%02X ignored (read-only)", addr & 0xFFu, value);
    (void)cfg;
}

// ============================================================
// Lifecycle
// ============================================================

av_mace_t *av_mace_init(config_t *cfg, checkpoint_t *cp) {
    av_mace_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->cfg = cfg;
    m->regs[MACE_BIUCC] = 0x00;
    m->regs[MACE_FIFOCC] = 0x00;
    m->regs[MACE_PLSCC] = 0x00;
    if (cp) {
        size_t data_size = offsetof(av_mace_t, cfg);
        system_read_checkpoint_data(cp, m, data_size);
    }
    return m;
}

void av_mace_delete(av_mace_t *mace) {
    free(mace);
}

void av_mace_checkpoint(av_mace_t *mace, checkpoint_t *cp) {
    if (!mace || !cp)
        return;
    size_t data_size = offsetof(av_mace_t, cfg);
    system_write_checkpoint_data(cp, mace, data_size);
}
