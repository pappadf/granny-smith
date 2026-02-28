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
#include "log.h"
#include "system.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

LOG_USE_CATEGORY_NAME("adb");

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

// Keyboard idle response: no key event
#define KBD_NO_KEY 0xFF

// Mouse idle response bytes: no movement, button up
#define MOUSE_IDLE_B1 0x80 // bit 7 = 1 (button up), bits 6-0 = Y delta = 0
#define MOUSE_IDLE_B2 0x80 // bit 7 = 1 (reserved), bits 6-0 = X delta = 0

// Keyboard event queue capacity (generous to avoid dropping key-up events)
#define KBD_QUEUE_SIZE 128

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

    // Device register 3 state (address + handler ID) for keyboard and mouse
    adb_device_t kbd;
    adb_device_t mouse;

    // === Pointers last (not checkpointed) ===
    via_t *via;
    struct scheduler *scheduler;
};

// ============================================================================
// Forward Declarations
// ============================================================================

static void adb_reset(adb_t *adb);
static void adb_deliver_next_byte(adb_t *adb);
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

// Dequeues one byte from the keyboard ring buffer (caller must check not empty)
static uint8_t kbd_dequeue(adb_t *adb) {
    assert(adb->kbd_queue.tail != adb->kbd_queue.head);
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

// Encodes a mouse axis delta into ADB's 7-bit signed format (2's complement, -64..+63)
static uint8_t encode_delta(int delta) {
    // Clamp to the representable range before masking to 7 bits
    if (delta > 63)
        delta = 63;
    if (delta < -64)
        delta = -64;
    return (uint8_t)(delta & 0x7F);
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

// Returns true if there is mouse or keyboard data worth asserting SRQ for
static bool has_pending_data(const adb_t *adb) {
    return !kbd_queue_empty(adb) || adb->mouse_dx != 0 || adb->mouse_dy != 0;
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

// Populates reply_buf with Mouse Register 0 data and resets accumulated deltas
static void prepare_mouse_reply(adb_t *adb) {
    // Byte 1: bit 7 = button (1=up, 0=down); bits 6-0 = signed Y delta
    uint8_t btn_bit = adb->mouse_button ? 0x00 : 0x80; // active-low button
    adb->reply_buf[0] = btn_bit | encode_delta(adb->mouse_dy);
    // Byte 2: bit 7 = 1 (reserved for 2nd button, always 1 on single-button mouse)
    adb->reply_buf[1] = 0x80 | encode_delta(adb->mouse_dx);
    adb->reply_len = 2;
    // Clear accumulated deltas after delivering them
    adb->mouse_dx = 0;
    adb->mouse_dy = 0;
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
        if (addr == adb->kbd.address) {
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

    LOG(2, "adb_decode_command: cmd=0x%02X addr=%d type=%d reg=%d", cmd, addr, type, reg);

    // Clear any previous reply and listen state before processing the new command
    adb->reply_len = 0;
    adb->reply_index = 0;

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
        break;
    }
}

// Delivers the next byte from the reply buffer via the VIA shift register.
// Keeps vADBInt high while bytes remain; pulls it low on the final dummy cycle
// so the OS recognises end-of-transfer and exits its fetch loop.
static void adb_deliver_next_byte(adb_t *adb) {
    if (adb->reply_index < adb->reply_len) {
        // Real reply byte: keep vADBInt high so the OS continues fetching
        uint8_t byte = adb->reply_buf[adb->reply_index++];
        LOG(3, "adb_deliver_next_byte: byte[%d]=0x%02X (%d remaining)", adb->reply_index - 1, byte,
            adb->reply_len - adb->reply_index);
        set_adb_int(adb, true); // bit3=1 → continue fetching
        via_input_sr(adb->via, byte);
    } else {
        // All reply bytes delivered (or reply_len=0 for "no device"):
        // Send a dummy byte with vADBInt low to terminate the OS fetch loop
        LOG(3, "adb_deliver_next_byte: dummy byte (end-of-transfer)");
        set_adb_int(adb, false); // bit3=0 → stop fetching
        via_input_sr(adb->via, 0xFF);
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

// Called by the machine's VIA shift-out callback when the OS finishes shifting a byte.
// In the command phase the byte is decoded as an ADB command; in the Listen phase
// subsequent bytes are accumulated as data.
void adb_shift_byte(adb_t *adb, uint8_t byte) {
    LOG(3, "adb_shift_byte: 0x%02X listen_active=%d", byte, adb->listen_active);

    if (adb->listen_active) {
        // OS is sending Listen data bytes; accumulate them
        if (adb->listen_index < 2)
            adb->listen_buf[adb->listen_index++] = byte;
        if (adb->listen_index == 2) {
            // Both bytes received; apply them and return to normal command processing
            adb->listen_active = false;
            apply_listen_data(adb);
        }
        return;
    }

    // Not in Listen mode: this byte is an ADB command
    adb_decode_command(adb, byte);
}

// Called by the machine's VIA port-B output callback when the OS changes ST0/ST1.
// In data states (1 or 2) the transceiver delivers the next reply byte.
// In idle (3) the SRQ line is updated based on remaining pending data.
void adb_port_b_output(adb_t *adb, uint8_t value) {
    int new_state = extract_state(value);
    adb->state = new_state;

    LOG(3, "adb_port_b_output: port_b=0x%02X state=%d", value, new_state);

    switch (new_state) {
    case ADB_STATE_CMD:
        // A new command is starting; abort any partially received Listen data
        adb->listen_active = false;
        adb->listen_index = 0;
        break;

    case ADB_STATE_EVEN:
    case ADB_STATE_ODD:
        // Data phase: deliver the next reply byte (or dummy if transfer is done).
        // Skip delivery when in Listen mode — the OS is shifting data OUT, not reading it.
        if (!adb->listen_active)
            adb_deliver_next_byte(adb);
        break;

    case ADB_STATE_IDLE:
        // Transaction complete: update vADBInt to reflect whether more data is pending
        set_adb_int(adb, !has_pending_data(adb));
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

    // Pull SRQ low to prompt the OS to poll sooner when we are idle
    if (adb->state == ADB_STATE_IDLE)
        set_adb_int(adb, false);
}

// Records a host mouse event: updates accumulated deltas and current button state.
// Deltas are reset to zero after each Talk R0 reply is delivered.
void adb_mouse_event(adb_t *adb, bool button, int dx, int dy) {
    LOG(3, "adb_mouse_event: button=%d dx=%d dy=%d", button, dx, dy);

    bool button_changed = (button != adb->mouse_button);
    adb->mouse_button = button;
    adb->mouse_dx += dx;
    adb->mouse_dy += dy;

    // Assert SRQ when idle and there is movement or a button-state change
    if (adb->state == ADB_STATE_IDLE && (dx != 0 || dy != 0 || button_changed))
        set_adb_int(adb, false);
}
