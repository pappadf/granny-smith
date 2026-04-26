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

#endif // RTC_H
