// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// host_keys.h
// A host keyboard, as the browser presents it, turned into key transitions
// for whatever machine is running (I4).
//
// The web host used to keep two paths: DOM codes to ADB for a Mac, and DOM
// codes straight to COPS bytes for a Lisa, chosen by model id -- with the
// held-key bookkeeping only on the Lisa side, so a key-up the browser kept
// (an accelerator, a focus change) left a Mac's key down.  Now there is one
// path for every machine: DOM code -> ADB raw keycode (the model's key
// identity, machine_profile.h) -> system_input_key, which each substrate
// translates for its own keyboard.  This file is platform-free so it builds
// natively and is unit-tested (tests/unit/suites/host_keys).

#ifndef HOST_KEYS_H
#define HOST_KEYS_H

#include <stdbool.h>
#include <stdint.h>

// The ADB raw keycode for a DOM KeyboardEvent.code ("KeyA", "ArrowLeft",
// ...), or -1 for a key no Apple keyboard has.
int host_keymap_dom_to_adb(const char *dom_code);

// Where a key transition goes: 0 when the machine took it, <0 when it
// refused (a key its keyboard has not got).  The web host passes
// system_input_key.
typedef int (*host_key_sink_t)(int adb_code, bool down);

// The keys the host holds down: for each host key (by its ADB code), the
// code actually delivered for it (+1; 0 = not held).  They differ when
// Control was delivered as Command.
typedef struct host_keys {
    uint8_t sent[128];
} host_keys_t;

// A host key went down.  Delivers it; if the machine refuses Control, retries
// as Command -- a generic rule, no model id: a keyboard without Control (the
// Lisa's) gets the key its software uses in Control's place (SCO Xenix reads
// Apple-D as Control-D).  Returns true when a key was delivered, so the
// caller consumes the event (an unconsumed Control-D is the browser's
// "bookmark").  A repeat of a held key delivers nothing and returns true.
bool host_keys_down(host_keys_t *k, int adb_code, host_key_sink_t sink);

// A host key went up.  Delivers the up for whatever its down delivered --
// whether or not the page still has focus, so a key never sticks.  Returns
// true when the key was held.
bool host_keys_up(host_keys_t *k, int adb_code, host_key_sink_t sink);

// Release every held key (focus or pointer lock lost: the ups will land
// elsewhere) -- except Caps Lock, a locking key the web frontend owns on
// every machine (app/web2 lib/capslock.ts).
void host_keys_release_all(host_keys_t *k, host_key_sink_t sink);

#endif // HOST_KEYS_H
