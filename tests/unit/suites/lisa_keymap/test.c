// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ADB virtual keycode -> Apple Lisa COPS keycode, checked against the boot
// ROM table it was derived from (code review 2026-09-03, F-11 follow-on).
//
// The model's universal key identity is the ADB keycode: system_keyboard_update
// already handed the same int to the ADB transceiver or to the Plus's M0110A,
// which converts on the wire with Guide 2e p.282's `(adb << 1) | 1`.  The Lisa
// joins that convention through lisa_keycode_for_adb, which is a hand-written
// table — so it needs checking rather than trusting, and the boot ROM ships
// the oracle to check it with.
//
// "Lisa Boot ROM RM248.G.TEXT" holds AsciiTable, keycode $20..$7F -> the ASCII
// the ROM produces, with a per-row comment naming each physical key. The ASCII
// column alone is ambiguous — keypad 5 and main-row 5 both give '5' — which is
// exactly why the previous resolver, which inverted that column by character,
// sent keyboard.press "5" to the KEYPAD. The comments disambiguate, and the
// tests below pin both halves: the character a key produces, AND which of the
// two keys with that character it is.

#include "lisa_keymap.h"

#include "test_assert.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The ADB half of the correspondence: the US-layout ADB virtual keycode for
// each printable character, transcribed from Inside Macintosh: Toolbox
// Essentials, "Virtual Key Codes" — the same table debug_mac.c carries. Kept
// here rather than linked so this suite tests the MAP, not debug_mac.c.
static const struct {
    char ch;
    uint8_t adb;
} adb_plain[] = {
    {'a',  0x00},
    {'b',  0x0B},
    {'c',  0x08},
    {'d',  0x02},
    {'e',  0x0E},
    {'f',  0x03},
    {'g',  0x05},
    {'h',  0x04},
    {'i',  0x22},
    {'j',  0x26},
    {'k',  0x28},
    {'l',  0x25},
    {'m',  0x2E},
    {'n',  0x2D},
    {'o',  0x1F},
    {'p',  0x23},
    {'q',  0x0C},
    {'r',  0x0F},
    {'s',  0x01},
    {'t',  0x11},
    {'u',  0x20},
    {'v',  0x09},
    {'w',  0x0D},
    {'x',  0x07},
    {'y',  0x10},
    {'z',  0x06},
    {'0',  0x1D},
    {'1',  0x12},
    {'2',  0x13},
    {'3',  0x14},
    {'4',  0x15},
    {'5',  0x17},
    {'6',  0x16},
    {'7',  0x1A},
    {'8',  0x1C},
    {'9',  0x19},
    {' ',  0x31},
    {'-',  0x1B},
    {'=',  0x18},
    {'[',  0x21},
    {']',  0x1E},
    {';',  0x29},
    {'\'', 0x27},
    {',',  0x2B},
    {'.',  0x2F},
    {'/',  0x2C},
};

// The ASCII the ROM says a Lisa keycode produces. Upper case, because the
// ROM's own header says the table "assumes alpha-lock so upper case only".
static int rom_ascii(uint8_t lisa_code) {
    if (lisa_code < 0x20 || lisa_code > 0x7F)
        return -1;
    return lisa_rom_ascii_table[lisa_code - 0x20];
}

static char upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// === Tests =================================================================

// The headline: every printable key the two keyboards share must land on a
// Lisa key that the ROM says produces the same character.  This is the whole
// derivation, checked.
TEST(test_every_printable_key_produces_its_own_character) {
    for (size_t i = 0; i < sizeof adb_plain / sizeof adb_plain[0]; i++) {
        char ch = adb_plain[i].ch;
        uint8_t lisa = lisa_keycode_for_adb(adb_plain[i].adb);
        if (lisa == LISA_NO_KEY) {
            fprintf(stderr, "[FAIL] ADB $%02X ('%c') maps to no Lisa key\n", adb_plain[i].adb, ch);
            exit(1);
        }
        int got = rom_ascii(lisa);
        if (got != (int)(unsigned char)upper(ch)) {
            fprintf(stderr, "[FAIL] ADB $%02X ('%c') -> Lisa $%02X, which the ROM says is '%c' (0x%02X)\n",
                    adb_plain[i].adb, ch, lisa, got > 0x20 ? got : '?', got);
            exit(1);
        }
    }
}

// The ambiguity that broke the old resolver: '5' exists twice on a Lisa, once
// on the main row ($64) and once on the keypad ($29), and the ROM's ASCII
// column cannot tell them apart.  A main-row ADB digit must reach the main
// row, and an ADB KEYPAD digit must reach the keypad.
TEST(test_main_row_and_keypad_digits_stay_apart) {
    static const struct {
        uint8_t adb_main, lisa_main, adb_pad, lisa_pad;
        char ch;
    } digits[] = {
        {0x1D, 0x51, 0x52, 0x49, '0'},
        {0x12, 0x74, 0x53, 0x4D, '1'},
        {0x13, 0x71, 0x54, 0x2D, '2'},
        {0x14, 0x72, 0x55, 0x2E, '3'},
        {0x15, 0x73, 0x56, 0x28, '4'},
        {0x17, 0x64, 0x57, 0x29, '5'},
        {0x16, 0x61, 0x58, 0x2A, '6'},
        {0x1A, 0x62, 0x59, 0x24, '7'},
        {0x1C, 0x63, 0x5B, 0x25, '8'},
        {0x19, 0x50, 0x5C, 0x26, '9'},
    };
    for (size_t i = 0; i < sizeof digits / sizeof digits[0]; i++) {
        ASSERT_EQ_INT(digits[i].lisa_main, lisa_keycode_for_adb(digits[i].adb_main));
        ASSERT_EQ_INT(digits[i].lisa_pad, lisa_keycode_for_adb(digits[i].adb_pad));
        // ...and the ROM agrees they both produce the character, which is
        // what made inverting its table by character the wrong move.
        ASSERT_EQ_INT((int)(unsigned char)digits[i].ch, rom_ascii(digits[i].lisa_main));
        ASSERT_EQ_INT((int)(unsigned char)digits[i].ch, rom_ascii(digits[i].lisa_pad));
    }
}

// Keys with no ASCII form, named in the ROM's row comments rather than its
// value column.
TEST(test_the_named_keys) {
    static const struct {
        uint8_t adb, lisa;
        const char *what;
    } named[] = {
        {0x24, 0x48, "Return"    },
        {0x30, 0x78, "Tab"       },
        {0x33, 0x45, "BackSpace" },
        {0x3E, 0x27, "Up"        },
        {0x3D, 0x2B, "Down"      },
        {0x3B, 0x22, "Left"      },
        {0x3C, 0x23, "Right"     },
        {0x38, 0x7E, "Shift"     },
        {0x3A, 0x68, "Option"    },
        {0x37, 0x7F, "Command"   },
        {0x39, 0x7D, "Alpha Lock"},
        {0x47, 0x20, "Pad Clear" },
        {0x4C, 0x2F, "Pad Enter" },
    };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
        uint8_t got = lisa_keycode_for_adb(named[i].adb);
        if (got != named[i].lisa) {
            fprintf(stderr, "[FAIL] %s: ADB $%02X -> $%02X, want Lisa $%02X\n", named[i].what, named[i].adb, got,
                    named[i].lisa);
            exit(1);
        }
    }
}

// Two keys must never share a Lisa code, or one of them is silently typing
// the other.
TEST(test_the_map_is_injective) {
    int owner[256];
    for (int i = 0; i < 256; i++)
        owner[i] = -1;
    for (int adb = 0; adb <= 0x7F; adb++) {
        uint8_t lisa = lisa_keycode_for_adb(adb);
        if (lisa == LISA_NO_KEY)
            continue;
        if (owner[lisa] >= 0) {
            fprintf(stderr, "[FAIL] Lisa $%02X is claimed by both ADB $%02X and ADB $%02X\n", lisa, owner[lisa], adb);
            exit(1);
        }
        owner[lisa] = adb;
    }
}

// Every code the map hands out has to be a key the ROM knows, and a legal
// COPS keycode: the wire byte is `d rrr nnnn`, so bit 7 is the direction and
// a keycode is seven bits.
TEST(test_every_mapped_code_is_a_real_lisa_key) {
    for (int adb = 0; adb <= 0x7F; adb++) {
        uint8_t lisa = lisa_keycode_for_adb(adb);
        if (lisa == LISA_NO_KEY)
            continue;
        if (lisa < 0x20 || lisa > 0x7F) {
            fprintf(stderr, "[FAIL] ADB $%02X -> $%02X, outside the ROM table's $20..$7F\n", adb, lisa);
            exit(1);
        }
        // $30..$3F and a handful of others are marked "unused" in the ROM.
        // A key with no ASCII is fine ONLY if it is one of the named ones.
        static const uint8_t no_ascii_but_real[] = {0x68, 0x78, 0x7D, 0x7E, 0x7F};
        if (rom_ascii(lisa) == 0x00) {
            bool ok = false;
            for (size_t k = 0; k < sizeof no_ascii_but_real; k++)
                if (lisa == no_ascii_but_real[k])
                    ok = true;
            if (!ok) {
                fprintf(stderr, "[FAIL] ADB $%02X -> Lisa $%02X, an unused slot in the ROM table\n", adb, lisa);
                exit(1);
            }
        }
    }
}

// Keys the Lisa does not have are reported, not substituted.  Getting this
// wrong is worse than an error: a test would press Control and get something
// else entirely.
TEST(test_absent_keys_are_refused) {
    ASSERT_EQ_INT(LISA_NO_KEY, lisa_keycode_for_adb(0x36)); // Control
    ASSERT_EQ_INT(LISA_NO_KEY, lisa_keycode_for_adb(0x7A)); // F1
    ASSERT_EQ_INT(LISA_NO_KEY, lisa_keycode_for_adb(0x32)); // backquote
    ASSERT_EQ_INT(LISA_NO_KEY, lisa_keycode_for_adb(0x2A)); // backslash
    ASSERT_EQ_INT(LISA_NO_KEY, lisa_keycode_for_adb(0x43)); // keypad *
    ASSERT_EQ_INT(LISA_NO_KEY, lisa_keycode_for_adb(0x45)); // keypad +
    ASSERT_EQ_INT(LISA_NO_KEY, lisa_keycode_for_adb(-1));
    ASSERT_EQ_INT(LISA_NO_KEY, lisa_keycode_for_adb(0x80));
}

int main(void) {
    RUN(test_every_printable_key_produces_its_own_character);
    RUN(test_main_row_and_keypad_digits_stay_apart);
    RUN(test_the_named_keys);
    RUN(test_the_map_is_injective);
    RUN(test_every_mapped_code_is_a_real_lisa_key);
    RUN(test_absent_keys_are_refused);
    printf("[PASS] All Lisa keymap tests passed\n");
    return 0;
}
