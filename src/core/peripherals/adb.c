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
#include "checkpoint.h"
#include "debug_mac.h"
#include "keyboard.h"
#include "log.h"
#include "machine_profile.h"
#include "mouse.h"
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
#define ADB_SHIFT_DELAY ((uint64_t)800 * 1000)

// Delay (nanoseconds) before delivering the next reply byte via the VIA shift
// register.  On real hardware the ADB transceiver completes a full command–
// response cycle in ~3 ms (800 µs attention + 800 µs command + stop-to-start
// time + 800 µs response data).  We use 2.64 ms to match the deferred-delivery
// timing in keyboard.c (RX_TO_TX_DELAY = 330 * 8 * 1000 ns).  The delay must
// be long enough for the ROM's VBL handler (which enables interrupts mid-flight)
// to finish setting up the ADB state machine callback pointers before the next
// SR interrupt fires.
#define ADB_BYTE_DELAY (330 * 8 * 1000)

// Default ADB device addresses assigned at power-on
#define KBD_DEFAULT_ADDR   2
#define MOUSE_DEFAULT_ADDR 3

// Standard handler IDs (single-button mouse and extended keyboard both use 0x01)
#define KBD_HANDLER_ID   0x01
#define MOUSE_HANDLER_ID 0x01

// The other handler ID the classic two-byte Apple Desktop Bus Mouse answers
// to: $02 is the 200 cpi variant of the same report format.
#define MOUSE_HANDLER_ID_200CPI 0x02

// Handler $04: the Apple Extended Mouse protocol.  Register 0 grows to four
// bytes -- buttons 2-5 and delta bits 7-12 in bytes 2 and 3 -- and Register 1
// holds an 8-byte device record (ID, resolution, class, button count).  The
// Cursor Device Manager in the 1992-and-later ROMs (CrsrDev.a, CrsrDevReInit)
// asks every mouse for it, reads Register 1, and decodes the wide deltas in
// CrsrDevHandleADB, so one report can carry a whole frame's motion.
#define MOUSE_HANDLER_ID_EXTENDED 0x04

// The extended mouse's Register 1 record: what CrsrDevReInit copies into its
// CrsrDevRec.  72 counts per inch is the resolution it assumes for a plain
// handler-1 mouse, so one count is still one pixel; class 1 is classMouse.
static const uint8_t k_mouse_reg1[8] = {'g', 's', 'm', 's', 0x00, 72, 0x01, 0x01};

// The widest delta a 4-byte extended report carries: 13-bit two's complement.
#define MOUSE_EXT_DELTA_MAX 4095

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
    // Register 3 bit 13, Service Request enable.  Real per-device state, not
    // a constant: Guide 2e :7810-7812 -- "To disable a device's ability to
    // send a Service Request signal, set bit 13 in register 3 to 0 by using a
    // Listen Register 3 command with a Device Handler ID of $00... To enable
    // the Service Request ability, set this bit to 1."  Powers up set
    // (Table 8-15 marks bit 14 "always 1 if not used" and bit 13 the SRQ
    // enable; a device that never had it turned off can service-request).
    bool srq_enabled;
} adb_device_t;

// Full ADB transceiver state; plain data placed first so the checkpoint
// boundary at offsetof(adb_t, via) works correctly (same pattern as keyboard.c)
struct adb {
    // === Plain data (checkpointed via memcpy up to the 'via' pointer) ===

    // Current VIA transaction state (0-3), extracted from port B bits 5:4
    int state;

    // Reply buffer for the current Talk command (8 bytes: an ADB register's
    // most; the extended mouse sends 4, its Register 1 record 8)
    uint8_t reply_buf[8]; // bytes to deliver to the OS
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
    // When each held key went down (a running count), so a re-report after
    // a bus reset or Flush replays the holds in the order they were made.
    uint32_t kbd_press_seq[128];
    uint32_t kbd_press_count;

    // Mouse state: deltas accumulated since last Talk R0; reset after each report
    int mouse_dx;
    int mouse_dy;
    bool mouse_button; // current button state (true = pressed)
    bool mouse_data_pending; // set by adb_mouse_event, cleared after auto-poll delivery
    // The host has read Register 1 while the mouse is in handler $04: its
    // driver knows the extended format, so reports may use the wide deltas.
    // MkLinux DR3 selects handler $04 for its middle and right buttons but
    // never reads Register 1 and decodes only bits 0-6 of each delta, so
    // until then reports stay within the classic +-63 (adb_mouse.c, POS()).
    bool mouse_ext_identified;

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

    // Aborted-keyboard-Talk recovery.  prepare_kbd_reply destructively dequeues
    // up to 2 bytes whenever the ROM issues a Talk R0 to the keyboard — but the
    // ROM probes the keyboard during its SRQ scan and may transition CMD->IDLE
    // without ever fetching those bytes.  These two fields let the IDLE handler
    // un-do the dequeue (dequeue only advances tail; the bytes are still in the
    // ring buffer) so the key transitions are re-presented on the next poll
    // instead of being silently lost.  Real ADB keyboards retain unread data.
    bool reply_from_kbd_queue; // reply_buf holds freshly-dequeued keyboard bytes
    unsigned int kbd_reply_tail; // kbd_queue.tail snapshot taken before the dequeue
    bool reply_from_mouse; // reply_buf holds freshly-consumed mouse deltas
    int mouse_reply_dx, mouse_reply_dy; // the consumed deltas (for abort restore)

    // The most recently used ADB address, in the IOP ADB Driver ERS's sense:
    // the one that answered the last autonomous auto-poll.  adb_autopoll_next
    // anchors its scan here.  Plain data, so it checkpoints with the rest.
    uint8_t autopoll_mru;

    // The VIA-side transceiver's idle Talk: when the last one started (paces
    // the next, adb_poll_wait_ns) and whether one is scheduled.
    double idle_talk_start_ns;
    bool idle_talk_pending;

    // Shadow of the last VIA1 port-B output, for the ST-transition filter in
    // adb_port_b_output.  It lived in four machine-state structs -- se30_t,
    // iicx/iix, iici and q700 -- each with its own copy of the filter and,
    // in three of the four, no comment saying what the filter was for.  None
    // of those copies was checkpointed: all three initialisers set it to
    // $30 at machine init and a restore simply got $30 back, whatever the
    // VIA's actual ORB was.  Here it rides in adb_t's plain-data block and
    // round-trips exactly.
    uint8_t last_port_b;

    // === Pointers last (not checkpointed) ===
    via_t *via;
    struct scheduler *scheduler;
    // Told when a device gets new data (adb_set_data_hook)
    void (*data_hook)(void *ctx);
    void *data_hook_ctx;
};

// ============================================================================
// Forward Declarations
// ============================================================================

static void adb_reset(adb_t *adb);
static void adb_deliver_next_byte(adb_t *adb);
static void adb_deliver_next_byte_deferred(void *source, uint64_t data);
static void adb_shift_complete_deferred(void *source, uint64_t data);

static void adb_idle_talk_event(void *source, uint64_t data);
static void device_data_arrived(adb_t *adb);
static int report_bytes(const adb_t *adb, uint8_t addr);
static void adb_decode_command(adb_t *adb, uint8_t cmd);

// ============================================================================
// Static Helpers
// ============================================================================

// Enqueues one byte into the keyboard ring buffer; drops the oldest on overflow
static inline unsigned kbd_queue_next(unsigned i) {
    return (i + 1 == KBD_QUEUE_SIZE) ? 0 : i + 1;
}

// Take one byte out of the middle of the ring, closing the gap behind it.
static void kbd_queue_remove(adb_t *adb, unsigned idx) {
    unsigned i = idx;
    for (unsigned next = kbd_queue_next(i); next != adb->kbd_queue.head; next = kbd_queue_next(next)) {
        adb->kbd_queue.buf[i] = adb->kbd_queue.buf[next];
        i = next;
    }
    adb->kbd_queue.head = i;
}

// Drop the oldest queued byte that satisfies `want`, if there is one.
static bool kbd_queue_drop_first(adb_t *adb, bool (*want)(uint8_t, uint8_t), uint8_t arg) {
    for (unsigned i = adb->kbd_queue.tail; i != adb->kbd_queue.head; i = kbd_queue_next(i)) {
        if (want(adb->kbd_queue.buf[i], arg)) {
            kbd_queue_remove(adb, i);
            return true;
        }
    }
    return false;
}

static bool kbd_byte_is_press(uint8_t b, uint8_t arg) {
    (void)arg;
    return (b & 0x80u) == 0u; // Register 0 bit 7: 0 = down, 1 = up
}

static bool kbd_byte_same_key(uint8_t b, uint8_t arg) {
    return b == arg; // an identical transition already queued: dropping it changes nothing
}

// WHICH BYTE GOES WHEN THE QUEUE IS FULL decides whether the guest loses a
// character or is left with a stuck modifier, and the two are not close.
//
// Register 0 reports a release as bit 7 of the keycode, so a dropped RELEASE
// leaves the guest holding a key it will never see let go.  For Shift that
// silently converts every character after it: in a digits-only field the
// digits arrive as `!@#$%^&*()` and the field discards them without a sound,
// which reads exactly like software rejecting the input.  Nothing corrects it
// either, because this side's kbd_pressed[] has already moved on -- the two
// halves disagree from then on.  A dropped PRESS costs one character and
// nothing else.
//
// So a release always gets in, at the cost of the oldest press still queued;
// and a press that will not fit is refused rather than displacing anything.
// Dropping a press does leave its own release to be delivered unpaired, which
// a guest treats as a release of a key it does not think is held -- harmless,
// and the benign direction of the two.
//
// Threading: the queue has no locking, and the full-queue paths move the
// READ side (tail) from the writer.  That is sound because every producer
// (host key events, delivered through the shell/input layer) and the
// consumer (the transceiver's Talk replies) run on the emulator thread.
static void kbd_enqueue(adb_t *adb, uint8_t byte) {
    unsigned int head = kbd_queue_next(adb->kbd_queue.head);
    if (head == adb->kbd_queue.tail) {
        if (!(byte & 0x80u)) {
            LOG(1, "kbd_queue full, refusing key-down $%02X rather than displace a queued release", byte);
            return;
        }
        if (kbd_queue_drop_first(adb, kbd_byte_is_press, 0)) {
            LOG(1, "kbd_queue full, dropped the oldest key-down to make room for release $%02X", byte);
        } else if (kbd_queue_drop_first(adb, kbd_byte_same_key, byte)) {
            // Nothing but releases left, so a press was refused earlier and its
            // release is now unpaired.  An identical one is already waiting;
            // dropping that costs nothing and leaves every OTHER key's release
            // where it is -- which is the whole point of the exercise.
            LOG(1, "kbd_queue full of releases, dropped a duplicate $%02X", byte);
        } else {
            // Every entry is a release for a different key, which would mean
            // more keys held than a keyboard has.  Refuse rather than evict
            // someone else's release.
            LOG(1, "kbd_queue full of distinct releases, refusing $%02X", byte);
            return;
        }
        head = kbd_queue_next(adb->kbd_queue.head);
    }
    adb->kbd_queue.buf[adb->kbd_queue.head] = byte;
    adb->kbd_queue.head = head;
    device_data_arrived(adb);
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

// Clamps a mouse axis delta to ADB's 7-bit signed range (-64..+63), carrying
// the remainder.  The shared helper is in mouse.h; the range is ADB's own.
static int clamp_delta(int delta, int *remaining) {
    return input_clamp_delta(delta, -64, 63, remaining);
}

// Encodes a clamped delta into ADB's 7-bit signed format (2's complement)
static uint8_t encode_delta(int clamped) {
    return (uint8_t)(clamped & 0x7F);
}

// Drives the vADBInt line on VIA1 port B bit 3: high = idle/continue, low = SRQ/done.
// On IOP-based machines (Macintosh IIfx) the ADB module is initialised with a
// NULL VIA pointer because the SWIM IOP drives the bus directly; this is a no-op
// in that case — IRQ delivery from RcvMsg[3] is the IOP's job.
static void set_adb_int(adb_t *adb, bool high) {
    if (!adb || !adb->via)
        return;
    via_input(adb->via, 1, ADB_INT_PIN, high);
}

// Extracts the ADB state (ST1:ST0) from a VIA1 port B output value
static int extract_state(uint8_t port_b_val) {
    // ST0 is bit 4, ST1 is bit 5; together they form a 2-bit state index
    return (port_b_val >> ADB_ST0_BIT) & 0x03;
}

// Returns true if there is mouse or keyboard data worth reporting via auto-poll.
static bool has_pending_data(const adb_t *adb) {
    return !kbd_queue_empty(adb) || adb->mouse_data_pending;
}

// Returns true if the device at `addr` both has data AND is allowed to say so
// unasked -- Register 3 bit 13, the Service Request enable.  This is what the
// bit MEANS: as real per-device state, a device with it clear must stop
// triggering the SRQ path, or the state is cosmetic readback and
// the host's SetSRQ has no effect.  A device with SRQ off is still polled and
// still answers; it simply cannot interrupt to announce itself.
static bool device_can_service_request(const adb_t *adb, uint8_t addr);

// Returns true if the device at the given ADB address has unreported data.
// On real hardware, a device with no pending data simply doesn't respond to
// Talk R0 — the transceiver sees a timeout and stays quiet.  For the mouse
// that means motion or a button CHANGE: a button held still is reported once,
// by the report of the press, and every report after it carries the level.
static bool device_has_pending_data(const adb_t *adb, uint8_t addr) {
    if (addr == adb->kbd.address)
        return !kbd_queue_empty(adb);
    if (addr == adb->mouse.address)
        return adb->mouse_data_pending;
    return false;
}

static bool device_can_service_request(const adb_t *adb, uint8_t addr) {
    if (addr == adb->kbd.address)
        return !kbd_queue_empty(adb) && adb->kbd.srq_enabled;
    if (addr == adb->mouse.address) {
        // NEW data only: motion or a button change, the same data that
        // makes the mouse answer Talk R0.
        return adb->mouse_data_pending && adb->mouse.srq_enabled;
    }
    return false;
}

// True if any device OTHER than `except` is service-requesting.
static bool other_device_service_requesting(const adb_t *adb, uint8_t except) {
    for (uint8_t addr = 0; addr < 16; addr++)
        if (addr != except && device_can_service_request(adb, addr))
            return true;
    return false;
}

// ADB virtual key codes of the keys that Register 2 reports.  Caps Lock is the
// one that matters for booting Copland: on a real Apple keyboard it is a
// mechanically LOCKING switch, and so is the only one that can already be down
// when the machine is powered on.  Two consequences, both modelled: the OS can
// read the latch here (this register is the only place it is visible as state),
// and the latch survives a bus reset (see adb_reset).
#define ADB_KEY_DELETE   0x33
#define ADB_KEY_CAPSLOCK 0x39
#define ADB_KEY_CONTROL  0x36
#define ADB_KEY_SHIFT    0x38
#define ADB_KEY_OPTION   0x3A
#define ADB_KEY_COMMAND  0x37
#define ADB_KEY_CLEAR    0x47 // keypad Clear, doubles as Num Lock
#define ADB_KEY_F14      0x6B // doubles as Scroll Lock

// Re-reports every key that is still held on Register 0 after something has
// cleared the keyboard's idea of what it last sent — a bus reset or a Flush.
// A matrix-scanning keyboard reports a change against its own cleared state,
// so a switch that is still closed produces a fresh key-down on the next
// scan.  For Caps Lock that switch is mechanically locking; for any other key
// it is a finger holding it down through the reset, which is exactly what the
// power-on key combinations are: Command-Option-O-F (Open Firmware),
// Command-Option-P-R (zap PRAM), Shift (extensions off).  The person presses
// the keys, then the power button; the ROM's ADB init comes afterwards and
// can only learn about them from this re-report.
//
// Both events need it, and the second is the one that is easy to miss.  The
// ROM's ADB init does SendReset, enumerates by shuffling addresses through 15,
// and then Flushes each device to drop stale data — so a key-down replayed at
// the reset is thrown away a few hundred microseconds later and never reaches
// software.
//
// This behaviour is deduced rather than documented, so here is the argument.
// Copland's boot blocks decide with `btst #1,($017B).w`, KeyMap's bit for key
// 0x39; classic Mac OS builds KeyMap only from the Register 0 transition
// stream, and this ROM issues zero Talk R2 commands across a whole boot
// (measured).  Apple's own instructions are "put down the Caps Lock key" and
// then restart, and that worked on the real machines.  For all three to be
// true, the keyboard must report a still-latched Caps Lock after the ADB init
// has cleared it — there is no other path from the switch to that bit.  The
// claim is falsifiable and was falsified in the useful direction: with this,
// the latch diverts the boot into the NuKernel loader; without it, the machine
// starts System 7.5 and the bit is never set.  The beige G3's Open Firmware
// makes the same argument for momentary keys: it builds its key map from the
// Register 0 stream after its own ADB reset, and Command-Option-O-F held from
// power-on stops it at the prompt only with the held keys re-reported.
//
// The keys come back in the order they went down.  Firmware that looks for a
// combination can test it on the last key's key-down — the beige G3's Open
// Firmware stops at its prompt for Command-Option-O-F only when F arrives
// after O — and a person holding a combination pressed it in that order.
static void kbd_rereport_held(adb_t *adb, const char *why) {
    bool sent[128] = {false};
    for (;;) {
        int next = -1;
        for (unsigned k = 0; k < 128; k++)
            if (adb->kbd_pressed[k] && !sent[k] && (next < 0 || adb->kbd_press_seq[k] < adb->kbd_press_seq[next]))
                next = (int)k;
        if (next < 0)
            return;
        sent[next] = true;
        LOG(2, "%s: key $%02X is still held: re-reporting the key-down", why, next);
        kbd_enqueue(adb, (uint8_t)next);
    }
}

// Where the keyboard and mouse currently live on the bus (Listen R3 moves
// them; see adb.h).
uint8_t adb_keyboard_address(adb_t *adb) {
    return adb ? adb->kbd.address : 2;
}

uint8_t adb_mouse_address(adb_t *adb) {
    return adb ? adb->mouse.address : 3;
}

uint16_t adb_device_mask(const adb_t *adb) {
    if (!adb)
        return 0;
    return (uint16_t)((1u << (adb->kbd.address & 0x0F)) | (1u << (adb->mouse.address & 0x0F)));
}

// True if this address may be polled under `mask`.  A zero mask means the
// host has not installed one, so nothing is excluded.
static bool autopoll_addr_enabled(uint16_t mask, uint8_t addr) {
    return mask == 0 || (mask & (1u << addr)) != 0;
}

// The first address from `first` round that may be polled under `mask` and
// has data, or -1.
static int autopoll_first_with_data(const adb_t *adb, uint16_t mask, uint8_t first) {
    for (int step = 0; step < 16; step++) {
        uint8_t addr = (uint8_t)((first + step) & 0x0F);
        if (autopoll_addr_enabled(mask, addr) && device_has_pending_data(adb, addr))
            return addr;
    }
    return -1;
}

// True if any address other than `except` is service-requesting under `mask`.
// This is the model's stand-in for the ADB bus's Service Request line, which
// nothing here drives: a device with data AND Register 3 bit 13 set is a
// device that would be pulling SRQ low.
static bool autopoll_others_pending(const adb_t *adb, uint16_t mask, uint8_t except) {
    for (uint8_t addr = 0; addr < 16; addr++) {
        if (addr == except || !autopoll_addr_enabled(mask, addr))
            continue;
        if (device_can_service_request(adb, addr))
            return true;
    }
    return false;
}

// Which address an auto-poll would hear from now, or -1.
static int autopoll_pick(const adb_t *adb, uint16_t enable_mask) {
    uint8_t mru = (uint8_t)(adb->autopoll_mru & 0x0F);

    // The Apple IOP ADB Driver ERS:
    // "it will poll the most recently used device (which has its polling
    // enable bit set) until it receives data, or until another device asserts
    // Service Request.  To handle a service request, it will start polling
    // all of the other devices ... in most recently used order until it hits
    // one that returns data.  If after polling all of the enabled devices,
    // SRQ is active, and no data was received from any of the devices, SRQ
    // polling will continue, polling ALL device addresses, ignoring the
    // enable mask."
    //
    // Three clauses, three scans.  The only liberty taken is that the ERS's
    // per-address MRU CHAIN collapses to "start at the MRU address and go
    // round": the chain is permuted only by which device replied, and with
    // no SRQ line to make the intermediate hops observable, the order in
    // which empty addresses are visited cannot be seen by any guest.  What
    // IS observable -- who is re-polled, and who wins when two devices have
    // data at once -- is exactly what the clauses below decide.
    //
    // Honouring the SRQ clause matters, and is not pedantry: without it the
    // rule is "re-poll the MRU device", and a mouse in continuous motion
    // always has data, so typing while dragging would never be delivered.
    if (autopoll_addr_enabled(enable_mask, mru) && device_has_pending_data(adb, mru) &&
        !autopoll_others_pending(adb, enable_mask, mru))
        return mru; // Clause 1: the MRU device, and nobody else is asking.

    // Clause 2: somebody else is asking (or the MRU has nothing) -- walk the
    // others, MRU-relative, starting past the MRU address.
    int answered = autopoll_first_with_data(adb, enable_mask, (uint8_t)((mru + 1) & 0x0F));

    // Clause 3: nothing enabled answered.  If an address outside the mask has
    // data, SRQ is still asserted as far as the bus is concerned, so poll
    // everything.  (Skipped when there is no mask: that scan just ran.)
    if (answered < 0 && enable_mask != 0)
        answered = autopoll_first_with_data(adb, 0, (uint8_t)((mru + 1) & 0x0F));
    return answered;
}

// The Register 0 report the device at `addr` sends: the extended mouse's is
// 4 bytes, everything else's 2.
static int report_bytes(const adb_t *adb, uint8_t addr) {
    if (addr == adb->mouse.address && adb->mouse.handler == MOUSE_HANDLER_ID_EXTENDED)
        return 4;
    return 2;
}

uint64_t adb_autopoll_talk_ns(const adb_t *adb, uint16_t enable_mask) {
    int addr = adb ? autopoll_pick(adb, enable_mask) : -1;
    return addr < 0 ? 0 : adb_talk_ns(report_bytes(adb, (uint8_t)addr));
}

bool adb_autopoll_next(adb_t *adb, uint16_t enable_mask, uint8_t *cmd_out, uint8_t *out_data, int *len_out) {
    if (!adb || !cmd_out || !out_data || !len_out)
        return false;
    int addr = autopoll_pick(adb, enable_mask);
    if (addr < 0)
        return false;
    uint8_t cmd = (uint8_t)((addr << 4) | 0x0C); // Talk register 0
    int n = 0;
    if (!adb_iop_transact(adb, cmd, NULL, 0, out_data, &n) || n <= 0)
        return false;
    *cmd_out = cmd;
    *len_out = n;
    adb->autopoll_mru = (uint8_t)addr;
    return true;
}

// Resets all ADB devices to power-on defaults and clears all data queues
static void adb_reset(adb_t *adb) {
    LOG(2, "adb_reset: resetting all devices to defaults");

    adb->kbd.address = KBD_DEFAULT_ADDR;
    adb->kbd.handler = KBD_HANDLER_ID;
    adb->kbd.srq_enabled = true;
    adb->mouse.address = MOUSE_DEFAULT_ADDR;
    adb->mouse.handler = MOUSE_HANDLER_ID;
    adb->mouse.srq_enabled = true;

    kbd_queue_reset(adb);

    // A bus reset releases nothing the user is still holding: kbd_pressed[]
    // is the physical switch state (a latched Caps Lock, a finger on a key),
    // so it is kept, Register 2 keeps reporting it, and the scan after the
    // reset re-reports each held key on Register 0 (kbd_rereport_held).
    kbd_rereport_held(adb, "reset");

    adb->mouse_dx = 0;
    adb->mouse_dy = 0;
    adb->mouse_button = false;
    adb->mouse_data_pending = false;
    adb->mouse_ext_identified = false;
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
        kbd_rereport_held(adb, "flush_device");
    } else if (addr == adb->mouse.address) {
        LOG(2, "flush_device: flushing mouse at addr %d", addr);
        adb->mouse_dx = 0;
        adb->mouse_dy = 0;
        // Clear the pending flag too, or device_has_pending_data() keeps
        // reporting data and the next Talk R0 delivers a zero-delta report
        // the host did not ask for.  Guide 2e: "Any user input data being
        // stored by the device ... are lost."
        //
        // mouse_button is deliberately NOT cleared: a held button is the
        // switch's state, not buffered input, so the next report (on motion)
        // still carries it.
        adb->mouse_data_pending = false;
    } else {
        LOG(2, "flush_device: unknown device at addr %d, ignoring", addr);
    }
}

// Populates reply_buf with up to 2 pending keyboard key bytes, or 0xFF 0xFF if none
static void prepare_kbd_reply(adb_t *adb) {
    // Savepoint so an aborted Talk (ROM probes during its SRQ scan but never
    // fetches the reply) can restore the queue rather than drop the bytes.
    adb->kbd_reply_tail = adb->kbd_queue.tail;
    adb->reply_from_kbd_queue = true;
    if (kbd_queue_empty(adb)) {
        // No pending events: return the idle/null response
        adb->reply_buf[0] = KBD_NO_KEY;
        adb->reply_buf[1] = KBD_NO_KEY;
        adb->reply_from_kbd_queue = false; // nothing dequeued, nothing to restore
    } else {
        adb->reply_buf[0] = kbd_dequeue(adb);
        // Pad with 0xFF if only one key is queued; otherwise deliver the second
        adb->reply_buf[1] = kbd_queue_empty(adb) ? KBD_NO_KEY : kbd_dequeue(adb);
    }
    adb->reply_len = 2;
}

// Populates reply_buf with Mouse Register 0 data.
// Only the portion of the delta the report can carry is consumed -- 7-bit
// signed, or 13-bit once an extended-mouse driver has identified the device
// -- and the remainder stays in the accumulator for subsequent polls.
static void prepare_mouse_reply(adb_t *adb) {
    bool ext = adb->mouse.handler == MOUSE_HANDLER_ID_EXTENDED;
    int remain_dy, remain_dx, dy, dx;
    if (ext && adb->mouse_ext_identified) {
        dy = input_clamp_delta(adb->mouse_dy, -MOUSE_EXT_DELTA_MAX - 1, MOUSE_EXT_DELTA_MAX, &remain_dy);
        dx = input_clamp_delta(adb->mouse_dx, -MOUSE_EXT_DELTA_MAX - 1, MOUSE_EXT_DELTA_MAX, &remain_dx);
    } else {
        dy = clamp_delta(adb->mouse_dy, &remain_dy);
        dx = clamp_delta(adb->mouse_dx, &remain_dx);
    }

    // Byte 1: bit 7 = button (1=up, 0=down); bits 6-0 = signed Y delta
    uint8_t btn_bit = adb->mouse_button ? 0x00 : 0x80; // active-low button
    adb->reply_buf[0] = btn_bit | encode_delta(dy);
    // Byte 2: bit 7 = 1 (reserved for 2nd button, always 1 on single-button mouse)
    adb->reply_buf[1] = 0x80 | encode_delta(dx);
    adb->reply_len = 2;
    if (ext) {
        // Bytes 3-4, the extended format (CrsrDev.a, CrsrDevHandleADB):
        //   b2 y9 y8 y7 b3 x9 x8 x7  /  b4 y12 y11 y10 b5 x12 x11 x10
        // Buttons 2-5 up (1); the delta bits are the two's-complement high
        // bits, all copies of the sign while the delta fits in 7 bits.
        adb->reply_buf[2] = (uint8_t)(0x88 | ((dy >> 3) & 0x70) | ((dx >> 7) & 0x07));
        adb->reply_buf[3] = (uint8_t)(0x88 | ((dy >> 6) & 0x70) | ((dx >> 10) & 0x07));
        adb->reply_len = 4;
    }

    // Keep only the unconsumed remainder
    adb->mouse_dy = remain_dy;
    adb->mouse_dx = remain_dx;

    // Savepoint the consumed deltas so an aborted Talk (ROM probes during
    // its SRQ scan but never fetches the reply) can put them back — a real
    // mouse keeps its accumulated motion until the host actually reads it.
    // Without this the re-poll rebuilds the report from the zeroed
    // accumulators and the movement is lost (frozen cursor), and a report
    // that carried only a button change is not re-presented at all.
    adb->reply_from_mouse = true;
    adb->mouse_reply_dx = dx;
    adb->mouse_reply_dy = dy;

    // Clear the pending flag if all deltas have been consumed
    if (remain_dy == 0 && remain_dx == 0)
        adb->mouse_data_pending = false;
}

// Populates reply_buf with Register 3 (Guide 2e Table 8-15, :7726-7739):
//
//   15    reserved, must be 0
//   14    exceptional event, device specific; always 1 if not used
//   13    Service Request enable; 1 = enabled
//   12    reserved, must be 0
//   11-8  device address
//   7-0   device handler ID
//
// So an ordinary idle device answers $6X, and this used to answer $0X --
// bits 14 and 13 both clear, i.e. "an exceptional event is in progress and
// I cannot service-request".  The specific value $6X is DERIVED from the
// table rather than quoted: the Guide has register-0 and register-2 content
// tables per device (8-4, 8-7, 8-8, 8-10, 8-11) but no register-3 one.
static void prepare_reg3_reply(adb_t *adb, const adb_device_t *dev) {
    uint8_t hi = 0x40; // bit 14: no exceptional event
    if (dev->srq_enabled)
        hi |= 0x20; // bit 13
    adb->reply_buf[0] = (uint8_t)(hi | (dev->address & 0x0F));
    adb->reply_buf[1] = dev->handler;
    adb->reply_len = 2;
}

// Populates reply_buf with Keyboard Register 2 (modifier keys + LEDs).
//
// Layout per docs/reference/hardware/adb.md "Register 2 (Modifier Keys)": a 0 bit
// means the key is DOWN or the LED is ON; every unused/reserved bit reads 1.
// The modifier bits are derived from kbd_pressed[] rather than kept as separate
// state, so a key held with `keyboard.down` stays reported until `keyboard.up`
// — which is exactly how a locking Caps Lock behaves.  The Caps Lock LED
// mirrors the key, as it does on real hardware.
static void prepare_kbd_reg2_reply(adb_t *adb) {
    uint16_t reg2 = 0xFFFF; // nothing pressed, no LED lit

    // Bit N clears while the corresponding key is held.
    if (adb->kbd_pressed[ADB_KEY_DELETE])
        reg2 &= (uint16_t) ~(1u << 14);
    if (adb->kbd_pressed[ADB_KEY_CAPSLOCK])
        reg2 &= (uint16_t) ~(1u << 13);
    if (adb->kbd_pressed[ADB_KEY_CONTROL])
        reg2 &= (uint16_t) ~(1u << 11);
    if (adb->kbd_pressed[ADB_KEY_SHIFT])
        reg2 &= (uint16_t) ~(1u << 10);
    if (adb->kbd_pressed[ADB_KEY_OPTION])
        reg2 &= (uint16_t) ~(1u << 9);
    if (adb->kbd_pressed[ADB_KEY_COMMAND])
        reg2 &= (uint16_t) ~(1u << 8);
    if (adb->kbd_pressed[ADB_KEY_CLEAR])
        reg2 &= (uint16_t) ~(1u << 7);
    if (adb->kbd_pressed[ADB_KEY_F14])
        reg2 &= (uint16_t) ~(1u << 6);

    // LED 2 (Caps Lock) follows the key; LEDs 1 and 3 stay off.
    if (adb->kbd_pressed[ADB_KEY_CAPSLOCK])
        reg2 &= (uint16_t) ~(1u << 1);

    adb->reply_buf[0] = (uint8_t)(reg2 >> 8);
    adb->reply_buf[1] = (uint8_t)(reg2 & 0xFF);
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
    // Cleared here; prepare_kbd_reply / prepare_mouse_reply re-set them when
    // they consume data.
    adb->reply_from_kbd_queue = false;
    adb->reply_from_mouse = false;

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
    } else if (reg == 1 && addr == adb->mouse.address && adb->mouse.handler == MOUSE_HANDLER_ID_EXTENDED) {
        // The extended mouse's device record.  Reading it is what tells the
        // mouse its driver knows the extended format (mouse_ext_identified).
        memcpy(adb->reply_buf, k_mouse_reg1, sizeof k_mouse_reg1);
        adb->reply_len = (int)sizeof k_mouse_reg1;
        adb->mouse_ext_identified = true;
        LOG(2, "talk R1 mouse: extended device record");
    } else if (reg == 2 && addr == adb->kbd.address) {
        // Keyboard Register 2 is a state register, not an event queue: it
        // always answers, whether or not any key has changed since the last
        // poll.  This is how the OS learns that Caps Lock is latched.
        prepare_kbd_reg2_reply(adb);
        LOG(2, "talk R2 kbd: [%02X %02X]", adb->reply_buf[0], adb->reply_buf[1]);
    } else {
        // Register 1 exists only on the extended mouse, and only the keyboard
        // implements Register 2; anything else reads as no device.
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
        // Register 3.  The handler byte selects a COMMAND, not a value to
        // store (ADB spec; both the ROM's enumeration and Copland's
        // FullProbe depend on it):
        //   $FE — change address (if no collision; none are modelled),
        //         PRESERVE the handler ID
        //   $00 — change address unconditionally, preserve the handler
        //   $FF — initiate self-test: no state change
        //   $FD — change address only if the activator is pressed: not
        //         modelled, no state change
        //   else — adopt the handler ID (a real device only adopts IDs it
        //         implements; ours adopt any plain ID), address unchanged
        // Storing $FE literally is how Copland's ADB server came to see
        // "not a mouse" when it read R3 back after a move, and dropped the
        // device — its boot console's "ADB device @ 0 = 3-FE" was this
        // model talking.
        adb_device_t *dev = find_device(adb, adb->listen_addr);
        if (dev) {
            uint8_t new_addr = adb->listen_buf[0] & 0x0F;
            uint8_t cmd_byte = adb->listen_buf[1];
            switch (cmd_byte) {
            case 0x00:
                // The $00 form is the one the Guide ties bit 13 to
                // (:7810-7812): "To disable a device's ability to send a
                // Service Request signal, set bit 13 in register 3 to 0 by
                // using a Listen Register 3 command with a Device Handler ID
                // of $00... To enable the Service Request ability, set this
                // bit to 1."  So this form moves the address AND sets SRQ.
                LOG(2, "listen R3 addr=%d: move to addr=%d, SRQ %s (handler preserved)", adb->listen_addr, new_addr,
                    (adb->listen_buf[0] & 0x20) ? "on" : "off");
                dev->address = new_addr;
                dev->srq_enabled = (adb->listen_buf[0] & 0x20) != 0;
                break;
            case 0xFE:
                // The collision-safe move.  Bit 13 is DELIBERATELY not taken
                // from this form.  Nothing in the Guide ties bit 13 to $FE --
                // :7810-7812 names $00 and only $00 -- and the two readings
                // are not symmetric in cost: if a host moves a device with
                // $FE and a bare address in bits 11-8, taking bit 13 from it
                // would silently switch that device's Service Request off and
                // its input would stop arriving unasked.  Leaving SRQ alone
                // here costs nothing if the guess is wrong, because the host
                // has the $00 form when it means to change the bit.
                LOG(2, "listen R3 addr=%d: move to addr=%d (handler and SRQ preserved)", adb->listen_addr, new_addr);
                dev->address = new_addr;
                break;
            case 0xFD:
            case 0xFF:
                LOG(2, "listen R3 addr=%d: command 0x%02X (no state change)", adb->listen_addr, cmd_byte);
                break;
            default:
                // A device adopts only the handler IDs it implements.  The
                // mouse is the classic Apple mouse, $01/$02, and an Apple
                // Extended Mouse, $04.  It used to adopt $04 while still
                // sending the two-byte report, and MkLinux DR3 -- whose
                // driver asks for $04 and takes the middle and right buttons
                // from the extended bytes -- lost its button in X (#144).
                // In $04 it now sends the extended report.
                if (dev == &adb->mouse && cmd_byte != MOUSE_HANDLER_ID && cmd_byte != MOUSE_HANDLER_ID_200CPI &&
                    cmd_byte != MOUSE_HANDLER_ID_EXTENDED) {
                    LOG(2, "listen R3 addr=%d: handler 0x%02X not implemented by the mouse, kept 0x%02X",
                        adb->listen_addr, cmd_byte, dev->handler);
                    break;
                }
                LOG(2, "listen R3 addr=%d: handler 0x%02X adopted", adb->listen_addr, cmd_byte);
                dev->handler = cmd_byte;
                if (dev == &adb->mouse)
                    adb->mouse_ext_identified = false; // a new driver: identified again by Register 1
                break;
            }
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
        // Type-00 sub-commands: bits 1-0 distinguish SendReset from Flush.
        // Guide to the Macintosh Family Hardware 2e, Table 8-13 "Command byte
        // syntax" (p.315):
        //
        //     x x x x 0 0 0 0   SendReset      <- address bits IGNORED
        //     A3 A2 A1 A0 0 0 0 1   Flush      <- addressed
        //     ...
        //     Note: x = ignored
        //
        // and p.316: "The SendReset command causes all devices on the network
        // to reset to their power-on states."  So $30 and $00 are the same
        // command on the wire, and resetting every device for any $X0 is
        // correct rather than over-broad.  Apple's own egretequ.a agrees
        // ("%0000xxxx SendReset (addr field ignored)").
        //
        // An earlier version of this comment cited Inside Mac V for "$X0
        // reserved for SendReset to addr X"; Table 8-13 contradicts that, and
        // narrowing $X0 to addr==0 would make the model refuse a valid
        // broadcast reset.
        //
        // Flush stays addressed: treating $X1 as a broadcast reset would wipe
        // the device-address remap set up by an earlier Listen-R3 — every Talk
        // poll after the Flush would see the original default addresses and
        // the OS would loop probing the same devices forever.
        if (reg == 1)
            flush_device(adb, addr);
        else
            adb_reset(adb);
        break;

    case CMD_TYPE_FLUSH:
        // Type-01 ($X4-$X7) is *Reserved* per Table 8-13 ("x x x x 0 1 x x"),
        // exactly as $X2 and $X3 are.  Flush is type-00 sub=01, handled above.
        // Mapping this to flush_device was a "defensive alias" that is not in
        // the spec: a host probing with $24 would get its keyboard buffer
        // flushed.  Log and ignore, the way an unimplemented encoding should.
        LOG(1, "ADB: reserved command type 01 ($%02X) ignored (Guide 2e Table 8-13)", cmd);
        break;

    case CMD_TYPE_LISTEN:
        // Arm the Listen accumulator; subsequent shift_cb calls will fill listen_buf
        adb->listen_active = true;
        adb->listen_index = 0;
        adb->listen_addr = addr;
        adb->listen_reg = reg;
        break;

    case CMD_TYPE_TALK:
        // Build the reply buffer; bytes are delivered via output_cb state
        // transitions.  Address 0 is the host's own and no device answers
        // there, so a Talk to it gets the no-reply (timeout) path like any
        // other empty address -- deliberately not special-cased.
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
    } else if (adb->reply_len == 0 && adb->state == ADB_STATE_ODD) {
        // Second byte of a no-reply Talk.  The ROM's SR handler reads vADBInt
        // per phase, not as one "done" line (universal ROM $40807002):
        //   - first EVEN byte low  = the device did not answer ($4080 70DA,
        //     fDBFlag bit 1); the byte is still read into the buffer
        //   - first ODD byte low   = Service Request from some device
        //     ($40807124: fDBFlag bit 2 + $15D bit 3 -> rescan every device)
        //   - a later EVEN byte low = end of data -> completion routine
        // So this byte must carry the bus's SRQ state, not "end of transfer":
        // pulled low here after every idle poll, the ROM set its SRQ-pending
        // bit each time and never left the scan loop ($408071F2) -- two
        // explicit Talk R0s every 12 ms for the life of the machine, and no
        // transceiver auto-poll at all.  The completion routine is never
        // reached on this path either way (no-reply exits via $4080719C /
        // $40807184), so the byte value is irrelevant.
        bool srq = other_device_service_requesting(adb, 0xFF); // any device at all
        LOG(3, "adb_deliver_next_byte: no-reply second byte (SRQ=%d)", srq);
        adb->dummy_sent = true;
        set_adb_int(adb, !srq);
        via_input_sr(adb->via, 0xFF);
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

// === The VIA-side transceiver's auto-poll ===================================
//
// In IDLE (state 3) the real transceiver repeats the last Talk R0 every ~11 ms
// and interrupts only when a device answers -- or, when another device is
// service-requesting, with SRQ.  The host sees only that interrupt, so the
// model runs the idle Talk when there is something to hear: when a device
// gets data while the bus is idle, and when the bus goes idle with data still
// waiting (the rest of a motion, or a reply the ROM walked away from).  The
// interrupt comes when that Talk would have finished on the bus.  A quiet bus
// runs nothing.
//
// (A fixed 11 ms poll used to stand in for this, and since the ROM goes idle
// after each Talk it issues to resume polling, it delayed every report by a
// full poll, and the first report after an SRQ scan by two: ~29 ms.)
static void xcvr_idle_talk(adb_t *adb) {
    if (!adb->via || adb->state != ADB_STATE_IDLE || adb->idle_talk_pending || !has_pending_data(adb))
        return;
    // The polled device answers with its report; if it has nothing, the
    // Talk times out and the transceiver sees another device's SRQ.  The
    // Talk starts once a poll period is up since the last one (adb.h).
    uint8_t addr = adb->last_poll_addr;
    int bytes = device_has_pending_data(adb, addr) ? report_bytes(adb, addr) : 0;
    double now = scheduler_time_ns(adb->scheduler);
    uint64_t wait = adb_poll_wait_ns(now, adb->idle_talk_start_ns, ADB_POLL_PERIOD_NS);
    adb->idle_talk_start_ns = now + (double)wait;
    adb->idle_talk_pending = true;
    scheduler_new_cpu_event(adb->scheduler, &adb_idle_talk_event, adb, 0, 0, wait + adb_talk_ns(bytes));
}

// Cancel a scheduled idle Talk: the ROM started a transaction of its own.
static void xcvr_cancel_idle_talk(adb_t *adb) {
    remove_event(adb->scheduler, &adb_idle_talk_event, adb);
    adb->idle_talk_pending = false;
}

// A device has new data: the VIA-side transceiver pulls SRQ and runs its idle
// Talk, and a transceiver outside this file is told (adb_set_data_hook).
static void device_data_arrived(adb_t *adb) {
    if (adb->via && adb->state == ADB_STATE_IDLE) {
        set_adb_int(adb, false);
        xcvr_idle_talk(adb);
    }
    if (adb->data_hook)
        adb->data_hook(adb->data_hook_ctx);
}

// The idle Talk has finished: the polled device's report is ready, so fire
// IFR_SR with bit3=HIGH and let the ROM's FDBShiftInt handler fetch the bytes
// via EVEN/ODD transitions -- or bit3=LOW for another device's SRQ.
//
// Important: the real transceiver always repeats the LAST Talk R0 command
// issued by the ROM (tracked in last_poll_addr).  It does NOT choose which
// device to poll.  If a non-active device has data, the transceiver signals
// SRQ (bit3=LOW) during the idle response so the ROM's SRQ handler can
// discover and poll the correct device.  Overriding the poll address broke
// the SE/30 ROM's ADB state machine because the ROM's device-handler pointer
// at $134(ADBBase) was set for last_poll_addr, not for the device we chose.
//
// The SR byte written here serves only as a wake-up: the ROM's FDBShiftInt
// ISR uses IFR_SR to enter the handler but does not process the SR value as
// data.  Actual reply bytes are read via EVEN/ODD port-B state transitions,
// so reply_index must be 0 when the idle Talk completes.
static void adb_idle_talk_event(void *source, uint64_t data) {
    (void)data;
    adb_t *adb = (adb_t *)source;
    adb->idle_talk_pending = false;

    LOG(3, "idle talk: entry state=%d pending=%d mouse_pending=%d mouse_btn=%d", adb->state, has_pending_data(adb),
        adb->mouse_data_pending, adb->mouse_button);

    // Only the VIA-side transceiver schedules this (xcvr_idle_talk); a
    // machine whose ADB is Egret, Cuda or the IOP has no VIA here.
    if (!adb->via)
        return;

    // Stale event: the ROM started a transaction meanwhile
    if (adb->state != ADB_STATE_IDLE)
        return;

    // Always repeat the last Talk R0 target, matching real transceiver behaviour.
    uint8_t poll_addr = adb->last_poll_addr;
    bool polled_device_has_data = device_has_pending_data(adb, poll_addr);

    // There is something to do only if the polled device answers, or if some
    // other device is SERVICE-REQUESTING.  The second half is where Register
    // 3 bit 13 bites: a device the host has told to stop
    // service-requesting has data nobody has asked for, and the transceiver
    // stays quiet until that device is polled again.  Before bit 13 was real
    // state the test here was has_pending_data(), which could not tell the
    // difference.
    if (!polled_device_has_data && !other_device_service_requesting(adb, poll_addr)) {
        // The real transceiver gets no response and stays quiet.  Don't fire
        // IFR_SR — the ROM remains waiting, and so does the model: new data
        // for the polled device schedules the next answer.
        LOG(3, "idle talk: nothing to report");
        return;
    }

    if (polled_device_has_data) {
        // Last-polled device has data: prepare reply and fire IFR_SR with
        // bit3=HIGH so the ROM's FDBShiftInt handler enters the data path.
        // The ROM's exit/idle handler calls the device handler at $134(ADBBase),
        // which was set up for last_poll_addr — so it matches correctly.
        // The SR byte (0xFF) is a wake-up only; actual data is delivered via
        // EVEN/ODD transitions starting from reply_index 0.
        prepare_talk_reply(adb, poll_addr, 0);
        LOG(2, "idle talk: addr=%d has data, signalling ROM", poll_addr);
        adb->reply_index = 0;
        adb->dummy_sent = false;
        set_adb_int(adb, true); // bit3=HIGH → data available
        via_input_sr(adb->via, 0xFF); // wake-up byte fires IFR_SR
    } else {
        // Last-polled device has no data, but another device does (SRQ case).
        // Signal SRQ (bit3=LOW) to prompt the ROM's SRQ handler to poll other
        // devices and discover which one needs attention.
        LOG(2, "idle talk: addr=%d no data, SRQ for other device", poll_addr);
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
    scheduler_new_event_type(scheduler, "adb", adb, "idle_talk", &adb_idle_talk_event);

    // Set device register 3 defaults and clear all queues/deltas
    adb_reset(adb);
    adb->state = ADB_STATE_IDLE;
    adb->last_port_b = 0x30; // ADB ST1:ST0 idle = 11; before the read below,
                             // so a restore overwrites it with the saved value

    if (checkpoint) {
        // Restore plain-data state; pointers are re-filled above
        size_t data_size = offsetof(adb_t, via);
        system_read_checkpoint_data(checkpoint, adb, data_size, "adb");
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

// The host-side transceiver back to its power-on transport state: idle, no
// transfer in flight, no auto-poll armed.  Without it the auto-poll timer the
// running OS left armed keeps firing through the ROM's next start-up,
// shifting stale bytes into a VIA that was just powered up.  Only a power
// cycle calls it (adb_power_on): nothing we hold puts the transceiver on the
// /RESET net.
static void adb_transceiver_reset(adb_t *adb) {
    if (!adb)
        return;
    remove_event(adb->scheduler, &adb_deliver_next_byte_deferred, adb);
    remove_event(adb->scheduler, &adb_shift_complete_deferred, adb);
    xcvr_cancel_idle_talk(adb);
    adb->state = ADB_STATE_IDLE;
    adb->listen_active = false;
    adb->listen_index = 0;
    adb->reply_len = 0;
    adb->reply_index = 0;
    adb->dummy_sent = false;
    adb->last_port_b = 0x30; // ST1:ST0 idle, as adb_init leaves it
    set_adb_int(adb, true); // vADBInt deasserted, as on a cold boot
}

// See adb.h.  The device half is adb_reset, which is also what an ADB
// SendReset does; a machine reset leaves the devices alone because they are
// not on the board's /RESET net, but a power cycle takes their power away.
void adb_power_on(adb_t *adb) {
    if (!adb)
        return;
    adb_transceiver_reset(adb);
    adb_reset(adb);
    adb->autopoll_mru = 0;
}

// Frees all resources associated with an ADB controller instance
void adb_delete(adb_t *adb) {
    if (!adb)
        return;
    // Four callbacks are scheduled with `adb` as their source; drop them all
    // before the controller is freed.
    scheduler_forget_source(adb->scheduler, adb);
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
    system_write_checkpoint_data(checkpoint, adb, data_size, "adb");
}

// ============================================================================
// VIA Callback Hooks
// ============================================================================

// Called by the machine's VIA port-B output callback when the OS changes ST0/ST1.
//
// On the SE/30 the VIA shift register operates in mode 7 (shift-out under
// external clock driven by the ADB transceiver).  Rather than waiting for the
// VIA's internal sr_shift_complete callback (which fires for every SR write,
// even spurious ones during interrupt handling), we read VIA SR directly at
// each CMD and Listen-data transition.  This matches real hardware where the
// ADB transceiver controls shift timing via CB1 (BUG-004).
void adb_port_b_output(adb_t *adb, uint8_t value) {
    // ST-transition filter.  The ROM bit-bangs the RTC on PB0-PB2 without
    // intending to touch ST1:ST0 on PB5:PB4, and on real hardware the
    // transceiver ignores writes where the ST lines do not change
    // electrically (BUG-004).  Four machines each carried this test in their
    // VIA1 port-B callback; only se30.c carried the reason.  It belongs
    // here, where the ST lines are what the module is about.
    const uint8_t st_mask = 0x30; // PB5:PB4 = ST1:ST0
    bool st_changed = ((value ^ adb->last_port_b) & st_mask) != 0;
    adb->last_port_b = value;
    if (!st_changed)
        return;

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
        xcvr_cancel_idle_talk(adb);
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
        // expectations.  Recover the un-fetched data so the next auto-poll can
        // re-present it; without this a transition is lost forever.
        if (adb->reply_len > 0 && adb->reply_index == 0 && !adb->dummy_sent) {
            if (adb->reply_from_kbd_queue) {
                // The aborted Talk was a Talk R0 to the keyboard, which already
                // dequeued up to 2 key bytes.  dequeue() only advances tail, so
                // restoring the saved tail puts those exact bytes back at the
                // front of the queue to be re-presented next poll.  This is the
                // keyboard's discrete-event analogue of the mouse re-mark below:
                // a real ADB keyboard keeps unread data until the host reads it.
                LOG(2, "IDLE: aborted kbd Talk, restoring queue tail (re-present %d byte(s))", adb->reply_len);
                adb->kbd_queue.tail = adb->kbd_reply_tail;
            } else if (adb->reply_from_mouse) {
                // Put the consumed deltas back into the accumulators
                // (additive — new motion may have arrived since the prepare).
                LOG(2, "IDLE: aborted mouse Talk, restoring deltas (dx=%d dy=%d)", adb->mouse_reply_dx,
                    adb->mouse_reply_dy);
                adb->mouse_dx += adb->mouse_reply_dx;
                adb->mouse_dy += adb->mouse_reply_dy;
                adb->reply_from_mouse = false;
                adb->mouse_data_pending = true;
            } else {
                // A register read (Talk R2 / R3) consumes nothing, so there is
                // nothing to put back -- and no device gains data from it.
                LOG(2, "IDLE: aborted register Talk (reply_len=%d), nothing to restore", adb->reply_len);
            }
        }
        xcvr_cancel_idle_talk(adb);
        set_adb_int(adb, !has_pending_data(adb));
        xcvr_idle_talk(adb); // data still waiting: the idle Talk hears it
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
        adb->kbd_press_seq[key] = ++adb->kbd_press_count;
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

    kbd_enqueue(adb, byte); // tells the transceiver (device_data_arrived)
}

// Records a host mouse event: updates accumulated deltas and current button state.
// Deltas are reset to zero after each Talk R0 reply is delivered.
void adb_mouse_event(adb_t *adb, bool button, int dx, int dy) {
    LOG(3, "adb_mouse_event: button=%d dx=%d dy=%d", button, dx, dy);

    bool button_changed = (button != adb->mouse_button);
    adb->mouse_button = button;
    adb->mouse_dx += dx;
    adb->mouse_dy += dy;

    // Motion or a button change is new data: the transceiver hears it
    if (dx != 0 || dy != 0 || button_changed) {
        adb->mouse_data_pending = true;
        device_data_arrived(adb);
    }
}

bool adb_has_data(const adb_t *adb) {
    return adb && has_pending_data(adb);
}

void adb_set_data_hook(adb_t *adb, void (*hook)(void *ctx), void *ctx) {
    if (!adb)
        return;
    adb->data_hook = hook;
    adb->data_hook_ctx = ctx;
}

// Injects mouse movement deltas without changing the current button state.
// Used by set-mouse to move the cursor through the ADB hardware path.
void adb_mouse_move(adb_t *adb, int dx, int dy) {
    adb_mouse_event(adb, adb->mouse_button, dx, dy);
}

// Deltas queued but not yet consumed by a Talk R0 — closed-loop callers
// (host absolute-position tracking computes corrections against the
// guest's cursor globals) subtract these so corrections queued while the
// guest is still catching up are not injected twice.
void adb_mouse_pending(const adb_t *adb, int *dx, int *dy) {
    if (dx)
        *dx = adb ? adb->mouse_dx : 0;
    if (dy)
        *dy = adb ? adb->mouse_dy : 0;
}

// IOP-based ADB transaction (Macintosh IIfx).  See adb.h for the protocol
// background.  Runs the same Talk / Listen / Reset / Flush dispatch as the
// VIA-shift path but returns the Talk reply by value instead of clocking
// it out byte-by-byte through via_input_sr().
//
// Note: this reuses adb_decode_command() — which leaves a few internal
// fields (reply_buf, listen_active, listen_addr/reg) set up as if a real
// VIA Talk/Listen were in progress.  That's fine because the SWIM IOP
// firmware on real hardware drives each transaction to completion before
// the next host kick, so the next call's adb_decode_command() clears the
// stale state on entry.
bool adb_iop_transact(adb_t *adb, uint8_t cmd, const uint8_t *in_data, int in_data_len, uint8_t *out_data,
                      int *out_data_len) {
    if (!adb || !out_data || !out_data_len)
        return false;
    *out_data_len = 0;

    uint8_t type = (cmd >> 2) & 0x03;

    // Run the existing command dispatch.  This populates adb->reply_buf
    // for Talk, sets listen_active/listen_addr/listen_reg for Listen,
    // or runs Reset / Flush in place.
    adb_decode_command(adb, cmd);

    if (type == CMD_TYPE_TALK) {
        if (adb->reply_len == 0)
            return false; // no device at this address
        // Bound by the SOURCE: reply_buf is 8 bytes, the most an ADB
        // register holds and the size of the caller's buffer.
        int n = adb->reply_len;
        if (n > (int)sizeof adb->reply_buf)
            n = (int)sizeof adb->reply_buf;
        memcpy(out_data, adb->reply_buf, (size_t)n);
        *out_data_len = n;
        // Drain the reply buffer so a follow-up Talk poll on the same
        // address with no new data returns NoReply (matches real-hw
        // autopoll behaviour — devices only respond once per change).
        adb->reply_len = 0;
        adb->reply_index = 0;
        return true;
    }

    if (type == CMD_TYPE_LISTEN) {
        // adb_decode_command set listen_active/listen_addr/listen_reg.
        // Feed the IOP-supplied data into listen_buf[] and run the same
        // apply_listen_data() the VIA-shift path uses.
        int n = in_data_len;
        if (n < 0)
            n = 0;
        if (n > (int)sizeof adb->listen_buf)
            n = (int)sizeof adb->listen_buf;
        if (in_data && n > 0)
            memcpy(adb->listen_buf, in_data, (size_t)n);
        adb->listen_index = n;
        apply_listen_data(adb);
        adb->listen_active = false;
        return true;
    }

    // SendReset / Flush: dispatched by adb_decode_command(), no reply data.
    return true;
}

// === ADB bus container ======================================================
//
// `machine.adb` is the logical input-device node. The physical transport is
// an implementation detail; this node just groups the two well-known devices
// — keyboard and mouse — as named children, the shape the user expects. It
// is a namespace-only process-singleton (like the keyboard/mouse facades it
// parents), created lazily under machine_object().
//
// The name is ADB but the contents are not: `keyboard.press` and `mouse.move`
// route through the machine substrate, so on a Mac Plus they reach the VIA
// shift-register keyboard and the quadrature mouse, and on a Lisa they reach
// the COPS.  Neither machine has an ADB bus.  That mismatch is known and the
// name is deliberate — it is the logical bus the user expects, not the wire
// that happens to carry it, and the path is load-bearing (AGENTS.md's
// canonical node list, ~1,588 references across the tests, the web UI and the
// docs).  Renaming it to `machine.input` with an alias was considered and
// refused.  Read this node as "input devices", not "ADB".
static const class_desc_t adb_class = {
    .name = "adb",
    .members = NULL,
    .n_members = 0,
    .doc = "Apple Desktop Bus: keyboard and mouse",
};

static struct object *s_adb_object = NULL;

struct object *adb_bus_object(void) {
    if (!s_adb_object) {
        s_adb_object = object_new(&adb_class, NULL, "adb");
        if (s_adb_object) {
            object_set_label(s_adb_object, "ADB");
            object_set_order(s_adb_object, 70);
            object_attach(machine_object(), s_adb_object);
        }
    }
    return s_adb_object;
}
