// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for host_keys.c: the one host keyboard path (I4).
//
// The web host used to send a Lisa COPS bytes from its own DOM table and a
// Mac ADB codes from another.  Now every machine gets ADB raw codes and the
// Lisa translates them (lisa_keymap.c), so the old Lisa table is the spec:
// every key it mapped must reach the same COPS key through the ADB path.

#include "host_keys.h"
#include "lisa_keymap.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>

// --- A recording sink, optionally a Lisa ------------------------------------

static struct {
    int adb;
    bool down;
} delivered[64];
static int n_delivered;
static bool sink_is_lisa;

static int sink(int adb, bool down) {
    if (sink_is_lisa && lisa_keycode_for_adb(adb) == LISA_NO_KEY)
        return -1; // a key the Lisa keyboard has not got
    delivered[n_delivered].adb = adb;
    delivered[n_delivered].down = down;
    n_delivered++;
    return 0;
}

static void reset(bool lisa) {
    n_delivered = 0;
    sink_is_lisa = lisa;
}

// --- The DOM table --------------------------------------------------------

TEST(dom_codes_map_to_adb_raw_codes) {
    ASSERT_EQ_INT(host_keymap_dom_to_adb("KeyA"), 0x00);
    ASSERT_EQ_INT(host_keymap_dom_to_adb("Enter"), 0x24);
    ASSERT_EQ_INT(host_keymap_dom_to_adb("ShiftRight"), 0x38);
    // The arrows by their RAW codes, not the virtual $7B-$7E.
    ASSERT_EQ_INT(host_keymap_dom_to_adb("ArrowLeft"), 0x3B);
    ASSERT_EQ_INT(host_keymap_dom_to_adb("ArrowUp"), 0x3E);
    ASSERT_EQ_INT(host_keymap_dom_to_adb("F13"), -1);
    ASSERT_EQ_INT(host_keymap_dom_to_adb(NULL), -1);
}

// Every key the old DOM -> COPS table sent a Lisa reaches the same COPS key
// through DOM -> ADB -> lisa_keycode_for_adb (Control through the retry).
// Two of its entries were not Lisa keys and are pinned separately below:
// Backquote sent $68, which is Option (the table had no Alt), and Backslash
// sent $42, a code the boot ROM's key table does not name.
TEST(every_key_a_lisa_has_survives) {
    static const struct {
        const char *dom;
        int cops_down;
    } old_lisa[] = {
        {"KeyA",         0xF0},
        {"KeyB",         0xEE},
        {"KeyC",         0xED},
        {"KeyD",         0xFB},
        {"KeyE",         0xE0},
        {"KeyF",         0xE9},
        {"KeyG",         0xEA},
        {"KeyH",         0xEB},
        {"KeyI",         0xD3},
        {"KeyJ",         0xD4},
        {"KeyK",         0xD5},
        {"KeyL",         0xD9},
        {"KeyM",         0xD8},
        {"KeyN",         0xEF},
        {"KeyO",         0xDF},
        {"KeyP",         0xC4},
        {"KeyQ",         0xF5},
        {"KeyR",         0xE5},
        {"KeyS",         0xF6},
        {"KeyT",         0xE6},
        {"KeyU",         0xD2},
        {"KeyV",         0xEC},
        {"KeyW",         0xF7},
        {"KeyX",         0xFA},
        {"KeyY",         0xE7},
        {"KeyZ",         0xF9},
        {"Digit1",       0xF4},
        {"Digit2",       0xF1},
        {"Digit3",       0xF2},
        {"Digit4",       0xF3},
        {"Digit5",       0xE4},
        {"Digit6",       0xE1},
        {"Digit7",       0xE2},
        {"Digit8",       0xE3},
        {"Digit9",       0xD0},
        {"Digit0",       0xD1},
        {"Minus",        0xC0},
        {"Equal",        0xC1},
        {"BracketLeft",  0xD6},
        {"BracketRight", 0xD7},
        {"Semicolon",    0xDA},
        {"Quote",        0xDB},
        {"Comma",        0xDD},
        {"Period",       0xDE},
        {"Slash",        0xCC},
        {"Space",        0xDC},
        {"Enter",        0xC8},
        {"Tab",          0xF8},
        {"Backspace",    0xC5},
        {"ShiftLeft",    0xFE},
        {"ShiftRight",   0xFE},
        {"CapsLock",     0xFD},
        {"MetaLeft",     0xFF},
        {"MetaRight",    0xFF},
        {"OSLeft",       0xFF},
        {"OSRight",      0xFF},
        {"ControlLeft",  0xFF},
        {"ControlRight", 0xFF},
    };
    for (size_t i = 0; i < sizeof(old_lisa) / sizeof(old_lisa[0]); i++) {
        host_keys_t keys = {0};
        reset(true);
        int adb = host_keymap_dom_to_adb(old_lisa[i].dom);
        if (adb < 0) {
            printf("  %s: no ADB code\n", old_lisa[i].dom);
            ASSERT_TRUE(adb >= 0);
        }
        bool took = host_keys_down(&keys, adb, sink);
        if (!took)
            printf("  %s: ADB $%02X refused\n", old_lisa[i].dom, adb);
        ASSERT_TRUE(took);
        ASSERT_EQ_INT(n_delivered, 1);
        int cops = lisa_keycode_for_adb(delivered[0].adb);
        if (cops != (old_lisa[i].cops_down & 0x7F))
            printf("  %s: COPS $%02X, was $%02X\n", old_lisa[i].dom, cops, old_lisa[i].cops_down & 0x7F);
        ASSERT_EQ_INT(cops, old_lisa[i].cops_down & 0x7F);
    }
}

TEST(option_comes_from_alt_and_the_lisa_has_no_backquote_or_backslash) {
    host_keys_t keys = {0};
    reset(true);
    ASSERT_TRUE(host_keys_down(&keys, host_keymap_dom_to_adb("AltLeft"), sink));
    ASSERT_EQ_INT(lisa_keycode_for_adb(delivered[0].adb), 0x68); // Option
    ASSERT_TRUE(!host_keys_down(&keys, host_keymap_dom_to_adb("Backquote"), sink));
    ASSERT_TRUE(!host_keys_down(&keys, host_keymap_dom_to_adb("Backslash"), sink));
}

// --- The held-key state machine -----------------------------------------------

TEST(control_is_retried_as_command_where_refused) {
    host_keys_t keys = {0};
    reset(true);
    ASSERT_TRUE(host_keys_down(&keys, 0x36, sink)); // Control on a Lisa
    ASSERT_EQ_INT(n_delivered, 1);
    ASSERT_EQ_INT(delivered[0].adb, 0x37); // went in as Command
    ASSERT_TRUE(host_keys_up(&keys, 0x36, sink));
    ASSERT_EQ_INT(delivered[1].adb, 0x37); // and comes out as Command
    ASSERT_TRUE(!(delivered[1].down));

    // A Mac takes Control as Control.
    reset(false);
    ASSERT_TRUE(host_keys_down(&keys, 0x36, sink));
    ASSERT_EQ_INT(delivered[0].adb, 0x36);
}

TEST(a_repeat_is_not_a_transition_and_every_down_gets_its_up) {
    host_keys_t keys = {0};
    reset(false);
    ASSERT_TRUE(host_keys_down(&keys, 0x00, sink));
    ASSERT_TRUE(host_keys_down(&keys, 0x00, sink)); // auto-repeat
    ASSERT_EQ_INT(n_delivered, 1);
    ASSERT_TRUE(host_keys_up(&keys, 0x00, sink));
    ASSERT_TRUE(!(host_keys_up(&keys, 0x00, sink))); // no down, no up
    ASSERT_EQ_INT(n_delivered, 2);
}

TEST(a_refused_key_is_not_held) {
    host_keys_t keys = {0};
    reset(true);
    ASSERT_TRUE(!(host_keys_down(&keys, 0x43, sink))); // keypad * : no Lisa key
    ASSERT_TRUE(!(host_keys_up(&keys, 0x43, sink)));
    ASSERT_EQ_INT(n_delivered, 0);
}

TEST(release_all_lets_go_of_everything_but_caps_lock) {
    host_keys_t keys = {0};
    reset(false);
    host_keys_down(&keys, 0x38, sink); // Shift
    host_keys_down(&keys, 0x00, sink); // a
    host_keys_down(&keys, 0x39, sink); // Caps Lock
    n_delivered = 0;
    host_keys_release_all(&keys, sink);
    ASSERT_EQ_INT(n_delivered, 2);
    for (int i = 0; i < n_delivered; i++) {
        ASSERT_TRUE(!(delivered[i].down));
        ASSERT_TRUE(delivered[i].adb != 0x39);
    }
    ASSERT_TRUE(keys.sent[0x39] != 0); // still latched
}

int main(void) {
    RUN(dom_codes_map_to_adb_raw_codes);
    RUN(every_key_a_lisa_has_survives);
    RUN(option_comes_from_alt_and_the_lisa_has_no_backquote_or_backslash);
    RUN(control_is_retried_as_command_where_refused);
    RUN(a_repeat_is_not_a_transition_and_every_down_gets_its_up);
    RUN(a_refused_key_is_not_held);
    RUN(release_all_lets_go_of_everything_but_caps_lock);
    return 0;
}
