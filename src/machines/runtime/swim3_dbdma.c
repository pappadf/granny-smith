// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// swim3_dbdma.c
// The SWIM3 byte ring and DBDMA device port shared by Grand Central (TNT)
// and Heathrow (Gossamer) -- see swim3_dbdma.h.

#include "swim3_dbdma.h"

#include "dbdma.h"

#include <string.h>

// ============================================================================
// Static Helpers: the byte ring
// ============================================================================

// Bytes the ring holds
static uint32_t ring_count(const swim3_dbdma_ring_t *r) {
    return r->tail - r->head;
}

// Append one byte; false when the ring is full
static bool ring_push(swim3_dbdma_ring_t *r, uint8_t v) {
    if (ring_count(r) >= SWIM3_DBDMA_RING_SIZE)
        return false;
    r->buf[r->tail % SWIM3_DBDMA_RING_SIZE] = v;
    r->tail++;
    return true;
}

// Take one byte; false when the ring is empty
static bool ring_pop(swim3_dbdma_ring_t *r, uint8_t *v) {
    if (ring_count(r) == 0)
        return false;
    *v = r->buf[r->head % SWIM3_DBDMA_RING_SIZE];
    r->head++;
    return true;
}

// ============================================================================
// Static Helpers: the backend the shared model calls
// ============================================================================

// True while the floppy channel runs a program
static bool fd_dma_running(void *ctx) {
    swim3_dbdma_t *p = (swim3_dbdma_t *)ctx;
    return dbdma_active(p->dbdma, p->chan);
}

// A read byte toward memory: into the ring, and the channel is told there
// is something to drain.  With no DBDMA program running the byte has
// nowhere to go — the chip's FIFO would overrun, and so does ours.
static bool fd_dma_put(void *ctx, uint8_t value) {
    swim3_dbdma_t *p = (swim3_dbdma_t *)ctx;
    if (!dbdma_active(p->dbdma, p->chan))
        return false;
    if (!ring_push(&p->ring, value))
        return false;
    dbdma_kick(p->dbdma, p->chan);
    return true;
}

// A byte to write: from the ring, which the channel's OUTPUT commands keep
// topped up; an empty ring with the channel still running is a stall the
// kick resolves, an empty ring with the channel stopped is an underrun.
static bool fd_dma_get(void *ctx, uint8_t *out) {
    swim3_dbdma_t *p = (swim3_dbdma_t *)ctx;
    if (ring_pop(&p->ring, out))
        return true;
    if (!dbdma_active(p->dbdma, p->chan))
        return false;
    dbdma_kick(p->dbdma, p->chan);
    return ring_pop(&p->ring, out);
}

// The chip's IRQ pin, to the board's interrupt sink
static void fd_set_irq(void *ctx, bool level) {
    swim3_dbdma_t *p = (swim3_dbdma_t *)ctx;
    p->set_irq(p->irq_ctx, level);
}

// ============================================================================
// Static Helpers: the DBDMA device port
// ============================================================================

// OUTPUT_*: memory -> device.  Take what the ring has room for.
static int fd_port_out(void *ctx, const uint8_t *buf, int len) {
    swim3_dbdma_t *p = (swim3_dbdma_t *)ctx;
    int n = 0;
    while (n < len && ring_push(&p->ring, buf[n]))
        n++;
    return n;
}

// INPUT_*: device -> memory.  Give what the ring holds; a short answer
// stalls the channel until the engine pushes more and kicks.
static int fd_port_in(void *ctx, uint8_t *buf, int len) {
    swim3_dbdma_t *p = (swim3_dbdma_t *)ctx;
    int n = 0;
    while (n < len && ring_pop(&p->ring, &buf[n]))
        n++;
    return n;
}

// The driver stopped the channel: the program and whatever it staged in the
// ring part company.  A write's OUTPUT command is typically larger than the
// sector (Mac OS hands the channel a whole buffer), so the channel tops the
// ring up past the one stream the chip consumes; left there, that tail would
// be laid down as the start of the NEXT write -- every sector of MacTest
// Pro's seek test carried the previous one's data.
static void fd_port_stop(void *ctx) {
    swim3_dbdma_t *p = (swim3_dbdma_t *)ctx;
    p->ring.head = p->ring.tail;
}

// ============================================================================
// Lifecycle
// ============================================================================

void swim3_dbdma_init(swim3_dbdma_t *p, struct dbdma *dbdma, int chan, void (*set_irq)(void *ctx, bool level),
                      void *irq_ctx) {
    // The ring starts empty; the wiring names this board's channel and sink
    memset(&p->ring, 0, sizeof(p->ring));
    p->dbdma = dbdma;
    p->chan = chan;
    p->set_irq = set_irq;
    p->irq_ctx = irq_ctx;
    const dbdma_port_t port = {
        .out = fd_port_out,
        .in = fd_port_in,
        .s_bits = NULL,
        .ctx = p,
        .stop = fd_port_stop,
    };
    dbdma_set_port(dbdma, chan, &port);
}

void swim3_dbdma_bind(swim3_dbdma_t *p, swim3_t *chip, struct floppy *fd, struct scheduler *sched) {
    const swim3_backend_t be = {
        .dma_running = fd_dma_running,
        .dma_get = fd_dma_get,
        .dma_put = fd_dma_put,
        .set_irq = fd_set_irq,
        .ctx = p,
    };
    swim3_bind(chip, fd, sched, &be);
}
