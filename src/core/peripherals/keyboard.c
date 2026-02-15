// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// keyboard.c
// Implements Mac Plus keyboard emulation via VIA shift register interface.

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "keyboard.h"
#include "log.h"
#include "scheduler.h"
#include "system.h"
#include "via.h"

LOG_USE_CATEGORY_NAME("keyboard");

// Increased queue size to reduce risk of dropping key-up events under bursty host repeats
#define QUEUE_SIZE 128

// Model number byte per spec (bit0=1, bits1-3=model, bits4-6=next device id, bit7=chained)
// Chosen values: model=5 (101), next device=0 (000), chained=0 -> 0b00001011 = 0x0B
#define MODEL_NUMBER 0x0B

#define CMD_INQUIRY      0x10
#define CMD_INSTANT      0x14
#define CMD_MODEL_NUMBER 0x16
#define CMD_TEST         0x36

// Test command responses (only ACK implemented for now)
#define TEST_ACK_RESPONSE 0x7D
// #define TEST_NAK_RESPONSE 0x77 // (unused – would indicate self-test failure)

#define NULL_RESPONSE 0x7B

// Keyboard protocol response delay: the keyboard waits before sending its response.
// One serial bit takes ~330µs on the keyboard line. The keyboard typically waits
// at least 8 bit-times (~2.64ms) after receiving a command before responding.
// Note: This is the keyboard's own response timing, separate from VIA shift timing.
#define RX_TO_TX_DELAY (330 * 8 * 1000)

// Holds the state for the emulated Mac Plus keyboard
struct keyboard {
    // Plain-data first for contiguous checkpointing
    bool tx_pending; // Host (Mac) is waiting for a reply on current command
    uint8_t active_cmd; // Last received command

    struct {
        unsigned int tail; // Index of next byte to dequeue
        unsigned int head; // Index of next free slot
        uint8_t buf[QUEUE_SIZE]; // Circular buffer of pending key codes
    } queue;

    bool pressed[128]; // Tracks which keys are currently held down

    // Pointers last (not checkpointed)
    struct scheduler *scheduler; // Scheduler for timed events
    scc_t *scc; // SCC peripheral (unused here, kept for API symmetry)
    via_t *via; // VIA for shift register communication
};

// Context passed to scheduled key update events (currently unused)
typedef struct {
    keyboard_t *keyboard;
    key_event_t event;
    uint8_t key;
} key_update_t;

// Adds a byte to the keyboard transmit queue
static void enqueue(keyboard_t *keyboard, uint8_t byte) {
    int head = keyboard->queue.head + 1;

    if (head == QUEUE_SIZE)
        head = 0; // wrap

    if (head == keyboard->queue.tail) {
        // Queue full: drop oldest (so we preserve recent transitions, especially key-up)
        // Advance tail one element to make room.
        LOG(1, "queue overflow – dropping oldest byte to insert new one");
        keyboard->queue.tail++;
        if (keyboard->queue.tail == QUEUE_SIZE)
            keyboard->queue.tail = 0;
    }

    keyboard->queue.buf[keyboard->queue.head] = byte;
    keyboard->queue.head = head;
}

static uint8_t dequeue(keyboard_t *keyboard) {
    assert(keyboard->queue.tail != keyboard->queue.head);

    uint8_t byte = keyboard->queue.buf[keyboard->queue.tail];

    keyboard->queue.tail++;

    // wrap around
    if (keyboard->queue.tail == QUEUE_SIZE)
        keyboard->queue.tail = 0;

    return byte;
}

// Returns true if the transmit queue is empty
static bool queue_empty(keyboard_t *keyboard) {
    return keyboard->queue.head == keyboard->queue.tail;
}

// Clears the transmit queue
static void queue_reset(keyboard_t *keyboard) {
    keyboard->queue.tail = keyboard->queue.head = 0;
}

// Sends a byte to the Mac via the VIA shift register
static void tx_to_via(keyboard_t *keyboard, uint8_t byte) {
    LOG(3, "tx_to_via: 0x%02X", (int)byte);

    via_input_sr(keyboard->via, byte);
}

// Called when inquiry command times out with no key events
void timeout_callback(void *source, uint64_t data) {
    keyboard_t *keyboard = (keyboard_t *)source;

    LOG(3, "timeout_callback: inquiry timed out, sending NULL_RESPONSE 0x%02X", NULL_RESPONSE);

    // Send null response to signal that no key event occurred
    tx_to_via(keyboard, NULL_RESPONSE);

    // Mac is no longer waiting for input
    keyboard->tx_pending = false;
}

// Enters the data transfer phase after receiving a command
void tx_callback(void *source, uint64_t data) {
    keyboard_t *keyboard = (keyboard_t *)source;

    LOG(3, "tx_callback: active_cmd=0x%02X, queue_empty=%d", keyboard->active_cmd, queue_empty(keyboard));

    // if there is data queued up...
    if (!queue_empty(keyboard)) {
        uint8_t byte = dequeue(keyboard);
        LOG(3, "tx_callback: sending queued byte 0x%02X", byte);

        // ...then send it
        tx_to_via(keyboard, byte);

        // remove any pending timeout
        remove_event(keyboard->scheduler, &timeout_callback, (void *)keyboard);
    } else if (keyboard->active_cmd == CMD_INSTANT) {
        LOG(3, "tx_callback: INSTANT with empty queue, sending NULL_RESPONSE");
        assert(!has_event(keyboard->scheduler, &timeout_callback));
        tx_to_via(keyboard, NULL_RESPONSE);
    } else {
        LOG(3, "tx_callback: no data, setting tx_pending=true");
        keyboard->tx_pending = true;
    }
}

// Queues a key event byte or sends immediately if Mac is waiting
static void add_key_event(keyboard_t *keyboard, uint8_t key) {
    LOG(2, "add_key_event: key=0x%02X, tx_pending=%d, queue_empty=%d", key, keyboard->tx_pending,
        queue_empty(keyboard));

    // If Mac is waiting and nothing queued, send immediately
    if (keyboard->tx_pending && queue_empty(keyboard)) {
        LOG(2, "add_key_event: sending immediately to via");
        tx_to_via(keyboard, key);
        keyboard->tx_pending = false;
        remove_event(keyboard->scheduler, &timeout_callback, (void *)keyboard);
    } else {
        LOG(2, "add_key_event: enqueueing for later");
        enqueue(keyboard, key);
    }
}

// Processes a key event from the host and converts to Mac keyboard protocol
extern void keyboard_update(keyboard_t *keyboard, key_event_t event, int host_key) {
    LOG(3, "keyboard_update: event=%s, host_key=0x%02X", event == key_down ? "key_down" : "key_up", host_key);

    if (host_key < 0 || host_key >= 128) {
        LOG(1, "keyboard_update: invalid host_key=%d, ignoring", host_key);
        return;
    }

    // Suppress auto-repeat key_down if already pressed
    if (event == key_down) {
        if (keyboard->pressed[host_key]) {
            LOG(3, "keyboard_update: ignoring repeat for key 0x%02X", host_key);
            return; // ignore repeat
        }
        keyboard->pressed[host_key] = true;
    } else { // key_up
        if (!keyboard->pressed[host_key]) {
            LOG(3, "keyboard_update: spurious key_up for key 0x%02X, ignoring", host_key);
            // Spurious key_up – ignore (or could still emit)
            return;
        }
        keyboard->pressed[host_key] = false;
    }

    // Translate ADB virtual key code to Mac Plus raw code.
    // See notes/key-mappings.md for the complete translation table.
    uint8_t raw_code;
    bool needs_keypad_prefix = false;

    switch (host_key) {
    // Main keyboard letters and numbers: raw = (virtual << 1) | 1
    case 0x00:
        raw_code = 0x01;
        break; // A
    case 0x01:
        raw_code = 0x03;
        break; // S
    case 0x02:
        raw_code = 0x05;
        break; // D
    case 0x03:
        raw_code = 0x07;
        break; // F
    case 0x04:
        raw_code = 0x09;
        break; // H
    case 0x05:
        raw_code = 0x0B;
        break; // G
    case 0x06:
        raw_code = 0x0D;
        break; // Z
    case 0x07:
        raw_code = 0x0F;
        break; // X
    case 0x08:
        raw_code = 0x11;
        break; // C
    case 0x09:
        raw_code = 0x13;
        break; // V
    case 0x0B:
        raw_code = 0x17;
        break; // B
    case 0x0C:
        raw_code = 0x19;
        break; // Q
    case 0x0D:
        raw_code = 0x1B;
        break; // W
    case 0x0E:
        raw_code = 0x1D;
        break; // E
    case 0x0F:
        raw_code = 0x1F;
        break; // R
    case 0x10:
        raw_code = 0x21;
        break; // Y
    case 0x11:
        raw_code = 0x23;
        break; // T
    case 0x12:
        raw_code = 0x25;
        break; // 1
    case 0x13:
        raw_code = 0x27;
        break; // 2
    case 0x14:
        raw_code = 0x29;
        break; // 3
    case 0x15:
        raw_code = 0x2B;
        break; // 4
    case 0x16:
        raw_code = 0x2D;
        break; // 6
    case 0x17:
        raw_code = 0x2F;
        break; // 5
    case 0x18:
        raw_code = 0x31;
        break; // =
    case 0x19:
        raw_code = 0x33;
        break; // 9
    case 0x1A:
        raw_code = 0x35;
        break; // 7
    case 0x1B:
        raw_code = 0x37;
        break; // -
    case 0x1C:
        raw_code = 0x39;
        break; // 8
    case 0x1D:
        raw_code = 0x3B;
        break; // 0
    case 0x1E:
        raw_code = 0x3D;
        break; // ]
    case 0x1F:
        raw_code = 0x3F;
        break; // O
    case 0x20:
        raw_code = 0x41;
        break; // U
    case 0x21:
        raw_code = 0x43;
        break; // [
    case 0x22:
        raw_code = 0x45;
        break; // I
    case 0x23:
        raw_code = 0x47;
        break; // P
    case 0x24:
        raw_code = 0x49;
        break; // Return
    case 0x25:
        raw_code = 0x4B;
        break; // L
    case 0x26:
        raw_code = 0x4D;
        break; // J
    case 0x27:
        raw_code = 0x4F;
        break; // '
    case 0x28:
        raw_code = 0x51;
        break; // K
    case 0x29:
        raw_code = 0x53;
        break; // ;
    case 0x2A:
        raw_code = 0x55;
        break; // backslash
    case 0x2B:
        raw_code = 0x57;
        break; // ,
    case 0x2C:
        raw_code = 0x59;
        break; // /
    case 0x2D:
        raw_code = 0x5B;
        break; // N
    case 0x2E:
        raw_code = 0x5D;
        break; // M
    case 0x2F:
        raw_code = 0x5F;
        break; // .
    case 0x30:
        raw_code = 0x61;
        break; // Tab
    case 0x31:
        raw_code = 0x63;
        break; // Space
    case 0x32:
        raw_code = 0x65;
        break; // `
    case 0x33:
        raw_code = 0x67;
        break; // Delete
    case 0x35:
        raw_code = 0x6B;
        break; // Escape (map to Mac Plus if desired)
    case 0x37:
        raw_code = 0x6F;
        break; // Command
    case 0x38:
        raw_code = 0x71;
        break; // Shift
    case 0x39:
        raw_code = 0x73;
        break; // Caps Lock
    case 0x3A:
        raw_code = 0x75;
        break; // Option

    // Arrow keys: need $79 prefix, then shared letter key code
    case 0x7B:
        raw_code = 0x0D;
        needs_keypad_prefix = true;
        break; // Left (Z)
    case 0x7C:
        raw_code = 0x05;
        needs_keypad_prefix = true;
        break; // Right (D)
    case 0x7D:
        raw_code = 0x11;
        needs_keypad_prefix = true;
        break; // Down (C)
    case 0x7E:
        raw_code = 0x1B;
        needs_keypad_prefix = true;
        break; // Up (W)

    // Numeric keypad: need $79 prefix, then shared main key code
    case 0x41:
        raw_code = 0x03;
        needs_keypad_prefix = true;
        break; // Keypad . (S)
    case 0x43:
        raw_code = 0x05;
        needs_keypad_prefix = true;
        break; // Keypad * (D)
    case 0x45:
        raw_code = 0x0D;
        needs_keypad_prefix = true;
        break; // Keypad + (Z)
    case 0x47:
        raw_code = 0x0F;
        break; // Keypad Clear (X) - unique, no prefix
    case 0x4B:
        raw_code = 0x1B;
        needs_keypad_prefix = true;
        break; // Keypad / (W)
    case 0x4C:
        raw_code = 0x19;
        needs_keypad_prefix = true;
        break; // Keypad Enter (Q)
    case 0x4E:
        raw_code = 0x1D;
        needs_keypad_prefix = true;
        break; // Keypad - (E)
    case 0x51:
        raw_code = 0x11;
        needs_keypad_prefix = true;
        break; // Keypad = (C)
    case 0x52:
        raw_code = 0x25;
        needs_keypad_prefix = true;
        break; // Keypad 0 (1)
    case 0x53:
        raw_code = 0x27;
        needs_keypad_prefix = true;
        break; // Keypad 1 (2)
    case 0x54:
        raw_code = 0x29;
        needs_keypad_prefix = true;
        break; // Keypad 2 (3)
    case 0x55:
        raw_code = 0x2B;
        needs_keypad_prefix = true;
        break; // Keypad 3 (4)
    case 0x56:
        raw_code = 0x2D;
        needs_keypad_prefix = true;
        break; // Keypad 4 (6)
    case 0x57:
        raw_code = 0x2F;
        needs_keypad_prefix = true;
        break; // Keypad 5 (5)
    case 0x58:
        raw_code = 0x31;
        needs_keypad_prefix = true;
        break; // Keypad 6 (=)
    case 0x59:
        raw_code = 0x33;
        needs_keypad_prefix = true;
        break; // Keypad 7 (9)
    case 0x5B:
        raw_code = 0x37;
        needs_keypad_prefix = true;
        break; // Keypad 8 (-)
    case 0x5C:
        raw_code = 0x39;
        needs_keypad_prefix = true;
        break; // Keypad 9 (8)

    default:
        LOG(1, "keyboard_update: unknown ADB virtual key 0x%02X", host_key);
        return;
    }

    // Emit keypad prefix if needed (for arrow keys and numeric keypad)
    if (needs_keypad_prefix) {
        add_key_event(keyboard, 0x79);
    }

    // Set key-up flag in bit 7
    if (event == key_up)
        raw_code |= 0x80;

    LOG(2, "keyboard_update: ADB 0x%02X -> raw 0x%02X (%s)", host_key, raw_code,
        (raw_code & 0x80) ? "key_up" : "key_down");

    add_key_event(keyboard, raw_code);
}

// Resets keyboard state including queue and pressed key tracking
static void reset(keyboard_t *keyboard) {
    LOG(2, "reset: clearing queue and pressed state");
    queue_reset(keyboard);
    memset(keyboard->pressed, 0, sizeof(keyboard->pressed));
}

// Handles a command byte received from the Mac via VIA shift register
void keyboard_input(keyboard_t *keyboard, uint8_t byte) {
    LOG(3, "keyboard_input: received command 0x%02X", byte);

    keyboard->active_cmd = byte;

    bool schedule_tx = true;

    switch (byte) {
    case 0: // NOP / unused
        LOG(3, "keyboard_input: NOP command, no response");
        schedule_tx = false;
        break;
    case CMD_INQUIRY:
        LOG(2, "keyboard_input: INQUIRY command, scheduling 250ms timeout");
        scheduler_new_cpu_event(keyboard->scheduler, &timeout_callback, keyboard, 0, 0, NS_PER_SEC / 4);
        // Respond promptly (spec: host polls roughly every 0.25s; keyboard must not wait that long)
        break;
    case CMD_INSTANT:
        LOG(2, "keyboard_input: INSTANT command, immediate response");
        // Immediate response (no 0.25s timeout); we'll still use the standard small delay.
        break;
    case CMD_MODEL_NUMBER:
        LOG(2, "keyboard_input: MODEL_NUMBER command, responding 0x%02X", MODEL_NUMBER);
        reset(keyboard);
        enqueue(keyboard, MODEL_NUMBER);
        break;
    case CMD_TEST:
        LOG(2, "keyboard_input: TEST command, responding ACK 0x%02X", TEST_ACK_RESPONSE);
        enqueue(keyboard, TEST_ACK_RESPONSE);
        break;
    default:
        LOG(1, "keyboard_input: unsupported command 0x%02X", byte);
        schedule_tx = false;
        break;
    }

    if (schedule_tx) {
        LOG(3, "keyboard_input: scheduling tx callback");
        scheduler_new_cpu_event(keyboard->scheduler, &tx_callback, keyboard, 0, 0, RX_TO_TX_DELAY);
    }
}

// Creates and initializes a keyboard instance
keyboard_t *keyboard_init(struct scheduler *scheduler, scc_t *scc, via_t *via, checkpoint_t *checkpoint) {
    keyboard_t *keyboard = (keyboard_t *)malloc(sizeof(keyboard_t));
    if (!keyboard)
        return NULL;

    memset(keyboard, 0, sizeof(keyboard_t));
    keyboard->scheduler = scheduler;
    keyboard->scc = scc;
    keyboard->via = via;
    queue_reset(keyboard);
    memset(keyboard->pressed, 0, sizeof(keyboard->pressed));

    // Register event types for checkpointing
    scheduler_new_event_type(scheduler, "keyboard", keyboard, "timeout", &timeout_callback);
    scheduler_new_event_type(scheduler, "keyboard", keyboard, "tx", &tx_callback);

    // Load from checkpoint if provided
    if (checkpoint) {
        size_t data_size = offsetof(keyboard_t, scheduler);
        system_read_checkpoint_data(checkpoint, keyboard, data_size);
    }

    return keyboard;
}

// Frees resources associated with a keyboard instance
void keyboard_delete(keyboard_t *keyboard) {
    if (!keyboard)
        return;
    free(keyboard);
}

// Saves keyboard state to a checkpoint
void keyboard_checkpoint(keyboard_t *restrict keyboard, checkpoint_t *checkpoint) {
    if (!keyboard || !checkpoint)
        return;

    // Save keyboard state (plain data portion only)
    size_t data_size = offsetof(keyboard_t, scheduler);
    system_write_checkpoint_data(checkpoint, keyboard, data_size);
}
