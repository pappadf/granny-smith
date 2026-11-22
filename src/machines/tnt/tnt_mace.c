// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// tnt_mace.c
// The TNT boards' MACE Ethernet: the shared Curio MACE core (core/
// peripherals/mace.c) at Grand Central +$11000 (thirty-two byte-wide
// registers on $10 centres), its interrupt on Grand Central source 14,
// transmit on DBDMA channel 2 and receive on channel 3 -- with no wire
// behind it.
//
// What it is for: Rhapsody DR2's Mace driver (and Open Firmware's, and
// MkLinux's) transmits through channel 2 as soon as the interface comes
// up.  With the aperture reading open bus and no device on the channel,
// the first frame's OUTPUT_LAST stalled for good, the driver's transmit
// watchdog fired ("Tx DMA Error - Status = 8400 FS = 00") and the boot
// sat at "Configuring network" forever.
//
// The wiring: a frame the channel hands over is the core's FIFO-direct
// transmit, its OUTPUT_LAST the EOF; the core completes it as a good
// transmission (XMTFS = XMTSV, XMTINT in IR) and, with no cable bound,
// the frame goes nowhere -- as on a port with nothing plugged in.  Nothing
// is received: the receive port never offers a byte, so the driver's
// receive program just waits.
//
// Sources: Linux drivers/net/ethernet/apple/mace.{c,h} (the Grand Central
// register stride, channels 2 and 3, the transmit completion path that
// reads IR then XMTFS); the Grand Central interrupt map
// (docs/reference/machines/tnt/grand-central.md §3.2: source 14, MACE).

#include "tnt.h"

#include "dbdma.h"
#include "log.h"

LOG_USE_CATEGORY_NAME("mace");

#define MACE_TX_CHAN 2 // Grand Central DBDMA channel 2: Ethernet transmit
#define MACE_RX_CHAN 3 // channel 3: Ethernet receive

// The core's INTR on Grand Central source 14.
static void mace_irq(void *ctx, bool level) {
    tnt_gc_set_source((config_t *)ctx, TNT_INT_MACE, level);
}

void tnt_mace_reset(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    st->mace_tx_bytes = 0;
    mace_reset(st->mace);
}

// --- the DBDMA ports --------------------------------------------------------

// Transmit: every byte the channel offers goes into the transmit FIFO.
static int tx_port_out(void *ctx, const uint8_t *buf, int len) {
    tnt_state_t *st = tnt_st((config_t *)ctx);
    st->mace_tx_bytes += (uint32_t)len;
    mace_tx_write(st->mace, buf, len);
    return len;
}

// The OUTPUT_LAST that ends a frame is the FIFO's EOF: the frame goes.
static void tx_port_out_last(void *ctx) {
    tnt_state_t *st = tnt_st((config_t *)ctx);
    LOG(3, "frame of %u bytes transmitted (no wire)", st->mace_tx_bytes);
    st->mace_tx_bytes = 0;
    mace_tx_eof(st->mace);
}

// The transmit channel's device status: s5 is MACE's "transmit status
// valid" (XMTSV).  Rhapsody DR2's driver ends every frame with an
// OUTPUT_LAST carrying w=IFCLR against WaitSelect $00200020 -- the channel
// holds after the frame until s5 rises -- so a port without it parks the
// channel ACTIVE after the first frame (the "Tx DMA Error - Status = 8400"
// watchdog).  The status is valid from the frame's completion until the
// guest reads XMTFS.
static uint8_t tx_port_s_bits(void *ctx) {
    tnt_state_t *st = tnt_st((config_t *)ctx);
    return (mace_peek(st->mace, MACE_XMTFS) & MACE_XMTFS_XMTSV) ? 0x20u : 0x00u;
}

// Receive: nothing ever arrives; the channel waits on its INPUT.
static int rx_port_in(void *ctx, uint8_t *buf, int len) {
    (void)ctx;
    (void)buf;
    (void)len;
    return 0;
}

int tnt_mace_init(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    st->mace = mace_init();
    if (!st->mace)
        return -1;
    mace_set_irq(st->mace, mace_irq, cfg);
    dbdma_port_t tx = {.out = tx_port_out, .out_last = tx_port_out_last, .s_bits = tx_port_s_bits, .ctx = cfg};
    dbdma_port_t rx = {.in = rx_port_in, .ctx = cfg};
    dbdma_set_port(st->dbdma, MACE_TX_CHAN, &tx);
    dbdma_set_port(st->dbdma, MACE_RX_CHAN, &rx);
    return 0;
}

void tnt_mace_teardown(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    mace_delete(st->mace);
    st->mace = NULL;
}

// --- the register file ------------------------------------------------------

uint8_t tnt_mace_read(config_t *cfg, uint32_t off) {
    return mace_read(tnt_st(cfg)->mace, (off >> 4) & 0x1Fu);
}

uint8_t tnt_mace_peek(config_t *cfg, uint32_t off) {
    return mace_peek(tnt_st(cfg)->mace, (off >> 4) & 0x1Fu);
}

void tnt_mace_write(config_t *cfg, uint32_t off, uint8_t value) {
    mace_write(tnt_st(cfg)->mace, (off >> 4) & 0x1Fu, value);
}
