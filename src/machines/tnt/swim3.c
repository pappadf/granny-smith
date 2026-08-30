// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// swim3.c
// The TNT family's face of the shared SWIM3 model (core/peripherals/
// swim3.c): sixteen byte-wide registers on $10 centres at Grand Central
// +$15000 (index = offset >> 4), the IRQ pin on Grand Central interrupt 19,
// and the data path on DBDMA channel 1.
//
// The engine and the DMA engine meet in the middle.  The SWIM3 engine
// PUSHES: as a sector passes under the head it offers the decoded bytes
// one at a time, and a byte the channel cannot take is an overrun.  DBDMA
// PULLS: an INPUT command asks the device port for up to N bytes, and a
// port that returns fewer stalls the channel until the device kicks it.
// A byte ring between them turns the one into the other: reads fill the
// ring and kick the channel, which drains it into the descriptor's
// buffer; writes go the other way, the channel filling the ring from
// memory and the engine consuming it.  The ring is sized for the largest
// thing the engine streams in one service slot (a GCR sector is 704
// bytes; a whole-track format or raw capture streams a track, which the
// engine hands over in slot-sized pieces), with room to spare.
//
// Sources: Apple, SWIM3 ERS v1.2; the Grand Central register map (Linux
// drivers/block/swim3.c, `struct swim3` at 16-byte stride, DBDMA channel 1
// for the floppy; NetBSD macppc); Open Firmware's own `swim3` package,
// which allocates its buffers with dma-alloc/dma-map-in — the DBDMA path.

#include "tnt.h"

#include "dbdma.h"
#include "floppy.h"
#include "log.h"
#include "swim3.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("tnt");

#define FD_CHAN 1 // Grand Central DBDMA channel 1: the floppy

// --- the byte ring ----------------------------------------------------------

static uint32_t ring_count(const tnt_fdring_t *r) {
    return r->tail - r->head;
}

static bool ring_push(tnt_fdring_t *r, uint8_t v) {
    if (ring_count(r) >= TNT_FDRING_SIZE)
        return false;
    r->buf[r->tail % TNT_FDRING_SIZE] = v;
    r->tail++;
    return true;
}

static bool ring_pop(tnt_fdring_t *r, uint8_t *v) {
    if (ring_count(r) == 0)
        return false;
    *v = r->buf[r->head % TNT_FDRING_SIZE];
    r->head++;
    return true;
}

// --- the backend the shared model calls ------------------------------------

static bool fd_dma_running(void *ctx) {
    config_t *cfg = (config_t *)ctx;
    return tnt_dbdma_active(tnt_st(cfg)->dbdma, FD_CHAN);
}

// A read byte toward memory: into the ring, and the channel is told there
// is something to drain.  With no DBDMA program running the byte has
// nowhere to go — the chip's FIFO would overrun, and so does ours.
static bool fd_dma_put(void *ctx, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    tnt_state_t *st = tnt_st(cfg);
    if (!tnt_dbdma_active(st->dbdma, FD_CHAN))
        return false;
    if (!ring_push(&st->fdring, value))
        return false;
    tnt_dbdma_kick(st->dbdma, FD_CHAN);
    return true;
}

// A byte to write: from the ring, which the channel's OUTPUT commands keep
// topped up; an empty ring with the channel still running is a stall the
// kick resolves, an empty ring with the channel stopped is an underrun.
static bool fd_dma_get(void *ctx, uint8_t *out) {
    config_t *cfg = (config_t *)ctx;
    tnt_state_t *st = tnt_st(cfg);
    if (ring_pop(&st->fdring, out))
        return true;
    if (!tnt_dbdma_active(st->dbdma, FD_CHAN))
        return false;
    tnt_dbdma_kick(st->dbdma, FD_CHAN);
    return ring_pop(&st->fdring, out);
}

static void fd_set_irq(void *ctx, bool level) {
    tnt_gc_set_source((config_t *)ctx, TNT_INT_SWIM3, level);
}

// --- the DBDMA device port --------------------------------------------------

// OUTPUT_*: memory -> device.  Take what the ring has room for.
static int fd_port_out(void *ctx, const uint8_t *buf, int len) {
    tnt_state_t *st = tnt_st((config_t *)ctx);
    int n = 0;
    while (n < len && ring_push(&st->fdring, buf[n]))
        n++;
    return n;
}

// INPUT_*: device -> memory.  Give what the ring holds; a short answer
// stalls the channel until the engine pushes more and kicks.
static int fd_port_in(void *ctx, uint8_t *buf, int len) {
    tnt_state_t *st = tnt_st((config_t *)ctx);
    int n = 0;
    while (n < len && ring_pop(&st->fdring, &buf[n]))
        n++;
    return n;
}

// --- board glue -------------------------------------------------------------

void tnt_swim3_bind(config_t *cfg) {
    const swim3_backend_t be = {
        .dma_running = fd_dma_running,
        .dma_get = fd_dma_get,
        .dma_put = fd_dma_put,
        .set_irq = fd_set_irq,
        .ctx = cfg,
    };
    swim3_bind(&tnt_st(cfg)->swim3, cfg->floppy, cfg->scheduler, &be);
}

void tnt_swim3_init(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    memset(&st->fdring, 0, sizeof(st->fdring));
    tnt_dbdma_port_t port = {
        .out = fd_port_out,
        .in = fd_port_in,
        .s_bits = NULL,
        .ctx = cfg,
    };
    tnt_dbdma_set_port(st->dbdma, FD_CHAN, &port);
}

void tnt_swim3_register_events(config_t *cfg) {
    swim3_register_events(&tnt_st(cfg)->swim3);
    swim3_xfer_register_events(&tnt_st(cfg)->swim3);
}

uint8_t tnt_swim3_read(config_t *cfg, uint32_t off) {
    return swim3_read(&tnt_st(cfg)->swim3, (off >> 4) & 15u);
}

void tnt_swim3_write(config_t *cfg, uint32_t off, uint8_t value) {
    swim3_write(&tnt_st(cfg)->swim3, (off >> 4) & 15u, value);
}
