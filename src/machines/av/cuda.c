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

#include "vdc.h"

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
#define CMD_RDWRIIC    0x22 // I2C master transaction (DMSD/VDC — video-in.md §2)
#define CMD_RESET      0x11 // cold reset
#define CMD_SETAUTOP   0x14 // set autopoll rate
#define CMD_RDDEVLIST  0x1A // read the ADB device list (16-bit address bitmap)
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
// How long after the host negates ByteAck at the end of a sync cycle Cuda
// takes to negate TREQ.  The firmware is a microcontroller reacting to a
// pin, not a combinational path: a host that polls TREQ the instant after
// raising ByteAck sees it still asserted.  AIX's cuda_sync leans on
// exactly that -- after raising ByteAck it waits for TREQ LOW, then for
// the idle-acknowledge byte -- and a model that negated TREQ inside the
// host's own store failed every sync, so cuda_reset gave up before
// enable_via_intr and the keyboard never interrupted.  The Macintosh
// CudaInit waits for TREQ HIGH here with a 10 ms timeout, so a few
// microseconds of latency is invisible to it.
#define CUDA_SYNC_TREQ_NS 8000.0
// How long after a RESET SYSTEM command the reset line is asserted.  A real
// Cuda takes milliseconds; what matters here is only that it is not zero,
// so the guest instruction that asked for it has retired first (see
// CMD_RESET).
#define CUDA_RESET_DELAY_NS 100000.0

// How long an unclaimed response waits before Cuda gives up on the host
// (state SENDING with the attention byte still untaken).  Bounded from
// both sides by real guests:
//   * AIX's Cuda transport on the Network Server sends a Talk R3 from its
//     keyboard configuration and comes back for the reply 5.4 ms later;
//     a 5 ms watchdog reaped it first, the driver synced three times to
//     recover and the keyboard never spoke.  So: longer than that.
//   * MkLinux DR3 on the TNT Macintoshes inherits the Booter's last
//     unread response at the hand-off; a watchdog long enough for that
//     response to outlive the hand-off (1 s certainly does) delivers it
//     to the kernel's driver, whose keyboard then never works
//     (mklinux-boot, pm7500).  So: shorter than the hand-off.
// 20 ms satisfies both.  (The OF-to-68k handoff on the TNT Macintosh boot,
// the case this watchdog was first written for, is also recognised from
// the SENDING state at the 68k's CudaInit sync.)
#define CUDA_SEND_ABANDON_NS 20000000.0

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
    bool push_last; // ...and it is the last byte of a response (TREQ negates with it)

    bool autopoll_enabled;
    bool onesec_enabled;
    uint8_t onesec_mode; // Wr1SecMode value; 3 = Mode3Clock (tick carries RTC)
    uint8_t autopoll_phase;
    // A response whose ATTENTION byte the host never takes is abandoned
    // after a firmware-style timeout (the TNT ROM's Open Firmware hands
    // off to the 68k with its last ADB response unread; the 68k's
    // CudaInit sync then needs an IDLE transport, exactly as real Cuda's
    // own transaction timeout provides).
    bool send_timeout_pending;
    // A response the host sync-aborted mid-flight — or one the send
    // watchdog above reaped unclaimed — is RE-PRESENTED once the bus
    // settles: the abort resets the TRANSPORT, not the firmware's output
    // queue.  Load-bearing on TNT — its ROM sends the
    // ADB SendReset through the early polled driver, then installs the
    // interrupt driver (VIA reinit + CudaInit sync) with the reply still
    // in flight; the ADB Manager's command stays outstanding forever
    // unless the reply re-arrives for the new driver.  A re-presented
    // response nobody reads is reaped by the abandonment watchdog above
    // (which is how the OF-era leftover stays harmless).
    bool resend_pending;
    // The packet on the wire is a timeout-reaped one being RE-PRESENTED to a
    // host that was merely busy.  If that host then runs a CudaInit sync
    // instead of taking it, it is a different driver generation (MkLinux's
    // Cuda driver syncing over Mac OS's leftover RdTime reply) and the
    // packet is stale: drop it at the sync rather than re-present it yet
    // again — the second re-presentation derailed MkLinux's init.
    bool tx_represented;
    // TREQ negation owed at the end of a sync cycle (see CUDA_SYNC_TREQ_NS).
    bool treq_release_pending;

    // --- pointers / callbacks (not checkpointed) ---
    struct via *via1;
    struct rtc *rtc;
    struct adb *adb;
    struct scheduler *sched;
    struct av_vdc *vdc; // I2C targets behind pseudo-command $22 (may be NULL)

    // Fixed machine property (config, not guest state): whether the one-second
    // tick may carry the RTC in the Mode3Clock RdTime form.  Enabled only for
    // the PDM family, whose live guest clock needs a real seed; the AV Quadras
    // keep the bare tick so their sound goldens stay on the wall-clock-agnostic
    // path they were captured on.
    bool mode3_clock;
};

static void cuda_tick_event(void *source, uint64_t data);
static void cuda_reset_event(void *source, uint64_t data);
static void cuda_autopoll_event(void *source, uint64_t data);
static void cuda_push_event(void *source, uint64_t data);
static void cuda_send_timeout_event(void *source, uint64_t data);
static void cuda_resend_event(void *source, uint64_t data);
static void cuda_treq_release_event(void *source, uint64_t data);

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
    cuda->push_last = false;
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
    if (cuda->push_last)
        cuda_set_treq(cuda, true); // the last response byte carries TREQ negated
    cuda->push_last = false;
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
    cuda->tx_represented = false; // a fresh presentation (the resend path re-flags)
    cuda->state = CUDA_SENDING;
    cuda->tx_idx = 0;
    LOG(3, "send %d bytes: type=$%02X flags=$%02X cmd=$%02X", cuda->tx_len, cuda->tx_buf[1], cuda->tx_buf[2],
        cuda->tx_buf[3]);
    cuda_push_byte(cuda, 0);
    // Give up on a host that never takes the attention byte (see
    // CUDA_SEND_ABANDON_NS); any transport progress cancels this.
    cuda->send_timeout_pending = true;
    remove_event(cuda->sched, &cuda_send_timeout_event, cuda);
    scheduler_new_cpu_event(cuda->sched, &cuda_send_timeout_event, cuda, 0, 0, (uint64_t)CUDA_SEND_ABANDON_NS);
}

// Cancel the abandonment watchdog: the host engaged with the response.
static void cuda_send_progress(av_cuda_t *cuda) {
    if (!cuda->send_timeout_pending)
        return;
    cuda->send_timeout_pending = false;
    remove_event(cuda->sched, &cuda_send_timeout_event, cuda);
}

// Re-present a sync-aborted response once the bus has settled: the sync
// reset the transport, but the firmware's output queue still holds the
// packet, so it re-raises TREQ and clocks the attention byte again.  A
// command the host started in the meantime supersedes the stale reply.
// The delay spans the host's driver-install window (the TNT ROM enables
// its SR interrupt ~5.7 ms after the sync; re-presenting inside that
// window would let the abandonment watchdog reap the reply again).
#define CUDA_RESEND_DELAY_NS 2000000.0

static void cuda_resend_event(void *source, uint64_t data) {
    (void)data;
    av_cuda_t *cuda = (av_cuda_t *)source;
    if (!cuda->resend_pending)
        return;
    cuda->resend_pending = false;
    if (cuda->state != CUDA_IDLE || cuda->tx_len < 4)
        return; // the host moved on (or a later sync flushed the queue)
    LOG(2, "re-presenting the parked response (%d bytes)", cuda->tx_len);
    cuda_begin_send(cuda);
    cuda->tx_represented = true;
    // No abandonment watchdog on a re-presented reply: the host is
    // mid-driver-install with interrupts masked (that is WHY the abort
    // happened), and the reply must survive until its unmask.  The next
    // sync still clears it if the host never engages.
    cuda_send_progress(cuda);
}

// End of a sync cycle: Cuda negates TREQ a few microseconds after the host
// negated ByteAck (the idle acknowledge byte follows separately).
static void cuda_treq_release_event(void *source, uint64_t data) {
    (void)data;
    av_cuda_t *cuda = (av_cuda_t *)source;
    if (!cuda->treq_release_pending)
        return;
    cuda->treq_release_pending = false;
    cuda_set_treq(cuda, true);
}

// The host never took the attention byte: return the transport to idle,
// as the firmware's own transaction timeout does.  The response itself is
// NOT thrown away (a tick aside): real Cuda simply keeps TREQ asserted
// until the host engages, and a host that is merely busy — Mac OS 8.1
// legitimately sits at IPL 1 for >100 ms spin-waiting on serial DMA — must
// still get its data.  An autopoll packet's ADB data was consumed from the
// device queue when the packet was built, so dropping it loses real input;
// worse, the attention byte already reached the VIA SR, and 8.1's CudaMgr
// folds that orphan byte into its next receive session, desynchronizing
// the ADB stream (the corrupted session count eventually overruns the ADB
// Manager's 12-byte stack buffer and crashes the machine).  So: reap the
// transport for the handoff case the timeout exists for, and re-present
// the packet once the bus settles, exactly like a sync-aborted reply.
static void cuda_send_timeout_event(void *source, uint64_t data) {
    (void)data;
    av_cuda_t *cuda = (av_cuda_t *)source;
    if (!cuda->send_timeout_pending)
        return;
    cuda->send_timeout_pending = false;
    if (cuda->state != CUDA_SENDING || cuda->tx_idx != 0)
        return;
    cuda->state = CUDA_IDLE;
    cuda_cancel_push(cuda);
    cuda_set_treq(cuda, true);
    if (cuda->tx_buf[1] == PKT_TICK) {
        LOG(2, "tick unclaimed by host — transport reset to idle, tick dropped");
        return;
    }
    LOG(2, "response unclaimed by host — transport reset to idle, parked for re-presentation");
    cuda->resend_pending = true;
    remove_event(cuda->sched, &cuda_resend_event, cuda);
    scheduler_new_cpu_event(cuda->sched, &cuda_resend_event, cuda, 0, 0, (uint64_t)CUDA_RESEND_DELAY_NS);
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
    LOG(4, "pseudo cmd=$%02X data_len=%d", cmd, data_len);

    if (cuda_cmd_rejected(cmd)) {
        cuda_send_error(cuda, CUDA_ERR_INVPSEUDO, PKT_PSEUDO, cmd);
        return;
    }

    int n = cuda_put_header(cuda, PKT_PSEUDO, 0, cmd);

    switch (cmd) {
    case CMD_RDTIME: {
        uint32_t secs = cuda->rtc ? rtc_get_seconds(cuda->rtc) : 0;
        LOG(2, "RdTime -> $%08X", secs);
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
        LOG(4, "Rd6805 addr=$%04X", addr);
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
            LOG(2, "RdPram rejected addr=$%02X%02X", data[0], data[1]);
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
    case CMD_RDDEVLIST: {
        // The firmware's autopoll list: one bit per ADB address that answered
        // during its bus scan, high byte first.  Mac OS never asks; AIX's
        // cfgcuda does, and defines its keyboard and mouse adapters from the
        // bits (address 2 -> cudaka0, address 3 -> cudama0) -- a zero list is
        // a machine without a keyboard, and its graphics console configures
        // without one.  Report the devices where they live now (see the
        // autopoll event for why the model is asked rather than shadowed).
        uint16_t list = 0;
        if (cuda->adb)
            list = (uint16_t)((1u << (adb_keyboard_address(cuda->adb) & 0xF)) |
                              (1u << (adb_mouse_address(cuda->adb) & 0xF)));
        cuda->tx_buf[n++] = (uint8_t)(list >> 8);
        cuda->tx_buf[n++] = (uint8_t)list;
        LOG(2, "RdDevList -> $%04X", list);
        break;
    }
    case CMD_WR1SECMODE:
        cuda->onesec_mode = (data_len >= 1) ? data[0] : 0;
        cuda->onesec_enabled = cuda->onesec_mode != 0;
        LOG(2, "Wr1SecMode -> mode %u (tick %s)", cuda->onesec_mode, cuda->onesec_enabled ? "on" : "off");
        break;
    case CMD_PWRDOWN:
        LOG(1, "Cuda PwrDown (accept-and-log; no soft power model)");
        break;
    case CMD_RESET:
        // RESET SYSTEM.  Cuda's own state goes back to power-on, and then
        // it ASSERTS THE SYSTEM RESET LINE — that is the whole point of the
        // command, and until this was here the model quietly reset only
        // itself.  Open Firmware's `reset-all` is the caller that made it
        // matter: on an Apple Network Server the Service-keyswitch install
        // path ends `cd RESETing to change Configuration!` and then waits
        // for the machine to come back, so a Cuda that accepted the command
        // and did nothing left the firmware spinning forever, reporting
        // `Can't reset-all` on its diagnostic port.  A Macintosh Restart
        // takes the same path.
        //
        // The reset is DEFERRED by one scheduler event rather than driven
        // from here: this runs inside the guest's own store to the VIA shift
        // register, and resetting the CPU mid-instruction would have the
        // dispatch loop advance the program counter afterwards, landing four
        // bytes past the reset vector.  A scheduler event fires between
        // instructions.  The delay is nominal — a real Cuda takes
        // milliseconds to pull the line — and nothing observes it.
        cuda->autopoll_enabled = false;
        cuda->onesec_enabled = false;
        cuda->onesec_mode = 0;
        LOG(1, "Cuda Reset: asserting the system reset line");
        remove_event(cuda->sched, &cuda_reset_event, cuda);
        scheduler_new_cpu_event(cuda->sched, &cuda_reset_event, cuda, 0, 0, (uint64_t)CUDA_RESET_DELAY_NS);
        break;
    case CMD_RDWRIIC: {
        // I2C master transaction (OS/CudaMgr.a SetTransferParams wire
        // format, video-in.md §2.3): data[0] = slave address, direction =
        // its bit 0 (even = write, odd = read); the remaining bytes are
        // what goes on the I2C wire — for these Philips parts the first is
        // the subaddress.  Reads append the data bytes to the header; the
        // host terminates when it has its count (open-ended, like RdPRAM).
        if (data_len < 1 || !cuda->vdc)
            break; // no slave byte / no bus: header-only acknowledgement
        uint8_t slave = data[0];
        if (!av_vdc_i2c_slave_known(slave)) {
            // Only the DMSD and VDC are on the bus; the real handler's
            // behavior for other addresses was never analysed (GAPS.md
            // §2.4) — reject loudly so a guest probing one is visible.
            LOG(1, "RdWrIIC to unknown I2C slave $%02X rejected", slave);
            cuda_send_error(cuda, CUDA_ERR_INVPSEUDO, PKT_PSEUDO, cmd);
            return;
        }
        if (slave & 1)
            n += av_vdc_i2c_read(cuda->vdc, slave, data_len >= 2, (data_len >= 2) ? data[1] : 0, &cuda->tx_buf[n],
                                 CUDA_TX_MAX - n);
        else
            av_vdc_i2c_write(cuda->vdc, slave, &data[1], data_len - 1);
        break;
    }
    case CMD_WRDFAC:
    case CMD_NOP:
    default:
        // Accepted commands with no modelled behavior (EnDisPDM, …):
        // header-only acknowledgement.
        break;
    }
    cuda->tx_len = n;
    cuda_begin_send(cuda);
}

// Dispatch a completed command packet by its packet-type byte.
static void cuda_process_command(av_cuda_t *cuda) {
    cuda->resend_pending = false; // a new command supersedes a stale reply
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
        } else {
            LOG(2, "host byte $%02X dropped mid-send (tx_idx=%d/%d)", byte, cuda->tx_idx, cuda->tx_len);
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

    // Abort/Sync: ByteAck asserted (falling) while TIP stays negated.  This is
    // recognised from ANY state, which is the whole point of it — the host-side
    // line-state table (CudaMgr.a:519-536, transcribed in the PDM notes
    // "cuda-adb.md" §3) lists TIP=1/ByteAck=0 as Abort/Sync for TREQ low *and*
    // TREQ high, i.e. whatever Cuda happens to be doing.  CudaInit's sync
    // explicitly expects to run while we are mid-transaction: its step 2 is "if
    // TREQ is already low, Cuda is mid-transaction: wait for its SR interrupt",
    // and only then does it assert ByteAck.
    //
    // Honouring it only from CUDA_IDLE deadlocks any host that syncs while an
    // unsolicited packet is in flight, because nothing ever releases TREQ.
    // Copland does exactly that: it enables autopoll, our keyboard has a
    // latched Caps Lock to report, the autopoll packet asserts TREQ, and its
    // CudaInit then hangs at step 5 ("raise ByteAck, wait TREQ high") forever.
    // The ROM never noticed because it syncs before enabling anything async.
    if (tip_new && ba_old && !ba_new) {
        LOG(2, "sync cycle: acknowledged (state=%d, aborting %d tx byte(s))", cuda->state,
            cuda->state == CUDA_SENDING ? cuda->tx_len - cuda->tx_idx : 0);
        cuda_cancel_push(cuda); // an in-flight byte must not land mid-sync
        // A SOLICITED response whose attention byte the host never took is
        // requeued: the sync resets the transport, not the output queue,
        // and it re-presents after the sync (see cuda_resend_event).  The
        // asynchronous sources (ticks, autopoll data) stay silenced — that
        // is half the point of the sync.
        if (cuda->state == CUDA_SENDING && cuda->tx_idx == 0 && cuda->tx_buf[1] != PKT_TICK &&
            !(cuda->tx_buf[2] & CUDA_FLAG_AUTOPOLL) && !cuda->tx_represented) {
            cuda->resend_pending = true; // tx_buf/tx_len kept for the resend
        } else {
            cuda->tx_len = 0;
            cuda->tx_idx = 0;
            cuda->resend_pending = false; // an idle-bus sync flushes the queue
        }
        cuda->rx_len = 0;
        cuda->state = CUDA_SYNC;
        // The sync resets the TRANSPORT.  It does NOT clear the autopoll
        // setting: AIX's kernel enables autopoll, then runs cuda_reset --
        // a sync cycle -- and never sends APoll again, and its keyboard
        // works on the real machine.  (The Macintosh ROM re-enables
        // autopoll explicitly after its CudaInit sync either way.)  The
        // one-second tick is still silenced, as CudaInit documents.
        cuda->onesec_enabled = false;
        cuda->onesec_mode = 0;
        cuda_set_treq(cuda, false);
        cuda_push_delayed(cuda, 0x00); // sync acknowledge byte
        return;
    }

    switch (cuda->state) {
    case CUDA_IDLE:
        break;

    case CUDA_RECEIVING:
        // TIP negation marks the end of the command packet.
        if (tip_rise)
            cuda_process_command(cuda);
        break;

    case CUDA_SENDING:
        if (tip_rise || tip_fall || ba_toggle)
            cuda_send_progress(cuda); // host engaged: cancel the watchdog
        if (tip_new && ba_old && !ba_new && ((via_get_acr(cuda->via1) >> 2) & 7) == 7) {
            // ByteAck asserted with TIP negated while the host's shift
            // register is in OUTPUT mode: that is CudaInit's sync cycle,
            // not a response-byte acknowledge — the host is not listening
            // (a reader flips the SR to input first).  Happens when a
            // response is abandoned across a driver handoff (the TNT
            // ROM's Open Firmware leaves its last ADB response untaken
            // and the 68k's CudaInit syncs into it).  Requeue a solicited
            // response (see the main sync branch) and run the sync
            // exactly as from idle.
            LOG(2, "sync cycle aborts an unread response");
            cuda_send_progress(cuda);
            if (cuda->tx_idx == 0 && cuda->tx_buf[1] != PKT_TICK && !(cuda->tx_buf[2] & CUDA_FLAG_AUTOPOLL) &&
                !cuda->tx_represented)
                cuda->resend_pending = true;
            cuda->state = CUDA_SYNC;
            cuda->onesec_enabled = false; // autopoll survives a sync (see above)
            cuda_set_treq(cuda, false);
            cuda_push_delayed(cuda, 0x00); // sync acknowledge byte
            break;
        }
        if (tip_rise) {
            // Host terminated the response (normal end or open-ended cut):
            // release TREQ and clock the idle acknowledge byte.
            cuda->state = CUDA_IDLE;
            cuda_set_treq(cuda, true);
            cuda_push_delayed(cuda, 0x00);
        } else if (tip_fall && cuda->tx_idx == 0) {
            // Host accepted the attention byte.  The first real byte
            // follows after Cuda's think time, NOT inside this store: AIX's
            // handler asserts TIP first and reads the attention byte from
            // the SR afterwards, and a byte clocked synchronously here
            // would overwrite it -- the handler then waits for a byte that
            // has already been and gone, with TIP held low, forever.
            if (cuda->tx_idx + 1 < cuda->tx_len) {
                cuda->tx_idx++;
                cuda_push_delayed(cuda, cuda->tx_buf[cuda->tx_idx]);
                cuda->push_last = (cuda->tx_idx == cuda->tx_len - 1);
            }
        } else if (ba_toggle) {
            if (cuda->tx_idx == cuda->tx_len - 1 && tip_new) {
                // Polled no-TIP read (the TNT ROM's early-boot driver):
                // the host clocks the whole response with ByteAck toggles
                // alone, TIP never asserted, so the final byte's
                // acknowledge IS the termination — go idle and clock the
                // idle acknowledge it then spin-waits on.  With TIP
                // asserted (every other host driver) the tip_rise branch
                // above stays the terminator.
                cuda->state = CUDA_IDLE;
                cuda_set_treq(cuda, true);
                cuda_push_delayed(cuda, 0x00);
            } else {
                cuda_advance_tx(cuda); // host consumed a byte
            }
        }
        break;

    case CUDA_SYNC:
        // ByteAck negated terminates the sync cycle: negate TREQ, then the
        // idle acknowledge arrives after Cuda's documented ~25 us delay
        // (the host clears the SR interrupt first — a synchronous push
        // would be eaten and CudaInit would reach DeadCuda).
        if (!ba_old && ba_new) {
            cuda->treq_release_pending = true;
            remove_event(cuda->sched, &cuda_treq_release_event, cuda);
            scheduler_new_cpu_event(cuda->sched, &cuda_treq_release_event, cuda, 0, 0, (uint64_t)CUDA_SYNC_TREQ_NS);
            cuda_push_delayed(cuda, 0x00);
            cuda->state = CUDA_IDLE;
            LOG(2, "sync cycle: terminated");
            if (cuda->resend_pending) {
                // A sync-aborted solicited response re-presents once the
                // bus settles (after the idle acknowledge above).
                remove_event(cuda->sched, &cuda_resend_event, cuda);
                scheduler_new_cpu_event(cuda->sched, &cuda_resend_event, cuda, 0, 0, (uint64_t)CUDA_RESEND_DELAY_NS);
            }
        }
        break;
    }
}

// === Autonomous tick + autopoll =============================================

static bool cuda_bus_idle(av_cuda_t *cuda) {
    // Only initiate unsolicited traffic when both we and the host are idle
    // AND the previous transaction's idle acknowledge has actually been
    // clocked out.  A pending push means CudaMgr's @waitIdleAck is still
    // spinning on that byte; cuda_begin_send() cancels it, so the host would
    // consume our attention byte as the acknowledge, finish the old
    // transaction and go idle — while we sit in CUDA_SENDING with TREQ
    // asserted, waiting for a TIP that never comes.  Both sides then wait
    // for each other forever.
    //
    // A response parked for re-presentation (reaped by the abandonment
    // watchdog or sync-aborted) is still the head of the firmware's output
    // queue: starting an autopoll or tick packet ahead of it would push a
    // second attention byte while the first still sits unread in the SR —
    // Mac OS 8.1's CudaMgr folds that orphan byte into its next receive
    // session and the desynchronised ADB stream overruns the ADB Manager's
    // stack buffer (the Finder dies mid AFP copy: the driver spins at IPL 1
    // on serial DMA for >100 ms, every autopoll gets reaped, and a moving
    // mouse supplies a fresh autopoll every 11 ms).
    return cuda->state == CUDA_IDLE && !cuda->push_pending && !cuda->resend_pending && (cuda->last_pb & PB_TIP) != 0;
}

// The deferred half of CMD_RESET (see there): assert the system reset line
// now that the guest instruction that asked for it has retired.
static void cuda_reset_event(void *source, uint64_t data) {
    (void)source;
    (void)data;
    system_hardware_reset();
}

// 1-second tick: [attn, tickPkt] — drives the OS one-second timer.
static void cuda_tick_event(void *source, uint64_t data) {
    (void)data;
    av_cuda_t *cuda = (av_cuda_t *)source;
    if (cuda->onesec_enabled && cuda_bus_idle(cuda)) {
        if (cuda->onesec_mode == 3 && cuda->mode3_clock) {
            // Mode3Clock: the tick is an RdTime response carrying the
            // 32-bit BE seconds, so the OS's CudaTickHandler seeds lowmem
            // Time from the real clock every second instead of merely
            // incrementing a counter that began at zero (cuda-adb.md §7 —
            // "always send the RdTime form; the handler accepts both").
            uint32_t secs = cuda->rtc ? rtc_get_seconds(cuda->rtc) : 0;
            int n = cuda_put_header(cuda, PKT_PSEUDO, 0, CMD_RDTIME);
            cuda->tx_buf[n++] = (uint8_t)(secs >> 24);
            cuda->tx_buf[n++] = (uint8_t)(secs >> 16);
            cuda->tx_buf[n++] = (uint8_t)(secs >> 8);
            cuda->tx_buf[n++] = (uint8_t)secs;
            cuda->tx_len = n;
            LOG(3, "tick pkt (Mode3Clock, secs=$%08X)", secs);
        } else {
            LOG(3, "tick pkt (bare)");
            cuda->tx_buf[0] = 0x00;
            cuda->tx_buf[1] = PKT_TICK;
            cuda->tx_len = 2;
        }
        cuda_begin_send(cuda);
    }
    scheduler_new_cpu_event(cuda->sched, &cuda_tick_event, cuda, 0, 0, (uint64_t)CUDA_TICK_NS);
}

// ADB auto-poll: Talk-Reg-0 the active devices; deliver fresh data as an
// unsolicited adbPkt with the autopoll flag set.
static void cuda_autopoll_event(void *source, uint64_t data) {
    (void)data;
    av_cuda_t *cuda = (av_cuda_t *)source;
    LOG(4, "autopoll gate: enabled=%d adb=%d state=%d push=%d pb=$%02X", cuda->autopoll_enabled, cuda->adb != NULL,
        cuda->state, cuda->push_pending, cuda->last_pb);
    if (cuda->autopoll_enabled && cuda->adb && cuda_bus_idle(cuda)) {
        // Poll the devices where they live NOW: an OS's ADB init can move
        // them off the default addresses via Listen R3 and leave them there
        // (Copland does; classic Mac OS moves them back), and real Cuda
        // firmware tracks the moves.  Asking the model beats shadowing the
        // Listen traffic.
        const uint8_t poll_addr[2] = {adb_mouse_address(cuda->adb), adb_keyboard_address(cuda->adb)};
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

av_cuda_t *av_cuda_init(struct via *via1, struct rtc *rtc, struct adb *adb, struct scheduler *sched, checkpoint_t *cp,
                        bool mode3_clock) {
    av_cuda_t *cuda = (av_cuda_t *)calloc(1, sizeof(*cuda));
    if (!cuda)
        return NULL;
    cuda->via1 = via1;
    cuda->rtc = rtc;
    cuda->adb = adb;
    cuda->sched = sched;
    cuda->mode3_clock = mode3_clock; // config; lives outside the checkpointed region
    cuda->state = CUDA_IDLE;
    cuda->treq_high = true; // TREQ idles high (no request)
    cuda->last_pb = PB_TIP | PB_BYTEACK; // host idle state
    // Async sources start OFF: real Cuda sends nothing until the host's
    // sync + enable commands, and unsolicited SR interrupts during the
    // ROM's destructive RAM test would derail the CPU.
    cuda->autopoll_enabled = false;
    cuda->onesec_enabled = false;
    cuda->onesec_mode = 0;

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
        scheduler_new_event_type(cuda->sched, "cuda", cuda, "sendto", &cuda_send_timeout_event);
        scheduler_new_event_type(cuda->sched, "cuda", cuda, "resend", &cuda_resend_event);
        scheduler_new_event_type(cuda->sched, "cuda", cuda, "reset", &cuda_reset_event);
        scheduler_new_event_type(cuda->sched, "cuda", cuda, "treqrel", &cuda_treq_release_event);
        scheduler_new_cpu_event(cuda->sched, &cuda_tick_event, cuda, 0, 0, (uint64_t)CUDA_TICK_NS);
        scheduler_new_cpu_event(cuda->sched, &cuda_autopoll_event, cuda, 0, 0, (uint64_t)CUDA_AUTOPOLL_NS);
        // A checkpoint taken with a push or abandonment watchdog in
        // flight re-arms it here.
        if (cuda->push_pending)
            scheduler_new_cpu_event(cuda->sched, &cuda_push_event, cuda, 0, 0, (uint64_t)CUDA_PUSH_DELAY_NS);
        if (cuda->send_timeout_pending)
            scheduler_new_cpu_event(cuda->sched, &cuda_send_timeout_event, cuda, 0, 0, (uint64_t)CUDA_SEND_ABANDON_NS);
        if (cuda->treq_release_pending)
            scheduler_new_cpu_event(cuda->sched, &cuda_treq_release_event, cuda, 0, 0, (uint64_t)CUDA_SYNC_TREQ_NS);
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

void av_cuda_attach_vdc(av_cuda_t *cuda, struct av_vdc *vdc) {
    if (cuda)
        cuda->vdc = vdc;
}
