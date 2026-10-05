// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iop_scc.c
// Behavioural model of the SCC IOP's 65C02 firmware.  Replaces the 6502
// core itself with C code that produces the same host-visible effects.
//
// The SCC IOP fronts the Zilog Z8530 dual-channel serial controller
// (modem on channel A, printer on channel B).  Its firmware is the IOP
// Kernel (Apple, "IOP Kernel ERS", 1989), into which the host downloads
// per-channel drivers: the serial drivers ('SERD') or, for AppleTalk, the
// IOP LocalTalk driver ('iopc', downloaded by the System's IOP LocalTalk
// 'ltlk').  What our behavioural model covers:
//
//   - Boot-time bypass enable, so the host's IOPMgr sees iopInBypassMode
//     on its first iopStatCtl read.
//
//   - The Kernel's command channel, XmtMsg[1]: Allocate Driver ($01),
//     DeAllocate Driver ($02), Initialize Driver ($03) and ByPass Mode
//     ($04), each answered with its result code in byte 0 of the slot, as
//     the firmware overwrites the request with its reply.
//
//   - The IOP LocalTalk driver on channel B (boxes 5-7).  The IOP runs the
//     LLAP link only -- node acquisition by lapENQ, the lapRTS/lapCTS
//     dialog before a directed frame, framing -- and passes data frames to
//     and from the host, whose .MPP keeps DDP and everything above.  The
//     wire is the emulated AppleTalk network, reached through the SCC's
//     SDLC divert (scc_set_sdlc_divert): its frames come here instead of
//     into the Z8530's receiver, and ours go to its frame sink.
//
// Driver B's message protocol, read from 'iopc' 58.0 and the 'ltlk' 58.2.2
// that drives it.  XmtMsg[5] byte 0 is the opcode on the way in and the
// result on the way out ($00 done, $FF failed, $FE unknown opcode):
//
//   $01        open: reply bytes 1-2 = IOP address of the write buffer
//   $02 nn     lapENQ for node nn: $00 = nobody answered, nn is now the
//              IOP's node; $FF = a node answered (nn is taken)
//   $03 ll ll  transmit the ll ll bytes in the write buffer -- an LLAP
//              frame whose source byte the IOP fills in with its node
//   $04        reply bytes 1-2 = IOP address of the 48-byte statistics
//   $05 nn     acquire an additional node (as $02, keeping the first)
//   $06 nn     give up node nn (the first node: all of them)
//
// A received frame goes to the host in RcvMsg[5] -- $01, the frame's IOP
// address (2 bytes), its length plus the two FCS bytes (2 bytes) -- under
// Int1, and stays in IOP RAM until the host releases the slot.
//
// Any other driver image is the asynchronous serial driver (Apple, "Serial
// IOP Driver ERS", 1989), modelled as a port whose output goes down the
// cable -- the SCC's output file or device -- and whose reads wait, as
// nothing yet feeds received bytes back through the IOP.

#include "iop_internal.h"

#include "log.h"
#include "scc.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("iop_scc");

// IOP Kernel commands and results, as the shipped kernel answers them (its
// codes are one step off the 1989 ERS's: UnKnwnMsg takes -2)
#define KERN_SLOT         1
#define KERN_ALLOC        0x01
#define KERN_DEALLOC      0x02
#define KERN_INIT         0x03
#define KERN_BYPASS       0x04
#define KERN_SCC_CNTL     0x06
#define KERN_NO_ERR       0x00
#define KERN_ERROR        0xFF // driver number out of range
#define KERN_UNKNOWN      0xFE // UnKnwnMsg
#define KERN_DVR_IN_USE   0xFD
#define KERN_IN_BYPASS    0xFC
#define KERN_NOT_ALLOC    0xFB
#define KERN_BAD_ID       0xFA
#define KERN_RESET_OWNER  0xFF // the bypass owner the kernel starts with
#define DRIVER_A          0
#define DRIVER_B          1
#define DRIVER_FIRST_SLOT 2 // driver A owns boxes 2-4, driver B boxes 5-7

// IOP LocalTalk driver B
#define LT_SLOT        5
#define LT_OPEN        0x01
#define LT_ENQ         0x02
#define LT_WRITE       0x03
#define LT_STATS       0x04
#define LT_ADD_NODE    0x05
#define LT_REMOVE_NODE 0x06
#define LT_DONE        0x00
#define LT_FAILED      0xFF
#define LT_BAD_OPCODE  0xFE
#define LT_STATS_SIZE  48

// LLAP
#define LLAP_HEADER   3
#define LLAP_FCS      2
#define LLAP_BCAST    0xFF
#define LLAP_ENQ      0x81
#define LLAP_ACK      0x82
#define LLAP_RTS      0x84
#define LLAP_CTS      0x85
#define LLAP_CTL_BASE 0x80

static scc_t *iop_scc_chip(iop_t *iop) {
    return (scc_t *)iop->bypass_device;
}

// ============================================================================
//  The LocalTalk driver's view of the wire
// ============================================================================

// The control frame a dialog is waiting for, caught while its request is on
// the wire: the network answers a lapENQ or lapRTS within the same call.
static struct {
    bool waiting;
    uint8_t type, dst, src;
    bool seen;
} g_lt_expect;

static bool lt_has_node(const iop_scc_state_t *s, uint8_t node) {
    return (s->lt_nodes[node >> 3] >> (node & 7)) & 1;
}

static void lt_set_node(iop_scc_state_t *s, uint8_t node, bool on) {
    if (on)
        s->lt_nodes[node >> 3] |= (uint8_t)(1u << (node & 7));
    else
        s->lt_nodes[node >> 3] &= (uint8_t) ~(1u << (node & 7));
}

static void lt_wire_tx(iop_t *iop, const uint8_t *frame, size_t len) {
    scc_sdlc_divert_tx(iop_scc_chip(iop), frame, len);
}

// Put a control frame on the wire and say whether `answer` came back from
// `dst` -- the IOP's lapENQ (answered by lapACK) and lapRTS (lapCTS).
static bool lt_dialog(iop_t *iop, uint8_t dst, uint8_t src, uint8_t type, uint8_t answer) {
    uint8_t frame[LLAP_HEADER] = {dst, src, type};
    g_lt_expect.waiting = true;
    g_lt_expect.type = answer;
    g_lt_expect.dst = src;
    g_lt_expect.src = dst;
    g_lt_expect.seen = false;
    lt_wire_tx(iop, frame, sizeof(frame));
    g_lt_expect.waiting = false;
    return g_lt_expect.seen;
}

// Hand the oldest queued frame to the host, if RcvMsg[5] is free.
static void lt_post_next(iop_t *iop) {
    iop_scc_state_t *s = &iop->scc;
    if (s->lt_rx_posted || s->lt_rxq_count == 0)
        return;
    if (iop->ram[IOPRcvMsgBase + IOPMsgState(LT_SLOT)] != MsgIdle)
        return;
    const iop_lt_frame_t *f = &s->lt_rxq[s->lt_rxq_head];
    memcpy(&iop->ram[s->lt_rbuf], f->buf, f->len);
    memset(&iop->ram[s->lt_rbuf + f->len], 0, LLAP_FCS);
    unsigned len = f->len + LLAP_FCS;
    uint8_t msg[5] = {0x01, (uint8_t)(s->lt_rbuf >> 8), (uint8_t)s->lt_rbuf, (uint8_t)(len >> 8), (uint8_t)len};
    s->lt_rxq_head = (uint8_t)((s->lt_rxq_head + 1) % IOP_LT_RXQ_DEPTH);
    s->lt_rxq_count--;
    s->lt_rx_posted = true;
    iop_post_reply(iop, LT_SLOT, msg, sizeof(msg), true);
}

// A frame from the network (scc_sdlc_send, diverted to us).
static void lt_rx_frame(void *ctx, const uint8_t *frame, size_t len) {
    iop_t *iop = (iop_t *)ctx;
    iop_scc_state_t *s = &iop->scc;
    if (!s->lt_open || len < LLAP_HEADER)
        return;
    uint8_t dst = frame[0], src = frame[1], type = frame[2];

    if (type >= LLAP_CTL_BASE) {
        if (g_lt_expect.waiting && type == g_lt_expect.type && dst == g_lt_expect.dst && src == g_lt_expect.src) {
            g_lt_expect.seen = true;
            return;
        }
        if (dst == LLAP_BCAST || !lt_has_node(s, dst))
            return;
        if (type == LLAP_ENQ) {
            uint8_t ack[LLAP_HEADER] = {src, dst, LLAP_ACK};
            lt_wire_tx(iop, ack, sizeof(ack));
        } else if (type == LLAP_RTS) {
            uint8_t cts[LLAP_HEADER] = {src, dst, LLAP_CTS};
            lt_wire_tx(iop, cts, sizeof(cts));
        }
        return;
    }

    if (dst != LLAP_BCAST && !lt_has_node(s, dst))
        return;
    if (len > IOP_LT_FRAME_MAX)
        return;
    if (s->lt_rxq_count >= IOP_LT_RXQ_DEPTH) {
        LOG(2, "SCC IOP: LocalTalk receive queue full, dropping a %zu-byte frame", len);
        return;
    }
    iop_lt_frame_t *f = &s->lt_rxq[(s->lt_rxq_head + s->lt_rxq_count) % IOP_LT_RXQ_DEPTH];
    memcpy(f->buf, frame, len);
    f->len = (uint16_t)len;
    s->lt_rxq_count++;
    LOG(4, "SCC IOP: LocalTalk rx %zu bytes %02x->%02x type %02x", len, src, dst, type);
    lt_post_next(iop);
}

static bool lt_ready(void *ctx) {
    return ((iop_t *)ctx)->scc.lt_open;
}

static const scc_sdlc_divert_t lt_divert = {
    .ready = lt_ready,
    .rx_frame = lt_rx_frame,
};

// Claim channel B's link while the LocalTalk driver is open; give it back
// to the chip otherwise.
static void lt_sync_divert(iop_t *iop) {
    scc_set_sdlc_divert(iop_scc_chip(iop), iop->scc.lt_open ? &lt_divert : NULL, iop);
}

static void lt_close(iop_t *iop) {
    iop_scc_state_t *s = &iop->scc;
    s->lt_open = false;
    s->lt_node = 0;
    memset(s->lt_nodes, 0, sizeof(s->lt_nodes));
    s->lt_rxq_count = 0;
    s->lt_rx_posted = false;
    lt_sync_divert(iop);
}

// Acquire node `node` by lapENQ: true when nobody answered.
static bool lt_enq(iop_t *iop, uint8_t node) {
    return !lt_dialog(iop, node, node, LLAP_ENQ, LLAP_ACK);
}

// Transmit the frame in the write buffer; false when a directed frame's
// destination never granted the lapRTS.
static bool lt_write(iop_t *iop, unsigned len) {
    iop_scc_state_t *s = &iop->scc;
    if (len < LLAP_HEADER || len > IOP_LT_FRAME_MAX)
        return false;
    uint8_t frame[IOP_LT_FRAME_MAX];
    memcpy(frame, &iop->ram[s->lt_wbuf], len);
    frame[1] = s->lt_node;
    uint8_t dst = frame[0], type = frame[2];
    if (dst != LLAP_BCAST && type < LLAP_CTL_BASE && !lt_dialog(iop, dst, s->lt_node, LLAP_RTS, LLAP_CTS)) {
        LOG(4, "SCC IOP: LocalTalk tx to %02x: no lapCTS", dst);
        return false;
    }
    LOG(4, "SCC IOP: LocalTalk tx %u bytes %02x->%02x type %02x", len, s->lt_node, dst, type);
    lt_wire_tx(iop, frame, len);
    return true;
}

// XmtMsg[5]: one driver request, answered in place.
static void lt_command(iop_t *iop, uint8_t *msg) {
    iop_scc_state_t *s = &iop->scc;
    uint8_t op = msg[0], node = msg[1];
    uint8_t result = LT_DONE;
    switch (op) {
    case LT_OPEN:
        lt_close(iop);
        s->lt_open = true;
        lt_sync_divert(iop);
        msg[1] = (uint8_t)(s->lt_wbuf >> 8);
        msg[2] = (uint8_t)s->lt_wbuf;
        LOG(2, "SCC IOP: LocalTalk driver open");
        break;
    case LT_ENQ:
        msg[1] = 0;
        if (s->lt_node && s->lt_node != node)
            lt_set_node(s, s->lt_node, false);
        if (lt_enq(iop, node)) {
            s->lt_node = node;
            lt_set_node(s, node, true);
        } else {
            s->lt_node = 0;
            lt_set_node(s, node, false);
            result = LT_FAILED;
        }
        LOG(3, "SCC IOP: LocalTalk lapENQ node %02x: %s", node, result ? "taken" : "free");
        break;
    case LT_ADD_NODE:
        msg[1] = 0;
        if (lt_enq(iop, node))
            lt_set_node(s, node, true);
        else
            result = LT_FAILED;
        break;
    case LT_REMOVE_NODE:
        msg[1] = 0;
        if (node == s->lt_node) {
            s->lt_node = 0;
            memset(s->lt_nodes, 0, sizeof(s->lt_nodes));
        } else {
            lt_set_node(s, node, false);
        }
        break;
    case LT_WRITE:
        if (!lt_write(iop, ((unsigned)msg[1] << 8) | msg[2]))
            result = LT_FAILED;
        msg[1] = msg[2] = 0;
        break;
    case LT_STATS:
        msg[1] = (uint8_t)(s->lt_stats >> 8);
        msg[2] = (uint8_t)s->lt_stats;
        break;
    default:
        result = LT_BAD_OPCODE;
        break;
    }
    msg[0] = result;
}

// ============================================================================
//  The serial drivers
//
// Channel c's driver owns boxes 2+3c (read), 3+3c (write) and 4+3c
// (open/control/close).  Byte 0 of each is the result; the open/control/
// close box carries its opcode in byte 1 ($01 open, $00 control with the
// control opcode in byte 2, $02 close).  Written bytes go down the cable
// as the chip's would (scc_port_tx_bytes); nothing feeds received bytes
// back yet, so a read is held -- claimed, not completed -- until the port
// closes.
// ============================================================================

#define SER_READ_BOX    0 // box offsets within a driver's three
#define SER_WRITE_BOX   1
#define SER_CONTROL_BOX 2
#define SER_OPEN        0x01
#define SER_CONTROL     0x00
#define SER_CLOSE       0x02
#define SER_WBUF_SIZE   256
#define SER_STATS_SIZE  7

static int ser_slot(int drv, int box) {
    return DRIVER_FIRST_SLOT + 3 * drv + box;
}

// Place the driver's write buffer and status bytes inside its downloaded
// image (never run), or in free RAM when the image is too small.
static void ser_place_buffers(iop_t *iop, int drv) {
    iop_scc_state_t *s = &iop->scc;
    unsigned base = iop->dl_lo;
    if (iop->dl_hi < iop->dl_lo || (unsigned)iop->dl_hi + 1 - base < SER_WBUF_SIZE + SER_STATS_SIZE)
        base = 0x6800u + 0x200u * (unsigned)drv;
    s->ser_wbuf[drv] = (uint16_t)base;
    s->ser_stats[drv] = (uint16_t)(base + SER_WBUF_SIZE);
    // Status (Serial IOP Driver ERS, "Determining Status Information"):
    // the first byte is the host's flag, the six after it SerStaRec, all
    // clear -- nothing pending, no errors, no handshake holding output.
    memset(&iop->ram[s->ser_stats[drv]], 0, SER_STATS_SIZE);
    iop->ram[s->ser_stats[drv]] = 0xFF;
}

// A held read completes with nothing in the buffer: the driver is closing.
static void ser_release_read(iop_t *iop, int drv) {
    iop_scc_state_t *s = &iop->scc;
    if (!s->ser_read_waiting[drv])
        return;
    s->ser_read_waiting[drv] = false;
    int slot = ser_slot(drv, SER_READ_BOX);
    uint8_t *msg = &iop->ram[IOPMsgPayload(IOPXmtMsgBase, slot)];
    memset(msg, 0, 16);
    msg[0] = KERN_NO_ERR;
    iop_complete_xmt(iop, slot);
}

// One request on driver `drv`'s box `box`.  False when the reply is held.
static bool ser_command(iop_t *iop, int drv, int box, uint8_t *msg) {
    iop_scc_state_t *s = &iop->scc;
    switch (box) {
    case SER_READ_BOX:
        if (!s->ser_open[drv]) {
            msg[0] = KERN_ERROR;
            return true;
        }
        s->ser_read_waiting[drv] = true;
        iop->ram[IOPXmtMsgBase + IOPMsgState(ser_slot(drv, box))] = MsgReceived;
        return false;
    case SER_WRITE_BOX: {
        unsigned len = ((unsigned)msg[4] << 8) | msg[5];
        if (len > SER_WBUF_SIZE)
            len = SER_WBUF_SIZE;
        if (s->ser_open[drv])
            scc_port_tx_bytes(iop_scc_chip(iop), (unsigned)drv, &iop->ram[s->ser_wbuf[drv]], len);
        msg[0] = s->ser_open[drv] ? KERN_NO_ERR : KERN_ERROR;
        LOG(4, "SCC IOP: serial %c write %u bytes", 'A' + drv, len);
        return true;
    }
    default:
        switch (msg[1]) {
        case SER_OPEN:
            s->ser_open[drv] = true;
            msg[8] = (uint8_t)(s->ser_wbuf[drv] >> 8);
            msg[9] = (uint8_t)s->ser_wbuf[drv];
            msg[10] = (uint8_t)(SER_WBUF_SIZE >> 8);
            msg[11] = (uint8_t)SER_WBUF_SIZE;
            msg[12] = (uint8_t)(s->ser_stats[drv] >> 8);
            msg[13] = (uint8_t)s->ser_stats[drv];
            LOG(2, "SCC IOP: serial driver %c open", 'A' + drv);
            break;
        case SER_CLOSE:
            ser_release_read(iop, drv);
            s->ser_open[drv] = false;
            LOG(2, "SCC IOP: serial driver %c closed", 'A' + drv);
            break;
        default: // control: the settings have no effect on a byte stream
            LOG(4, "SCC IOP: serial %c control $%02x", 'A' + drv, msg[2]);
            break;
        }
        msg[0] = KERN_NO_ERR;
        return true;
    }
}

// ============================================================================
//  IOP Kernel
// ============================================================================

// A driver image the host downloaded is the LocalTalk driver: it names
// itself ("IOP LocalTalk", "LocalTalk,Version").
static bool image_is_localtalk(const iop_t *iop) {
    static const char tag[] = "LocalTalk";
    if (iop->dl_hi < iop->dl_lo)
        return false;
    size_t n = sizeof(tag) - 1;
    for (unsigned a = iop->dl_lo; a + n <= (unsigned)iop->dl_hi + 1; a++)
        if (memcmp(&iop->ram[a], tag, n) == 0)
            return true;
    return false;
}

// The driver B image just initialised is the LocalTalk driver: place its
// buffers inside it (we never run its code, and the host reads the
// addresses from the replies), well clear of each other.
static void lt_place_buffers(iop_t *iop) {
    iop_scc_state_t *s = &iop->scc;
    unsigned base = iop->dl_lo;
    if ((unsigned)iop->dl_hi + 1 - base < 2u * (IOP_LT_FRAME_MAX + LLAP_FCS) + LT_STATS_SIZE)
        base = 0x6000; // an image too small to hold them: free RAM above the drivers
    s->lt_wbuf = (uint16_t)base;
    s->lt_rbuf = (uint16_t)(base + IOP_LT_FRAME_MAX + LLAP_FCS);
    s->lt_stats = (uint16_t)(s->lt_rbuf + IOP_LT_FRAME_MAX + LLAP_FCS);
    memset(&iop->ram[s->lt_stats], 0, LT_STATS_SIZE);
}

// Each driver's init entry point less one, as AllocDvr reports it (the
// kernel enters it by RTS): driver A's region starts at $2D55, B's at $56AA.
static const uint16_t k_driver_init_rts[2] = {0x2D54, 0x56A9};

static void kernel_command(iop_t *iop, uint8_t *msg) {
    iop_scc_state_t *s = &iop->scc;
    uint8_t cmd = msg[0], drv = msg[1], client = msg[2];
    uint8_t result = KERN_NO_ERR;
    if (s->bypass_client && cmd != KERN_BYPASS && cmd != KERN_SCC_CNTL) {
        // In bypass only ByPass and SCCCntl are taken
        result = KERN_IN_BYPASS;
        msg[1] = s->bypass_client;
        msg[2] = 0;
    } else if (cmd >= KERN_ALLOC && cmd <= KERN_INIT && drv > DRIVER_B) {
        result = KERN_ERROR;
        msg[1] = msg[2] = 0;
    } else {
        switch (cmd) {
        case KERN_ALLOC:
            if (s->driver_client[drv]) {
                result = KERN_DVR_IN_USE;
                msg[1] = s->driver_client[drv];
                msg[2] = 0;
                break;
            }
            s->driver_client[drv] = client;
            s->driver_kind[drv] = IOP_DRIVER_NONE;
            iop->dl_lo = 0xFFFF; // the image comes next
            iop->dl_hi = 0;
            msg[1] = 0;
            msg[2] = (uint8_t)(k_driver_init_rts[drv] >> 8);
            msg[3] = (uint8_t)k_driver_init_rts[drv];
            break;
        case KERN_DEALLOC:
            if (s->driver_kind[drv] == IOP_DRIVER_LOCALTALK)
                lt_close(iop);
            ser_release_read(iop, drv);
            s->ser_open[drv] = false;
            s->driver_client[drv] = 0;
            s->driver_kind[drv] = IOP_DRIVER_NONE;
            msg[1] = 0;
            break;
        case KERN_INIT:
            msg[1] = 0;
            if (!s->driver_client[drv]) {
                result = KERN_NOT_ALLOC;
                break;
            }
            // The LocalTalk driver names itself; any other image is the
            // serial driver.  LocalTalk runs on channel B, the AppleTalk
            // port, only.
            if (image_is_localtalk(iop)) {
                if (drv != DRIVER_B) {
                    LOG(2, "SCC IOP: LocalTalk on channel A is not modelled");
                    result = KERN_ERROR;
                    break;
                }
                s->driver_kind[drv] = IOP_DRIVER_LOCALTALK;
                lt_place_buffers(iop);
            } else {
                s->driver_kind[drv] = IOP_DRIVER_SERIAL;
                s->ser_open[drv] = false;
                s->ser_read_waiting[drv] = false;
                ser_place_buffers(iop, drv);
            }
            LOG(2, "SCC IOP: %s driver %c initialised ($%04x-$%04x)",
                s->driver_kind[drv] == IOP_DRIVER_LOCALTALK ? "LocalTalk" : "serial", 'A' + drv, iop->dl_lo,
                iop->dl_hi);
            break;
        case KERN_BYPASS:
            if (drv) { // on
                if (s->driver_client[DRIVER_A] || s->driver_client[DRIVER_B]) {
                    result = KERN_DVR_IN_USE;
                    msg[1] = s->driver_client[DRIVER_A];
                    msg[2] = s->driver_client[DRIVER_B];
                } else if (s->bypass_client) {
                    result = KERN_IN_BYPASS;
                    msg[1] = s->bypass_client;
                    msg[2] = 0;
                } else {
                    s->bypass_client = client;
                    msg[1] = 0;
                }
            } else if (client == s->bypass_client) { // off, by its owner
                s->bypass_client = 0;
                msg[1] = msg[2] = 0;
            } else {
                result = KERN_BAD_ID;
                msg[1] = s->bypass_client;
            }
            break;
        case KERN_SCC_CNTL: // the clock source: nothing a byte stream sees
            msg[1] = 0;
            break;
        default:
            result = KERN_UNKNOWN;
            msg[1] = msg[2] = 0;
            break;
        }
    }
    msg[0] = result;
    LOG(3, "SCC IOP: kernel cmd $%02x drv %u client $%02x -> $%02x", cmd, drv, client, result);
}

// ============================================================================
//  Behaviour callbacks
// ============================================================================

// On RUN-bit 0→1: firmware's $040E reset entry (annotated source).
//
// We replicate the firmware-visible end state:
//   - iopInBypassMode set in iopStatCtl  (host can drive SCC directly)
//   - All mailbox slots idle  (iop.c's iop_init_mailbox already did this)
//   - $031F = $FF                       (alive flag, already set)
//   - The Kernel's driver table empty, bypass on
static void iop_scc_on_run_start(iop_t *iop) {
    iop->stat_ctl |= iopInBypassModeBit;
    lt_close(iop);
    memset(&iop->scc, 0, sizeof(iop->scc));
    iop->scc.bypass_client = KERN_RESET_OWNER;
    LOG(2, "SCC IOP: firmware started, bypass mode enabled");
}

// On host-kick: the firmware's $0521 handler.  Walk XmtMsg for requests
// (answered in place, then MsgCompleted) and RcvMsg for messages the host
// has finished with.
static void iop_scc_on_host_kick(iop_t *iop) {
    for (int slot = 1; slot <= MaxIopMsgNum; slot++) {
        uint8_t state = iop->ram[IOPXmtMsgBase + IOPMsgState(slot)];
        if (state != NewMsgSent)
            continue;
        uint8_t *msg = &iop->ram[IOPMsgPayload(IOPXmtMsgBase, slot)];
        LOG(4, "SCC IOP: XmtMsg[%d] req=$%02x $%02x $%02x", slot, msg[0], msg[1], msg[2]);
        if (slot == KERN_SLOT) {
            kernel_command(iop, msg);
        } else {
            int drv = (slot - DRIVER_FIRST_SLOT) / 3, box = (slot - DRIVER_FIRST_SLOT) % 3;
            switch (iop->scc.driver_kind[drv]) {
            case IOP_DRIVER_LOCALTALK:
                // Boxes 6 and 7 carry nothing the LocalTalk driver acts on
                if (slot == LT_SLOT)
                    lt_command(iop, msg);
                else
                    msg[0] = LT_DONE;
                break;
            case IOP_DRIVER_SERIAL:
                if (!ser_command(iop, drv, box, msg))
                    continue; // held: completes later
                break;
            default:
                msg[0] = KERN_NOT_ALLOC; // no driver there to answer
                break;
            }
        }
        iop_complete_xmt(iop, slot);
    }
    if (iop->ram[IOPRcvMsgBase + IOPMsgState(LT_SLOT)] == MsgCompleted) {
        iop->ram[IOPRcvMsgBase + IOPMsgState(LT_SLOT)] = MsgIdle;
        iop->scc.lt_rx_posted = false;
        lt_post_next(iop);
    }
}

// Reset and power-on hold the 65C02 in reset: its drivers are gone, and
// the link goes back to the chip.
static void iop_scc_cancel_events(iop_t *iop) {
    lt_close(iop);
    memset(&iop->scc, 0, sizeof(iop->scc));
}

static void iop_scc_on_restore(iop_t *iop) {
    lt_sync_divert(iop);
}

static void iop_scc_on_delete(iop_t *iop) {
    scc_set_sdlc_divert(iop_scc_chip(iop), NULL, NULL);
}

const iop_behavior_t iop_scc_behavior = {
    .name = "SCC IOP",
    .kind = SccIopNum,
    // FNV-1a32 of iop-scc.bin captured 2026-05-16 from a IIfx ROM boot.
    .expected_fnv1a = 0x752d244au,
    .on_run_start = iop_scc_on_run_start,
    .on_host_kick = iop_scc_on_host_kick,
    .cancel_events = iop_scc_cancel_events,
    .on_restore = iop_scc_on_restore,
    .on_delete = iop_scc_on_delete,
};
