// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rtc.h
// Public interface for Real-Time Clock (RTC) emulation.

#ifndef RTC_H
#define RTC_H

// === Includes ===
#include "common.h"
#include "scheduler.h"

#include <stdbool.h>
#include <stdint.h>

// === Forward Declarations ===
struct via;
typedef struct via via_t;

// === Type Definitions ===
struct rtc;
typedef struct rtc rtc_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// `extended` selects the chip variant: true for the 256-byte XPRAM RTC used
// by the Plus, SE, SE/30 and the whole Mac II family; false only for the
// 20-byte RTC of the 128K and 512K (Apple part 343-0040).  Note the 512K
// *enhanced* already carries the 256-byte part — Guide to the Macintosh Family
// Hardware 2e p.2270: "The RTC in the Macintosh 128K and 512K computers also
// contains 20 bytes of RAM ... The RTC in the 512K enhanced and later
// Macintosh computers contains 256 bytes."  So this is "128K/512K only", not
// "pre-Plus".
//
// It changes how the legacy one-byte PRAM commands map onto physical PRAM
// bytes so the XPRAM 'NuMc' signature ($0C..$0F) doesn't collide with the
// SysParam block.
//
// NO MACHINE SELECTS false TODAY — all four call sites pass true.  Kept
// because it is the only record in the code of the 512Ke transition, which
// both primary sources document and a future compact machine will need.  Note
// the flag is also only HALF honoured: legacy_pram_addr() consults it, but the
// extended-command path (read_ext/write_ext) indexes all 256 bytes regardless,
// so a 20-byte machine would still have working extended commands it must not
// have.  Fix that before wiring one up.
rtc_t *rtc_init(struct scheduler *scheduler, checkpoint_t *checkpoint, bool extended);

void rtc_delete(rtc_t *rtc);

void rtc_checkpoint(rtc_t *restrict rtc, checkpoint_t *checkpoint);

// === Operations ===

void rtc_input(rtc_t *restrict rtc, bool disable, bool clock, bool data);

void rtc_set_via(rtc_t *restrict rtc, via_t *via);

// Override the wall clock with an absolute Mac-epoch (1904) seconds value.
// Used by the `set-time` script command to make boot deterministic.
void rtc_set_seconds(rtc_t *restrict rtc, uint32_t mac_seconds);

// === M7b — object-model accessors ===========================================
//
// Read-only views and a controlled PRAM-write helper for the `rtc`
// object class. The PRAM read/write helpers honor the write-protect
// bit the same way the chip-level command stream does, so writes via
// the new path can't bypass software's read-only setting.

uint32_t rtc_get_seconds(const rtc_t *rtc);
bool rtc_get_read_only(const rtc_t *rtc);
uint8_t rtc_pram_read(const rtc_t *rtc, uint8_t addr);
// Returns true on success, false if the byte was rejected (read-only).
bool rtc_pram_write(rtc_t *rtc, uint8_t addr, uint8_t value);

#endif // RTC_H
