// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// host_keys.c
// See host_keys.h.

#include "host_keys.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ADB_CONTROL  0x36
#define ADB_COMMAND  0x37
#define ADB_CAPSLOCK 0x39

// One row per DOM KeyboardEvent.code: its ADB raw keycode (Inside Macintosh
// Vol V's key numbering; the raw and virtual codes differ only for the arrows
// and the right-hand modifiers) and, for the keys that type one, the US-layout
// character unshifted and with Shift.  Both left and right Shift/Option/
// Command/Control map to the one key an Apple keyboard reports.  The arrows
// are the RAW codes $3B-$3E: raw $7B-$7D are the right-hand modifiers, and
// sending those made arrows press phantom modifiers on every ADB machine
// (found by Quake ignoring the web UI's arrows while a scripted $3D worked).
//
// This is the only key table: the DOM path (host_keymap_dom_to_adb), the
// typed-text path (host_keymap_ascii_to_adb, keyboard.type) and the key-name
// path (host_keymap_name_to_adb, keyboard.press/down/up) all read it, so a
// key the web UI can press, a script can name.
static const struct adb_key {
    const char *dom; // KeyboardEvent.code
    char ascii; // the character the key types unshifted (0: none)
    char shifted; // ... and with Shift (0: none)
    uint8_t adb; // ADB raw keycode
} k_adb_keys[] = {
    // DOM KeyboardEvent.code -> ADB raw keycode (Inside Macintosh Vol V's key
    // numbering; the raw and virtual codes differ only for the arrows and the
    // right-hand modifiers).  Both left and right Shift/Option/Command/Control map
    // to the one key an Apple keyboard reports.  The arrows are the RAW codes
    // $3B-$3E: raw $7B-$7D are the right-hand modifiers, and sending those made
    // arrows press phantom modifiers on every ADB machine (found by Quake ignoring
    // the web UI's arrows while a scripted $3D worked).
    {"KeyA",           'a',  'A', 0x00},
    {"KeyS",           's',  'S', 0x01},
    {"KeyD",           'd',  'D', 0x02},
    {"KeyF",           'f',  'F', 0x03},
    {"KeyH",           'h',  'H', 0x04},
    {"KeyG",           'g',  'G', 0x05},
    {"KeyZ",           'z',  'Z', 0x06},
    {"KeyX",           'x',  'X', 0x07},
    {"KeyC",           'c',  'C', 0x08},
    {"KeyV",           'v',  'V', 0x09},
    {"KeyB",           'b',  'B', 0x0B},
    {"KeyQ",           'q',  'Q', 0x0C},
    {"KeyW",           'w',  'W', 0x0D},
    {"KeyE",           'e',  'E', 0x0E},
    {"KeyR",           'r',  'R', 0x0F},
    {"KeyY",           'y',  'Y', 0x10},
    {"KeyT",           't',  'T', 0x11},
    {"KeyU",           'u',  'U', 0x20},
    {"KeyI",           'i',  'I', 0x22},
    {"KeyO",           'o',  'O', 0x1F},
    {"KeyP",           'p',  'P', 0x23},
    {"KeyL",           'l',  'L', 0x25},
    {"KeyJ",           'j',  'J', 0x26},
    {"KeyK",           'k',  'K', 0x28},
    {"KeyN",           'n',  'N', 0x2D},
    {"KeyM",           'm',  'M', 0x2E},
    {"Digit1",         '1',  '!', 0x12},
    {"Digit2",         '2',  '@', 0x13},
    {"Digit3",         '3',  '#', 0x14},
    {"Digit4",         '4',  '$', 0x15},
    {"Digit5",         '5',  '%', 0x17},
    {"Digit6",         '6',  '^', 0x16},
    {"Digit7",         '7',  '&', 0x1A},
    {"Digit8",         '8',  '*', 0x1C},
    {"Digit9",         '9',  '(', 0x19},
    {"Digit0",         '0',  ')', 0x1D},
    {"Minus",          '-',  '_', 0x1B}, // - / _
    {"Equal",          '=',  '+', 0x18}, // = / +
    {"BracketLeft",    '[',  '{', 0x21}, // [ / {
    {"BracketRight",   ']',  '}', 0x1E}, // ] / }
    {"Backslash",      '\\', '|', 0x2A}, // \ / |
    {"Semicolon",      ';',  ':', 0x29}, // ; / :
    {"Quote",          '\'', '"', 0x27}, // ' / "
    {"Comma",          ',',  '<', 0x2B}, // , / <
    {"Period",         '.',  '>', 0x2F}, // . / >
    {"Slash",          '/',  '?', 0x2C}, // / / ?
    {"Backquote",      '`',  '~', 0x32}, // ` / ~
    {"IntlBackslash",  0,    0,   0x0A}, // § / ± (non-US ISO layout)
    {"Tab",            '\t', 0,   0x30},
    {"Space",          ' ',  0,   0x31},
    {"Backspace",      0,    0,   0x33}, // Delete key on Mac
    {"Enter",          '\n', 0,   0x24}, // Return
    {"Escape",         0,    0,   0x35},
    {"ControlLeft",    0,    0,   0x36},
    {"ControlRight",   0,    0,   0x36},
    {"ShiftLeft",      0,    0,   0x38},
    {"ShiftRight",     0,    0,   0x38},
    {"CapsLock",       0,    0,   0x39},
    {"AltLeft",        0,    0,   0x3A}, // Option (left)
    {"AltRight",       0,    0,   0x3A}, // Option (right)
    {"MetaLeft",       0,    0,   0x37}, // Command (left)
    {"MetaRight",      0,    0,   0x37}, // Command (right)
    {"OSLeft",         0,    0,   0x37}, // Command (Windows key)
    {"OSRight",        0,    0,   0x37}, // Command (Windows key)
    {"ArrowLeft",      0,    0,   0x3B},
    {"ArrowRight",     0,    0,   0x3C},
    {"ArrowDown",      0,    0,   0x3D},
    {"ArrowUp",        0,    0,   0x3E},
    // The Apple Extended Keyboard block: Page Up/Down, Home, End, forward
    // Delete, Help and the function keys.  A Mac Plus keyboard has none of
    // them, but every ADB machine's guest can use them and some REQUIRE them:
    // Windows NT's text-mode Setup pages its licence agreement with Page Down
    // and accepts it with F8 (ans-nt-install), and without these the web UI
    // is stuck on that screen.  Raw register-0 codes like the arrows; none is
    // one of the right-hand modifiers at raw $7B-$7D ($7A, F1, is the
    // closest).  $79 and $64 are confirmed end to end through NT Setup.
    {"PageUp",         0,    0,   0x74},
    {"PageDown",       0,    0,   0x79},
    {"Home",           0,    0,   0x73},
    {"End",            0,    0,   0x77},
    {"Delete",         0,    0,   0x75}, // forward delete; Backspace is $33
    {"Insert",         0,    0,   0x72}, // Help on an Apple keyboard
    {"F1",             0,    0,   0x7A},
    {"F2",             0,    0,   0x78},
    {"F3",             0,    0,   0x63},
    {"F4",             0,    0,   0x76},
    {"F5",             0,    0,   0x60},
    {"F6",             0,    0,   0x61},
    {"F7",             0,    0,   0x62},
    {"F8",             0,    0,   0x64},
    {"F9",             0,    0,   0x65},
    {"F10",            0,    0,   0x6D},
    {"F11",            0,    0,   0x67},
    {"F12",            0,    0,   0x6F},
    {"NumpadDecimal",  0,    0,   0x41}, // Keypad .
    {"NumpadMultiply", 0,    0,   0x43}, // Keypad *
    {"NumpadAdd",      0,    0,   0x45}, // Keypad +
    {"NumLock",        0,    0,   0x47}, // Keypad Clear
    {"NumpadDivide",   0,    0,   0x4B}, // Keypad /
    {"NumpadEnter",    0,    0,   0x4C}, // Keypad Enter
    {"NumpadSubtract", 0,    0,   0x4E}, // Keypad -
    {"NumpadEqual",    0,    0,   0x51}, // Keypad =
    {"Numpad0",        0,    0,   0x52},
    {"Numpad1",        0,    0,   0x53},
    {"Numpad2",        0,    0,   0x54},
    {"Numpad3",        0,    0,   0x55},
    {"Numpad4",        0,    0,   0x56},
    {"Numpad5",        0,    0,   0x57},
    {"Numpad6",        0,    0,   0x58},
    {"Numpad7",        0,    0,   0x59},
    {"Numpad8",        0,    0,   0x5B},
    {"Numpad9",        0,    0,   0x5C},
};

#define N_ADB_KEYS (sizeof(k_adb_keys) / sizeof(k_adb_keys[0]))

int host_keymap_dom_to_adb(const char *dom_code) {
    if (!dom_code)
        return -1;
    for (size_t i = 0; i < N_ADB_KEYS; i++)
        if (strcmp(dom_code, k_adb_keys[i].dom) == 0)
            return k_adb_keys[i].adb;
    return -1;
}

int host_keymap_ascii_to_adb(char c, bool *shift) {
    if (shift)
        *shift = false;
    if (c == '\r')
        c = '\n'; // either line ending types Return
    if (c == 0)
        return -1;
    for (size_t i = 0; i < N_ADB_KEYS; i++) {
        if (k_adb_keys[i].ascii == c)
            return k_adb_keys[i].adb;
        if (k_adb_keys[i].shifted == c) {
            if (shift)
                *shift = true;
            return k_adb_keys[i].adb;
        }
    }
    return -1;
}

// Key names that are not DOM codes, or that mean another key than the DOM
// code of the same spelling: "delete" is the Mac's Delete (Backspace, $33),
// not the DOM's forward "Delete" ($75), which is "forwarddelete" / "del".
static const struct {
    const char *name;
    uint8_t adb;
} k_key_aliases[] = {
    {"return",        0x24},
    {"enter",         0x24},
    {"space",         0x31},
    {"escape",        0x35},
    {"esc",           0x35},
    {"tab",           0x30},
    {"delete",        0x33},
    {"backspace",     0x33},
    {"forwarddelete", 0x75},
    {"del",           0x75},
    {"help",          0x72},
    // The arrows' RAW codes, $3B-$3E.  These were the virtual codes
    // $7B-$7E, which on the ADB wire are the right-hand modifiers:
    // "left" pressed Right Shift.
    {"up",            0x3E},
    {"down",          0x3D},
    {"left",          0x3B},
    {"right",         0x3C},
    {"command",       0x37},
    {"cmd",           0x37},
    {"shift",         0x38},
    {"option",        0x3A},
    {"alt",           0x3A},
    {"control",       0x36},
    {"ctrl",          0x36},
    // Caps Lock is a locking switch on real Apple keyboards: hold it with
    // keyboard.down and it stays reported in ADB Register 2 until
    // keyboard.up.
    {"capslock",      0x39},
    {"caps",          0x39},
};

int host_keymap_name_to_adb(const char *name) {
    if (!name || !*name)
        return -1;
    // The names above, matched case-insensitively
    for (size_t i = 0; i < sizeof(k_key_aliases) / sizeof(k_key_aliases[0]); i++)
        if (!strcasecmp(name, k_key_aliases[i].name))
            return k_key_aliases[i].adb;
    // Any DOM code, case-insensitively: "f8", "pagedown", "home", "numpad5"
    for (size_t i = 0; i < N_ADB_KEYS; i++)
        if (!strcasecmp(name, k_adb_keys[i].dom))
            return k_adb_keys[i].adb;
    // Hex keycode (e.g., 0x24)
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        char *endp = NULL;
        long val = strtol(name, &endp, 16);
        if (*endp == '\0' && val >= 0 && val <= 0x7F)
            return (int)val;
    }
    // A single printable character -- "a", "7", ";": the KEY that types it.
    // Shift is deliberately ignored: a caller wanting a capital presses
    // shift itself with keyboard.down "shift", and a character that only
    // exists shifted (e.g. "!") resolves to its unshifted key.
    if (name[1] == '\0')
        return host_keymap_ascii_to_adb(name[0], NULL);
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
