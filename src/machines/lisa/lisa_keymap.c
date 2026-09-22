// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa_keymap.c — see lisa_keymap.h.

#include "lisa_keymap.h"

// The boot ROM's keycode->ASCII table, transcribed from
// "Lisa Boot ROM RM248.G.TEXT", the block headed "Keycode to Ascii Table
// (assumes alpha-lock so upper case only)".  Indexed by keycode - $20.
//
// This is NOT a lookup path — nothing in the model converts keycodes to
// ASCII.  It is here as the oracle the map below is checked against: every
// printable ADB key must land on a Lisa key that produces the same character.
const unsigned char lisa_rom_ascii_table[96] = {
    /* $20 */ 0x1B, 0x2D, 0x11, 0x12, 0x37, 0x38, 0x39, 0x14,
    /* $28 */ 0x34, 0x35, 0x36, 0x13, 0x2E, 0x32, 0x33, 0x03,
    /* $30 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* $38 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* $40 */ 0x2D, 0x3D, 0x00, 0x00, 0x50, 0x08, 0x00, 0x00,
    /* $48 */ 0x0D, 0x30, 0x00, 0x00, 0x2F, 0x31, 0x00, 0x00,
    /* $50 */ 0x39, 0x30, 0x55, 0x49, 0x4A, 0x4B, 0x5B, 0x5D,
    /* $58 */ 0x4D, 0x4C, 0x3B, 0x27, 0x20, 0x2C, 0x2E, 0x4F,
    /* $60 */ 0x45, 0x36, 0x37, 0x38, 0x35, 0x52, 0x54, 0x59,
    /* $68 */ 0x00, 0x46, 0x47, 0x48, 0x56, 0x43, 0x42, 0x4E,
    /* $70 */ 0x41, 0x32, 0x33, 0x34, 0x31, 0x51, 0x53, 0x57,
    /* $78 */ 0x00, 0x5A, 0x58, 0x44, 0x00, 0x00, 0x00, 0x00,
};

// === ADB keycode -> Lisa COPS keycode =======================================
//
// The model's universal key identity is the ADB virtual keycode
// (machine_profile.h).  The Lisa's own keycodes are unrelated, so this is the
// translation, and it is DERIVED FROM A PRIMARY SOURCE rather than guessed:
// the boot ROM's AsciiTable carries a per-row comment naming the physical key
// behind every code ("Lisa Boot ROM RM248.G.TEXT", the block headed
// "Keycode to Ascii Table (assumes alpha-lock so upper case only)"):
//
//   $20 Pad:Clear  $21 Pad:-  $22 Left  $23 Right  $24 Pad:7 $25 Pad:8
//   $26 Pad:9      $27 Up     $28 Pad:4 $29 Pad:5  $2A Pad:6 $2B Down
//   $2C Pad:.      $2D Pad:2  $2E Pad:3 $2F Pad:Enter
//   $40 -   $41 =   $44 P   $45 BackSp  $48 Return  $49 Pad:0  $4C /  $4D Pad:1
//   $50 9   $51 0   $52 U   $53 I   $54 J   $55 K   $56 [   $57 ]
//   $58 M   $59 L   $5A ;   $5B '   $5C Space  $5D ,   $5E .   $5F O
//   $60 E   $61 6   $62 7   $63 8   $64 5   $65 R   $66 T   $67 Y
//   $68 Option  $69 F  $6A G  $6B H  $6C V  $6D C  $6E B  $6F N
//   $70 A   $71 2   $72 3   $73 4   $74 1   $75 Q   $76 S   $77 W
//   $78 Tab $79 Z   $7A X   $7B D   $7D Alpha  $7E Shift  $7F Cmd
//
// Those "Pad:" markers are what make the table safe to write.  The ROM's
// ASCII column alone is ambiguous -- keypad 5 and main-row 5 both produce
// '5' -- so inverting it by character picks whichever comes first, which is
// the keypad.  (That ambiguity is why the
// ROM-table-inverting resolver this replaces sent keyboard.press "5" to the
// KEYPAD 5 rather than the main row.)
//
// Keys on one side and not the other are left out, and an unmapped code is
// reported rather than silently substituted.  The Lisa has no Control and no
// function keys; it has no backquote and no backslash.  The ADB keypad's
// *, +, / and = have no Lisa equivalent.
uint8_t lisa_keycode_for_adb(int adb) {
    static const uint8_t map[128] = {
        // Letters, ADB's Apple-II-descended numbering on the left.
        [0x00] = 0x70,
        [0x0B] = 0x6E,
        [0x08] = 0x6D,
        [0x02] = 0x7B, // a b c d
        [0x0E] = 0x60,
        [0x03] = 0x69,
        [0x05] = 0x6A,
        [0x04] = 0x6B, // e f g h
        [0x22] = 0x53,
        [0x26] = 0x54,
        [0x28] = 0x55,
        [0x25] = 0x59, // i j k l
        [0x2E] = 0x58,
        [0x2D] = 0x6F,
        [0x1F] = 0x5F,
        [0x23] = 0x44, // m n o p
        [0x0C] = 0x75,
        [0x0F] = 0x65,
        [0x01] = 0x76,
        [0x11] = 0x66, // q r s t
        [0x20] = 0x52,
        [0x09] = 0x6C,
        [0x0D] = 0x77,
        [0x07] = 0x7A, // u v w x
        [0x10] = 0x67,
        [0x06] = 0x79, // y z

        // Main-row digits.
        [0x1D] = 0x51,
        [0x12] = 0x74,
        [0x13] = 0x71,
        [0x14] = 0x72, // 0 1 2 3
        [0x15] = 0x73,
        [0x17] = 0x64,
        [0x16] = 0x61,
        [0x1A] = 0x62, // 4 5 6 7
        [0x1C] = 0x63,
        [0x19] = 0x50, // 8 9

        // Punctuation the two keyboards share.
        [0x1B] = 0x40,
        [0x18] = 0x41,
        [0x21] = 0x56,
        [0x1E] = 0x57, // - = [ ]
        [0x29] = 0x5A,
        [0x27] = 0x5B,
        [0x2B] = 0x5D,
        [0x2F] = 0x5E, // ; \' , .
        [0x2C] = 0x4C,
        [0x31] = 0x5C, // / space

        // Editing and navigation.
        [0x24] = 0x48,
        [0x30] = 0x78,
        [0x33] = 0x45, // return tab backspace
        [0x7E] = 0x27,
        [0x7D] = 0x2B,
        [0x7B] = 0x22,
        [0x7C] = 0x23, // arrows

        // Modifiers.  No Control on a Lisa keyboard.
        [0x38] = 0x7E,
        [0x3A] = 0x68,
        [0x37] = 0x7F,
        [0x39] = 0x7D,

        // Numeric keypad.  ADB's *, +, / and = have no Lisa key.
        [0x47] = 0x20,
        [0x4E] = 0x21,
        [0x41] = 0x2C,
        [0x4C] = 0x2F,
        [0x52] = 0x49,
        [0x53] = 0x4D,
        [0x54] = 0x2D,
        [0x55] = 0x2E,
        [0x56] = 0x28,
        [0x57] = 0x29,
        [0x58] = 0x2A,
        [0x59] = 0x24,
        [0x5B] = 0x25,
        [0x5C] = 0x26,
    };
    if (adb < 0 || adb > 0x7F)
        return LISA_NO_KEY;
    // $00 is a real ADB code ('a'), so the empty slots are marked by mapping
    // to 0 and caught here -- except 'a' itself, whose Lisa code is $70.
    uint8_t code = map[adb];
    return code ? code : LISA_NO_KEY;
}
