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

rtc_t *rtc_init(struct scheduler *scheduler, checkpoint_t *checkpoint);

void rtc_delete(rtc_t *rtc);

void rtc_checkpoint(rtc_t *restrict rtc, checkpoint_t *checkpoint);

// === Operations ===

void rtc_input(rtc_t *restrict rtc, bool disable, bool clock, bool pram);

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
