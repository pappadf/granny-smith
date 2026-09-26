// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// host_input.c -- see host_input.h.

#include "host_input.h"

#include "debug_mac.h"
#include "log.h"
#include "machine_profile.h"
#include "object.h"
#include "scheduler.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

LOG_USE_CATEGORY_NAME("keyboard");

// Spacing between the key events keyboard.type() queues, in nanoseconds.
// A real keyboard reports two key transitions in one Talk R0 only when both
// happened inside one poll interval; a typist's Shift-down and the key it
// shifts never do.  Queued back to back they would, and a guest that reads
// one transition per report (the Network Server Diagnostic Utility does)
// then sees Shift go down and never come up.  4 ms per event is a brisk
// 125 events/s — faster than any typist, slower than any poll loop.
#define KEY_TYPE_SPACING ((uint64_t)4 * 1000 * 1000)

// The default key-queue budget, in key-transition bytes: the ADB ring and the
// Plus's M0110A queue are both 128 and a character costs two of them (four
// when shifted), so a refused line is one that could not have survived the
// ring's drop-oldest overflow anyway.  A substrate with a smaller queue says
// so -- the Lisa's COPS FIFO is 32.
#define KEY_QUEUE_BYTES_DEFAULT 96

// The ADB raw keycode for Shift.
#define KEY_SHIFT 0x38

struct host_input {
    struct config *cfg;
    struct scheduler *sched;
    int budget; // key-transition bytes this machine's queue holds
    struct object *node; // machine.adb.keyboard
};

// A keyboard.type() key transition coming due.  The payload is an ADB
// keycode and a direction bit, which is all a transition is now that key
// identity is machine-independent (machine_profile.h).
static void host_typed_key(void *source, uint64_t data) {
    (void)source;
    system_input_key((int)((data >> 1) & 0x7F), (data & 1u) != 0);
}

static host_input_t *hi_from(struct object *self) {
    return (host_input_t *)object_data(self);
}

// === Object-model class descriptor =========================================
//
// `keyboard.press(key)` — inject a key-down + key-up via the keyboard
// subsystem. The arg is either a string name ("return", "space",
// "esc", a-z, 0-9 …) resolved by debug_mac_resolve_key_name, or an
// integer ADB raw keycode (0x00–0x7F).

// Shared arg decode for press/down/up: a key NAME or an ADB raw keycode.
// Both resolve to an ADB keycode here, once, because that is the model's
// universal key identity -- see machine_profile.h's input_key.  Substrates
// never see a name, and an integer means the same key on every machine.
//
// It used to render the integer back into "0x%02x" and hand the string down
// for each substrate to re-parse, which is how `keyboard.press 0xC8` came to
// mean an ADB keycode on a Mac and a raw COPS wire byte on a Lisa.  The wire
// form has its own method now (`keyboard.raw`).
static int keyboard_arg_keycode(const value_t *arg) {
    if (arg->kind == V_STRING)
        return arg->s ? debug_mac_resolve_key_name(arg->s) : -1;
    if (arg->kind == V_INT || arg->kind == V_UINT) {
        long long raw = (arg->kind == V_INT) ? (long long)arg->i : (long long)arg->u;
        return (raw >= 0 && raw <= 0x7F) ? (int)raw : -1;
    }
    return -1;
}

// How the key was spelled, for an error message.
static const char *keyboard_arg_text(const value_t *arg, char *buf, size_t size) {
    if (arg->kind == V_STRING)
        return arg->s ? arg->s : "";
    if (arg->kind == V_INT || arg->kind == V_UINT) {
        long long raw = (arg->kind == V_INT) ? (long long)arg->i : (long long)arg->u;
        snprintf(buf, size, "0x%02llx", raw);
        return buf;
    }
    return "?";
}

static value_t keyboard_method_press(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    char buf[8];
    const char *as_written = keyboard_arg_text(&argv[0], buf, sizeof(buf));
    int code = keyboard_arg_keycode(&argv[0]);
    if (code < 0)
        return val_err("keyboard.press: unknown key '%s'", as_written);

    // Tap (down then up) through the machine substrate: Macs inject via the
    // keyboard / Toolbox path, the Lisa via its COPS — one uniform path
    // (proposal §4.4).  A negative result here means this KEYBOARD has no
    // such key, which is a different thing from an unknown name: the Lisa has
    // no Control key and no function keys.
    if (system_input_key(code, true) < 0)
        return val_err("keyboard.press: this machine's keyboard has no '%s' key", as_written);
    system_input_key(code, false);
    LOG(3, "keyboard.press: key=%s adb=0x%02X", as_written, code);
    return val_bool(true);
}

// `keyboard.down(key)` / `keyboard.up(key)` — the two halves of press, for
// chords that hold a modifier across another key (e.g. Shift+/ to type '?').
static value_t keyboard_half(const value_t *arg, bool down, const char *what) {
    char buf[8];
    const char *as_written = keyboard_arg_text(arg, buf, sizeof(buf));
    int code = keyboard_arg_keycode(arg);
    if (code < 0)
        return val_err("keyboard.%s: unknown key '%s'", what, as_written);
    if (system_input_key(code, down) < 0)
        return val_err("keyboard.%s: this machine's keyboard has no '%s' key", what, as_written);
    LOG(3, "keyboard.%s: key=%s adb=0x%02X", what, as_written, code);
    return val_bool(true);
}

static value_t keyboard_method_down(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    return keyboard_half(&argv[0], true, "down");
}

static value_t keyboard_method_up(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    return keyboard_half(&argv[0], false, "up");
}

// `keyboard.raw(byte)` — inject one byte in the machine's own keyboard
// encoding, direction bit and all.  Deliberately not portable: it is for
// tests that drive a keyboard wire rather than press a key.  The Lisa's COPS
// carries down/up in bit 7, so `raw 0xC8` is "$48 down" and `raw 0x48` is
// "$48 up"; no Mac implements this and the error says so.
static value_t keyboard_method_raw(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    if (argv[0].kind != V_INT && argv[0].kind != V_UINT)
        return val_err("keyboard.raw: expected a byte");
    long long raw = (argv[0].kind == V_INT) ? (long long)argv[0].i : (long long)argv[0].u;
    if (raw < 0 || raw > 0xFF)
        return val_err("keyboard.raw: 0x%llx is not a byte", raw);
    if (system_input_key_raw((uint8_t)raw) < 0)
        return val_err("keyboard.raw: this machine has no raw keyboard encoding — use press/down/up with a key name");
    LOG(3, "keyboard.raw: 0x%02llX", raw);
    return val_bool(true);
}

// `keyboard.type(text)` — tap the keys that produce `text` on a US layout,
// holding Shift for the characters that need it.  Newline and tab in the
// string type Return and Tab, so a whole command line ends itself.
//
// Everything is queued at once, with no guest time in between, so a line
// longer than the machine's keyboard queue would silently lose its head to
// that queue's drop-oldest overflow.  Rather than let a test type into a
// void, cost the line first and refuse one that could not be delivered —
// callers type a line at a time and let the guest run.
//
// The budget is per machine.  It was one constant sized for the 128-byte ADB
// ring, which was harmless while type() could only reach an ADB Mac; the
// Lisa's COPS FIFO is 32 bytes, so the same line that is fine on a Quadra
// overflows there.
static value_t keyboard_method_type(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    host_input_t *hi = hi_from(self);
    if (!hi || !hi->sched)
        return val_err("keyboard.type: the machine has no keyboard");
    if (argv[0].kind != V_STRING || !argv[0].s)
        return val_err("keyboard.type: text must be a string");
    const char *text = argv[0].s;

    // Cost the line before typing any of it: a partially typed command is
    // worse than a refused one.
    size_t cost = 0;
    for (const char *p = text; *p; p++) {
        bool shift = false;
        if (debug_mac_resolve_ascii(*p, &shift) < 0)
            return val_err("keyboard.type: no US-layout key types '%c' (0x%02x)", *p, (unsigned char)*p);
        cost += shift ? 4 : 2;
    }
    if (cost > (size_t)hi->budget)
        return val_err("keyboard.type: %zu bytes of key events exceeds the %d this machine's keyboard queue can "
                       "hold — type fewer characters per call",
                       cost, hi->budget);

    // Pace the transitions in guest time, continuing after whatever a
    // previous call left in flight so a line typed in pieces still arrives
    // one transition at a time.  The queue is the record of that -- see
    // scheduler_last_event_cycles -- rather than a shadow copy of it.
    //
    // The first transition lands one spacing out, never "now": a zero delay
    // is not a schedulable event.
    uint64_t now = (uint64_t)scheduler_time_ns(hi->sched);
    uint64_t pending = (uint64_t)scheduler_last_event_ns(hi->sched, &host_typed_key);
    uint64_t at = (pending > now) ? pending + KEY_TYPE_SPACING : now + KEY_TYPE_SPACING;

    uint64_t typed = 0;
    for (const char *p = text; *p; p++) {
        bool shift = false;
        int code = debug_mac_resolve_ascii(*p, &shift);
        // data: bit 0 = down, bits 7:1 = the ADB keycode
        if (shift) {
            scheduler_new_cpu_event(hi->sched, &host_typed_key, hi, (KEY_SHIFT << 1) | 1u, 0, at - now);
            at += KEY_TYPE_SPACING;
        }
        scheduler_new_cpu_event(hi->sched, &host_typed_key, hi, ((uint64_t)code << 1) | 1u, 0, at - now);
        at += KEY_TYPE_SPACING;
        scheduler_new_cpu_event(hi->sched, &host_typed_key, hi, (uint64_t)code << 1, 0, at - now);
        at += KEY_TYPE_SPACING;
        if (shift) {
            scheduler_new_cpu_event(hi->sched, &host_typed_key, hi, KEY_SHIFT << 1, 0, at - now);
            at += KEY_TYPE_SPACING;
        }
        typed++;
    }
    LOG(3, "keyboard.type: %llu character(s), paced to %llu ns", (unsigned long long)typed, (unsigned long long)at);
    return val_uint(8, typed);
}

static const arg_decl_t keyboard_raw_args[] = {
    {.name = "byte", .kind = V_UINT, .doc = "Raw keyboard byte in the machine's own encoding"},
};

static const arg_decl_t keyboard_type_args[] = {
    {.name = "text", .kind = V_STRING, .doc = "Text to type; newline types Return, tab types Tab"},
};

static const arg_decl_t keyboard_press_args[] = {
    // V_NONE: body accepts either a name string or a numeric ADB keycode.
    {.name = "key", .kind = V_NONE, .doc = "Key name (\"return\"/\"esc\"/\"a\"/...) or ADB keycode int"},
};

static const member_t keyboard_members[] = {
    {.kind = M_METHOD,
     .name = "press",
     .doc = "Tap a key (down + up) on the emulated keyboard",
     .method = {.args = keyboard_press_args, .nargs = 1, .result = V_BOOL, .fn = keyboard_method_press}},
    {.kind = M_METHOD,
     .name = "down",
     .doc = "Hold a key down on the emulated keyboard (pair with up)",
     .method = {.args = keyboard_press_args, .nargs = 1, .result = V_BOOL, .fn = keyboard_method_down} },
    {.kind = M_METHOD,
     .name = "up",
     .doc = "Release a key held by down",
     .method = {.args = keyboard_press_args, .nargs = 1, .result = V_BOOL, .fn = keyboard_method_up}   },
    {.kind = M_METHOD,
     .name = "type",
     .doc = "Type a short line of text (US layout; newline = Return)",
     .method = {.args = keyboard_type_args, .nargs = 1, .result = V_UINT, .fn = keyboard_method_type}  },
    {.kind = M_METHOD,
     .name = "raw",
     .doc = "Inject one byte in this machine's own keyboard encoding (Lisa COPS only)",
     .method = {.args = keyboard_raw_args, .nargs = 1, .result = V_BOOL, .fn = keyboard_method_raw}    },
};

static const class_desc_t keyboard_class = {
    .name = "keyboard",
    .members = keyboard_members,
    .n_members = sizeof(keyboard_members) / sizeof(keyboard_members[0]),
};

// === Per-machine lifecycle ==================================================

host_input_t *host_input_init(struct config *cfg, struct scheduler *scheduler) {
    if (!cfg)
        return NULL;
    host_input_t *hi = (host_input_t *)calloc(1, sizeof(*hi));
    if (!hi)
        return NULL;
    hi->cfg = cfg;
    hi->sched = scheduler;

    int declared = cfg->machine && cfg->machine->substrate ? cfg->machine->substrate->key_queue_bytes : 0;
    hi->budget = declared > 0 ? declared : KEY_QUEUE_BYTES_DEFAULT;

    // Registered here, before scheduler_start, so a checkpoint taken with
    // typing in flight can bind its saved events back to this callback --
    // scheduler_start resolves the restored (source_name, event_name) pairs,
    // and an event type registered after it would be too late.
    if (hi->sched)
        scheduler_new_event_type(hi->sched, "keyboard", hi, "typed", &host_typed_key);

    // instance_data is the host_input_t: this node is per machine, and that
    // is what makes the scheduler source above legitimate.
    hi->node = object_new(&keyboard_class, hi, "keyboard");
    if (hi->node) {
        object_set_label(hi->node, "Keyboard");
        object_set_order(hi->node, 10);
        object_attach(adb_bus_object(), hi->node);
    }
    LOG(2, "keyboard object ready (queue budget %d bytes)", hi->budget);
    return hi;
}

void host_input_delete(host_input_t *hi) {
    if (!hi)
        return;
    if (hi->node) {
        object_detach(hi->node);
        object_delete(hi->node);
    }
    // Typing still in flight dies with the machine it was aimed at.
    if (hi->sched)
        scheduler_forget_source(hi->sched, hi);
    free(hi);
}
