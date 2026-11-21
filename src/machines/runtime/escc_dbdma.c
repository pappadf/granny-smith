// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// escc_dbdma.c
// The MacRISC ESCC aperture and its four DBDMA ports, shared by Grand
// Central (TNT) and Heathrow (Gossamer) -- see escc_dbdma.h.

#include "escc_dbdma.h"

#include "dbdma.h"
#include "memory.h"
#include "scc.h"
#include "system_internal.h"

// ============================================================================
// Constants and Macros
// ============================================================================

#define ESCC_CHANNEL_REGS_END 0x40u // +$00..+$3F: the four channel registers

// ============================================================================
// Static Helpers
// ============================================================================

// Map an aperture offset onto the cell's classic address pins
static uint32_t escc_pins(uint32_t off) {
    uint32_t ab = (off >> 5) & 1u; // A channel at +$20
    uint32_t dc = (off >> 4) & 1u; // data register at +$10
    return (ab << 1) | (dc << 2);
}

// A channel's data register on the cell's pins: channel 0 (A) data is 6,
// channel 1 (B) data is 4
static uint32_t scc_data_pins(unsigned ch) {
    return (ch == 0 ? 2u : 0u) | 4u;
}

// Re-run whichever receive program is waiting for bytes
static void scc_dma_kick_rx(escc_dbdma_t *e) {
    if (!e->dbdma)
        return;
    for (unsigned ch = 0; ch < 2; ch++)
        if (dbdma_active(e->dbdma, e->rx_base + 2 * (int)ch))
            dbdma_kick(e->dbdma, e->rx_base + 2 * (int)ch);
}

// OUTPUT_*: memory -> the channel's data register, a byte at a time
static int scc_port_out(void *ctx, const uint8_t *buf, int len) {
    escc_dbdma_port_t *c = (escc_dbdma_port_t *)ctx;
    struct scc *scc = c->owner->cfg->scc;
    const memory_interface_t *mi = scc_get_memory_interface(scc);
    for (int n = 0; n < len; n++)
        mi->write_uint8(scc, scc_data_pins(c->ch), buf[n]);
    scc_dma_kick_rx(c->owner);
    return len;
}

// INPUT_*: what the channel has received -> memory
static int scc_port_in(void *ctx, uint8_t *buf, int len) {
    escc_dbdma_port_t *c = (escc_dbdma_port_t *)ctx;
    struct scc *scc = c->owner->cfg->scc;
    const memory_interface_t *mi = scc_get_memory_interface(scc);
    int n = 0;
    while (n < len && scc_channel_rx_pending(scc, c->ch) > 0)
        buf[n++] = mi->read_uint8(scc, scc_data_pins(c->ch));
    return n;
}

// ============================================================================
// Lifecycle
// ============================================================================

void escc_dbdma_init(escc_dbdma_t *e, struct config *cfg, struct dbdma *dbdma, int tx_base, int rx_base) {
    e->cfg = cfg;
    e->dbdma = dbdma;
    e->tx_base = tx_base;
    e->rx_base = rx_base;
    for (unsigned ch = 0; ch < 2; ch++) {
        e->port[ch].owner = e;
        e->port[ch].ch = ch;
        // Each SCC channel: a transmit port and a receive port, two apart per channel
        dbdma_port_t tx = {.out = scc_port_out, .in = NULL, .s_bits = NULL, .ctx = &e->port[ch]};
        dbdma_port_t rx = {.out = NULL, .in = scc_port_in, .s_bits = NULL, .ctx = &e->port[ch]};
        dbdma_set_port(dbdma, tx_base + 2 * (int)ch, &tx);
        dbdma_set_port(dbdma, rx_base + 2 * (int)ch, &rx);
    }
}

// ============================================================================
// Operations
// ============================================================================

bool escc_is_channel_reg(uint32_t off) {
    return off < ESCC_CHANNEL_REGS_END;
}

uint8_t escc_aperture_read(struct scc *scc, uint32_t off, bool peek) {
    // Past the four channel registers nothing reaches the cell
    if (!escc_is_channel_reg(off))
        return 0;
    return memory_iface_read8(scc_get_memory_interface(scc), scc, escc_pins(off), peek);
}

bool escc_aperture_write(escc_dbdma_t *e, struct scc *scc, uint32_t off, uint8_t value) {
    // Past the four channel registers nothing reaches the cell (the OT LocalTalk +$A0 stores)
    if (!escc_is_channel_reg(off))
        return false;
    scc_get_memory_interface(scc)->write_uint8(scc, escc_pins(off), value);
    scc_dma_kick_rx(e);
    return true;
}
