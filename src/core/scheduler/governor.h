// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// governor.h
// The adaptive speed governor: the closed-loop controller that picks the
// guest CPU's speed multiplier in paced and accelerated mode when
// scheduler.speed is auto.  Private to src/core/scheduler/ -- the scheduler
// embeds a governor_t, feeds it one measurement per main-loop tick and
// re-derives its effective CPI when the governor reports a step.  The
// controller itself knows nothing about the scheduler: it sees the pacing
// settings (floor, cap, mode), the host's per-frame cost and two pressure
// signals, and answers with a rung on the speed ladder.

#ifndef GOVERNOR_H
#define GOVERNOR_H

#include "scheduler.h"

#include <stdbool.h>
#include <stdint.h>

// Live controller state -- never checkpointed (like the scheduler's
// host-timing estimators): a restored or mode-switched machine re-learns its
// speed from fresh measurements.  Owned by the scheduler, reset through
// governor_reset, otherwise written only by governor_tick.
typedef struct governor {
    int rung; // current index into the speed ladder (GOV_RUNG_ONE = authentic 1x)
    double util_ewma; // smoothed utilization (host secs per frame / VBL period)
    double dwell_secs; // host seconds spent at the current rung
    double holdoff_secs; // remaining post-back-off climb holdoff
    int pressure_ticks; // consecutive governor ticks under pressure (behind real time, or audio drained)
    double pressure_secs; // ...and the host seconds those ticks spanned
    double audio_secs; // consecutive host seconds the audio ring has reported below GOV_AUDIO_LOW
    bool primed; // first governor tick since the reset seen (it starts the ladder at the floor)
} governor_t;

// Whether the governor is choosing the speed under this pacing: an auto
// speed in a mode whose frame-units are aligned to the host clock.  Turbo
// aligns nothing, and a pinned speed is the user's fixed choice.
bool governor_active(const host_pacing_t *pacing);

// Reset to the authentic speed with fresh estimators.  Called wherever the
// measurements go stale: mode switches, pin/unpin, floor and cap changes,
// init and checkpoint restore -- the controller re-learns the host's headroom
// from scratch rather than acting on stale utilization.
void governor_reset(governor_t *g);

// The speed multiplier (x256) at the governor's current rung, before the
// pacing's floor and cap are applied (the scheduler clamps).
uint32_t governor_speed_x256(const governor_t *g);

// One evaluation, from the paced main loop after a tick that ran frame-units:
// host_secs_this_vbl is the measured host cost of one frame-unit,
// elapsed_secs the host time since the previous tick, behind whether the
// paced accumulator is behind real time (a capped burst that still left the
// debt growing), and audio_fill the host audio ring's fill fraction against
// its target depth, or <0 where no such signal exists (headless, audio idle).
// Returns true when the rung changed, so the caller re-derives the effective
// CPI once, between sprints.
bool governor_tick(governor_t *g, const host_pacing_t *pacing, double host_secs_this_vbl, double elapsed_secs,
                   bool behind, double audio_fill);

#endif // GOVERNOR_H
