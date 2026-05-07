// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// adb.c
// Implements ADB (Apple Desktop Bus) transceiver emulation for the SE/30.
//
// The ADB controller interfaces with VIA1 in two ways:
//   - shift_cb: fired when the OS completes shifting a byte out (command or Listen data).
//   - output_cb: fired when the OS changes port B (ST0/ST1 state lines on bits 5:4).
//
// Keyboard (address 2) and mouse (address 3) are emulated as built-in ADB devices.
// The transceiver communicates back to the CPU exclusively through via_input_sr()
// (to deliver reply bytes) and via_input() (to control the vADBInt line on port B bit 3).

// ============================================================================
// Includes
// ============================================================================

#include "adb.h"
#include "debug_mac.h"
#include "keyboard.h"
#include "log.h"
#include "object.h"
#include "system.h"
#include "value.h"

#include <stdio.h>

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

LOG_USE_CATEGORY_NAME("adb");

// Delay (nanoseconds) before signalling shift-register completion after the OS
// writes a command or Listen byte.  On real hardware the ADB transceiver clocks
// 8 bits at ~38 µs each (~300 µs total).  We use a generous 800 µs to match the
// approximate ADB attention-plus-command duration, ensuring the ROM's VBL handler
// (which enables nested interrupts) has time to finish setting up ADB state
// machine callback pointers before the completion interrupt fires (BUG-003).
#define ADB_SHIFT_DELAY (800 * 1000)

// Delay (nanoseconds) before delivering the next reply byte via the VIA shift
// register.  On real hardware the ADB transceiver completes a full command–
// response cycle in ~3 ms (800 µs attention + 800 µs command + stop-to-start
// time + 800 µs response data).  We use 2.64 ms to match the deferred-delivery
// timing in keyboard.c (RX_TO_TX_DELAY = 330 * 8 * 1000 ns).  The delay must
// be long enough for the ROM's VBL handler (which enables interrupts mid-flight)
// to finish setting up the ADB state machine callback pointers before the next
// SR interrupt fires.
#define ADB_BYTE_DELAY (330 * 8 * 1000)

// Auto-poll interval (nanoseconds).  The real ADB transceiver repeats the last
// Talk R0 command approximately every 11 ms while in idle (state 3).  We use
// 11 ms to match real hardware timing.
#define ADB_AUTOPOLL_INTERVAL (11 * 1000 * 1000)

// Default ADB device addresses assigned at power-on
#define KBD_DEFAULT_ADDR   2
#define MOUSE_DEFAULT_ADDR 3

// Standard handler IDs (single-button mouse and extended keyboard both use 0x01)
#define KBD_HANDLER_ID   0x01
#define MOUSE_HANDLER_ID 0x01

// ADB command byte bit-field masks
#define CMD_ADDR_MASK 0xF0 // bits 7-4: target device address
#define CMD_TYPE_MASK 0x0C // bits 3-2: command type
#define CMD_REG_MASK  0x03 // bits 1-0: register number

// ADB command type codes (bits 3-2 of command byte)
#define CMD_TYPE_SENDRESET 0x00
#define CMD_TYPE_FLUSH     0x01
#define CMD_TYPE_LISTEN    0x02
#define CMD_TYPE_TALK      0x03

// VIA1 port B bit assignments for ADB signalling
#define ADB_INT_PIN 3 // vADBInt: active-low SRQ/end-of-transfer (bit 3)
#define ADB_ST0_BIT 4 // ST0: ADB state bit 0 (bit 4)
#define ADB_ST1_BIT 5 // ST1: ADB state bit 1 (bit 5)

// ADB transaction state values (ST1:ST0)
#define ADB_STATE_CMD  0 // Command phase: OS writes command byte to SR
#define ADB_STATE_EVEN 1 // Even data byte
#define ADB_STATE_ODD  2 // Odd data byte
#define ADB_STATE_IDLE 3 // Idle; transceiver may auto-poll every ~11 ms

// Keyboard event queue size (ring buffer capacity)
#define KBD_QUEUE_SIZE 128

// Keyboard idle response: no key event
#define KBD_NO_KEY 0xFF

// Mouse idle response bytes: no movement, button up
#define MOUSE_IDLE_B1 0x80 // bit 7 = 1 (button up), bits 6-0 = Y delta = 0
#define MOUSE_IDLE_B2 0x80 // bit 7 = 1 (reserved), bits 6-0 = X delta = 0

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// Tracks the current ADB address and handler ID for a single device
typedef struct {
    uint8_t address;
    uint8_t handler;
} adb_device_t;

// Full ADB transceiver state; plain data placed first so the checkpoint
// boundary at offsetof(adb_t, via) works correctly (same pattern as keyboard.c)
struct adb {
    // === Plain data (checkpointed via memcpy up to the 'via' pointer) ===

    // Current VIA transaction state (0-3), extracted from port B bits 5:4
    int state;

    // Reply buffer for the current Talk command
    uint8_t reply_buf[2]; // bytes to deliver to the OS
    int reply_len; // valid bytes in reply_buf (0 = no device)
    int reply_index; // index of next byte to deliver

    // Listen accumulation: the OS shifts out data bytes for a Listen command
    bool listen_active; // true while expecting Listen data via shift_cb
    uint8_t listen_buf[2]; // accumulated data bytes from the OS
    int listen_index; // how many Listen data bytes have been received
    uint8_t listen_addr; // target device address for the current Listen
    uint8_t listen_reg; // target register for the current Listen

    // Keyboard event queue (ring buffer of ADB Register 0 bytes)
    struct {
        unsigned int head; // next free slot index
        unsigned int tail; // next byte to dequeue index
        uint8_t buf[KBD_QUEUE_SIZE];
    } kbd_queue;

    // Tracks which ADB keys are currently held to suppress auto-repeat
    bool kbd_pressed[128];

    // Mouse state: deltas accumulated since last Talk R0; reset after each report
    int mouse_dx;
    int mouse_dy;
    bool mouse_button; // current button state (true = pressed)
    bool mouse_data_pending; // set by adb_mouse_event, cleared after auto-poll delivery

    // Device register 3 state (address + handler ID) for keyboard and mouse
    adb_device_t kbd;
    adb_device_t mouse;

    // Set after the end-of-transfer dummy byte is delivered; cleared when
    // a new command is decoded.  Prevents the EVEN/ODD handler from scheduling
    // spurious deliveries during the ROM's data replay phase (BUG-006d).
    bool dummy_sent;

    // Address of the last Talk R0 target device; used by the auto-poll mechanism
    // to repeat the last command while in idle state, matching the real ADB
    // transceiver's behaviour.
    uint8_t last_poll_addr;

    // === Pointers last (not checkpointed) ===
    via_t *via;
    struct scheduler *scheduler;
};

// ============================================================================
// Forward Declarations
// ============================================================================

static void adb_reset(adb_t *adb);
static void adb_deliver_next_byte(adb_t *adb);
static void adb_deliver_next_byte_deferred(void *source, uint64_t data);
static void adb_shift_complete_deferred(void *source, uint64_t data);
static void adb_autopoll_deferred(void *source, uint64_t data);
static void adb_decode_command(adb_t *adb, uint8_t cmd);

// ============================================================================
// Static Helpers
// ============================================================================

// Enqueues one byte into the keyboard ring buffer; drops the oldest on overflow
static void kbd_enqueue(adb_t *adb, uint8_t byte) {
    unsigned int head = adb->kbd_queue.head + 1;
    if (head == KBD_QUEUE_SIZE)
        head = 0;
    if (head == adb->kbd_queue.tail) {
        // Queue full: drop oldest entry to make room for the new key event
        LOG(1, "kbd_queue overflow, dropping oldest byte");
        adb->kbd_queue.tail++;
        if (adb->kbd_queue.tail == KBD_QUEUE_SIZE)
            adb->kbd_queue.tail = 0;
    }
    adb->kbd_queue.buf[adb->kbd_queue.head] = byte;
    adb->kbd_queue.head = head;
}

// Dequeues one byte from the keyboard ring buffer; returns KBD_NO_KEY ($FF)
// when empty.  No assert: a guest that polls the keyboard register without
// first checking host availability would otherwise crash the emulator (same
// assert-on-guest-action class as ef003fe in scc.c).
static uint8_t kbd_dequeue(adb_t *adb) {
    if (adb->kbd_queue.tail == adb->kbd_queue.head)
        return KBD_NO_KEY;
    uint8_t byte = adb->kbd_queue.buf[adb->kbd_queue.tail];
    adb->kbd_queue.tail++;
    if (adb->kbd_queue.tail == KBD_QUEUE_SIZE)
        adb->kbd_queue.tail = 0;
    return byte;
}

// Returns true if the keyboard queue is empty
static bool kbd_queue_empty(const adb_t *adb) {
    return adb->kbd_queue.head == adb->kbd_queue.tail;
}

// Clears the keyboard queue (does not reset pressed[] — call adb_reset for that)
static void kbd_queue_reset(adb_t *adb) {
    adb->kbd_queue.head = adb->kbd_queue.tail = 0;
}

// Clamps a mouse axis delta to ADB's 7-bit signed range (-64..+63) and returns
// the clamped value. *remaining is set to the leftover delta not yet reported.
static int clamp_delta(int delta, int *remaining) {
    int clamped = delta;
    if (clamped > 63)
        clamped = 63;
    if (clamped < -64)
        clamped = -64;
    *remaining = delta - clamped;
    return clamped;
}

// Encodes a clamped delta into ADB's 7-bit signed format (2's complement)
static uint8_t encode_delta(int clamped) {
    return (uint8_t)(clamped & 0x7F);
}

// Drives the vADBInt line on VIA1 port B bit 3: high = idle/continue, low = SRQ/done
static void set_adb_int(adb_t *adb, bool high) {
    via_input(adb->via, 1, ADB_INT_PIN, high);
}

// Extracts the ADB state (ST1:ST0) from a VIA1 port B output value
static int extract_state(uint8_t port_b_val) {
    // ST0 is bit 4, ST1 is bit 5; together they form a 2-bit state index
    return (port_b_val >> ADB_ST0_BIT) & 0x03;
}

// Returns true if there is mouse or keyboard data worth reporting via auto-poll.
// Mouse button held down counts as pending (the Mac needs to see it every poll).
static bool has_pending_data(const adb_t *adb) {
    return !kbd_queue_empty(adb) || adb->mouse_data_pending || adb->mouse_button;
}

// Returns true if the device at the given ADB address has unreported data.
// On real hardware, a device with no pending data simply doesn't respond to
// Talk R0 — the transceiver sees a timeout and stays quiet.
static bool device_has_pending_data(const adb_t *adb, uint8_t addr) {
    if (addr == adb->kbd.address)
        return !kbd_queue_empty(adb);
    if (addr == adb->mouse.address)
        return adb->mouse_data_pending || adb->mouse_button;
    return false;
}

// Resets all ADB devices to power-on defaults and clears all data queues
static void adb_reset(adb_t *adb) {
    LOG(2, "adb_reset: resetting all devices to defaults");

    adb->kbd.address = KBD_DEFAULT_ADDR;
    adb->kbd.handler = KBD_HANDLER_ID;
    adb->mouse.address = MOUSE_DEFAULT_ADDR;
    adb->mouse.handler = MOUSE_HANDLER_ID;

    kbd_queue_reset(adb);
    memset(adb->kbd_pressed, 0, sizeof(adb->kbd_pressed));

    adb->mouse_dx = 0;
    adb->mouse_dy = 0;
    adb->mouse_button = false;
    adb->mouse_data_pending = false;
    adb->last_poll_addr = MOUSE_DEFAULT_ADDR;

    adb->listen_active = false;
    adb->listen_index = 0;
    adb->reply_len = 0;
    adb->reply_index = 0;
}

// Returns the device record for the given ADB address, or NULL if unknown
static adb_device_t *find_device(adb_t *adb, uint8_t addr) {
    if (addr == adb->kbd.address)
        return &adb->kbd;
    if (addr == adb->mouse.address)
        return &adb->mouse;
    return NULL;
}

// Flushes the data buffer for the device at the given ADB address
static void flush_device(adb_t *adb, uint8_t addr) {
    if (addr == adb->kbd.address) {
        LOG(2, "flush_device: flushing keyboard at addr %d", addr);
        kbd_queue_reset(adb);
    } else if (addr == adb->mouse.address) {
        LOG(2, "flush_device: flushing mouse at addr %d", addr);
        adb->mouse_dx = 0;
        adb->mouse_dy = 0;
    } else {
        LOG(2, "flush_device: unknown device at addr %d, ignoring", addr);
    }
}

// Populates reply_buf with up to 2 pending keyboard key bytes, or 0xFF 0xFF if none
static void prepare_kbd_reply(adb_t *adb) {
    if (kbd_queue_empty(adb)) {
        // No pending events: return the idle/null response
        adb->reply_buf[0] = KBD_NO_KEY;
        adb->reply_buf[1] = KBD_NO_KEY;
    } else {
        adb->reply_buf[0] = kbd_dequeue(adb);
        // Pad with 0xFF if only one key is queued; otherwise deliver the second
        adb->reply_buf[1] = kbd_queue_empty(adb) ? KBD_NO_KEY : kbd_dequeue(adb);
    }
    adb->reply_len = 2;
}

// Populates reply_buf with Mouse Register 0 data.
// Only the portion of the delta that fits in 7-bit signed range is consumed;
// the remainder stays in the accumulator for subsequent polls.
static void prepare_mouse_reply(adb_t *adb) {
    int remain_dy, remain_dx;
    int dy = clamp_delta(adb->mouse_dy, &remain_dy);
    int dx = clamp_delta(adb->mouse_dx, &remain_dx);

    // Byte 1: bit 7 = button (1=up, 0=down); bits 6-0 = signed Y delta
    uint8_t btn_bit = adb->mouse_button ? 0x00 : 0x80; // active-low button
    adb->reply_buf[0] = btn_bit | encode_delta(dy);
    // Byte 2: bit 7 = 1 (reserved for 2nd button, always 1 on single-button mouse)
    adb->reply_buf[1] = 0x80 | encode_delta(dx);
    adb->reply_len = 2;

    // Keep only the unconsumed remainder
    adb->mouse_dy = remain_dy;
    adb->mouse_dx = remain_dx;

    // Clear the pending flag if all deltas have been consumed
    if (remain_dy == 0 && remain_dx == 0)
        adb->mouse_data_pending = false;
}

// Populates reply_buf with Register 3 data (address + handler ID) for a device
static void prepare_reg3_reply(adb_t *adb, const adb_device_t *dev) {
    // Byte 0: device address in bits 3-0; byte 1: handler ID
    adb->reply_buf[0] = dev->address & 0x0F;
    adb->reply_buf[1] = dev->handler;
    adb->reply_len = 2;
}

// Configures a "no device" reply: zero bytes cause bit3=0 on the first SR interrupt
static void prepare_no_device_reply(adb_t *adb) {
    // reply_len=0 means the first call to adb_deliver_next_byte() immediately
    // sends a dummy byte with bit3=0, matching the OS "no-reply" detection path
    adb->reply_len = 0;
    adb->reply_index = 0;
}

// Prepares the reply buffer for a Talk command targeting the given address + register
static void prepare_talk_reply(adb_t *adb, uint8_t addr, uint8_t reg) {
    adb->reply_index = 0;

    if (reg == 0) {
        // On real ADB hardware, a device with no new data does not drive the
        // bus in response to Talk R0 — the transceiver sees a timeout.  This
        // is critical for the ROM's SRQ scan: after SRQ, the ROM polls each
        // registered device in turn; only the device with pending data should
        // respond.  Idle devices must timeout so the scan advances.
        if (!device_has_pending_data(adb, addr)) {
            LOG(2, "talk R0 addr=%d: no pending data (timeout)", addr);
            prepare_no_device_reply(adb);
        } else if (addr == adb->kbd.address) {
            prepare_kbd_reply(adb);
            LOG(2, "talk R0 kbd: [%02X %02X]", adb->reply_buf[0], adb->reply_buf[1]);
        } else if (addr == adb->mouse.address) {
            prepare_mouse_reply(adb);
            LOG(2, "talk R0 mouse: [%02X %02X]", adb->reply_buf[0], adb->reply_buf[1]);
        } else {
            LOG(2, "talk R0 unknown addr=%d: no device", addr);
            prepare_no_device_reply(adb);
        }
    } else if (reg == 3) {
        adb_device_t *dev = find_device(adb, addr);
        if (dev) {
            prepare_reg3_reply(adb, dev);
            LOG(2, "talk R3 addr=%d: [%02X %02X]", addr, adb->reply_buf[0], adb->reply_buf[1]);
        } else {
            LOG(2, "talk R3 unknown addr=%d: no device", addr);
            prepare_no_device_reply(adb);
        }
    } else {
        // Registers 1 and 2 are not implemented; treat as no device
        LOG(2, "talk R%d addr=%d: unimplemented register", reg, addr);
        prepare_no_device_reply(adb);
    }
}

// Applies the accumulated Listen bytes to the target device register
static void apply_listen_data(adb_t *adb) {
    if (adb->listen_index < 2) {
        // Incomplete Listen data; OS must have aborted the transaction
        LOG(1, "apply_listen_data: only %d bytes received, ignoring", adb->listen_index);
        return;
    }

    if (adb->listen_reg == 3) {
        // Register 3: update device address (byte 0 bits 3-0) and handler (byte 1)
        adb_device_t *dev = find_device(adb, adb->listen_addr);
        if (dev) {
            uint8_t new_addr = adb->listen_buf[0] & 0x0F;
            uint8_t new_handler = adb->listen_buf[1];
            LOG(2, "listen R3 addr=%d: new_addr=%d handler=0x%02X", adb->listen_addr, new_addr, new_handler);
            dev->address = new_addr;
            dev->handler = new_handler;
        } else {
            LOG(2, "listen R3 unknown addr=%d, ignoring", adb->listen_addr);
        }
    } else {
        // Registers 0, 1, 2 not implemented; accept and discard
        LOG(2, "listen R%d addr=%d: unimplemented register, ignoring", adb->listen_reg, adb->listen_addr);
    }
}

// Decodes a command byte received from the VIA shift register and prepares the reply
static void adb_decode_command(adb_t *adb, uint8_t cmd) {
    uint8_t addr = (cmd >> 4) & 0x0F;
    uint8_t type = (cmd >> 2) & 0x03;
    uint8_t reg = cmd & 0x03;
    static const char *type_names[] = {"SendReset", "Flush", "Listen", "Talk"};

    LOG(2, "adb_decode_command: cmd=0x%02X (%s R%d) addr=%d", cmd, type_names[type], reg, addr);

    // Clear any previous reply and listen state before processing the new command
    adb->reply_len = 0;
    adb->reply_index = 0;
    adb->dummy_sent = false;

    if (cmd == 0x00) {
        // SendReset (broadcast) resets all devices to their default addresses
        adb_reset(adb);
        return;
    }

    switch (type) {
    case CMD_TYPE_SENDRESET:
        // Address-specific SendReset; treat the same as broadcast reset
        adb_reset(adb);
        break;

    case CMD_TYPE_FLUSH:
        // Discard buffered data for the target device
        flush_device(adb, addr);
        break;

    case CMD_TYPE_LISTEN:
        // Arm the Listen accumulator; subsequent shift_cb calls will fill listen_buf
        adb->listen_active = true;
        adb->listen_index = 0;
        adb->listen_addr = addr;
        adb->listen_reg = reg;
        break;

    case CMD_TYPE_TALK:
        // Build the reply buffer; bytes are delivered via output_cb state transitions
        prepare_talk_reply(adb, addr, reg);
        // Track the last Talk R0 target for auto-poll (the transceiver repeats it)
        if (reg == 0)
            adb->last_poll_addr = addr;
        break;
    }
}

// Delivers the next byte from the reply buffer via the VIA shift register.
// Keeps vADBInt high while bytes remain; pulls it low on the final dummy cycle
// so the OS recognises end-of-transfer and exits its fetch loop.
static void adb_deliver_next_byte(adb_t *adb) {
    if (adb->reply_index < adb->reply_len) {
        // Real reply byte: keep vADBInt HIGH (deasserted) so the ROM's fetch
        // loop continues.  The ROM's FDBShiftInt entry does BTST #3 on port B
        // before jumping to the resume handler; bit3=HIGH (Z=0) means "no SRQ,
        // normal data", while bit3=LOW (Z=1) signals SRQ/end-of-transfer.
        uint8_t byte = adb->reply_buf[adb->reply_index++];
        LOG(3, "adb_deliver_next_byte: byte[%d]=0x%02X (%d remaining)", adb->reply_index - 1, byte,
            adb->reply_len - adb->reply_index);
        set_adb_int(adb, true); // bit3=1 → continue fetching
        via_input_sr(adb->via, byte);
    } else {
        // All reply bytes delivered (or reply_len=0 for "no device"):
        // Send a dummy byte with vADBInt LOW (asserted) to signal end-of-transfer.
        // The ROM sees bit3=LOW → BEQ at fetch exit → completion handler.
        LOG(3, "adb_deliver_next_byte: dummy byte (end-of-transfer)");
        adb->dummy_sent = true; // suppress further deliveries during replay
        set_adb_int(adb, false); // bit3=0 → stop fetching
        via_input_sr(adb->via, 0xFF);
    }
}

// Scheduler callback that delivers the next ADB reply byte after a realistic delay.
// On real hardware the ADB transceiver takes ~200 µs to clock 8 bits via CB1.
// Deferring the delivery ensures the current interrupt handler has time to finish
// and RTE before the next SR interrupt fires; without this delay the ROM's ADB
// state machine sees uninitialised callback pointers (BUG-003).
static void adb_deliver_next_byte_deferred(void *source, uint64_t data) {
    (void)data;
    adb_t *adb = (adb_t *)source;
    adb_deliver_next_byte(adb);
}

// Scheduler callback that signals shift-register completion after the ADB
// transceiver finishes clocking a command or Listen byte.  On real hardware the
// transceiver drives CB1 to clock 8 bits through the VIA shift register;
// when complete, the resulting edge on CB1 sets IFR_SR.  We simulate this by
// feeding the byte back through via_input_sr() after ADB_SHIFT_DELAY ns.
static void adb_shift_complete_deferred(void *source, uint64_t data) {
    (void)data;
    adb_t *adb = (adb_t *)source;
    LOG(2, "adb_shift_complete_deferred: state=%d reply_len=%d reply_idx=%d", adb->state, adb->reply_len,
        adb->reply_index);
    // Feed back the current SR value to set IFR_SR; the VIA must be in shift-in
    // mode (ACR mode 3) by now — the ROM switches from mode 7 to mode 3
    // after writing SR.  The byte value does not matter; the ROM reads
    // the actual command from memory, not from the shift register.
    via_input_sr(adb->via, via_read_sr(adb->via));
}

// Scheduler callback that implements the ADB transceiver's auto-poll behaviour.
// In IDLE state the real transceiver repeats the last Talk R0 command every ~11 ms.
// When a device has data, the transceiver clocks it in and fires IFR_SR with
// bit3=HIGH so the ROM's FDBShiftInt handler can fetch the reply bytes via
// EVEN/ODD transitions.  When no device responds, the transceiver stays quiet
// and reschedules the next poll — the ROM remains waiting at ShiftIntResume.
//
// Important: the real transceiver always repeats the LAST Talk R0 command
// issued by the ROM (tracked in last_poll_addr).  It does NOT choose which
// device to poll.  If a non-active device has data, the transceiver signals
// SRQ (bit3=LOW) during the idle response so the ROM's SRQ handler can
// discover and poll the correct device.  Overriding the poll address broke
// the SE/30 ROM's ADB state machine because the ROM's device-handler pointer
// at $134(ADBBase) was set for last_poll_addr, not for the device we chose.
//
// The SR byte written during autopoll serves only as a wake-up: the ROM's
// FDBShiftInt ISR uses IFR_SR to enter the handler but does not process the
// SR value as data.  Actual reply bytes are read via EVEN/ODD port-B state
// transitions, so reply_index must be 0 when the autopoll fires.
static void adb_autopoll_deferred(void *source, uint64_t data) {
    (void)data;
    adb_t *adb = (adb_t *)source;

    LOG(1, "autopoll: entry state=%d pending=%d mouse_pending=%d mouse_btn=%d", adb->state, has_pending_data(adb),
        adb->mouse_data_pending, adb->mouse_button);

    // Stale event: state has moved on since this was scheduled
    if (adb->state != ADB_STATE_IDLE)
        return;

    if (!has_pending_data(adb)) {
        // No device has data: the real transceiver gets no response and stays
        // quiet.  Don't fire IFR_SR — the ROM remains waiting.
        LOG(3, "autopoll: no pending data, rescheduling");
        scheduler_new_cpu_event(adb->scheduler, &adb_autopoll_deferred, adb, 0, 0, ADB_AUTOPOLL_INTERVAL);
        return;
    }

    // Always repeat the last Talk R0 target, matching real transceiver behaviour.
    uint8_t poll_addr = adb->last_poll_addr;
    bool polled_device_has_data = device_has_pending_data(adb, poll_addr);

    if (polled_device_has_data) {
        // Last-polled device has data: prepare reply and fire IFR_SR with
        // bit3=HIGH so the ROM's FDBShiftInt handler enters the data path.
        // The ROM's exit/idle handler calls the device handler at $134(ADBBase),
        // which was set up for last_poll_addr — so it matches correctly.
        // The SR byte (0xFF) is a wake-up only; actual data is delivered via
        // EVEN/ODD transitions starting from reply_index 0.
        prepare_talk_reply(adb, poll_addr, 0);
        LOG(2, "autopoll: addr=%d has data, signalling ROM", poll_addr);
        adb->reply_index = 0;
        adb->dummy_sent = false;
        set_adb_int(adb, true); // bit3=HIGH → data available
        via_input_sr(adb->via, 0xFF); // wake-up byte fires IFR_SR
    } else {
        // Last-polled device has no data, but another device does (SRQ case).
        // Signal SRQ (bit3=LOW) to prompt the ROM's SRQ handler to poll other
        // devices and discover which one needs attention.
        LOG(2, "autopoll: addr=%d no data, SRQ for other device", poll_addr);
        adb->reply_len = 0;
        adb->reply_index = 0;
        set_adb_int(adb, false); // bit3=LOW → SRQ from another device
        via_input_sr(adb->via, 0xFF); // fire IFR_SR for the SRQ path
    }
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// Allocates and initialises an ADB controller instance, optionally from a checkpoint
adb_t *adb_init(via_t *via, struct scheduler *scheduler, checkpoint_t *checkpoint) {
    adb_t *adb = (adb_t *)malloc(sizeof(adb_t));
    if (!adb)
        return NULL;
    memset(adb, 0, sizeof(adb_t));

    adb->via = via;
    adb->scheduler = scheduler;

    // Register the deferred delivery event type for checkpoint save/restore
    scheduler_new_event_type(scheduler, "adb", adb, "deliver", &adb_deliver_next_byte_deferred);

    // Register the shift-complete event type (command/Listen byte completion)
    scheduler_new_event_type(scheduler, "adb", adb, "shift_done", &adb_shift_complete_deferred);

    // Register the auto-poll event type (IDLE-state Talk R0 repetition)
    scheduler_new_event_type(scheduler, "adb", adb, "autopoll", &adb_autopoll_deferred);

    // Set device register 3 defaults and clear all queues/deltas
    adb_reset(adb);
    adb->state = ADB_STATE_IDLE;

    if (checkpoint) {
        // Restore plain-data state; pointers are re-filled above
        size_t data_size = offsetof(adb_t, via);
        system_read_checkpoint_data(checkpoint, adb, data_size);
        // vADBInt was restored as part of the VIA checkpoint; no extra call needed
    } else {
        // Cold boot: no pending data, so deassert SRQ (vADBInt high)
        set_adb_int(adb, true);
    }

    return adb;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Frees all resources associated with an ADB controller instance
void adb_delete(adb_t *adb) {
    if (!adb)
        return;
    free(adb);
}

// ============================================================================
// Lifecycle: Checkpointing
// ============================================================================

// Saves plain ADB state (up to the 'via' pointer boundary) to a checkpoint
void adb_checkpoint(adb_t *restrict adb, checkpoint_t *checkpoint) {
    if (!adb || !checkpoint)
        return;
    size_t data_size = offsetof(adb_t, via);
    system_write_checkpoint_data(checkpoint, adb, data_size);
}

// ============================================================================
// VIA Callback Hooks
// ============================================================================

// Called by the machine's VIA shift-out callback when the VIA completes an
// internal 80-cycle shift timer.  On the SE/30, VIA1 SR operates in mode 7
// (shift out under external clock CB1), so the real shift timing is controlled
// by the ADB transceiver, not the VIA's internal timer.  The ROM sometimes
// writes SR during interrupt handling (e.g., to clear it) while ACR is still
// in mode 7, which triggers spurious sr_shift_complete callbacks.
//
// To avoid decoding stale or spurious bytes, the ADB module reads VIA SR
// directly at each port-B state transition (CMD, EVEN, ODD) instead of
// relying on this callback.  This function is therefore intentionally a no-op.
void adb_shift_byte(adb_t *adb, uint8_t byte) {
    (void)adb;
    (void)byte;
}

// Called by the machine's VIA port-B output callback when the OS changes ST0/ST1.
//
// On the SE/30 the VIA shift register operates in mode 7 (shift-out under
// external clock driven by the ADB transceiver).  Rather than waiting for the
// VIA's internal sr_shift_complete callback (which fires for every SR write,
// even spurious ones during interrupt handling), we read VIA SR directly at
// each CMD and Listen-data transition.  This matches real hardware where the
// ADB transceiver controls shift timing via CB1 (BUG-004).
void adb_port_b_output(adb_t *adb, uint8_t value) {
    int new_state = extract_state(value);
    adb->state = new_state;

    LOG(3, "adb_port_b_output: port_b=0x%02X new_state=%d reply_len=%d reply_idx=%d", value, new_state, adb->reply_len,
        adb->reply_index);

    switch (new_state) {
    case ADB_STATE_CMD:
        // A new command is starting; abort any partially received Listen data.
        // Read the command byte directly from the VIA shift register.  The ROM
        // pre-loads SR (via adb_start_xfer) before writing port B to CMD, so
        // the byte is already available.  We read it here instead of relying
        // on the VIA's sr_shift_complete callback, because the ROM sometimes
        // writes SR while ACR is still in mode 7 during interrupt handling,
        // which would cause spurious sr_shift_complete firings (BUG-004).
        adb->listen_active = false;
        adb->listen_index = 0;
        // Cancel the VIA's generic shift-complete timer; ADB reads SR directly
        // and controls completion timing via adb_shift_complete_deferred.
        via_cancel_pending_shift(adb->via);
        // Deassert vADBInt (set HIGH) at the start of every new command.
        // The previous transaction's deliver_next_byte may have left vADBInt
        // LOW to signal end-of-transfer; the ROM checks bit 3 during the CMD
        // completion interrupt and takes different paths depending on its state.
        // On real hardware the ADB transceiver deasserts vADBInt when it sees
        // a new attention pulse from the host (BUG-004).
        set_adb_int(adb, true);
        adb_decode_command(adb, via_read_sr(adb->via));
        // Cancel any stale reply delivery or auto-poll from a previous
        // transaction, then schedule a deferred completion event that fires
        // IFR_SR well after the VBL handler has finished setting up ADB state
        // machine callbacks.
        remove_event(adb->scheduler, &adb_deliver_next_byte_deferred, adb);
        remove_event(adb->scheduler, &adb_shift_complete_deferred, adb);
        remove_event(adb->scheduler, &adb_autopoll_deferred, adb);
        scheduler_new_cpu_event(adb->scheduler, &adb_shift_complete_deferred, adb, 0, 0, ADB_SHIFT_DELAY);
        break;

    case ADB_STATE_EVEN:
    case ADB_STATE_ODD:
        if (adb->listen_active) {
            // Listen data phase: read the data byte directly from VIA SR
            // (same rationale as CMD — avoid spurious sr_shift_complete).
            via_cancel_pending_shift(adb->via);
            uint8_t byte = via_read_sr(adb->via);
            if (adb->listen_index < 2)
                adb->listen_buf[adb->listen_index++] = byte;
            if (adb->listen_index == 2) {
                adb->listen_active = false;
                apply_listen_data(adb);
            }
            remove_event(adb->scheduler, &adb_shift_complete_deferred, adb);
            scheduler_new_cpu_event(adb->scheduler, &adb_shift_complete_deferred, adb, 0, 0, ADB_SHIFT_DELAY);
        } else {
            // Reply phase: schedule delivery of the next reply byte after a
            // realistic ADB bus delay.  The delivery is deferred so the current
            // SR interrupt handler can finish and RTE before the next IFR_SR
            // fires.  Without this delay the ROM's ADB state machine reads
            // uninitialised callback pointers (BUG-003).
            remove_event(adb->scheduler, &adb_deliver_next_byte_deferred, adb);
            scheduler_new_cpu_event(adb->scheduler, &adb_deliver_next_byte_deferred, adb, 0, 0, ADB_BYTE_DELAY);
        }
        break;

    case ADB_STATE_IDLE:
        // Transaction complete: cancel any stale events from the previous
        // transaction and schedule a new auto-poll.  The real ADB transceiver
        // repeats the last Talk R0 command every ~11 ms while in IDLE state.
        //
        // Cancel stale shift-complete and byte-delivery events from a previous
        // CMD or EVEN/ODD phase.  If the ROM transitions CMD→IDLE without
        // fetching reply data (aborted Talk during SRQ scan), a pending
        // shift_complete_deferred would fire spuriously and confuse the ROM's
        // ADB state machine, preventing subsequent auto-polls from working.
        remove_event(adb->scheduler, &adb_shift_complete_deferred, adb);
        remove_event(adb->scheduler, &adb_deliver_next_byte_deferred, adb);
        //
        // Detect aborted Talk: if a reply was prepared (reply_len > 0) but no
        // bytes were fetched by the ROM (reply_index == 0 and no dummy sent),
        // the ROM went CMD→IDLE without reading the data.  This happens during
        // the SE/30 ROM's SRQ scan when timing doesn't match the ROM's
        // expectations.  Re-mark mouse data as pending so the next auto-poll
        // can deliver it.  Without this, a button-up event can be lost forever.
        if (adb->reply_len > 0 && adb->reply_index == 0 && !adb->dummy_sent) {
            LOG(2, "IDLE: aborted Talk detected (reply_len=%d), re-marking pending", adb->reply_len);
            adb->mouse_data_pending = true;
        }
        remove_event(adb->scheduler, &adb_autopoll_deferred, adb);
        set_adb_int(adb, !has_pending_data(adb));
        scheduler_new_cpu_event(adb->scheduler, &adb_autopoll_deferred, adb, 0, 0, ADB_AUTOPOLL_INTERVAL);
        break;
    }
}

// ============================================================================
// Operations (Public API)
// ============================================================================

// Enqueues a host keyboard event into the ADB keyboard queue in Register 0 format.
// Bit 7 = key-up flag; bits 6-0 = ADB virtual key code.  Auto-repeat is suppressed.
void adb_keyboard_event(adb_t *adb, key_event_t event, int key) {
    LOG(2, "adb_keyboard_event: %s key=0x%02X", event == key_down ? "key_down" : "key_up", key);

    if (key < 0 || key >= 128) {
        LOG(1, "adb_keyboard_event: invalid key %d, ignoring", key);
        return;
    }

    if (event == key_down) {
        if (adb->kbd_pressed[key]) {
            // Suppress host auto-repeat: the OS manages key repeat itself
            LOG(3, "adb_keyboard_event: suppressing repeat for key 0x%02X", key);
            return;
        }
        adb->kbd_pressed[key] = true;
    } else {
        if (!adb->kbd_pressed[key]) {
            // Spurious key-up (key was never seen as down); discard to avoid confusion
            LOG(3, "adb_keyboard_event: spurious key_up for key 0x%02X, ignoring", key);
            return;
        }
        adb->kbd_pressed[key] = false;
    }

    // ADB Register 0 format: bit 7 = 1 for key-up, 0 for key-down; bits 6-0 = keycode
    uint8_t byte = (uint8_t)(key & 0x7F);
    if (event == key_up)
        byte |= 0x80;

    kbd_enqueue(adb, byte);

    // Pull SRQ low to prompt the OS to poll sooner when we are idle.
    // Reschedule the auto-poll to fire quickly so the ROM picks up the key event.
    if (adb->state == ADB_STATE_IDLE) {
        set_adb_int(adb, false);
        remove_event(adb->scheduler, &adb_autopoll_deferred, adb);
        scheduler_new_cpu_event(adb->scheduler, &adb_autopoll_deferred, adb, 0, 0, ADB_SHIFT_DELAY);
    }
}

// Records a host mouse event: updates accumulated deltas and current button state.
// Deltas are reset to zero after each Talk R0 reply is delivered.
void adb_mouse_event(adb_t *adb, bool button, int dx, int dy) {
    LOG(3, "adb_mouse_event: button=%d dx=%d dy=%d", button, dx, dy);

    bool button_changed = (button != adb->mouse_button);
    adb->mouse_button = button;
    adb->mouse_dx += dx;
    adb->mouse_dy += dy;

    // Mark mouse as having unreported data
    if (dx != 0 || dy != 0 || button_changed)
        adb->mouse_data_pending = true;

    // Assert SRQ when idle and there is movement or a button-state change.
    // Reschedule the auto-poll to fire quickly so the ROM picks up the new data.
    if (adb->state == ADB_STATE_IDLE && (dx != 0 || dy != 0 || button_changed)) {
        set_adb_int(adb, false);
        remove_event(adb->scheduler, &adb_autopoll_deferred, adb);
        scheduler_new_cpu_event(adb->scheduler, &adb_autopoll_deferred, adb, 0, 0, ADB_SHIFT_DELAY);
    }
}

// Injects mouse movement deltas without changing the current button state.
// Used by set-mouse to move the cursor through the ADB hardware path.
void adb_mouse_move(adb_t *adb, int dx, int dy) {
    adb_mouse_event(adb, adb->mouse_button, dx, dy);
}

// === Object-model class descriptor =========================================
//
// `keyboard.press(key)` — inject a key-down + key-up via the keyboard
// subsystem. The arg is either a string name ("return", "space",
// "esc", a-z, 0-9 …) resolved by debug_mac_resolve_key_name, or an
// integer ADB virtual keycode (0x00–0x7F).

static value_t keyboard_method_press(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("keyboard.press: expected (key) — name string or keycode int");
    int keycode = -1;
    char hexbuf[8];
    const char *display_name = NULL;
    if (argv[0].kind == V_STRING) {
        const char *name = argv[0].s ? argv[0].s : "";
        keycode = debug_mac_resolve_key_name(name);
        display_name = name;
    } else if (argv[0].kind == V_INT || argv[0].kind == V_UINT) {
        long long raw = (argv[0].kind == V_INT) ? (long long)argv[0].i : (long long)argv[0].u;
        snprintf(hexbuf, sizeof(hexbuf), "0x%02llx", raw);
        keycode = debug_mac_resolve_key_name(hexbuf);
        display_name = hexbuf;
    } else {
        return val_err("keyboard.press: key must be a string name or integer keycode");
    }
    if (keycode < 0) {
        printf("Unknown key: %s\n", display_name ? display_name : "(?)");
        return val_bool(false);
    }
    system_keyboard_update(key_down, keycode);
    system_keyboard_update(key_up, keycode);
    printf("key: 0x%02X (%s)\n", keycode, display_name);
    return val_bool(true);
}

static const arg_decl_t keyboard_press_args[] = {
    // V_NONE: body accepts either a name string or a numeric ADB keycode.
    {.name = "key", .kind = V_NONE, .doc = "Key name (\"return\"/\"esc\"/\"a\"/...) or ADB keycode int"},
};

static const member_t keyboard_members[] = {
    {.kind = M_METHOD,
     .name = "press",
     .doc = "Tap a key (down + up) on the emulated keyboard",
     .method = {.args = keyboard_press_args, .nargs = 1, .result = V_BOOL, .fn = keyboard_method_press}},
};

const class_desc_t keyboard_class = {
    .name = "keyboard",
    .members = keyboard_members,
    .n_members = sizeof(keyboard_members) / sizeof(keyboard_members[0]),
};

// === Process-singleton lifecycle ============================================
//
// `keyboard` is a stateless facade — its press() method routes through
// adb_press_key on the active machine. Register once at shell_init.

static struct object *s_keyboard_object = NULL;

void keyboard_class_register(void) {
    if (s_keyboard_object)
        return;
    s_keyboard_object = object_new(&keyboard_class, NULL, "keyboard");
    if (s_keyboard_object)
        object_attach(object_root(), s_keyboard_object);
}

void keyboard_class_unregister(void) {
    if (s_keyboard_object) {
        object_detach(s_keyboard_object);
        object_delete(s_keyboard_object);
        s_keyboard_object = NULL;
    }
}
