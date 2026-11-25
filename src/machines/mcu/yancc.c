// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// yancc.c
// The YANCC/Kiwi NuBus controller register file -- see yancc.h.

#include "yancc.h"

#include "log.h"
#include "regfile.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("board");

void yancc_init(yancc_t *y, const char *name, yancc_irq_fn err_irq, void *context) {
    y->name = name;
    y->err_irq = err_irq;
    y->irq_ctx = context;
}

void yancc_reset(yancc_t *y) {
    bool was = yancc_err_irq(y);
    memset(y->regs, 0, sizeof y->regs);
    y->touched = 0;
    // A request pending at /RESET goes away with the flag.
    if (was && y->err_irq)
        y->err_irq(y->irq_ctx, false);
}

bool yancc_err_irq(const yancc_t *y) {
    return (y->regs[YANCC_BIT_ERR] & 1u) && (y->regs[YANCC_BIT_ERR_EN] & 1u);
}

uint8_t yancc_read(yancc_t *y, uint32_t off, bool peek, uint32_t pc) {
    uint32_t idx = (off >> 2) % YANCC_REG_COUNT;
    uint32_t v = y->regs[idx];
    // Log-once per register: the read path sets the bit too, or a register
    // the ROM only reads would log on every read.
    if (!peek && !(y->touched & (1ull << idx))) {
        y->touched |= 1ull << idx;
        LOG(2, "%s read  $%04X -> $%08X (pc=%08X)", y->name, off, v, pc);
    }
    return be_lane8(v, off & 3u);
}

void yancc_write(yancc_t *y, uint32_t off, uint8_t value, uint32_t pc) {
    uint32_t idx = (off >> 2) % YANCC_REG_COUNT;
    bool was = yancc_err_irq(y);
    be_lane8_set(&y->regs[idx], off & 3u, value);
    LOG(2, "%s write $%04X = $%08X (pc=%08X)", y->name, off, y->regs[idx], pc);
    y->touched |= 1ull << idx;
    bool now = yancc_err_irq(y);
    // The level-7 request follows the AND of the flag and its enable.
    if (now != was && y->err_irq)
        y->err_irq(y->irq_ctx, now);
}
