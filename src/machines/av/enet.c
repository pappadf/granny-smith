// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// enet.c
// The AV built-in Ethernet -- see enet.h.  The MACE core is the shared
// model; what lives here is what Curio and the PSC put around it on these
// boards, as Apple's AV Ethernet driver (DeclData/DeclNet/Mace/Mace.a and
// MaceEnet.a in the ROM source) programs it:
//
//   * Transmit, PSC channel 2 (ENetWr).  One frame per register set: Addr
//     the packet's physical address, Cnt its length in bytes, armed
//     SENSE|IE|ENABLED (PrimeXmitRS).  The set's bytes go into the MACE's
//     transmit FIFO and its terminal count is the frame's EOF; the PSC's
//     completion (IF, the active set flips) interrupts at level 4 and the
//     MACE's XMTINT at level 3.  Curio's MACE buffers ONE transmit status, so
//     the channel's other set waits until the level-3 handler has read
//     XMTFS (the core's tx_ready hook), never overrunning it.
//   * Receive, PSC channel 1 (ENetRd) in chain mode.  Addr the physical base
//     of a chain of 2048-byte buffers, Cnt the number of buffers (InitRecv
//     writes the chain's Limit; MaceRecv takes In = Limit - Cnt).  Each frame
//     fills one buffer: the four RCVFS bytes, each on both halves of a word,
//     in the first eight bytes, the frame (FCS included, RCVFC = $00) from
//     offset 16 (RecvBuff: MaceStat 8, fill 8, Packet), one IF per frame.
//   * The MACE's INTR on PSC level 3 bit 0.
//   * The AAUI port is cabled to the emulator's AppleTalk network: frames
//     the MACE puts on the wire reach its EtherTalk port, and frames it
//     sends come in through the receiver's address filter.
//
// Timing is coarse and only there so nothing completes inside the register
// write that started it: a transmit set moves AV_ENET_TX_NS after it is
// armed, and received frames reach memory one per wire time (10 Mb/s, plus
// the inter-frame gap).

#include "enet.h"

#include "av.h"
#include "mace.h"
#include "psc.h"

#include "appletalk.h"
#include "checkpoint.h"
#include "log.h"
#include "scheduler.h"
#include "system.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("mace");

#define AV_ENET_TX_NS      20000u // arm-to-transfer delay of a transmit set
#define AV_ENET_NS_PER_BYT 800u // 10 Mb/s
#define AV_ENET_IFG_NS     9600u // inter-frame gap
#define AV_ENET_RX_STRIDE  2048u // RecvBuff: ALIGN 2048
#define AV_ENET_RX_DATA    16u // RecvBuff.Packet: MaceStat (8) + fill (8)

// The Apple address PROM: the station address bytes read backwards from
// $51 and bit-reversed by the driver's NormAddr; the XOR of all eight bytes
// must equal $FF (MaceEnet.a Open, "checksum the addresss PROM" -- a
// mismatch fails the open before the MACE is touched).  Locally-
// administered 02:00:00:09:07:02 -> bit-reversed 40 00 00 90 E0 40, byte 6
// zero and byte 7 chosen for the checksum: $70 ^ $8F = $FF.  (The register
// stub this replaces carried $CF there, an XOR of $BF, so .ENET never
// opened.)
static const uint8_t av_enet_prom[8] = {0x40, 0x00, 0x00, 0x90, 0xE0, 0x40, 0x00, 0x8F};

struct av_enet {
    // --- pointers (nothing of the glue's own is checkpointed: the core is,
    // and the pending transfers are scheduler events) ---
    config_t *cfg;
    mace_t *mace;
};

static inline av_state_t *av_st(config_t *cfg) {
    return (av_state_t *)cfg->machine_context;
}

static inline av_enet_t *enet_of(config_t *cfg) {
    return av_st(cfg)->enet;
}

static av_psc_t *psc_of(av_enet_t *e) {
    return av_st(e->cfg)->psc;
}

// ============================================================
// The DMA pumps
// ============================================================

static void av_enet_tx_event(void *source, uint64_t data);
static void av_enet_rx_event(void *source, uint64_t data);

// Arm `cb` `ns` from now unless it is already pending: a pending transfer
// keeps its time.
static void enet_arm(av_enet_t *e, event_callback_t cb, uint64_t ns) {
    scheduler_t *s = e->cfg->scheduler;
    if (!s || has_event(s, cb))
        return;
    scheduler_new_cpu_event(s, cb, e, 0, 0, ns);
}

// Wire time of a `len`-byte frame.
static uint64_t wire_ns(int len) {
    return (uint64_t)len * AV_ENET_NS_PER_BYT + AV_ENET_IFG_NS;
}

// Transmit: the active set of ENetWr into the MACE, one frame.
static void av_enet_tx_event(void *source, uint64_t data) {
    (void)data;
    av_enet_t *e = (av_enet_t *)source;
    av_psc_t *psc = psc_of(e);
    if (!mace_tx_ready(e->mace))
        return; // the core's tx_ready hook re-arms us
    uint32_t n = av_psc_dma_remaining(psc, AV_PSC_DMA_MACE_TX);
    if (n == 0 || av_psc_dma_dir(psc, AV_PSC_DMA_MACE_TX) != 0)
        return; // nothing armed: the next touch re-arms us
    uint8_t buf[MACE_FRAME_MAX];
    // The whole set is one frame; a count longer than any frame leaves the
    // excess on the set, and the frame ends with what the FIFO holds.
    if (n > sizeof(buf))
        n = sizeof(buf);
    int got = av_psc_dma_device_out(psc, AV_PSC_DMA_MACE_TX, buf, (int)n);
    if (got <= 0)
        return;
    LOG(3, "ENetWr: %d-byte frame into the transmit FIFO", got);
    mace_tx_write(e->mace, buf, got);
    mace_tx_eof(e->mace); // terminal count: the PSC asserts EOF on the last transfer
}

// Receive: the frame at the head of the MACE's receive FIFO into the next
// buffer of ENetRd's chain.
static void av_enet_rx_event(void *source, uint64_t data) {
    (void)data;
    av_enet_t *e = (av_enet_t *)source;
    av_psc_t *psc = psc_of(e);
    int len = mace_rx_pending(e->mace);
    if (len == 0 || av_psc_dma_remaining(psc, AV_PSC_DMA_MACE_RX) == 0)
        return; // nothing waits, or no buffer: rx_ready / the next touch re-arms us
    uint8_t frame[MACE_FRAME_MAX];
    uint8_t rfs[4];
    len = mace_rx_take(e->mace, frame, (int)sizeof(frame), rfs);
    // FIFO-direct status: each of RFS0-RFS3 on both halves of a word.
    uint8_t head[8];
    for (int i = 0; i < 4; i++) {
        head[2 * i] = rfs[i];
        head[2 * i + 1] = rfs[i];
    }
    av_psc_dma_chain_in(psc, AV_PSC_DMA_MACE_RX, head, (int)sizeof(head), frame, len, AV_ENET_RX_STRIDE,
                        AV_ENET_RX_DATA);
    LOG(3, "ENetRd: %d-byte frame delivered", len);
    int next = mace_rx_pending(e->mace);
    if (next)
        enet_arm(e, av_enet_rx_event, wire_ns(next));
}

void av_enet_dma_touch(void *ctx) {
    config_t *cfg = (config_t *)ctx;
    av_state_t *st = av_st(cfg);
    av_enet_t *e = st ? st->enet : NULL;
    if (!e || !st->psc)
        return;
    if (av_psc_dma_remaining(st->psc, AV_PSC_DMA_MACE_TX) && mace_tx_ready(e->mace))
        enet_arm(e, av_enet_tx_event, AV_ENET_TX_NS);
    int len = mace_rx_pending(e->mace);
    if (len && av_psc_dma_remaining(st->psc, AV_PSC_DMA_MACE_RX))
        enet_arm(e, av_enet_rx_event, wire_ns(len));
}

// ============================================================
// The core's hooks
// ============================================================

// INTR -> PSC level 3, bit 0 (MACE).
static void enet_irq(void *ctx, bool level) {
    av_enet_t *e = (av_enet_t *)ctx;
    av_psc_t *psc = psc_of(e);
    if (psc)
        av_psc_level_source(psc, AV_PSC_L3, 0, level);
}

// The wire: what the MACE transmits reaches the AppleTalk network's
// Ethernet port.
static void enet_wire_tx(void *ctx, const uint8_t *frame, int len) {
    av_enet_t *e = (av_enet_t *)ctx;
    if (e->cfg->atalk)
        atalk_conn_ether_input(e->cfg->atalk, frame, (size_t)len);
}

static void enet_rx_ready(void *ctx) {
    av_enet_t *e = (av_enet_t *)ctx;
    int len = mace_rx_pending(e->mace);
    if (len && av_psc_dma_remaining(psc_of(e), AV_PSC_DMA_MACE_RX))
        enet_arm(e, av_enet_rx_event, wire_ns(len));
}

static void enet_tx_ready(void *ctx) {
    av_enet_t *e = (av_enet_t *)ctx;
    if (av_psc_dma_remaining(psc_of(e), AV_PSC_DMA_MACE_TX))
        enet_arm(e, av_enet_tx_event, AV_ENET_TX_NS);
}

// The network's frames for this machine come in through the receiver.
static void enet_wire_rx(void *ctx, const uint8_t *frame, size_t len) {
    av_enet_t *e = (av_enet_t *)ctx;
    mace_wire_rx(e->mace, frame, (int)len);
}

// ============================================================
// I/O island handlers
// ============================================================

uint8_t av_enet_mace_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    return mace_read(enet_of(cfg)->mace, (addr & 0x1FFu) >> 4);
}

uint8_t av_enet_mace_peek(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off;
    return mace_peek(enet_of(cfg)->mace, (addr & 0x1FFu) >> 4);
}

void av_enet_mace_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off;
    mace_write(enet_of(cfg)->mace, (addr & 0x1FFu) >> 4, value);
}

uint8_t av_enet_prom_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off; // this window's handler decodes from addr itself
    (void)cfg;
    // One byte per 16-byte group, at offset $x1 of each.
    if ((addr & 0xFu) != 1)
        return 0xFF;
    return av_enet_prom[(addr >> 4) & 7];
}

void av_enet_prom_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off; // this window's handler decodes from addr itself
    LOG(2, "PROM write $%X = $%02X ignored (read-only)", addr & 0xFFu, value);
    (void)cfg;
}

// ============================================================
// Lifecycle
// ============================================================

av_enet_t *av_enet_init(config_t *cfg, checkpoint_t *cp) {
    av_enet_t *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->cfg = cfg;
    e->mace = mace_init();
    if (!e->mace) {
        free(e);
        return NULL;
    }
    if (cp)
        mace_restore(e->mace, cp);
    mace_set_irq(e->mace, enet_irq, e);
    static const mace_ops_t ops = {.wire_tx = enet_wire_tx, .rx_ready = enet_rx_ready, .tx_ready = enet_tx_ready};
    mace_set_ops(e->mace, &ops, e);
    // Registered at construction, before a checkpoint's event queue replays.
    scheduler_new_event_type(cfg->scheduler, "enet", e, "tx", &av_enet_tx_event);
    scheduler_new_event_type(cfg->scheduler, "enet", e, "rx", &av_enet_rx_event);
    av_psc_set_enet_touch_hook(av_st(cfg)->psc, av_enet_dma_touch, cfg);
    if (cfg->atalk)
        atalk_conn_attach_ether(cfg->atalk, enet_wire_rx, e);
    return e;
}

void av_enet_delete(av_enet_t *e) {
    if (!e)
        return;
    if (e->cfg->atalk)
        atalk_conn_attach_ether(e->cfg->atalk, NULL, NULL);
    if (e->cfg->scheduler)
        scheduler_forget_source(e->cfg->scheduler, e);
    mace_delete(e->mace);
    free(e);
}

void av_enet_checkpoint(av_enet_t *e, checkpoint_t *cp) {
    if (!e || !cp)
        return;
    mace_checkpoint(e->mace, cp);
}

void av_enet_reset(av_enet_t *e) {
    if (!e)
        return;
    if (e->cfg->scheduler) {
        remove_event(e->cfg->scheduler, &av_enet_tx_event, e);
        remove_event(e->cfg->scheduler, &av_enet_rx_event, e);
    }
    mace_reset(e->mace);
}
