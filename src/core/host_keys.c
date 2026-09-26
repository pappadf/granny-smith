// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// host_keys.c
// See host_keys.h.

#include "host_keys.h"

#include <stddef.h>
#include <string.h>

#define ADB_CONTROL  0x36
#define ADB_COMMAND  0x37
#define ADB_CAPSLOCK 0x39

// DOM KeyboardEvent.code -> ADB raw keycode (Inside Macintosh Vol V's key
// numbering; the raw and virtual codes differ only for the arrows and the
// right-hand modifiers).  Both left and right Shift/Option/Command/Control map
// to the one key an Apple keyboard reports.  The arrows are the RAW codes
// $3B-$3E: raw $7B-$7D are the right-hand modifiers, and sending those made
// arrows press phantom modifiers on every ADB machine (found by Quake ignoring
// the web UI's arrows while a scripted $3D worked).
static const struct {
    const char *dom;
    uint8_t adb;
} dom_to_adb[] = {
    {"KeyA",           0x00},
    {"KeyS",           0x01},
    {"KeyD",           0x02},
    {"KeyF",           0x03},
    {"KeyH",           0x04},
    {"KeyG",           0x05},
    {"KeyZ",           0x06},
    {"KeyX",           0x07},
    {"KeyC",           0x08},
    {"KeyV",           0x09},
    {"KeyB",           0x0B},
    {"KeyQ",           0x0C},
    {"KeyW",           0x0D},
    {"KeyE",           0x0E},
    {"KeyR",           0x0F},
    {"KeyY",           0x10},
    {"KeyT",           0x11},
    {"KeyU",           0x20},
    {"KeyI",           0x22},
    {"KeyO",           0x1F},
    {"KeyP",           0x23},
    {"KeyL",           0x25},
    {"KeyJ",           0x26},
    {"KeyK",           0x28},
    {"KeyN",           0x2D},
    {"KeyM",           0x2E},
    {"Digit1",         0x12},
    {"Digit2",         0x13},
    {"Digit3",         0x14},
    {"Digit4",         0x15},
    {"Digit5",         0x17},
    {"Digit6",         0x16},
    {"Digit7",         0x1A},
    {"Digit8",         0x1C},
    {"Digit9",         0x19},
    {"Digit0",         0x1D},
    {"Minus",          0x1B}, // - / _
    {"Equal",          0x18}, // = / +
    {"BracketLeft",    0x21}, // [ / {
    {"BracketRight",   0x1E}, // ] / }
    {"Backslash",      0x2A}, // \ / |
    {"Semicolon",      0x29}, // ; / :
    {"Quote",          0x27}, // ' / "
    {"Comma",          0x2B}, // , / <
    {"Period",         0x2F}, // . / >
    {"Slash",          0x2C}, // / / ?
    {"Backquote",      0x32}, // ` / ~
    {"IntlBackslash",  0x0A}, // § / ± (non-US ISO layout)
    {"Tab",            0x30},
    {"Space",          0x31},
    {"Backspace",      0x33}, // Delete key on Mac
    {"Enter",          0x24}, // Return
    {"Escape",         0x35},
    {"ControlLeft",    0x36},
    {"ControlRight",   0x36},
    {"ShiftLeft",      0x38},
    {"ShiftRight",     0x38},
    {"CapsLock",       0x39},
    {"AltLeft",        0x3A}, // Option (left)
    {"AltRight",       0x3A}, // Option (right)
    {"MetaLeft",       0x37}, // Command (left)
    {"MetaRight",      0x37}, // Command (right)
    {"OSLeft",         0x37}, // Command (Windows key)
    {"OSRight",        0x37}, // Command (Windows key)
    {"ArrowLeft",      0x3B},
    {"ArrowRight",     0x3C},
    {"ArrowDown",      0x3D},
    {"ArrowUp",        0x3E},
    {"NumpadDecimal",  0x41}, // Keypad .
    {"NumpadMultiply", 0x43}, // Keypad *
    {"NumpadAdd",      0x45}, // Keypad +
    {"NumLock",        0x47}, // Keypad Clear
    {"NumpadDivide",   0x4B}, // Keypad /
    {"NumpadEnter",    0x4C}, // Keypad Enter
    {"NumpadSubtract", 0x4E}, // Keypad -
    {"NumpadEqual",    0x51}, // Keypad =
    {"Numpad0",        0x52},
    {"Numpad1",        0x53},
    {"Numpad2",        0x54},
    {"Numpad3",        0x55},
    {"Numpad4",        0x56},
    {"Numpad5",        0x57},
    {"Numpad6",        0x58},
    {"Numpad7",        0x59},
    {"Numpad8",        0x5B},
    {"Numpad9",        0x5C},
};

int host_keymap_dom_to_adb(const char *dom_code) {
    if (!dom_code)
        return -1;
    for (size_t i = 0; i < sizeof(dom_to_adb) / sizeof(dom_to_adb[0]); i++)
        if (strcmp(dom_code, dom_to_adb[i].dom) == 0)
            return dom_to_adb[i].adb;
    return -1;
}

bool host_keys_down(host_keys_t *k, int adb_code, host_key_sink_t sink) {
    if (adb_code < 0 || adb_code > 0x7F)
        return false;
    if (k->sent[adb_code])
        return true; // a repeat: the key is already down, as delivered
    int sent = adb_code;
    int rc = sink(adb_code, true);
    if (rc < 0 && adb_code == ADB_CONTROL) {
        sent = ADB_COMMAND;
        rc = sink(ADB_COMMAND, true);
    }
    if (rc < 0)
        return false;
    k->sent[adb_code] = (uint8_t)(sent + 1);
    return true;
}

bool host_keys_up(host_keys_t *k, int adb_code, host_key_sink_t sink) {
    if (adb_code < 0 || adb_code > 0x7F || !k->sent[adb_code])
        return false;
    int sent = k->sent[adb_code] - 1;
    k->sent[adb_code] = 0;
    sink(sent, false);
    return true;
}

void host_keys_release_all(host_keys_t *k, host_key_sink_t sink) {
    for (int code = 0; code < 128; code++)
        if (code != ADB_CAPSLOCK && k->sent[code])
            host_keys_up(k, code, sink);
}
