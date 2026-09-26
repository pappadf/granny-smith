// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lisa_keymap.h
// ADB raw keycode -> Apple Lisa COPS keycode.
//
// The model's universal key identity is the ADB raw keycode
// (machine_profile.h's input_key); this is the Lisa's translation of it.
// Kept in its own file, with the boot ROM's own keycode->ASCII table beside
// it, so the mapping can be CHECKED against the source it was derived from
// rather than trusted — see tests/unit/suites/lisa_keymap.

#ifndef LISA_KEYMAP_H
#define LISA_KEYMAP_H

#include <stdint.h>

// Returned for an ADB key this keyboard does not have.  $FF is not a Lisa
// keycode: the COPS byte is `d rrr nnnn` with bit 7 the direction, so a
// keycode is 7 bits and $7F (Cmd) is the largest real one.
#define LISA_NO_KEY 0xFFu

// The Lisa keycode for an ADB virtual keycode, or LISA_NO_KEY.
uint8_t lisa_keycode_for_adb(int adb_code);

// The boot ROM's AsciiTable, indexed by (lisa_keycode - 0x20), 96 entries.
// $00 means "this key has no ASCII form" (Option, Tab, Alpha Lock, Shift,
// Command) or an unused slot.  Exposed for the test that verifies the map
// above against it, and for nothing else.
extern const unsigned char lisa_rom_ascii_table[96];

#endif // LISA_KEYMAP_H
