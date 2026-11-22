// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mace.c
// The Curio MACE core -- see mace.h.  Register semantics follow the AMD
// Am79C940 data manual (register detail pp. 57-73, the Register Table
// Summary p. 74); the deviations are Curio's, from Apple's two drivers for
// it: the one-deep transmit status ("MACE can only buffer 1 status (bug)",
// the PDM driver's MAX_STAT_PEND) and the $0940 chip ID.

#include "mace.h"

#include "checkpoint.h"
#include "codec.h"
#include "log.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mace");

// XMTFC
#define XMTFC_APADXMT 0x01u
#define XMTFC_DXMTFCS 0x08u
// RCVFC
#define RCVFC_ASTRPRCV 0x01u
// PR
#define PR_XMTSV  0x80u
#define PR_TDTREQ 0x40u
#define PR_RDTREQ 0x20u
// BIUCC
#define BIUCC_SWRST 0x01u
// FIFOCC: the watermark-update bits, cleared once the watermarks load
#define FIFOCC_XMTFWU 0x08u
#define FIFOCC_RCVFWU 0x04u
// MACCC address-match disables
#define MACCC_DRCVBC 0x04u
#define MACCC_DRCVPA 0x08u
// IAC
#define IAC_ADDRCHG 0x80u
#define IAC_PHYADDR 0x04u
#define IAC_LOGADDR 0x02u
// UTR: RTRD (bit 6) is sticky until reset; RCVFCSE (bit 3) gives the FCS
// engine to the receiver in loopback
#define UTR_RTRD    0x40u
#define UTR_RCVFCSE 0x08u
// RFS1
#define RFS1_OFLO 0x80u

#define ETH_MIN 60 // shortest frame before the FCS
#define ETH_FCS 4

struct mace {
    // === Plain data (checkpointed, see MACE_CP_FIELDS) ===
    uint8_t xmtfc, xmtfs, xmtrc, rcvfc, ir, imr;
    uint8_t biucc, fifocc, maccc, plscc, phycc, iac, utr;
    uint8_t padr[6], ladrf[8];
    uint8_t padr_idx, ladrf_idx; // IAC byte pointers
    uint8_t mpc, rntpc, rcvcc; // read-to-clear error counters
    uint8_t rfs[4]; // status of the last frame taken from the receive FIFO
    uint8_t rfs_idx; // next RCVFS read (4 = drained)

    // Transmit: the frame being written into the FIFO, and whether a whole
    // one is held there waiting for ENXMT or the previous status read.
    uint8_t tx_buf[MACE_FRAME_MAX];
    uint16_t tx_len;
    bool tx_held;

    // Receive: frames ready for the DMA engine, each with its FCS (unless
    // stripped) and its RFS status; rx_pos is a programmed-I/O read cursor.
    uint8_t rxq[MACE_RXQ_DEPTH][MACE_FRAME_MAX];
    uint16_t rxq_len[MACE_RXQ_DEPTH];
    uint8_t rx_head, rx_count;
    uint16_t rx_pos;

    uint32_t tx_frames, rx_frames;

    // === Pointers last (not checkpointed) ===
    irq_line_t irq; // INTR; its level is checkpointed after the list
    mace_ops_t ops;
    void *ops_ctx;
};

// The checkpointed state, one list for both directions (checkpoint.h).
#define MACE_CP_FIELDS(OP, cp, m)                                                                                      \
    CP_FIELD(OP, cp, (m)->xmtfc);                                                                                      \
    CP_FIELD(OP, cp, (m)->xmtfs);                                                                                      \
    CP_FIELD(OP, cp, (m)->xmtrc);                                                                                      \
    CP_FIELD(OP, cp, (m)->rcvfc);                                                                                      \
    CP_FIELD(OP, cp, (m)->ir);                                                                                         \
    CP_FIELD(OP, cp, (m)->imr);                                                                                        \
    CP_FIELD(OP, cp, (m)->biucc);                                                                                      \
    CP_FIELD(OP, cp, (m)->fifocc);                                                                                     \
    CP_FIELD(OP, cp, (m)->maccc);                                                                                      \
    CP_FIELD(OP, cp, (m)->plscc);                                                                                      \
    CP_FIELD(OP, cp, (m)->phycc);                                                                                      \
    CP_FIELD(OP, cp, (m)->iac);                                                                                        \
    CP_FIELD(OP, cp, (m)->utr);                                                                                        \
    CP_ARRAY(OP, cp, (m)->padr);                                                                                       \
    CP_ARRAY(OP, cp, (m)->ladrf);                                                                                      \
    CP_FIELD(OP, cp, (m)->padr_idx);                                                                                   \
    CP_FIELD(OP, cp, (m)->ladrf_idx);                                                                                  \
    CP_FIELD(OP, cp, (m)->mpc);                                                                                        \
    CP_FIELD(OP, cp, (m)->rntpc);                                                                                      \
    CP_FIELD(OP, cp, (m)->rcvcc);                                                                                      \
    CP_ARRAY(OP, cp, (m)->rfs);                                                                                        \
    CP_FIELD(OP, cp, (m)->rfs_idx);                                                                                    \
    CP_ARRAY(OP, cp, (m)->tx_buf);                                                                                     \
    CP_FIELD(OP, cp, (m)->tx_len);                                                                                     \
    CP_FIELD(OP, cp, (m)->tx_held);                                                                                    \
    CP_ARRAY(OP, cp, (m)->rxq);                                                                                        \
    CP_ARRAY(OP, cp, (m)->rxq_len);                                                                                    \
    CP_FIELD(OP, cp, (m)->rx_head);                                                                                    \
    CP_FIELD(OP, cp, (m)->rx_count);                                                                                   \
    CP_FIELD(OP, cp, (m)->rx_pos);                                                                                     \
    CP_FIELD(OP, cp, (m)->tx_frames);                                                                                  \
    CP_FIELD(OP, cp, (m)->rx_frames);                                                                                  \
    CP_FIELD(OP, cp, (m)->irq.level)

// ============================================================
// Helpers
// ============================================================

// INTR follows the unmasked interrupt bits (IMR: one = masked).
static void update_irq(mace_t *m) {
    irq_line_set(&m->irq, (m->ir & (uint8_t)~m->imr) != 0);
}

// Raise IR bits.
static void raise(mace_t *m, uint8_t bits) {
    m->ir |= bits;
    update_irq(m);
}

// Count a missed frame: MPC wraps 255 -> 0 and raises MPCO.
static void count_missed(mace_t *m) {
    if (++m->mpc == 0)
        raise(m, MACE_IR_MPCO);
}

// The loopback mode UTR selects (0 = none).
static unsigned loop_mode(const mace_t *m) {
    return m->utr & MACE_UTR_LOOP_MASK;
}

// The internal loopback modes keep the cable out of it both ways.
static bool internal_loopback(const mace_t *m) {
    unsigned mode = loop_mode(m);
    return mode == MACE_UTR_INTLPB || mode == MACE_UTR_MENDECLPB;
}

// The logical-address filter hash: the FCS of the destination address,
// its high-order six bits as the data manual counts them (p. 70) -- the
// top six bits of the complemented reflected CRC-32 the FCS uses, as
// bmac.c computes the same hash.
static int ladrf_hash(const uint8_t *da) {
    return (int)((~codec_crc32(0, da, 6)) >> 26);
}

// Address matching (p. 55): promiscuous takes everything; otherwise the
// physical address, broadcast and the logical-address filter for the other
// multicasts, each unless its disable bit is set.
static bool address_match(const mace_t *m, const uint8_t *da) {
    if (m->maccc & MACE_MACCC_PROM)
        return true;
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (memcmp(da, bcast, 6) == 0)
        return !(m->maccc & MACCC_DRCVBC);
    if (da[0] & 1u) {
        int h = ladrf_hash(da);
        return (m->ladrf[h >> 3] >> (h & 7)) & 1u;
    }
    return !(m->maccc & MACCC_DRCVPA) && memcmp(da, m->padr, 6) == 0;
}

// Empty the receive FIFO (RCVFWU, reset): frames and any undrained status.
static void rx_flush(mace_t *m) {
    m->rx_head = 0;
    m->rx_count = 0;
    m->rx_pos = 0;
    m->rfs_idx = 4;
}

// Queue a frame for the DMA engine.  `fcs` appends the 4-byte FCS (the
// receiver keeps it unless ASTRPRCV strips it with the pad).
static bool rx_queue(mace_t *m, const uint8_t *frame, int len, bool fcs) {
    if (!(m->maccc & MACE_MACCC_ENRCV))
        return false;
    if (len < 14 || len + ETH_FCS > MACE_FRAME_MAX) {
        LOG(2, "receive: %d-byte frame dropped", len);
        return false;
    }
    if (!address_match(m, frame)) {
        LOG(4, "receive: frame for %02X:%02X:%02X:%02X:%02X:%02X filtered", frame[0], frame[1], frame[2], frame[3],
            frame[4], frame[5]);
        return false;
    }
    if (m->rx_count == MACE_RXQ_DEPTH) {
        count_missed(m); // the FIFO is full: a missed packet
        LOG(2, "receive: FIFO full, frame missed (MPC %u)", m->mpc);
        return false;
    }
    int slot = (m->rx_head + m->rx_count) % MACE_RXQ_DEPTH;
    uint8_t *q = m->rxq[slot];
    int n = len;
    memcpy(q, frame, (size_t)len);
    if (m->rcvfc & RCVFC_ASTRPRCV) {
        // Pad stripping: an 802.3 length field shorter than the frame says
        // where the data ends; the FCS goes too.
        unsigned field = ((unsigned)q[12] << 8) | q[13];
        if (field <= 1500 && 14 + (int)field < n)
            n = 14 + (int)field;
    } else if (fcs) {
        uint32_t crc = codec_crc32(0, q, (size_t)n);
        for (int i = 0; i < ETH_FCS; i++)
            q[n++] = (uint8_t)(crc >> (8 * i));
    }
    m->rxq_len[slot] = (uint16_t)n;
    m->rx_count++;
    LOG(3, "receive: %d-byte frame queued (%u waiting)", n, m->rx_count);
    if (m->ops.rx_ready)
        m->ops.rx_ready(m->ops_ctx);
    return true;
}

// Pop the head frame: its status becomes RCVFS, RCVINT is raised.
static void rx_pop(mace_t *m) {
    int n = m->rxq_len[m->rx_head];
    m->rfs[0] = (uint8_t)n; // RCVCNT[7:0]
    m->rfs[1] = (uint8_t)((n >> 8) & 0x0Fu); // no OFLO/CLSN/FRAM/FCS error
    m->rfs[2] = 0; // runts since the last good frame
    m->rfs[3] = 0; // collisions since the last good frame
    m->rfs_idx = 0;
    m->rx_head = (uint8_t)((m->rx_head + 1) % MACE_RXQ_DEPTH);
    m->rx_count--;
    m->rx_pos = 0;
    m->rx_frames++;
    raise(m, MACE_IR_RCVINT);
}

// Send the frame held in the transmit FIFO, if the transmitter may.
static void tx_send(mace_t *m) {
    if (!m->tx_held || !(m->maccc & MACE_MACCC_ENXMT) || (m->xmtfs & MACE_XMTFS_XMTSV))
        return;
    uint8_t frame[MACE_FRAME_MAX];
    int n = m->tx_len;
    memcpy(frame, m->tx_buf, (size_t)n);
    m->tx_len = 0;
    m->tx_held = false;
    if ((m->xmtfc & XMTFC_APADXMT) && n < ETH_MIN) {
        memset(frame + n, 0, (size_t)(ETH_MIN - n));
        n = ETH_MIN;
    }
    m->tx_frames++;
    // The status first: whatever the frame causes below (a frame received
    // back, the cable's answer) sees the transmission complete.
    m->xmtfs = MACE_XMTFS_XMTSV; // good, no retries, no carrier loss
    m->xmtrc = 0;
    raise(m, MACE_IR_XMTINT);
    unsigned mode = loop_mode(m);
    LOG(3, "transmit: %d-byte frame (loop %u)", n, mode >> 1);
    if (!internal_loopback(m) && m->ops.wire_tx)
        m->ops.wire_tx(m->ops_ctx, frame, n);
    if (mode) {
        // Loopback: the frame comes back through the receiver.  With
        // RCVFCSE clear the transmitter appended the FCS, so it arrives
        // four bytes longer; with it set the receiver checks the host's.
        bool fcs = !(m->utr & UTR_RCVFCSE) && !(m->xmtfc & XMTFC_DXMTFCS);
        rx_queue(m, frame, n, fcs);
    }
}

// ============================================================
// The register file
// ============================================================

uint8_t mace_peek(const mace_t *m, unsigned reg) {
    switch (reg & 0x1Fu) {
    case MACE_RCVFIFO:
        return (m->rx_count && m->rx_pos < m->rxq_len[m->rx_head]) ? m->rxq[m->rx_head][m->rx_pos] : 0;
    case MACE_XMTFC:
        return m->xmtfc;
    case MACE_XMTFS:
        return m->xmtfs;
    case MACE_XMTRC:
        return m->xmtrc;
    case MACE_RCVFC:
        return m->rcvfc;
    case MACE_RCVFS:
        return m->rfs_idx < 4 ? m->rfs[m->rfs_idx] : 0;
    case MACE_FIFOFC: {
        // Frames in the receive FIFO (high nibble) and the transmit FIFO.
        unsigned rx = m->rx_count > 15 ? 15 : m->rx_count;
        return (uint8_t)((rx << 4) | (m->tx_held ? 1u : 0u));
    }
    case MACE_IR:
        return m->ir;
    case MACE_IMR:
        return m->imr;
    case MACE_PR:
        return (uint8_t)((m->xmtfs & MACE_XMTFS_XMTSV ? PR_XMTSV : 0) | (mace_tx_ready(m) ? PR_TDTREQ : 0) |
                         (m->rx_count ? PR_RDTREQ : 0));
    case MACE_BIUCC:
        return m->biucc;
    case MACE_FIFOCC:
        return m->fifocc;
    case MACE_MACCC:
        return m->maccc;
    case MACE_PLSCC:
        return m->plscc;
    case MACE_PHYCC:
        return m->phycc;
    case MACE_CHIPIDLO:
        return (uint8_t)(MACE_CHIPID & 0xFFu);
    case MACE_CHIPIDHI:
        return (uint8_t)(MACE_CHIPID >> 8);
    case MACE_IAC:
        return m->iac; // ADDRCHG never reads back set: the change is immediate
    case MACE_LADRF:
        return m->ladrf[m->ladrf_idx & 7u];
    case MACE_PADR:
        return m->padr[m->padr_idx % 6u];
    case MACE_MPC:
        return m->mpc;
    case MACE_RNTPC:
        return m->rntpc;
    case MACE_RCVCC:
        return m->rcvcc;
    case MACE_UTR:
        return m->utr;
    default:
        return 0; // XMTFIFO and the reserved registers
    }
}

uint8_t mace_read(mace_t *m, unsigned reg) {
    reg &= 0x1Fu;
    uint8_t v = mace_peek(m, reg);
    switch (reg) {
    case MACE_RCVFIFO:
        // Programmed-I/O receive: one byte per read; the frame's last byte
        // ends it, and its status becomes readable through RCVFS.
        if (m->rx_count && m->rfs_idx >= 4) {
            if (++m->rx_pos >= m->rxq_len[m->rx_head])
                rx_pop(m);
        }
        break;
    case MACE_XMTFS:
        // Reading the status (XMTRC first, by the drivers' order) clears
        // both and frees the one-deep status buffer: the next frame may go.
        if (v & MACE_XMTFS_XMTSV) {
            m->xmtfs = 0;
            m->xmtrc = 0;
            tx_send(m);
            if (mace_tx_ready(m) && m->ops.tx_ready)
                m->ops.tx_ready(m->ops_ctx);
        }
        break;
    case MACE_RCVFS:
        if (m->rfs_idx < 4)
            m->rfs_idx++;
        break;
    case MACE_IR:
        m->ir = 0; // read-to-clear
        update_irq(m);
        break;
    case MACE_LADRF:
        if (++m->ladrf_idx >= 8)
            m->iac &= (uint8_t)~IAC_LOGADDR;
        m->ladrf_idx &= 7u;
        break;
    case MACE_PADR:
        if (++m->padr_idx >= 6)
            m->iac &= (uint8_t)~IAC_PHYADDR;
        m->padr_idx %= 6u;
        break;
    case MACE_MPC:
        m->mpc = 0;
        break;
    case MACE_RNTPC:
        m->rntpc = 0;
        break;
    case MACE_RCVCC:
        m->rcvcc = 0;
        break;
    default:
        break;
    }
    return v;
}

void mace_write(mace_t *m, unsigned reg, uint8_t value) {
    reg &= 0x1Fu;
    LOG(4, "write reg %u = $%02X", reg, value);
    switch (reg) {
    case MACE_XMTFIFO:
        // Programmed-I/O transmit bytes join the frame in the FIFO.
        mace_tx_write(m, &value, 1);
        break;
    case MACE_XMTFC:
        m->xmtfc = value;
        break;
    case MACE_RCVFC:
        m->rcvfc = value;
        break;
    case MACE_IMR:
        m->imr = value;
        update_irq(m);
        break;
    case MACE_BIUCC:
        if (value & BIUCC_SWRST) {
            mace_reset(m); // SWRST self-clears during the reset it starts
            break;
        }
        m->biucc = value;
        break;
    case MACE_FIFOCC:
        // The update bits load the watermarks, resetting that FIFO, and
        // clear themselves (data manual p. 66).
        if (value & FIFOCC_XMTFWU) {
            m->tx_len = 0;
            m->tx_held = false;
        }
        if (value & FIFOCC_RCVFWU)
            rx_flush(m);
        m->fifocc = (uint8_t)(value & ~(FIFOCC_XMTFWU | FIFOCC_RCVFWU));
        break;
    case MACE_MACCC:
        m->maccc = value;
        tx_send(m); // a frame held for ENXMT goes now
        if (mace_tx_ready(m) && m->ops.tx_ready)
            m->ops.tx_ready(m->ops_ctx);
        break;
    case MACE_PLSCC:
        m->plscc = value;
        break;
    case MACE_PHYCC:
        m->phycc = value;
        break;
    case MACE_IAC:
        // ADDRCHG stops reception (ENRCV clears) and completes at once --
        // nothing is in flight to wait for; the driver turns ENRCV back on.
        if (value & IAC_ADDRCHG)
            m->maccc &= (uint8_t)~MACE_MACCC_ENRCV;
        if (value & IAC_LOGADDR)
            m->ladrf_idx = 0;
        if (value & IAC_PHYADDR)
            m->padr_idx = 0;
        m->iac = (uint8_t)(value & (IAC_PHYADDR | IAC_LOGADDR));
        break;
    case MACE_LADRF:
        m->ladrf[m->ladrf_idx & 7u] = value;
        if (++m->ladrf_idx >= 8)
            m->iac &= (uint8_t)~IAC_LOGADDR;
        m->ladrf_idx &= 7u;
        break;
    case MACE_PADR:
        m->padr[m->padr_idx % 6u] = value;
        if (++m->padr_idx >= 6)
            m->iac &= (uint8_t)~IAC_PHYADDR;
        m->padr_idx %= 6u;
        break;
    case MACE_UTR:
        m->utr = (uint8_t)(value | (m->utr & UTR_RTRD)); // RTRD stays once set
        break;
    default:
        // Read-only registers, the reserved ones and RTR1/RTR2: ignored.
        break;
    }
}

// ============================================================
// The DMA engine's side
// ============================================================

bool mace_tx_ready(const mace_t *m) {
    return !(m->xmtfs & MACE_XMTFS_XMTSV) && !m->tx_held;
}

void mace_tx_write(mace_t *m, const uint8_t *buf, int len) {
    if (m->tx_held) {
        LOG(2, "transmit: %d bytes written over a held frame, dropped", len);
        return;
    }
    int room = MACE_FRAME_MAX - m->tx_len;
    if (len > room) {
        LOG(1, "transmit: frame longer than %d bytes truncated", MACE_FRAME_MAX);
        len = room;
    }
    memcpy(m->tx_buf + m->tx_len, buf, (size_t)len);
    m->tx_len = (uint16_t)(m->tx_len + len);
}

void mace_tx_eof(mace_t *m) {
    if (m->tx_held)
        return;
    m->tx_held = true;
    tx_send(m);
}

int mace_rx_pending(const mace_t *m) {
    return m->rx_count ? m->rxq_len[m->rx_head] : 0;
}

int mace_rx_take(mace_t *m, uint8_t *buf, int max, uint8_t rfs[4]) {
    if (!m->rx_count)
        return 0;
    int n = m->rxq_len[m->rx_head];
    memcpy(buf, m->rxq[m->rx_head], (size_t)(n < max ? n : max));
    rx_pop(m);
    memcpy(rfs, m->rfs, 4);
    m->rfs_idx = 4; // FIFO-direct status went to the DMA engine
    return n;
}

// ============================================================
// The cable's side
// ============================================================

bool mace_wire_rx(mace_t *m, const uint8_t *frame, int len) {
    if (internal_loopback(m))
        return false; // network reception is prohibited in internal loopback
    uint8_t padded[ETH_MIN];
    if (len >= 14 && len < ETH_MIN) {
        // Short frames arrive padded to the minimum, as on the wire.
        memcpy(padded, frame, (size_t)len);
        memset(padded + len, 0, (size_t)(ETH_MIN - len));
        frame = padded;
        len = ETH_MIN;
    }
    return rx_queue(m, frame, len, true);
}

void mace_get_padr(const mace_t *m, uint8_t mac[6]) {
    memcpy(mac, m->padr, 6);
}

uint32_t mace_tx_frames(const mace_t *m) {
    return m->tx_frames;
}

uint32_t mace_rx_frames(const mace_t *m) {
    return m->rx_frames;
}

// ============================================================
// Lifecycle
// ============================================================

void mace_reset(mace_t *m) {
    // Reset values, data manual p. 74.
    m->xmtfc = XMTFC_APADXMT;
    m->xmtfs = 0;
    m->xmtrc = 0;
    m->rcvfc = RCVFC_ASTRPRCV;
    m->ir = 0;
    m->imr = 0;
    m->biucc = 0x20; // XMTSP = 64 bytes
    m->fifocc = 0x20; // RCVFW = 64 bytes
    m->maccc = 0;
    m->plscc = 0;
    m->phycc = 0x80; // LNKFL
    m->iac = 0;
    m->utr = 0;
    m->padr_idx = 0;
    m->ladrf_idx = 0;
    m->mpc = 0;
    m->rntpc = 0;
    m->rcvcc = 0;
    m->tx_len = 0;
    m->tx_held = false;
    rx_flush(m);
    irq_line_force(&m->irq, false);
}

mace_t *mace_init(void) {
    mace_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    mace_reset(m);
    return m;
}

void mace_restore(mace_t *m, checkpoint_t *cp) {
    MACE_CP_FIELDS(system_read_checkpoint_data, cp, m);
    irq_line_force(&m->irq, (m->ir & (uint8_t)~m->imr) != 0); // re-drive the restored level
}

void mace_delete(mace_t *m) {
    free(m);
}

void mace_checkpoint(const mace_t *m, checkpoint_t *cp) {
    MACE_CP_FIELDS(system_write_checkpoint_data, cp, m);
}

void mace_set_irq(mace_t *m, irq_fn fn, void *ctx) {
    irq_line_bind(&m->irq, fn, ctx);
    irq_line_force(&m->irq, (m->ir & (uint8_t)~m->imr) != 0); // the consumer hears the current level
}

void mace_set_ops(mace_t *m, const mace_ops_t *ops, void *ctx) {
    m->ops = ops ? *ops : (mace_ops_t){0};
    m->ops_ctx = ctx;
}
