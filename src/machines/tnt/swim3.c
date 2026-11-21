// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// swim3.c
// The TNT family's face of the shared SWIM3 model (core/peripherals/
// swim3.c): sixteen byte-wide registers on $10 centres at Grand Central
// +$15000 (index = offset >> 4), the IRQ pin on Grand Central interrupt 19,
// and the data path on DBDMA channel 1.
//
// The byte ring between the engine and the channel, and the DBDMA device
// port over it, are the shared runtime/swim3_dbdma.c (Heathrow feeds its
// SWIM3 the same way); this file names Grand Central's channel and
// interrupt and decodes the register window.
//
// Sources: Apple, SWIM3 ERS v1.2; the Grand Central register map (Linux
// drivers/block/swim3.c, `struct swim3` at 16-byte stride, DBDMA channel 1
// for the floppy; NetBSD macppc); Open Firmware's own `swim3` package,
// which allocates its buffers with dma-alloc/dma-map-in — the DBDMA path.

#include "tnt.h"

#include "swim3.h"
#include "swim3_dbdma.h"

#define FD_CHAN 1 // Grand Central DBDMA channel 1: the floppy

// The chip's IRQ pin: Grand Central interrupt 19
static void fd_set_irq(void *ctx, bool level) {
    tnt_gc_set_source((config_t *)ctx, TNT_INT_SWIM3, level);
}

// --- board glue -------------------------------------------------------------

void tnt_swim3_bind(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    swim3_dbdma_bind(&st->fdport, &st->swim3, cfg->floppy, cfg->scheduler);
}

void tnt_swim3_init(config_t *cfg) {
    swim3_dbdma_init(&tnt_st(cfg)->fdport, tnt_st(cfg)->dbdma, FD_CHAN, fd_set_irq, cfg);
}

void tnt_swim3_register_events(config_t *cfg) {
    swim3_register_events(&tnt_st(cfg)->swim3);
    swim3_xfer_register_events(&tnt_st(cfg)->swim3);
}

uint8_t tnt_swim3_read(config_t *cfg, uint32_t off) {
    return swim3_read(&tnt_st(cfg)->swim3, (off >> 4) & 15u);
}

uint8_t tnt_swim3_peek(config_t *cfg, uint32_t off) {
    return swim3_peek(&tnt_st(cfg)->swim3, (off >> 4) & 15u);
}

void tnt_swim3_write(config_t *cfg, uint32_t off, uint8_t value) {
    swim3_write(&tnt_st(cfg)->swim3, (off >> 4) & 15u, value);
}
