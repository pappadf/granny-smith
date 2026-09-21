// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// keyboard.c
// Implements Mac Plus keyboard emulation via VIA shift register interface.

#include "keyboard.h"
#include "log.h"
#include "scheduler.h"
#include "system.h"
#include "via.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

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

// Adds a byte to the keyboard transmit queue
static void enqueue(keyboard_t *keyboard, uint8_t byte) {
    unsigned int head = keyboard->queue.head + 1;

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
static void keyboard_timeout_callback(void *source, uint64_t data) {
    keyboard_t *keyboard = (keyboard_t *)source;

    LOG(3, "keyboard_timeout_callback: inquiry timed out, sending NULL_RESPONSE 0x%02X", NULL_RESPONSE);

    // Send null response to signal that no key event occurred
    tx_to_via(keyboard, NULL_RESPONSE);

    // Mac is no longer waiting for input
    keyboard->tx_pending = false;
}

// Enters the data transfer phase after receiving a command
static void keyboard_tx_callback(void *source, uint64_t data) {
    keyboard_t *keyboard = (keyboard_t *)source;

    LOG(3, "keyboard_tx_callback: active_cmd=0x%02X, queue_empty=%d", keyboard->active_cmd, queue_empty(keyboard));

    // if there is data queued up...
    if (!queue_empty(keyboard)) {
        uint8_t byte = dequeue(keyboard);
        LOG(3, "keyboard_tx_callback: sending queued byte 0x%02X", byte);

        // ...then send it
        tx_to_via(keyboard, byte);

        // remove any pending timeout
        remove_event(keyboard->scheduler, &keyboard_timeout_callback, (void *)keyboard);
    } else if (keyboard->active_cmd == CMD_INSTANT) {
        LOG(3, "keyboard_tx_callback: INSTANT with empty queue, sending NULL_RESPONSE");

        // This used to assert that no timeout was armed.  It is an ordinary
        // guest sequence that arms one: INQUIRY schedules the 250 ms timer
        // and nothing cancels it when a later command supersedes it, so an
        // INSTANT arriving with the queue still empty finds it live.  The
        // INSTANT answers now, which is precisely what the INQUIRY's timeout
        // was there to do if nothing else did; leaving it armed would put a
        // second, unsolicited NULL_RESPONSE on the wire 250 ms later.
        if (has_event(keyboard->scheduler, &keyboard_timeout_callback)) {
            LOG(2, "keyboard_tx_callback: dropping the INQUIRY timeout superseded by this INSTANT");
            remove_event(keyboard->scheduler, &keyboard_timeout_callback, (void *)keyboard);
        }

        tx_to_via(keyboard, NULL_RESPONSE);
    } else {
        LOG(3, "keyboard_tx_callback: no data, setting tx_pending=true");
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
        remove_event(keyboard->scheduler, &keyboard_timeout_callback, (void *)keyboard);
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
    // The full table is docs/core/peripherals/keyboard.md §6.4, derived from
    // Guide to the Macintosh Family Hardware 2e Figure 7-6 (p.282).
    //
    // Three prefix forms, not two.  Guide 2e p.283 (:6694):
    //
    //   "If a key transition occurs for one of the arrow keys -- which are
    //    lowercase keys on the separate keypad -- the Macintosh Plus keyboard
    //    responds to an Inquiry command by sending back the Keypad response
    //    ($79) followed by the code shown in Figure 7-6.  If a key transition
    //    occurs on the Macintosh Plus numeric keypad for the plus sign (+),
    //    asterisk (*), or slash (/) keys -- which are UPPERCASE keys on the
    //    separate keypad -- the Macintosh Plus keyboard responds ... by
    //    sending back the Shift key-down transition response ($71), followed
    //    by the Keypad response ($79), followed by the code."
    //
    // The Shift prefix is load-bearing, not decorative: on a Plus the keypad
    // symbol and its arrow SHARE a raw code, and after the Keyboard Driver's
    // conversion ($40 + ((raw & $7F) >> 1), p.282) they share a KEY code too.
    // Shift is the only thing that tells them apart.
    uint8_t raw_code;
    enum { PREFIX_NONE, PREFIX_KEYPAD, PREFIX_SHIFT_KEYPAD } prefix = PREFIX_NONE;

    // One rule and one exception table, rather than 270 lines of switch.
    //
    // THE RULE (Guide 2e p.282): every main-keyboard key's Plus raw code is
    // `(adb << 1) | 1`.  Verified against all 56 entries of the switch this
    // replaces -- zero exceptions.  The Keyboard Driver inverts it by
    // stripping bit 7 and shifting right, which is why the round trip works.
    //
    // THE EXCEPTIONS are the 22 keys that are physically on the separate
    // keypad, where the raw code is SHARED with a main-keyboard key and a
    // prefix selects the keypad meaning.  Those cannot be derived; they are
    // Figure 7-6, transcribed.  The prefix column is the part that was wrong
    // until 2026-09-21 -- see the enum above.
    //
    // THE GAPS below $3B are deliberate: $36 is Control, which the M0110A
    // does not have; $0A is the ISO section/plus-minus key, a real (small)
    // gap for international layouts; $34 is unassigned on ADB.
    static const struct {
        uint8_t adb;
        uint8_t raw;
        uint8_t prefix;
    } keypad_aliases[] = {
        // Arrow keys -- lowercase keys on the separate keypad, so $79 only.
        // Each appears twice because the ADB extended keyboard reports them
        // in both the $3B-$3E and $7B-$7E ranges.
        {0x3B, 0x0D, PREFIX_KEYPAD      },
        {0x7B, 0x0D, PREFIX_KEYPAD      }, // Left
        {0x3C, 0x05, PREFIX_KEYPAD      },
        {0x7C, 0x05, PREFIX_KEYPAD      }, // Right
        {0x3D, 0x11, PREFIX_KEYPAD      },
        {0x7D, 0x11, PREFIX_KEYPAD      }, // Down
        {0x3E, 0x1B, PREFIX_KEYPAD      },
        {0x7E, 0x1B, PREFIX_KEYPAD      }, // Up

        // Numeric keypad.  The four operators are UPPERCASE keys on the
        // separate keypad and take the $71 Shift prefix as well; without it
        // they are indistinguishable from the arrows above, with which they
        // share their raw codes.
        {0x41, 0x03, PREFIX_KEYPAD      }, // .
        {0x43, 0x05, PREFIX_SHIFT_KEYPAD}, // *   (shares Right's $05)
        {0x45, 0x0D, PREFIX_SHIFT_KEYPAD}, // +   (shares Left's  $0D)
        {0x47, 0x0F, PREFIX_KEYPAD      }, // Clear (shares X's $0F)
        {0x4B, 0x1B, PREFIX_SHIFT_KEYPAD}, // /   (shares Up's    $1B)
        {0x4C, 0x19, PREFIX_KEYPAD      }, // Enter
        {0x4E, 0x1D, PREFIX_KEYPAD      }, // -
        {0x51, 0x11, PREFIX_SHIFT_KEYPAD}, // =   (shares Down's  $11)
        {0x52, 0x25, PREFIX_KEYPAD      }, // 0
        {0x53, 0x27, PREFIX_KEYPAD      }, // 1
        {0x54, 0x29, PREFIX_KEYPAD      }, // 2
        {0x55, 0x2B, PREFIX_KEYPAD      }, // 3
        {0x56, 0x2D, PREFIX_KEYPAD      }, // 4
        {0x57, 0x2F, PREFIX_KEYPAD      }, // 5
        {0x58, 0x31, PREFIX_KEYPAD      }, // 6
        {0x59, 0x33, PREFIX_KEYPAD      }, // 7
        {0x5B, 0x37, PREFIX_KEYPAD      }, // 8
        {0x5C, 0x39, PREFIX_KEYPAD      }, // 9
    };

    // Keys the Plus keyboard actually has, as ADB virtual codes: $00-$3A
    // minus the three gaps.  Anything outside this and the alias table is a
    // key the M0110A has no equivalent for.
    static const bool plus_has_key[0x3B] = {
        [0x00] = true,
        [0x01] = true,
        [0x02] = true,
        [0x03] = true,
        [0x04] = true,
        [0x05] = true,
        [0x06] = true,
        [0x07] = true,
        [0x08] = true,
        [0x09] = true, /* $0A ISO sect: absent */
        [0x0B] = true,
        [0x0C] = true,
        [0x0D] = true,
        [0x0E] = true,
        [0x0F] = true,
        [0x10] = true,
        [0x11] = true,
        [0x12] = true,
        [0x13] = true,
        [0x14] = true,
        [0x15] = true,
        [0x16] = true,
        [0x17] = true,
        [0x18] = true,
        [0x19] = true,
        [0x1A] = true,
        [0x1B] = true,
        [0x1C] = true,
        [0x1D] = true,
        [0x1E] = true,
        [0x1F] = true,
        [0x20] = true,
        [0x21] = true,
        [0x22] = true,
        [0x23] = true,
        [0x24] = true,
        [0x25] = true,
        [0x26] = true,
        [0x27] = true,
        [0x28] = true,
        [0x29] = true,
        [0x2A] = true,
        [0x2B] = true,
        [0x2C] = true,
        [0x2D] = true,
        [0x2E] = true,
        [0x2F] = true,
        [0x30] = true,
        [0x31] = true,
        [0x32] = true,
        [0x33] = true,
        /* $34 unassigned on ADB */
        [0x35] = true, /* Escape: the Plus has no Esc key, but the driver maps
                          the formula's $6B back to $35, so it round-trips */
        /* $36 Control: the M0110A has no Control key */
        [0x37] = true,
        [0x38] = true,
        [0x39] = true,
        [0x3A] = true,
    };

    bool found = false;
    for (size_t i = 0; i < sizeof keypad_aliases / sizeof keypad_aliases[0]; i++) {
        if (keypad_aliases[i].adb == (uint8_t)host_key) {
            raw_code = keypad_aliases[i].raw;
            prefix = keypad_aliases[i].prefix;
            found = true;
            break;
        }
    }
    if (!found) {
        if (host_key >= (int)(sizeof plus_has_key / sizeof plus_has_key[0]) || !plus_has_key[host_key]) {
            LOG(1, "keyboard_update: unknown ADB virtual key 0x%02X", host_key);
            return;
        }
        raw_code = (uint8_t)((host_key << 1) | 1);
    }

    // Emit the prefix, if any.  $71 is the Shift KEY-DOWN code, so on release
    // it becomes $F1 by the same bit-7 rule as every other key -- otherwise
    // the driver's Shift latch stays down and every later keystroke arrives
    // shifted.  INFERRED: Guide 2e specifies only the key-down sequence and
    // Figure 7-6 is the key-down figure; no source we hold states what the
    // Plus sends on release of keypad + * /.  The plus-keyboard test asserts
    // Shift's KeyMap bit is clear afterwards, which catches a stuck latch
    // whichever way the real hardware behaved.
    if (prefix == PREFIX_SHIFT_KEYPAD)
        add_key_event(keyboard, (event == key_up) ? 0xF1 : 0x71);
    if (prefix != PREFIX_NONE)
        add_key_event(keyboard, 0x79);

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
        scheduler_new_cpu_event(keyboard->scheduler, &keyboard_timeout_callback, keyboard, 0, 0, NS_PER_SEC / 4);
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
        scheduler_new_cpu_event(keyboard->scheduler, &keyboard_tx_callback, keyboard, 0, 0, RX_TO_TX_DELAY);
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
    scheduler_new_event_type(scheduler, "keyboard", keyboard, "timeout", &keyboard_timeout_callback);
    scheduler_new_event_type(scheduler, "keyboard", keyboard, "tx", &keyboard_tx_callback);

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
    // Drop everything the scheduler still holds for this object before any
    // of it is torn down (proposal-scheduler-source-lifetime).
    scheduler_forget_source(keyboard->scheduler, keyboard);
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
