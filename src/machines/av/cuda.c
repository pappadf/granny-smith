// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cuda.c
// Behavioral Cuda 2.37 model — see cuda.h for the transport contract.
// Forked from mdu/egret.c and diverged to the Cuda wire protocol:
//
//   Host -> Cuda (a command packet): the host puts the VIA SR in output
//     mode, writes the first byte, then asserts TIP (low).  Each further
//     byte is written to the SR and announced with a BYTEACK toggle; the
//     bytes reach us via the VIA's shift-out callback.  The host ends the
//     packet by negating TIP (low->high); we process the command then.
//
//   Cuda -> Host (response/unsolicited): we assert TREQ (low) and clock the
//     attention byte into the SR (via_input_sr).  The host answers by
//     asserting TIP; every BYTEACK toggle clocks the next byte.  TREQ rises
//     with the LAST byte (the host's per-byte "last?" check).  When the
//     host terminates the transaction (TIP high) we clock one extra "idle
//     acknowledge" byte — CudaMgr's @waitIdleAck spins on it.
//
//   Sync cycle (CudaInit): with the bus idle (TIP high) the host asserts
//     BYTEACK alone; we assert TREQ and clock a byte.  When BYTEACK rises
//     we negate TREQ and, ~25 us later, clock the idle acknowledge.  The
//     delay is mandatory: the host clears the SR interrupt after seeing
//     TREQ rise, so a synchronous push would be consumed early and the ROM
//     would reach DeadCuda.  Sync also silences all asynchronous sources.
//
// Response packet layout matches Egret's (rcvHeader in CudaMgr.a):
// [attn][pktType][flags][cmd] then data.  Error packets carry a 5-byte
// header [attn][errorPkt][code][pktType][cmd] (the host reads the fifth
// byte only for errorPkt).

#include "cuda.h"

#include "adb.h"
#include "checkpoint.h"
#include "log.h"
#include "rtc.h"
#include "scheduler.h"
#include "system.h"
#include "via.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("cuda");

// === VIA1 port-B handshake pins (via1-cuda.md §2) ===========================

#define PB_TREQ    (1u << 3) // PB3 vCudaTREQ — Cuda transaction request (host input, active-low)
#define PB_BYTEACK (1u << 4) // PB4 vCudaBYTEACK — per-byte level toggle (host output)
#define PB_TIP     (1u << 5) // PB5 vCudaTIP — transaction in progress (host output, active-low)

// === Packet types (egretequ.a; confirmed by the firmware dispatch) ==========

#define PKT_ADB    0x00
#define PKT_PSEUDO 0x01
#define PKT_ERROR  0x02
#define PKT_TICK   0x03

// === Pseudo-command codes ===================================================

#define CMD_NOP        0x00
#define CMD_APOLL      0x01 // start/stop ADB autopoll
#define CMD_RD6805     0x02 // read HC05 address space (PRAM window $100-$1FF)
#define CMD_RDTIME     0x03 // read RTC (4 data bytes)
#define CMD_RDPRAM     0x07 // read PRAM (open-ended)
#define CMD_WR6805     0x08 // write HC05 address space
#define CMD_WRTIME     0x09 // write RTC
#define CMD_PWRDOWN    0x0A // power off
#define CMD_WRPRAM     0x0C // write PRAM
#define CMD_WRDFAC     0x0E // audio gain (accept-and-log)
#define CMD_RESET      0x11 // cold reset
#define CMD_SETAUTOP   0x14 // set autopoll rate
#define CMD_WR1SECMODE 0x1B // 1-second-interrupt mode

// Error codes returned in an errorPkt (firmware: $12C4 dispatch).
#define CUDA_ERR_INVPSEUDO 2 // rejected / out-of-range pseudo-command
#define CUDA_ERR_PRAMADDR  4 // PRAM address out of the $100-$1FF page

// The twelve pseudo-commands Cuda 2.37's dispatch table REJECTS (their
// slots jump to the invalid-command path; via1-cuda.md §3c).  A model must
// reject them, not implement them.
static const uint8_t cuda_rejected_cmds[] = {0x04, 0x05, 0x06, 0x0F, 0x15, 0x17, 0x18, 0x1C, 0x1D, 0x1E, 0x1F, 0x20};
#define CUDA_MAX_PSEUDO 0x24 // MaxPseudoCmd; >= $25 rejected

// === ADB response status flags ==============================================

#define CUDA_FLAG_TIMEOUT  (1u << 1) // addressed device had no data
#define CUDA_FLAG_AUTOPOLL (1u << 6) // data came from an auto-poll

// Auto-poll cadence (~11 ms) and the 1-second tick.
#define CUDA_AUTOPOLL_NS 11000000.0
#define CUDA_TICK_NS     1000000000.0
// Delay before a Cuda-initiated SR byte lands (the firmware's ~25 us).
#define CUDA_PUSH_DELAY_NS 25000.0

// Transfer state.
typedef enum {
    CUDA_IDLE = 0, // bus idle
    CUDA_RECEIVING, // host is shifting a command packet to us
    CUDA_SENDING, // we are shifting a response/unsolicited packet out
    CUDA_SYNC, // CudaInit sync cycle in progress (sync ack sent)
} cuda_state_t;

#define CUDA_RX_MAX 24 // command bytes the host can send in one packet
#define CUDA_TX_MAX 264 // response bytes (header + up to 256 PRAM/ADB data)

struct av_cuda {
    // --- plain data (checkpointed up to the first pointer field) ---
    cuda_state_t state;

    uint8_t rx_buf[CUDA_RX_MAX];
    int rx_len;

    uint8_t tx_buf[CUDA_TX_MAX];
    int tx_len;
    int tx_idx; // index of the byte most recently pushed to the SR

    uint8_t last_pb; // last VIA1 port-B output seen (for edge detection)
    bool treq_high; // level we drive on TREQ (PB3); true = idle
    bool push_pending; // a delayed SR push is scheduled
    uint8_t push_byte; // byte the delayed push will deliver

    bool autopoll_enabled;
    bool onesec_enabled;
    uint8_t autopoll_phase;

    // --- pointers / callbacks (not checkpointed) ---
    struct via *via1;
    struct rtc *rtc;
    struct adb *adb;
    struct scheduler *sched;
};

static void cuda_tick_event(void *source, uint64_t data);
static void cuda_autopoll_event(void *source, uint64_t data);
static void cuda_push_event(void *source, uint64_t data);

// === TREQ / SR helpers ======================================================

// Drive the TREQ (PB3) input level the host sees.  Active-low: high = idle.
static void cuda_set_treq(av_cuda_t *cuda, bool high) {
    cuda->treq_high = high;
    via_input(cuda->via1, 1, 3, high);
}

// Clock a byte into the host's SR after the firmware's ~25 us think time.
static void cuda_push_delayed(av_cuda_t *cuda, uint8_t byte) {
    cuda->push_pending = true;
    cuda->push_byte = byte;
    remove_event(cuda->sched, &cuda_push_event, cuda);
    scheduler_new_cpu_event(cuda->sched, &cuda_push_event, cuda, 0, 0, (uint64_t)CUDA_PUSH_DELAY_NS);
}

// Cancel a stale delayed push.  The host is allowed to consume an in-flight
// byte as the idle acknowledge and move straight on (the polled RdXByte
// path does); a delayed push landing inside the NEXT transaction would set
// the SR interrupt out of phase and garble the byte stream, so any new bus
// activity invalidates whatever is still pending.
static void cuda_cancel_push(av_cuda_t *cuda) {
    if (!cuda->push_pending)
        return;
    cuda->push_pending = false;
    remove_event(cuda->sched, &cuda_push_event, cuda);
}

static void cuda_push_event(void *source, uint64_t data) {
    (void)data;
    av_cuda_t *cuda = (av_cuda_t *)source;
    if (!cuda->push_pending)
        return;
    cuda->push_pending = false;
    via_input_sr(cuda->via1, cuda->push_byte);
}

// Push the tx_buf byte at `idx`, raising TREQ with the last byte (the
// host's per-byte "was this the last?" check reads TREQ after the byte).
static void cuda_push_byte(av_cuda_t *cuda, int idx) {
    bool last = (idx == cuda->tx_len - 1);
    cuda_set_treq(cuda, last);
    via_input_sr(cuda->via1, cuda->tx_buf[idx]);
}

// Advance to and push the next response byte, or ignore if none remain
// (the trailing idle acknowledge is produced on the TIP rising edge).
static void cuda_advance_tx(av_cuda_t *cuda) {
    if (cuda->tx_idx + 1 < cuda->tx_len) {
        cuda->tx_idx++;
        cuda_push_byte(cuda, cuda->tx_idx);
    }
}

// === Response construction ==================================================

// Begin sending a freshly-built response: assert TREQ and clock the
// attention byte; the host asserts TIP to accept.
static void cuda_begin_send(av_cuda_t *cuda) {
    cuda_cancel_push(cuda); // a stale idle-ack must not fire mid-response
    cuda->state = CUDA_SENDING;
    cuda->tx_idx = 0;
    cuda_push_byte(cuda, 0);
}

// Lay down the 4-byte response header [attn, pktType, flags, cmd].
static int cuda_put_header(av_cuda_t *cuda, uint8_t pkt_type, uint8_t flags, uint8_t cmd) {
    cuda->tx_buf[0] = 0x00; // attention byte (discarded by the host)
    cuda->tx_buf[1] = pkt_type;
    cuda->tx_buf[2] = flags;
    cuda->tx_buf[3] = cmd;
    return 4;
}

// Build the 5-byte error packet [attn, errorPkt, code, pktType, cmd].
static void cuda_send_error(av_cuda_t *cuda, uint8_t code, uint8_t pkt_type, uint8_t cmd) {
    cuda->tx_buf[0] = 0x00;
    cuda->tx_buf[1] = PKT_ERROR;
    cuda->tx_buf[2] = code;
    cuda->tx_buf[3] = pkt_type;
    cuda->tx_buf[4] = cmd;
    cuda->tx_len = 5;
    LOG(2, "error packet: code=%d for pkt=$%02X cmd=$%02X", code, pkt_type, cmd);
    cuda_begin_send(cuda);
}

// Process a completed ADB command packet (rx = [adbPkt, adbCmd, listen...]).
static void cuda_process_adb(av_cuda_t *cuda) {
    uint8_t cmd = (cuda->rx_len >= 2) ? cuda->rx_buf[1] : 0;
    uint8_t out[8];
    int out_len = 0;
    bool replied = false;
    if (cuda->adb)
        replied = adb_iop_transact(cuda->adb, cmd, &cuda->rx_buf[2], cuda->rx_len - 2, out, &out_len);

    uint8_t flags = replied ? 0 : CUDA_FLAG_TIMEOUT;
    int n = cuda_put_header(cuda, PKT_ADB, flags, cmd);
    for (int i = 0; i < out_len && n < CUDA_TX_MAX; i++)
        cuda->tx_buf[n++] = out[i];
    cuda->tx_len = n;
    LOG(3, "ADB cmd $%02X -> %d bytes%s", cmd, out_len, replied ? "" : " (no device)");
    cuda_begin_send(cuda);
}

// True if Cuda 2.37's dispatch rejects this pseudo-command.
static bool cuda_cmd_rejected(uint8_t cmd) {
    if (cmd > CUDA_MAX_PSEUDO)
        return true;
    for (size_t i = 0; i < sizeof(cuda_rejected_cmds); i++)
        if (cuda_rejected_cmds[i] == cmd)
            return true;
    return false;
}

// Process a completed pseudo-command packet (rx = [pseudoPkt, cmd, data...]).
static void cuda_process_pseudo(av_cuda_t *cuda) {
    uint8_t cmd = (cuda->rx_len >= 2) ? cuda->rx_buf[1] : CMD_NOP;
    const uint8_t *data = &cuda->rx_buf[2];
    int data_len = cuda->rx_len - 2;
    if (data_len < 0)
        data_len = 0;

    if (cuda_cmd_rejected(cmd)) {
        cuda_send_error(cuda, CUDA_ERR_INVPSEUDO, PKT_PSEUDO, cmd);
        return;
    }

    int n = cuda_put_header(cuda, PKT_PSEUDO, 0, cmd);

    switch (cmd) {
    case CMD_RDTIME: {
        uint32_t secs = cuda->rtc ? rtc_get_seconds(cuda->rtc) : 0;
        cuda->tx_buf[n++] = (uint8_t)(secs >> 24);
        cuda->tx_buf[n++] = (uint8_t)(secs >> 16);
        cuda->tx_buf[n++] = (uint8_t)(secs >> 8);
        cuda->tx_buf[n++] = (uint8_t)secs;
        break;
    }
    case CMD_WRTIME:
        if (data_len >= 4 && cuda->rtc) {
            uint32_t secs = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
            rtc_set_seconds(cuda->rtc, secs);
        }
        break;
    case CMD_RD6805: {
        // The HC05 address space; only the PRAM page ($100-$1FF) is backed.
        // Open-ended — the host terminates once it has enough bytes.
        uint16_t addr = (data_len >= 2) ? (uint16_t)((data[0] << 8) | data[1]) : 0;
        for (int i = 0; i < 256 && n < CUDA_TX_MAX; i++) {
            uint16_t a = (uint16_t)(addr + i);
            cuda->tx_buf[n++] = (a >= 0x100 && a <= 0x1FF && cuda->rtc) ? rtc_pram_read(cuda->rtc, (uint8_t)a) : 0;
        }
        break;
    }
    case CMD_RDPRAM: {
        // PRAM is one 256-byte page (firmware rejects a nonzero address
        // high byte with error 4).
        if (data_len >= 2 && data[0] != 0) {
            cuda_send_error(cuda, CUDA_ERR_PRAMADDR, PKT_PSEUDO, cmd);
            return;
        }
        uint16_t addr = (data_len >= 2) ? (uint16_t)((data[0] << 8) | data[1]) : 0;
        for (int i = 0; i < 256 && n < CUDA_TX_MAX; i++)
            cuda->tx_buf[n++] = cuda->rtc ? rtc_pram_read(cuda->rtc, (uint8_t)(addr + i)) : 0;
        break;
    }
    case CMD_WR6805: {
        if (data_len >= 2 && cuda->rtc) {
            uint16_t addr = (uint16_t)((data[0] << 8) | data[1]);
            for (int i = 2; i < data_len; i++) {
                uint16_t a = (uint16_t)(addr + (i - 2));
                if (a >= 0x100 && a <= 0x1FF)
                    rtc_pram_write(cuda->rtc, (uint8_t)a, data[i]);
            }
        }
        break;
    }
    case CMD_WRPRAM: {
        if (data_len >= 2 && cuda->rtc) {
            if (data[0] != 0) {
                cuda_send_error(cuda, CUDA_ERR_PRAMADDR, PKT_PSEUDO, cmd);
                return;
            }
            uint16_t addr = (uint16_t)((data[0] << 8) | data[1]);
            for (int i = 2; i < data_len; i++)
                rtc_pram_write(cuda->rtc, (uint8_t)(addr + (i - 2)), data[i]);
        }
        break;
    }
    case CMD_APOLL:
        cuda->autopoll_enabled = (data_len >= 1) ? (data[0] != 0) : true;
        LOG(2, "APoll -> autopoll %s", cuda->autopoll_enabled ? "on" : "off");
        break;
    case CMD_SETAUTOP:
        cuda->autopoll_enabled = true; // setting a rate implies autopoll
        break;
    case CMD_WR1SECMODE:
        cuda->onesec_enabled = (data_len >= 1) ? (data[0] != 0) : true;
        LOG(2, "Wr1SecMode -> 1-sec tick %s", cuda->onesec_enabled ? "on" : "off");
        break;
    case CMD_PWRDOWN:
        LOG(1, "Cuda PwrDown (accept-and-log; no soft power model)");
        break;
    case CMD_RESET:
        cuda->autopoll_enabled = false;
        cuda->onesec_enabled = false;
        break;
    case CMD_WRDFAC:
    case CMD_NOP:
    default:
        // Accepted commands with no modelled behavior (EnDisPDM, RdWrIIC,
        // …): header-only acknowledgement.
        break;
    }
    cuda->tx_len = n;
    cuda_begin_send(cuda);
}

// Dispatch a completed command packet by its packet-type byte.
static void cuda_process_command(av_cuda_t *cuda) {
    uint8_t pkt_type = (cuda->rx_len >= 1) ? cuda->rx_buf[0] : PKT_PSEUDO;
    LOG(4, "command pkt type=$%02X len=%d", pkt_type, cuda->rx_len);
    switch (pkt_type) {
    case PKT_ADB:
        cuda_process_adb(cuda);
        break;
    case PKT_PSEUDO:
        cuda_process_pseudo(cuda);
        break;
    default:
        // Unknown packet type: header-only ack so the host never hangs.
        cuda->tx_len = cuda_put_header(cuda, pkt_type, 0, (cuda->rx_len >= 2) ? cuda->rx_buf[1] : 0);
        cuda_begin_send(cuda);
        break;
    }
}

// === VIA1 transport hooks ===================================================

void av_cuda_via1_shift_input(av_cuda_t *cuda, uint8_t byte) {
    cuda_cancel_push(cuda); // the host moved on — drop any stale idle-ack
    switch (cuda->state) {
    case CUDA_SENDING:
        if (cuda->tx_idx == 0) {
            // Collision: the host wrote a command byte while our attention
            // byte was in flight.  The host's abort path clears the SR
            // interrupt and waits for it again — re-clock the attention.
            LOG(2, "collision with attention byte — re-clocking");
            cuda_push_delayed(cuda, cuda->tx_buf[0]);
        }
        return;
    case CUDA_IDLE:
        cuda->state = CUDA_RECEIVING;
        cuda->rx_len = 0;
        break;
    case CUDA_RECEIVING:
        break;
    case CUDA_SYNC:
        return; // no data during the sync cycle
    }
    if (cuda->rx_len < CUDA_RX_MAX)
        cuda->rx_buf[cuda->rx_len++] = byte;
}

void av_cuda_via1_pb_input(av_cuda_t *cuda, uint8_t port_b) {
    uint8_t old = cuda->last_pb;
    cuda->last_pb = port_b;

    bool tip_old = (old & PB_TIP) != 0;
    bool tip_new = (port_b & PB_TIP) != 0;
    bool ba_old = (old & PB_BYTEACK) != 0;
    bool ba_new = (port_b & PB_BYTEACK) != 0;

    bool tip_fall = tip_old && !tip_new; // host asserts TIP
    bool tip_rise = !tip_old && tip_new; // host terminates transaction
    bool ba_toggle = ba_old != ba_new;

    switch (cuda->state) {
    case CUDA_IDLE:
        // ByteAck asserted with TIP negated = the CudaInit sync state:
        // acknowledge with TREQ + a clocked byte, silencing all async
        // sources (that is the sync's documented purpose).
        if (tip_new && ba_old && !ba_new) {
            cuda->state = CUDA_SYNC;
            cuda->autopoll_enabled = false;
            cuda->onesec_enabled = false;
            cuda_set_treq(cuda, false);
            cuda_push_delayed(cuda, 0x00); // sync acknowledge byte
            LOG(2, "sync cycle: acknowledged");
        }
        break;

    case CUDA_RECEIVING:
        // TIP negation marks the end of the command packet.
        if (tip_rise)
            cuda_process_command(cuda);
        break;

    case CUDA_SENDING:
        if (tip_rise) {
            // Host terminated the response (normal end or open-ended cut):
            // release TREQ and clock the idle acknowledge byte.
            cuda->state = CUDA_IDLE;
            cuda_set_treq(cuda, true);
            cuda_push_delayed(cuda, 0x00);
        } else if (tip_fall && cuda->tx_idx == 0) {
            cuda_advance_tx(cuda); // host accepted the attention byte
        } else if (ba_toggle) {
            cuda_advance_tx(cuda); // host consumed a byte
        }
        break;

    case CUDA_SYNC:
        // ByteAck negated terminates the sync cycle: negate TREQ, then the
        // idle acknowledge arrives after Cuda's documented ~25 us delay
        // (the host clears the SR interrupt first — a synchronous push
        // would be eaten and CudaInit would reach DeadCuda).
        if (!ba_old && ba_new) {
            cuda_set_treq(cuda, true);
            cuda_push_delayed(cuda, 0x00);
            cuda->state = CUDA_IDLE;
            LOG(2, "sync cycle: terminated");
        }
        break;
    }
}

// === Autonomous tick + autopoll =============================================

static bool cuda_bus_idle(av_cuda_t *cuda) {
    // Only initiate unsolicited traffic when both we and the host are idle.
    return cuda->state == CUDA_IDLE && (cuda->last_pb & PB_TIP) != 0;
}

// 1-second tick: [attn, tickPkt] — drives the OS one-second timer.
static void cuda_tick_event(void *source, uint64_t data) {
    (void)data;
    av_cuda_t *cuda = (av_cuda_t *)source;
    if (cuda->onesec_enabled && cuda_bus_idle(cuda)) {
        cuda->tx_buf[0] = 0x00;
        cuda->tx_buf[1] = PKT_TICK;
        cuda->tx_len = 2;
        cuda_begin_send(cuda);
    }
    scheduler_new_cpu_event(cuda->sched, &cuda_tick_event, cuda, 0, 0, (uint64_t)CUDA_TICK_NS);
}

// ADB auto-poll: Talk-Reg-0 the active devices; deliver fresh data as an
// unsolicited adbPkt with the autopoll flag set.
static void cuda_autopoll_event(void *source, uint64_t data) {
    (void)data;
    av_cuda_t *cuda = (av_cuda_t *)source;
    if (cuda->autopoll_enabled && cuda->adb && cuda_bus_idle(cuda)) {
        static const uint8_t poll_addr[2] = {3, 2}; // mouse, keyboard
        for (int k = 0; k < 2; k++) {
            uint8_t addr = poll_addr[(cuda->autopoll_phase + k) & 1];
            uint8_t cmd = (uint8_t)((addr << 4) | 0x0C); // Talk register 0
            uint8_t out[8];
            int out_len = 0;
            if (adb_iop_transact(cuda->adb, cmd, NULL, 0, out, &out_len) && out_len > 0) {
                int n = cuda_put_header(cuda, PKT_ADB, CUDA_FLAG_AUTOPOLL, cmd);
                for (int i = 0; i < out_len && n < CUDA_TX_MAX; i++)
                    cuda->tx_buf[n++] = out[i];
                cuda->tx_len = n;
                cuda_begin_send(cuda);
                cuda->autopoll_phase ^= 1;
                break;
            }
        }
    }
    scheduler_new_cpu_event(cuda->sched, &cuda_autopoll_event, cuda, 0, 0, (uint64_t)CUDA_AUTOPOLL_NS);
}

// === Lifecycle ==============================================================

av_cuda_t *av_cuda_init(struct via *via1, struct rtc *rtc, struct adb *adb, struct scheduler *sched, checkpoint_t *cp) {
    av_cuda_t *cuda = (av_cuda_t *)calloc(1, sizeof(*cuda));
    if (!cuda)
        return NULL;
    cuda->via1 = via1;
    cuda->rtc = rtc;
    cuda->adb = adb;
    cuda->sched = sched;
    cuda->state = CUDA_IDLE;
    cuda->treq_high = true; // TREQ idles high (no request)
    cuda->last_pb = PB_TIP | PB_BYTEACK; // host idle state
    // Async sources start OFF: real Cuda sends nothing until the host's
    // sync + enable commands, and unsolicited SR interrupts during the
    // ROM's destructive RAM test would derail the CPU.
    cuda->autopoll_enabled = false;
    cuda->onesec_enabled = false;

    if (cp) {
        size_t data_size = offsetof(av_cuda_t, via1);
        system_read_checkpoint_data(cp, cuda, data_size);
    }

    if (cuda->via1)
        via_input(cuda->via1, 1, 3, cuda->treq_high);

    if (cuda->sched) {
        scheduler_new_event_type(cuda->sched, "cuda", cuda, "tick", &cuda_tick_event);
        scheduler_new_event_type(cuda->sched, "cuda", cuda, "autopoll", &cuda_autopoll_event);
        scheduler_new_event_type(cuda->sched, "cuda", cuda, "push", &cuda_push_event);
        scheduler_new_cpu_event(cuda->sched, &cuda_tick_event, cuda, 0, 0, (uint64_t)CUDA_TICK_NS);
        scheduler_new_cpu_event(cuda->sched, &cuda_autopoll_event, cuda, 0, 0, (uint64_t)CUDA_AUTOPOLL_NS);
        // A checkpoint taken with a push in flight re-arms it here.
        if (cuda->push_pending)
            scheduler_new_cpu_event(cuda->sched, &cuda_push_event, cuda, 0, 0, (uint64_t)CUDA_PUSH_DELAY_NS);
    }

    LOG(1, "Cuda init (firmware Cuda 2.37)");
    return cuda;
}

void av_cuda_delete(av_cuda_t *cuda) {
    if (!cuda)
        return;
    if (cuda->sched) {
        remove_event(cuda->sched, &cuda_tick_event, cuda);
        remove_event(cuda->sched, &cuda_autopoll_event, cuda);
        remove_event(cuda->sched, &cuda_push_event, cuda);
    }
    free(cuda);
}

void av_cuda_checkpoint(av_cuda_t *cuda, checkpoint_t *cp) {
    if (!cuda || !cp)
        return;
    size_t data_size = offsetof(av_cuda_t, via1);
    system_write_checkpoint_data(cp, cuda, data_size);
}

const char *av_cuda_firmware(const av_cuda_t *cuda) {
    (void)cuda;
    return "Cuda 2.37";
}
