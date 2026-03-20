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

#endif // RTC_H
