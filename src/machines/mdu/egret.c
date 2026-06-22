// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// egret.c
// Functional model of the Apple "Egret" companion chip (Macintosh IIsi / LC).
// See egret.h for the public contract and OS/EgretMgr.a + OS/I2C/EgretEqu.h in
// the Apple source mirror for the authoritative host-side protocol.
//
// We do not emulate Egret's internal 65C02; we reproduce the byte-level wire
// protocol the host ROM/OS drives over VIA1's shift register plus the three
// port-B handshake pins.  The transfer is a simple byte pump:
//
//   Host -> Egret (a command packet): the host puts the VIA SR in output mode
//     (SRdir=1), asserts sysSes (PB5), and shifts out [pktType][cmd][data...].
//     Each shifted byte arrives via egret_via1_shift_input().  The host ends
//     the packet by negating sysSes (PB5 1->0); we take that falling edge as
//     "command complete", process it, and begin the response.
//
//   Egret -> Host (a response/unsolicited packet): we push bytes into the SR
//     with via_input_sr() (sets the host's ifSR) and drive xcvrSes (PB3,
//     active-LOW: held low while a packet is in flight, raised high on the
//     last byte).  The host paces us: after consuming a byte it pulses viaFull
//     (PB4) — the falling edge is our cue to push the next byte.  The very
//     first response byte ("attention") is pushed when the host signals it is
//     ready to receive (sysSes falling for a solicited response, or our own
//     initiation for an unsolicited tick/autopoll); the second byte follows the
//     host asserting sysSes (PB5 0->1), the rest on each viaFull falling edge.
//
// Response packet layout (RespHeader, EgretEqu.h): [attn][pktType][flags][cmd]
// then any data bytes.  The host discards the attention byte.

#include "egret.h"

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

LOG_USE_CATEGORY_NAME("egret");

// === VIA1 port-B handshake pins (EgretEqu.h) ================================

#define PB_XCVRSES (1u << 3) // PB3 xcvrSes — Egret session (host input, active-low)
#define PB_VIAFULL (1u << 4) // PB4 viaFull — host "byte serviced" (host output)
#define PB_SYSSES  (1u << 5) // PB5 sysSes  — host "transaction in progress" (host output)

// === Egret packet types (EgretEqu.h) =======================================

#define PKT_ADB    0x00 // adbPkt
#define PKT_PSEUDO 0x01 // pseudoPkt
#define PKT_ERROR  0x02 // errorPkt
#define PKT_TICK   0x03 // tickPkt — 1-second tick

// === Pseudo-command codes (EgretEqu.h) ======================================

#define CMD_NOP        0x00 // NopCmd
#define CMD_APOLL      0x01 // APoll       — start/stop ADB autopoll
#define CMD_RD6805     0x02 // Rd6805addr  — read 65C02 address space (open-ended)
#define CMD_RDTIME     0x03 // RdTime      — read RTC (4 data bytes)
#define CMD_RDPRAM     0x07 // RdPram      — read PRAM (open-ended)
#define CMD_WR6805     0x08 // Wr6805Addr  — write 65C02 address space
#define CMD_WRTIME     0x09 // WrTime      — write RTC (4 data bytes)
#define CMD_PWRDOWN    0x0A // PwrDown     — soft power off
#define CMD_WRPRAM     0x0C // WrPram      — write PRAM
#define CMD_WRDFAC     0x0E // WrDFAC      — write audio DFAC gain (accept-and-log)
#define CMD_EGRETDIAGS 0x0F // Egretdiags  — diagnostics (always succeed)
#define CMD_RESET      0x11 // ResetEgret  — cold reset
#define CMD_SETAUTOP   0x14 // SetAutopoll — set autopoll rate
#define CMD_WR1SECMODE 0x1B // Wr1SecMode  — 1-second-interrupt mode

// === ADB response status flags (EgretEqu.h) =================================

#define EG_FLAG_TIMEOUT  (1u << 1) // EgTimeOut  — addressed device had no data
#define EG_FLAG_AUTOPOLL (1u << 6) // EgAutoPoll — data came from an auto-poll

// Auto-poll cadence (~11 ms matches the real Egret / VIA-path poll).
#define EGRET_AUTOPOLL_NS 11000000.0
// 1-second tick cadence.
#define EGRET_TICK_NS 1000000000.0

// Transfer state.
typedef enum {
    EG_IDLE = 0, // bus idle, ready to receive a command or initiate a packet
    EG_RECEIVING, // host is shifting a command packet to us
    EG_SENDING, // we are shifting a response/unsolicited packet to the host
} eg_state_t;

#define EG_RX_MAX 24 // command bytes the host can send in one packet
#define EG_TX_MAX 264 // response bytes (4 header + up to 256 PRAM/ADB data)

struct egret {
    // --- plain data (checkpointed up to the first pointer field) ---
    eg_state_t state;

    uint8_t rx_buf[EG_RX_MAX]; // command bytes accumulated from the host
    int rx_len;

    uint8_t tx_buf[EG_TX_MAX]; // response bytes queued for the host
    int tx_len;
    int tx_idx; // index of the byte most recently pushed to the SR

    uint8_t last_pb; // last VIA1 port-B output seen (for edge detection)
    bool xcvr_high; // current level we drive on xcvrSes (PB3); true = idle/done

    bool autopoll_enabled; // ADB auto-poll active
    bool onesec_enabled; // 1-second tick active
    uint8_t autopoll_phase; // rotates the polled ADB address each tick

    // --- pointers / callbacks (not checkpointed) ---
    struct via *via1;
    struct rtc *rtc;
    struct adb *adb;
    struct scheduler *sched;
    void (*power_cb)(void *ctx);
    void *power_ctx;
};

// Forward declarations.
static void egret_tick_event(void *source, uint64_t data);
static void egret_autopoll_event(void *source, uint64_t data);

// === xcvrSes / SR helpers ===================================================

// Drive the xcvrSes (PB3) input level the host sees.  Active-low: high = idle.
static void egret_set_xcvr(egret_t *eg, bool high) {
    eg->xcvr_high = high;
    via_input(eg->via1, 1, 3, high); // port B, pin 3
}

// Push the tx_buf byte at `idx` into the host's SR, dropping xcvrSes low while
// bytes remain and raising it on the final byte (so the host's per-byte
// "is Egret done?" check terminates the packet).
static void egret_push_byte(egret_t *eg, int idx) {
    bool last = (idx == eg->tx_len - 1);
    egret_set_xcvr(eg, last); // high on the last byte, low otherwise
    via_input_sr(eg->via1, eg->tx_buf[idx]);
}

// Finish a send: return to idle with xcvrSes released high.
static void egret_finish_tx(egret_t *eg) {
    eg->state = EG_IDLE;
    egret_set_xcvr(eg, true);
}

// Advance to and push the next response byte, or finish if none remain.
static void egret_advance_tx(egret_t *eg) {
    eg->tx_idx++;
    if (eg->tx_idx < eg->tx_len)
        egret_push_byte(eg, eg->tx_idx);
    else
        egret_finish_tx(eg);
}

// === Response construction ==================================================

// Begin sending a freshly-built response (tx_buf/tx_len already populated):
// push the attention byte and enter the SENDING state.
static void egret_begin_send(egret_t *eg) {
    eg->state = EG_SENDING;
    eg->tx_idx = 0;
    egret_push_byte(eg, 0);
}

// Lay down the 4-byte response header [attn, pktType, flags, cmd].
static int egret_put_header(egret_t *eg, uint8_t pkt_type, uint8_t flags, uint8_t cmd) {
    eg->tx_buf[0] = 0x00; // attention byte (discarded by the host)
    eg->tx_buf[1] = pkt_type;
    eg->tx_buf[2] = flags;
    eg->tx_buf[3] = cmd;
    return 4;
}

// Process a completed ADB command packet (rx = [adbPkt, adbCmd, listen...]):
// run it against the ADB device state machine and build the reply packet.
static void egret_process_adb(egret_t *eg) {
    uint8_t cmd = (eg->rx_len >= 2) ? eg->rx_buf[1] : 0;
    uint8_t out[8];
    int out_len = 0;
    bool replied = false;
    if (eg->adb)
        replied = adb_iop_transact(eg->adb, cmd, &eg->rx_buf[2], eg->rx_len - 2, out, &out_len);

    uint8_t flags = replied ? 0 : EG_FLAG_TIMEOUT;
    int n = egret_put_header(eg, PKT_ADB, flags, cmd);
    for (int i = 0; i < out_len && n < EG_TX_MAX; i++)
        eg->tx_buf[n++] = out[i];
    eg->tx_len = n;
    LOG(3, "ADB cmd $%02X -> %d bytes%s", cmd, out_len, replied ? "" : " (no device)");
    egret_begin_send(eg);
}

// Process a completed pseudo-command packet (rx = [pseudoPkt, cmd, data...]).
static void egret_process_pseudo(egret_t *eg) {
    uint8_t cmd = (eg->rx_len >= 2) ? eg->rx_buf[1] : CMD_NOP;
    const uint8_t *data = &eg->rx_buf[2];
    int data_len = eg->rx_len - 2;
    if (data_len < 0)
        data_len = 0;

    int n = egret_put_header(eg, PKT_PSEUDO, 0, cmd);

    switch (cmd) {
    case CMD_RDTIME: {
        uint32_t secs = eg->rtc ? rtc_get_seconds(eg->rtc) : 0;
        eg->tx_buf[n++] = (uint8_t)(secs >> 24);
        eg->tx_buf[n++] = (uint8_t)(secs >> 16);
        eg->tx_buf[n++] = (uint8_t)(secs >> 8);
        eg->tx_buf[n++] = (uint8_t)secs;
        break;
    }
    case CMD_WRTIME:
        if (data_len >= 4 && eg->rtc) {
            uint32_t secs = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
            rtc_set_seconds(eg->rtc, secs);
        }
        break;
    case CMD_RD6805: {
        // Rd6805addr addr.W: read the Egret companion's 65C02 address space.
        // Erickson/Elsie (IIsi/LC) keep the 256 bytes of parameter RAM inside
        // the 65C02 at addresses $100-$1FF rather than in a separate RTC chip,
        // so the ROM's _ReadXPRam issues Read6805 (not ReadPRAM): a read of
        // $1xx returns PRAM byte $xx.  Open-ended, like RdPram — the host stops
        // us via the sysSes handshake once it has enough bytes.
        uint16_t addr = (data_len >= 2) ? (uint16_t)((data[0] << 8) | data[1]) : 0;
        for (int i = 0; i < 256 && n < EG_TX_MAX; i++) {
            uint16_t a = (uint16_t)(addr + i);
            eg->tx_buf[n++] = (a >= 0x100 && a <= 0x1FF && eg->rtc) ? rtc_pram_read(eg->rtc, (uint8_t)a) : 0;
        }
        break;
    }
    case CMD_RDPRAM: {
        // RdPram addr.W: stream PRAM bytes from `addr`; the host stops us via
        // the open-ended handshake (sysSes falling) once it has enough.
        uint16_t addr = (data_len >= 2) ? (uint16_t)((data[0] << 8) | data[1]) : 0;
        for (int i = 0; i < 256 && n < EG_TX_MAX; i++)
            eg->tx_buf[n++] = eg->rtc ? rtc_pram_read(eg->rtc, (uint8_t)(addr + i)) : 0;
        break;
    }
    case CMD_WR6805: {
        // Wr6805Addr addr.W data...: write the 65C02 address space; only the
        // PRAM window ($100-$1FF) is backed by storage (see CMD_RD6805).
        if (data_len >= 2 && eg->rtc) {
            uint16_t addr = (uint16_t)((data[0] << 8) | data[1]);
            for (int i = 2; i < data_len; i++) {
                uint16_t a = (uint16_t)(addr + (i - 2));
                if (a >= 0x100 && a <= 0x1FF)
                    rtc_pram_write(eg->rtc, (uint8_t)a, data[i]);
            }
        }
        break;
    }
    case CMD_WRPRAM: {
        // WrPram addr.W data...: write the supplied bytes into PRAM.
        if (data_len >= 2 && eg->rtc) {
            uint16_t addr = (uint16_t)((data[0] << 8) | data[1]);
            for (int i = 2; i < data_len; i++)
                rtc_pram_write(eg->rtc, (uint8_t)(addr + (i - 2)), data[i]);
        }
        break;
    }
    case CMD_APOLL:
        eg->autopoll_enabled = (data_len >= 1) ? (data[0] != 0) : true;
        LOG(2, "APoll -> autopoll %s", eg->autopoll_enabled ? "on" : "off");
        break;
    case CMD_SETAUTOP:
        // Setting an autopoll rate implies autopoll is wanted.
        eg->autopoll_enabled = true;
        break;
    case CMD_WR1SECMODE:
        eg->onesec_enabled = (data_len >= 1) ? (data[0] != 0) : true;
        LOG(2, "Wr1SecMode -> 1-sec tick %s", eg->onesec_enabled ? "on" : "off");
        break;
    case CMD_PWRDOWN:
        LOG(1, "Egret PwrDown");
        if (eg->power_cb)
            eg->power_cb(eg->power_ctx);
        break;
    case CMD_EGRETDIAGS:
        eg->tx_buf[n++] = 0x00; // diagnostics: success
        eg->tx_buf[n++] = 0x00;
        break;
    case CMD_RESET:
        eg->autopoll_enabled = false;
        eg->onesec_enabled = false;
        break;
    case CMD_WRDFAC:
    case CMD_NOP:
    default:
        // Accept-and-log: header-only acknowledgement.
        break;
    }
    eg->tx_len = n;
    egret_begin_send(eg);
}

// Dispatch a completed command packet by its packet-type byte.
static void egret_process_command(egret_t *eg) {
    uint8_t pkt_type = (eg->rx_len >= 1) ? eg->rx_buf[0] : PKT_PSEUDO;
    LOG(4, "command pkt type=$%02X len=%d", pkt_type, eg->rx_len);
    switch (pkt_type) {
    case PKT_ADB:
        egret_process_adb(eg);
        break;
    case PKT_PSEUDO:
        egret_process_pseudo(eg);
        break;
    default:
        // Unknown packet type: acknowledge with a header-only pseudo response
        // so the host's reader does not hang.
        eg->tx_len = egret_put_header(eg, PKT_PSEUDO, 0, (eg->rx_len >= 2) ? eg->rx_buf[1] : 0);
        egret_begin_send(eg);
        break;
    }
}

// === VIA1 transport hooks ===================================================

void egret_via1_shift_input(egret_t *eg, uint8_t byte) {
    if (eg->state != EG_RECEIVING) {
        // A shift-out while we believe the bus is idle means the host has begun
        // a command without our seeing the sysSes edge yet (early boot, before
        // DDRB is configured); start receiving from this byte.
        eg->state = EG_RECEIVING;
        eg->rx_len = 0;
    }
    if (eg->rx_len < EG_RX_MAX)
        eg->rx_buf[eg->rx_len++] = byte;
}

void egret_via1_pb_input(egret_t *eg, uint8_t port_b) {
    uint8_t old = eg->last_pb;
    eg->last_pb = port_b;

    bool sys_old = (old & PB_SYSSES) != 0;
    bool sys_new = (port_b & PB_SYSSES) != 0;
    bool full_old = (old & PB_VIAFULL) != 0;
    bool full_new = (port_b & PB_VIAFULL) != 0;

    bool sys_rise = !sys_old && sys_new;
    bool sys_fall = sys_old && !sys_new;
    bool full_fall = full_old && !full_new;

    switch (eg->state) {
    case EG_IDLE:
        if (sys_rise) {
            // Host is starting a command packet.
            eg->state = EG_RECEIVING;
            eg->rx_len = 0;
        }
        break;

    case EG_RECEIVING:
        // The host negates sysSes to mark the end of the command packet.  This
        // may coincide with a viaFull falling edge (the host clears both bits
        // in one write); the command-complete action takes precedence.
        if (sys_fall)
            egret_process_command(eg);
        break;

    case EG_SENDING:
        if (sys_fall) {
            // Host terminated early (open-ended read) or finished the packet.
            egret_finish_tx(eg);
        } else if (sys_rise || full_fall) {
            // Host consumed a byte and is ready for the next one.
            egret_advance_tx(eg);
        }
        break;
    }
}

// === Autonomous tick + autopoll =============================================

// Send an unsolicited packet (tick/autopoll) iff the bus is idle.  Returns
// false if Egret is mid-transfer and the caller should retry next cycle.
static bool egret_try_unsolicited(egret_t *eg) {
    return eg->state == EG_IDLE;
}

// 1-second tick: deliver a 2-byte [attn, tickPkt] packet that drives the OS
// one-second timer (the job VIA1 CA2 does on classic-ADB machines).
static void egret_tick_event(void *source, uint64_t data) {
    (void)data;
    egret_t *eg = (egret_t *)source;
    if (eg->onesec_enabled && egret_try_unsolicited(eg)) {
        eg->tx_buf[0] = 0x00; // attention
        eg->tx_buf[1] = PKT_TICK;
        eg->tx_len = 2;
        egret_begin_send(eg);
    }
    scheduler_new_cpu_event(eg->sched, &egret_tick_event, eg, 0, 0, (uint64_t)EGRET_TICK_NS);
}

// Force a tick now (test/object-model helper).
void egret_force_tick(egret_t *eg) {
    if (egret_try_unsolicited(eg)) {
        eg->tx_buf[0] = 0x00;
        eg->tx_buf[1] = PKT_TICK;
        eg->tx_len = 2;
        egret_begin_send(eg);
    }
}

// ADB auto-poll: Talk-Reg-0 the active ADB devices; when one has fresh data
// (mouse motion/button, keystroke) deliver it as an unsolicited adbPkt with the
// EgAutoPoll flag set.  Devices drain their reply buffer after a Talk, so an
// idle bus produces no packets.
static void egret_autopoll_event(void *source, uint64_t data) {
    (void)data;
    egret_t *eg = (egret_t *)source;
    if (eg->autopoll_enabled && eg->adb && egret_try_unsolicited(eg)) {
        // Rotate across the standard relocated addresses: 3 = mouse, 2 = kbd.
        static const uint8_t poll_addr[2] = {3, 2};
        for (int k = 0; k < 2; k++) {
            uint8_t addr = poll_addr[(eg->autopoll_phase + k) & 1];
            uint8_t cmd = (uint8_t)((addr << 4) | 0x0C); // Talk register 0
            uint8_t out[8];
            int out_len = 0;
            if (adb_iop_transact(eg->adb, cmd, NULL, 0, out, &out_len) && out_len > 0) {
                int n = egret_put_header(eg, PKT_ADB, EG_FLAG_AUTOPOLL, cmd);
                for (int i = 0; i < out_len && n < EG_TX_MAX; i++)
                    eg->tx_buf[n++] = out[i];
                eg->tx_len = n;
                egret_begin_send(eg);
                eg->autopoll_phase ^= 1; // give the other device priority next time
                break;
            }
        }
    }
    scheduler_new_cpu_event(eg->sched, &egret_autopoll_event, eg, 0, 0, (uint64_t)EGRET_AUTOPOLL_NS);
}

// === Lifecycle ==============================================================

egret_t *egret_init(struct via *via1, struct rtc *rtc, struct adb *adb, struct scheduler *sched, checkpoint_t *cp) {
    egret_t *eg = (egret_t *)calloc(1, sizeof(*eg));
    if (!eg)
        return NULL;
    eg->via1 = via1;
    eg->rtc = rtc;
    eg->adb = adb;
    eg->sched = sched;
    eg->state = EG_IDLE;
    eg->xcvr_high = true; // xcvrSes idles high (no session)
    // Auto-poll and the 1-second tick both start OFF: a real Egret does not
    // generate unsolicited traffic until the host's ADB manager / 1-sec setup
    // enables it (via the APoll / Wr1SecMode pseudo-commands).  Generating SR
    // interrupts before the OS has installed its interrupt vectors — e.g.
    // during the ROM's destructive RAM test, which transiently XORs the vector
    // table — would derail the CPU through a corrupted Level-1 vector.
    eg->autopoll_enabled = false;

    if (cp) {
        size_t data_size = offsetof(egret_t, via1);
        system_read_checkpoint_data(cp, eg, data_size);
    }

    // Drive the idle xcvrSes level onto PB3 and prime the port-B shadow.
    if (eg->via1)
        via_input(eg->via1, 1, 3, eg->xcvr_high);

    // Register checkpointable event types, then arm the tick + autopoll timers.
    if (eg->sched) {
        scheduler_new_event_type(eg->sched, "egret", eg, "tick", &egret_tick_event);
        scheduler_new_event_type(eg->sched, "egret", eg, "autopoll", &egret_autopoll_event);
        scheduler_new_cpu_event(eg->sched, &egret_tick_event, eg, 0, 0, (uint64_t)EGRET_TICK_NS);
        scheduler_new_cpu_event(eg->sched, &egret_autopoll_event, eg, 0, 0, (uint64_t)EGRET_AUTOPOLL_NS);
    }

    LOG(1, "Egret init (firmware Egret8)");
    return eg;
}

void egret_delete(egret_t *eg) {
    if (!eg)
        return;
    if (eg->sched) {
        remove_event(eg->sched, &egret_tick_event, eg);
        remove_event(eg->sched, &egret_autopoll_event, eg);
    }
    free(eg);
}

void egret_checkpoint(egret_t *eg, checkpoint_t *cp) {
    if (!eg || !cp)
        return;
    size_t data_size = offsetof(egret_t, via1);
    system_write_checkpoint_data(cp, eg, data_size);
}

// === Configuration / object-model helpers ===================================

void egret_set_power_off_callback(egret_t *eg, void (*cb)(void *ctx), void *ctx) {
    eg->power_cb = cb;
    eg->power_ctx = ctx;
}

const char *egret_firmware(const egret_t *eg) {
    (void)eg;
    return "Egret8";
}
