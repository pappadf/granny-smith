// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// tnt_mace.c
// The TNT boards' MACE Ethernet: the Am79C940 core at Grand Central
// +$11000 (thirty-two byte-wide registers on $10 centres), its interrupt
// on Grand Central source 14, transmit on DBDMA channel 2 and receive on
// channel 3 -- with no wire behind it.
//
// What it is for: Rhapsody DR2's Mace driver (and Open Firmware's, and
// MkLinux's) transmits through channel 2 as soon as the interface comes
// up.  With the aperture reading open bus and no device on the channel,
// the first frame's OUTPUT_LAST stalled for good, the driver's transmit
// watchdog fired ("Tx DMA Error - Status = 8400 FS = 00") and the boot
// sat at "Configuring network" forever.
//
// The model: a frame the channel hands over goes nowhere -- it is counted
// and dropped, as on a port with nothing plugged in -- and completes as a
// good transmission: XMTFS = XMTSV, XMTINT in IR, the interrupt raised
// unless IMR masks it.  Nothing is ever received: the receive port never
// offers a byte, so the driver's receive program just waits.  The station
// address the guest loads through IAC/PADR and the logical address filter
// are held and read back; the counters read zero.
//
// Sources: AMD Am79C940 MACE data sheet (register map, IR/IMR/XMTFS/PR
// bits, IAC address-change protocol, chip ID $0940); Linux drivers/net/
// ethernet/apple/mace.{c,h} (the Grand Central register stride, channels
// 2 and 3, the transmit completion path that reads IR then XMTFS); the
// Grand Central interrupt map (docs/reference/machines/tnt/grand-central.md
// §3.2: source 14, MACE).

#include "tnt.h"

#include "dbdma.h"
#include "log.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("mace");

#define MACE_TX_CHAN 2 // Grand Central DBDMA channel 2: Ethernet transmit
#define MACE_RX_CHAN 3 // channel 3: Ethernet receive

// Register indices (offset >> 4).
enum {
    MR_RCVFIFO = 0,
    MR_XMTFIFO = 1,
    MR_XMTFC = 2,
    MR_XMTFS = 3,
    MR_XMTRC = 4,
    MR_RCVFC = 5,
    MR_RCVFS = 6,
    MR_FIFOFC = 7,
    MR_IR = 8,
    MR_IMR = 9,
    MR_PR = 10,
    MR_BIUCC = 11,
    MR_FIFOCC = 12,
    MR_MACCC = 13,
    MR_PLSCC = 14,
    MR_PHYCC = 15,
    MR_CHIPID_LO = 16,
    MR_CHIPID_HI = 17,
    MR_IAC = 18,
    MR_LADRF = 20,
    MR_PADR = 21,
    MR_MPC = 24,
    MR_RNTPC = 26,
    MR_RCVCC = 27,
    MR_UTR = 29,
};

// IR
#define IR_XMTINT 0x01u
// XMTFS
#define XMTFS_XMTSV 0x80u
// PR
#define PR_XMTSV  0x80u
#define PR_TDTREQ 0x40u
// IAC
#define IAC_ADDRCHG 0x80u
#define IAC_PHYADDR 0x04u
#define IAC_LOGADDR 0x02u
// BIUCC
#define BIUCC_SWRST 0x01u

// Am79C940 revision $0940 (Linux treats $0941 as the part with the broken
// ADDRCHG handshake; the plain revision needs no workaround).
#define CHIPID 0x0940u

static tnt_mace_t *mace(config_t *cfg) {
    return &tnt_st(cfg)->mace;
}

static void update_irq(config_t *cfg) {
    tnt_mace_t *m = mace(cfg);
    // IMR bits MASK: a set bit keeps the matching IR bit off the pin.
    tnt_gc_set_source(cfg, TNT_INT_MACE, (m->ir & (uint8_t)~m->imr) != 0);
}

void tnt_mace_reset(config_t *cfg) {
    tnt_mace_t *m = mace(cfg);
    memset(m, 0, sizeof(*m));
    update_irq(cfg);
}

// --- the DBDMA ports --------------------------------------------------------

// Transmit: take every byte the channel offers -- the frame leaves for a
// cable that is not there.
static int tx_port_out(void *ctx, const uint8_t *buf, int len) {
    (void)buf;
    tnt_mace_t *m = mace((config_t *)ctx);
    m->tx_bytes += (uint32_t)len;
    return len;
}

// The OUTPUT_LAST that ends a frame: the transmission completes, good.
static void tx_port_out_last(void *ctx) {
    config_t *cfg = (config_t *)ctx;
    tnt_mace_t *m = mace(cfg);
    LOG(3, "frame of %u bytes transmitted (no wire)", m->tx_bytes);
    m->tx_bytes = 0;
    m->tx_frames++;
    m->xmtfs = XMTFS_XMTSV;
    m->ir |= IR_XMTINT;
    update_irq(cfg);
}

// The transmit channel's device status: s5 is MACE's "transmit status
// valid" (XMTSV).  Rhapsody DR2's driver ends every frame with an
// OUTPUT_LAST carrying w=IFCLR against WaitSelect $00200020 -- the channel
// holds after the frame until s5 rises -- so a port without it parks the
// channel ACTIVE after the first frame (the "Tx DMA Error - Status = 8400"
// watchdog).  The status is valid from the frame's completion until the
// guest reads XMTFS.
static uint8_t tx_port_s_bits(void *ctx) {
    tnt_mace_t *m = mace((config_t *)ctx);
    return (m->xmtfs & XMTFS_XMTSV) ? 0x20u : 0x00u;
}

// Receive: nothing ever arrives; the channel waits on its INPUT.
static int rx_port_in(void *ctx, uint8_t *buf, int len) {
    (void)ctx;
    (void)buf;
    (void)len;
    return 0;
}

void tnt_mace_init(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    dbdma_port_t tx = {.out = tx_port_out, .out_last = tx_port_out_last, .s_bits = tx_port_s_bits, .ctx = cfg};
    dbdma_port_t rx = {.in = rx_port_in, .ctx = cfg};
    dbdma_set_port(st->dbdma, MACE_TX_CHAN, &tx);
    dbdma_set_port(st->dbdma, MACE_RX_CHAN, &rx);
}

// --- the register file ------------------------------------------------------

static uint8_t mace_access_read(config_t *cfg, uint32_t off, bool peek) {
    tnt_mace_t *m = mace(cfg);
    unsigned reg = (off >> 4) & 0x1Fu;
    switch (reg) {
    case MR_XMTFS: {
        uint8_t v = m->xmtfs;
        if (!peek)
            m->xmtfs = 0; // read once per frame
        return v;
    }
    case MR_IR: {
        uint8_t v = m->ir;
        if (!peek) {
            m->ir = 0; // read-to-clear
            update_irq(cfg);
        }
        return v;
    }
    case MR_IMR:
        return m->imr;
    case MR_PR:
        // The transmit FIFO always has room; XMTSV mirrors a pending status.
        return (uint8_t)(PR_TDTREQ | ((m->xmtfs & XMTFS_XMTSV) ? PR_XMTSV : 0));
    case MR_BIUCC:
        return m->biucc;
    case MR_FIFOCC:
        return m->fifocc;
    case MR_MACCC:
        return m->maccc;
    case MR_PLSCC:
        return m->plscc;
    case MR_PHYCC:
        return m->phycc;
    case MR_CHIPID_LO:
        return (uint8_t)(CHIPID & 0xFFu);
    case MR_CHIPID_HI:
        return (uint8_t)(CHIPID >> 8);
    case MR_IAC:
        return m->iac & (uint8_t)~IAC_ADDRCHG; // the address change is immediate
    case MR_LADRF: {
        uint8_t v = m->ladrf[m->addr_idx & 7u];
        if (!peek)
            m->addr_idx++;
        return v;
    }
    case MR_PADR: {
        uint8_t v = m->padr[m->addr_idx % 6u];
        if (!peek)
            m->addr_idx++;
        return v;
    }
    case MR_UTR:
        return m->utr;
    default:
        // FIFOs, frame control/status, counters: nothing in flight, zero.
        return 0;
    }
}

uint8_t tnt_mace_read(config_t *cfg, uint32_t off) {
    return mace_access_read(cfg, off, false);
}

uint8_t tnt_mace_peek(config_t *cfg, uint32_t off) {
    return mace_access_read(cfg, off, true);
}

void tnt_mace_write(config_t *cfg, uint32_t off, uint8_t value) {
    tnt_mace_t *m = mace(cfg);
    unsigned reg = (off >> 4) & 0x1Fu;
    LOG(4, "write reg %u = $%02X", reg, value);
    switch (reg) {
    case MR_IMR:
        m->imr = value;
        update_irq(cfg);
        break;
    case MR_BIUCC:
        if (value & BIUCC_SWRST) {
            // Software reset: back to the power-on state (SWRST self-clears).
            tnt_mace_reset(cfg);
            break;
        }
        m->biucc = value;
        break;
    case MR_FIFOCC:
        m->fifocc = value;
        break;
    case MR_MACCC:
        m->maccc = value;
        break;
    case MR_PLSCC:
        m->plscc = value;
        break;
    case MR_PHYCC:
        m->phycc = value;
        break;
    case MR_IAC:
        // Selecting PHYADDR or LOGADDR restarts the byte pointer.
        m->iac = value;
        if (value & (IAC_PHYADDR | IAC_LOGADDR))
            m->addr_idx = 0;
        break;
    case MR_LADRF:
        m->ladrf[m->addr_idx & 7u] = value;
        m->addr_idx++;
        break;
    case MR_PADR:
        m->padr[m->addr_idx % 6u] = value;
        m->addr_idx++;
        break;
    case MR_UTR:
        m->utr = value;
        break;
    default:
        // XMTFIFO (programmed-I/O transmit) and the frame-control
        // registers: accepted and dropped like the DMA path's bytes.
        break;
    }
}
