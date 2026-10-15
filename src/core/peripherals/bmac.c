// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// bmac.c
// BMAC Ethernet cell — see bmac.h.  Register names and init sequences are
// the ones Apple's AppleBMacEthernet driver, Linux's bmac driver and the
// BSD bm driver share; where the drivers say what the silicon does on its
// own (STATUS read-to-clear, TXRST self-clearing, the receive trailer, the
// serial EEPROM protocol) the model does that.

#include "bmac.h"

#include "checkpoint.h"
#include "crc32.h"
#include "log.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("bmac");

// One register per 16-byte centre, $000..$770.
#define BMAC_NREGS (0x770u / 16u + 1u)
#define R(off)     ((off) >> 4)

// XIFC
#define XIFC_LOOPBACK 0x0006u // XIFLoopback | MIILoopback
// XCVRIF
#define XCVRIF_LINK_DOWN 0x0100u // read-only; 0 = link up
// MIFCSR
#define MIF_DATA_IN 0x0008u // nothing drives MDIO: reads 0
// SROMCSR
#define SROM_CS  0x0001u
#define SROM_CLK 0x0002u
#define SROM_DO  0x0004u // EEPROM -> host
#define SROM_DI  0x0008u // host -> EEPROM
// TXRST
#define TXRST_BIT 0x0001u
// TXCFG
#define TXCFG_ENABLE 0x0001u
#define TXCFG_NO_FCS 0x0080u
// RXCFG
#define RXCFG_ENABLE      0x0001u
#define RXCFG_PROMISC     0x0040u
#define RXCFG_CRC_IN_BUF  0x0100u // Linux "RxCRCNoStrip": the FCS stays in the buffer
#define RXCFG_REJECT_OWN  0x0200u
#define RXCFG_HASH_FILTER 0x0800u

// Receive trailer: bit 15 abort, bits 13-0 frame length (Apple's
// kRxAbortBit / kRxLengthMask).
#define TRAILER_LEN_MASK 0x3FFFu

#define ETH_MIN   60 // without FCS
#define ETH_FCS   4
#define FRAME_MAX 1536 // frame + FCS + trailer, with room to spare
#define RXQ_DEPTH 8

// The EEPROM: 64 x 16 bits, a 93C46-class Microwire part.  Words 10-12 are
// the station address (Apple's kSROMStartOffset/kSROMReadCount), each byte
// bit-reversed on the way out.
#define SROM_WORDS    64
#define SROM_MAC_WORD 10

enum { SROM_IDLE, SROM_CMD, SROM_READ, SROM_IGNORE };

struct bmac {
    uint16_t reg[BMAC_NREGS];
    uint16_t srom[SROM_WORDS];

    // EEPROM shifter
    uint16_t srom_csr; // last SROMCSR written (for the clock edge)
    uint16_t srom_cmd;
    uint8_t srom_state;
    uint8_t srom_nbits;
    uint8_t srom_addr;
    uint8_t srom_bit; // next data bit out (15..0)
    uint8_t srom_do;

    bool link_down;
    bool irq_level;

    // Transmit: the frame being assembled from OUTPUT_MORE/LAST pieces.
    uint8_t tx_buf[FRAME_MAX];
    int tx_len;

    // Receive: frames ready for DMA, each with FCS (if kept) and trailer.
    uint8_t rxq[RXQ_DEPTH][FRAME_MAX];
    int rxq_len[RXQ_DEPTH];
    int rx_head, rx_count, rx_pos;
    bool rx_boundary; // the last byte of a frame went out; the descriptor ends
    bool rx_s6;

    uint32_t tx_frames, rx_frames;
    int tx_chan, rx_chan;

    // The last frame transmitted (padded, no FCS): what the wire saw.
    uint8_t last_tx[FRAME_MAX];
    int last_tx_len;

    // === Pointers last (not checkpointed) ===
    bmac_irq_fn irq;
    void *irq_ctx;
    dbdma_t *dbdma;
};

// ============================================================
// Helpers
// ============================================================

static uint8_t bitrev8(uint8_t v) {
    v = (uint8_t)(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    return (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
}

static void update_irq(bmac_t *b) {
    bool level = (b->reg[R(BMAC_STATUS)] & ~b->reg[R(BMAC_INTDISABLE)]) != 0;
    if (level == b->irq_level)
        return;
    b->irq_level = level;
    if (b->irq)
        b->irq(b->irq_ctx, level);
}

static void station(const bmac_t *b, uint8_t mac[6]) {
    static const uint32_t madd[3] = {BMAC_MADD0, BMAC_MADD1, BMAC_MADD2};
    for (int i = 0; i < 3; i++) {
        uint16_t w = b->reg[R(madd[i])];
        mac[2 * i] = (uint8_t)(w >> 8);
        mac[2 * i + 1] = (uint8_t)w;
    }
}

// The multicast hash: the drivers' bmac_crc / mace_crc (CRC-32, polynomial
// $04C11DB7, register MSB first, each address byte fed LSB first), low six
// bits, bit-reversed.
static int hash_index(const uint8_t *da) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 6; i++) {
        uint8_t d = da[i];
        for (int k = 0; k < 8; k++) {
            bool hi = (crc & 0x80000000u) != 0;
            crc <<= 1;
            if (hi ^ (d & 1u))
                crc ^= 0x04C11DB7u;
            d >>= 1;
        }
    }
    uint8_t six = (uint8_t)(crc & 0x3Fu);
    return bitrev8(six) >> 2;
}

// Linux and Apple's init put hash bits 0-15 in HASH3; Apple's multicast
// update path writes them to HASH0.  A false positive costs the driver one
// software filter pass, so either mapping admits the frame.
static bool hash_hit(const bmac_t *b, const uint8_t *da) {
    int h = hash_index(da);
    int word = h >> 4, bit = h & 15;
    // HASH3..HASH0 sit at ascending offsets $700..$730.
    uint16_t a = b->reg[R(BMAC_HASH3) + (unsigned)word]; // HASH3 = bits 0-15
    uint16_t c = b->reg[R(BMAC_HASH0) - (unsigned)word]; // HASH0 = bits 0-15
    return ((a | c) >> bit) & 1u;
}

// ============================================================
// Receive
// ============================================================

static bool rx_accept(const bmac_t *b, const uint8_t *f) {
    uint16_t cfg = b->reg[R(BMAC_RXCFG)];
    if (!(cfg & RXCFG_ENABLE))
        return false;
    uint8_t me[6];
    station(b, me);
    if ((cfg & RXCFG_REJECT_OWN) && memcmp(f + 6, me, 6) == 0)
        return false;
    if (cfg & RXCFG_PROMISC)
        return true;
    if (memcmp(f, me, 6) == 0)
        return true;
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (memcmp(f, bcast, 6) == 0)
        return true;
    return (f[0] & 1u) && (cfg & RXCFG_HASH_FILTER) && hash_hit(b, f);
}

bool bmac_receive(bmac_t *b, const uint8_t *frame, int len) {
    if (len < 14 || len > FRAME_MAX - ETH_FCS - 2) {
        LOG(2, "receive: %d-byte frame dropped", len);
        return false;
    }
    if (!rx_accept(b, frame)) {
        LOG(3, "receive: frame for %02X:%02X:%02X:%02X:%02X:%02X filtered", frame[0], frame[1], frame[2], frame[3],
            frame[4], frame[5]);
        return false;
    }
    if (b->rx_count == RXQ_DEPTH) {
        b->reg[R(BMAC_STATUS)] |= BMAC_ST_RX_OVERFLOW;
        update_irq(b);
        LOG(2, "receive: queue full, frame dropped");
        return false;
    }
    int slot = (b->rx_head + b->rx_count) % RXQ_DEPTH;
    uint8_t *q = b->rxq[slot];
    int n = len;
    memcpy(q, frame, (size_t)len);
    if (n < ETH_MIN) {
        memset(q + n, 0, (size_t)(ETH_MIN - n));
        n = ETH_MIN;
    }
    if (b->reg[R(BMAC_RXCFG)] & RXCFG_CRC_IN_BUF) {
        uint32_t fcs = gs_crc32(0, q, (size_t)n);
        for (int i = 0; i < 4; i++)
            q[n++] = (uint8_t)(fcs >> (8 * i));
    }
    // The status trailer, big-endian: the frame length (FCS included when
    // it is in the buffer), no abort.
    q[n] = (uint8_t)((n & TRAILER_LEN_MASK) >> 8);
    q[n + 1] = (uint8_t)n;
    b->rxq_len[slot] = n + 2;
    b->rx_count++;
    LOG(3, "receive: %d-byte frame queued (%d waiting)", n, b->rx_count);
    if (b->dbdma)
        dbdma_kick(b->dbdma, b->rx_chan);
    return true;
}

static int rx_port_in(void *ctx, uint8_t *buf, int len) {
    bmac_t *b = (bmac_t *)ctx;
    if (b->rx_boundary || b->rx_count == 0)
        return 0;
    const uint8_t *q = b->rxq[b->rx_head];
    int flen = b->rxq_len[b->rx_head];
    if (b->rx_pos == 0)
        b->rx_s6 = false; // a new frame starts
    int n = flen - b->rx_pos;
    if (n > len)
        n = len;
    memcpy(buf, q + b->rx_pos, (size_t)n);
    b->rx_pos += n;
    if (b->rx_pos == flen) {
        b->rx_head = (b->rx_head + 1) % RXQ_DEPTH;
        b->rx_count--;
        b->rx_pos = 0;
        b->rx_boundary = true;
        b->rx_s6 = true;
        b->rx_frames++;
        b->reg[R(BMAC_STATUS)] |= BMAC_ST_FRAME_RECEIVED;
        update_irq(b);
    }
    return n;
}

static bool rx_port_end(void *ctx) {
    bmac_t *b = (bmac_t *)ctx;
    bool end = b->rx_boundary;
    b->rx_boundary = false;
    return end;
}

static uint8_t rx_port_sbits(void *ctx) {
    return ((bmac_t *)ctx)->rx_s6 ? BMAC_S6_RX_FRAME : 0;
}

static void rx_flush(bmac_t *b) {
    b->rx_head = b->rx_count = b->rx_pos = 0;
    b->rx_boundary = b->rx_s6 = false;
}

// ============================================================
// Transmit
// ============================================================

static int tx_port_out(void *ctx, const uint8_t *buf, int len) {
    bmac_t *b = (bmac_t *)ctx;
    int room = FRAME_MAX - b->tx_len;
    int n = len < room ? len : room;
    if (n < len)
        LOG(1, "transmit: frame longer than %d bytes truncated", FRAME_MAX);
    memcpy(b->tx_buf + b->tx_len, buf, (size_t)n);
    b->tx_len += n;
    return len; // the FIFO always drains: transmission is synchronous
}

static void tx_port_last(void *ctx) {
    bmac_t *b = (bmac_t *)ctx;
    int n = b->tx_len;
    b->tx_len = 0;
    if (!(b->reg[R(BMAC_TXCFG)] & TXCFG_ENABLE)) {
        LOG(2, "transmit: %d bytes with the transmitter disabled, dropped", n);
        return;
    }
    if (n < 14) {
        LOG(2, "transmit: %d-byte runt dropped", n);
        return;
    }
    // The chip pads a short frame and (TxDisableFCS clear) appends the FCS;
    // with no wire the FCS goes nowhere, and a looped-back frame gets the
    // receiver's own.
    uint8_t f[FRAME_MAX];
    memcpy(f, b->tx_buf, (size_t)n);
    if (n < ETH_MIN) {
        memset(f + n, 0, (size_t)(ETH_MIN - n));
        n = ETH_MIN;
    }
    memcpy(b->last_tx, f, (size_t)n);
    b->last_tx_len = n;
    b->tx_frames++;
    b->reg[R(BMAC_STATUS)] |= BMAC_ST_FRAME_SENT;
    update_irq(b);
    LOG(3, "transmit: %d-byte frame to %02X:%02X:%02X:%02X:%02X:%02X%s", n, f[0], f[1], f[2], f[3], f[4], f[5],
        (b->reg[R(BMAC_XIFC)] & XIFC_LOOPBACK) ? " (loopback)" : "");
    if (b->reg[R(BMAC_XIFC)] & XIFC_LOOPBACK)
        (void)bmac_receive(b, f, n);
}

static uint8_t tx_port_sbits(void *ctx) {
    (void)ctx;
    // The transmitter is done with every frame by the time its descriptor
    // completes, so the line the drivers' WAIT_IF_FALSE(S5) tests is held.
    return BMAC_S5_TX_DONE;
}

// ============================================================
// Serial EEPROM
// ============================================================

static void srom_write(bmac_t *b, uint16_t v) {
    uint16_t prev = b->srom_csr;
    b->srom_csr = v & (SROM_CS | SROM_CLK | SROM_DI);
    if (!(v & SROM_CS)) {
        // Deselect resets the shifter.
        b->srom_state = SROM_IDLE;
        b->srom_do = 0;
        return;
    }
    if (!(v & SROM_CLK) || (prev & SROM_CLK))
        return; // only a rising clock edge with chip select high moves the shifter
    unsigned di = (v & SROM_DI) ? 1u : 0u;
    switch (b->srom_state) {
    case SROM_IDLE:
        // Leading zeros before the start bit are ignored.
        if (di) {
            b->srom_state = SROM_CMD;
            b->srom_nbits = 0;
            b->srom_cmd = 0;
        }
        break;
    case SROM_CMD:
        b->srom_cmd = (uint16_t)((b->srom_cmd << 1) | di);
        if (++b->srom_nbits == 8) {
            // Two opcode bits, six address bits; READ is 10.
            if ((b->srom_cmd >> 6) == 2u) {
                b->srom_state = SROM_READ;
                b->srom_addr = (uint8_t)(b->srom_cmd & 0x3Fu);
                b->srom_bit = 15;
                b->srom_do = 0; // the dummy zero before D15
            } else {
                LOG(2, "EEPROM: opcode %u ignored (read-only part)", (unsigned)(b->srom_cmd >> 6));
                b->srom_state = SROM_IGNORE;
            }
        }
        break;
    case SROM_READ:
        b->srom_do = (uint8_t)((b->srom[b->srom_addr] >> b->srom_bit) & 1u);
        if (b->srom_bit == 0) {
            b->srom_bit = 15;
            b->srom_addr = (uint8_t)((b->srom_addr + 1u) & (SROM_WORDS - 1u)); // sequential read
        } else {
            b->srom_bit--;
        }
        break;
    default:
        break;
    }
}

// ============================================================
// Register file
// ============================================================

// A register read: the guest's, or an inspection's (`peek`), which returns the
// same value but leaves the read-to-clear STATUS register (and the IRQ line it
// drives) as it is.
static uint16_t reg_read(bmac_t *b, uint32_t off, bool peek) {
    off &= 0xFF0u;
    if (off > 0x770u) {
        if (!peek)
            LOG(2, "read of unmapped +$%03X", off);
        return 0;
    }
    uint16_t v = b->reg[R(off)];
    switch (off) {
    case BMAC_CHIPID:
        v = BMAC_CHIPID_HEATHROW;
        break;
    case BMAC_XCVRIF:
        v = (uint16_t)((v & ~XCVRIF_LINK_DOWN) | (b->link_down ? XCVRIF_LINK_DOWN : 0));
        break;
    case BMAC_MIFCSR:
        v &= (uint16_t)~MIF_DATA_IN; // no PHY on the management bus
        break;
    case BMAC_SROMCSR:
        v = (uint16_t)((b->srom_csr & ~SROM_DO) | (b->srom_do ? SROM_DO : 0));
        break;
    case BMAC_STATUS:
        if (peek)
            break;
        b->reg[R(BMAC_STATUS)] = 0; // reading clears it
        update_irq(b);
        break;
    case BMAC_TXRST:
        v = 0; // the reset is done by the time it can be read
        break;
    default:
        break;
    }
    if (!peek)
        LOG(4, "rd +$%03X = $%04X", off, v);
    return v;
}

uint16_t bmac_read(bmac_t *b, uint32_t off) {
    return reg_read(b, off, false);
}

uint16_t bmac_peek(bmac_t *b, uint32_t off) {
    return reg_read(b, off, true);
}

void bmac_write(bmac_t *b, uint32_t off, uint16_t value) {
    off &= 0xFF0u;
    if (off > 0x770u) {
        LOG(2, "write of unmapped +$%03X = $%04X", off, value);
        return;
    }
    LOG(4, "wr +$%03X = $%04X", off, value);
    switch (off) {
    case BMAC_CHIPID:
    case BMAC_STATUS:
        return; // read-only
    case BMAC_SROMCSR:
        srom_write(b, value);
        return;
    case BMAC_TXRST:
        if (value & TXRST_BIT)
            b->tx_len = 0;
        return;
    case BMAC_RXRST:
        rx_flush(b);
        return;
    case BMAC_INTDISABLE:
        b->reg[R(off)] = value;
        update_irq(b);
        return;
    case BMAC_RXCFG: {
        uint16_t old = b->reg[R(off)];
        b->reg[R(off)] = value;
        if ((value & RXCFG_ENABLE) && !(old & RXCFG_ENABLE) && b->dbdma)
            dbdma_kick(b->dbdma, b->rx_chan);
        return;
    }
    default:
        b->reg[R(off)] = value;
        return;
    }
}

// ============================================================
// Lifecycle
// ============================================================

void bmac_reset(bmac_t *b) {
    memset(b->reg, 0, sizeof b->reg);
    b->srom_csr = 0;
    b->srom_state = SROM_IDLE;
    b->srom_do = 0;
    b->tx_len = 0;
    rx_flush(b);
    update_irq(b);
}

bmac_t *bmac_init(checkpoint_t *cp, const uint8_t mac[6]) {
    bmac_t *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    for (int i = 0; i < 3; i++)
        b->srom[SROM_MAC_WORD + i] = (uint16_t)(bitrev8(mac[2 * i]) | (bitrev8(mac[2 * i + 1]) << 8));
    bmac_reset(b);
    if (cp)
        system_read_checkpoint_data(cp, b, offsetof(bmac_t, irq));
    return b;
}

void bmac_delete(bmac_t *b) {
    free(b);
}

void bmac_checkpoint(bmac_t *b, checkpoint_t *cp) {
    system_write_checkpoint_data(cp, b, offsetof(bmac_t, irq));
}

void bmac_set_irq(bmac_t *b, bmac_irq_fn fn, void *ctx) {
    b->irq = fn;
    b->irq_ctx = ctx;
}

void bmac_attach_dbdma(bmac_t *b, dbdma_t *d, int tx_chan, int rx_chan) {
    b->dbdma = d;
    b->tx_chan = tx_chan;
    b->rx_chan = rx_chan;
    dbdma_port_t tx = {.out = tx_port_out, .s_bits = tx_port_sbits, .ctx = b, .out_last = tx_port_last};
    dbdma_port_t rx = {.in = rx_port_in, .s_bits = rx_port_sbits, .ctx = b, .in_end = rx_port_end};
    dbdma_set_port(d, tx_chan, &tx);
    dbdma_set_port(d, rx_chan, &rx);
}

void bmac_set_link(bmac_t *b, bool up) {
    b->link_down = !up;
}

bool bmac_link(const bmac_t *b) {
    return !b->link_down;
}

void bmac_get_mac(const bmac_t *b, uint8_t mac[6]) {
    for (int i = 0; i < 3; i++) {
        uint16_t w = b->srom[SROM_MAC_WORD + i];
        mac[2 * i] = bitrev8((uint8_t)w);
        mac[2 * i + 1] = bitrev8((uint8_t)(w >> 8));
    }
}

uint32_t bmac_tx_frames(const bmac_t *b) {
    return b->tx_frames;
}

uint32_t bmac_rx_frames(const bmac_t *b) {
    return b->rx_frames;
}

int bmac_last_tx(const bmac_t *b, uint8_t *buf, int max) {
    int n = b->last_tx_len < max ? b->last_tx_len : max;
    memcpy(buf, b->last_tx, (size_t)n);
    return n;
}
