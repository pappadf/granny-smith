// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scheduler_internal.h
// The scheduler's private state, shared by its two translation units only:
// scheduler.c (the event queue, the sprint loop, pacing) and
// scheduler_class.c (the scheduler / pacing object-model nodes).  Nothing
// outside src/core/scheduler/ includes this -- struct scheduler is opaque
// everywhere else.

#ifndef SCHEDULER_INTERNAL_H
#define SCHEDULER_INTERNAL_H

#include "scheduler.h"

#include <stdbool.h>
#include <stdint.h>

struct object;

// Capacity of the event-type registry (names for checkpointing)
#define MAX_EVENT_TYPES 64

// A single scheduled event in the priority queue
struct event {
    event_t *next;
    uint64_t timestamp;
    event_callback_t callback;
    void *source;
    uint64_t data;
    // A repeating event re-arms itself inside the scheduler, at
    // timestamp + interval -- from the time it was SCHEDULED, not from the
    // time it was dispatched, so a periodic cannot drift.  `interval` is in
    // CPU cycles, whichever unit the caller armed with.
    uint64_t interval_cycles;
    bool periodic;
};

// Maps a source/callback pair to human-readable names for checkpointing
typedef struct {
    char source_name[64];
    void *source;
    char event_name[64];
    event_callback_t callback;
} event_type_t;

// The checkpointed part of the scheduler: guest-visible plain data, saved and
// restored as one block (scheduler_checkpoint / scheduler_init).  A nested
// struct, so the save size is a type's size rather than an offsetof() cut
// through a longer struct -- adding a field here changes the checkpoint
// format, and the static assert below makes that a deliberate act.
struct scheduler_saved {
    bool running;
    uint64_t cpu_cycles; // authoritative cycle counter, updated at sprint boundaries

    // Per-machine cycles-per-instruction constant — guest-visible, identical
    // in every pacing mode (one guest timeline)
    uint32_t cpi;

    // I/O wait-state time not yet burned as a phantom instruction, x256
    // cycles (under one effective CPI).  Guest-visible timing state, so it is
    // checkpointed; zero on a new machine.  g_sprint_io.penalty_remainder is its
    // sprint-time alias: copied in at sprint start, out at sprint end.
    uint32_t io_penalty_remainder;

    // Whole stall slots an I/O access still owes: its wait states ran past
    // the end of the sprint it started in (an event fell inside the stall).
    // They are burned first in the next sprint.  Guest-visible timing state,
    // checkpointed with the remainder.
    uint32_t io_stall_slots;

    // Cycles still owed to the VBL frame-unit in progress (0 = no frame open,
    // so the next scheduler_run_frame starts one by pulsing the VBL line).
    // The VBL is a 60 Hz tick of emulated time, so where the machine stands in
    // its frame is guest-visible: checkpointed, and a restore resumes the
    // frame it was saved in rather than pulsing the VBL early.
    uint64_t frame_cycles_left;

    // Instructions executed since power-on (scheduler.instr_count): the
    // machine's own count, so a restore carries it rather than estimating
    // it from cycles, which time spent accelerated would skew.
    uint64_t total_instructions;
};

// The on-disk size of the block above (48 bytes on every target: bool and
// uint64_t align the same on x86-64, aarch64 and wasm32).  Changing it breaks
// every existing checkpoint, so it is a conscious edit here, not a side
// effect of a new field.
_Static_assert(sizeof(struct scheduler_saved) == 48, "scheduler checkpoint block changed size");

// Core scheduler state
struct scheduler {
    // Guest-visible plain data: the checkpointed block.  Anything
    // host-relative or re-derivable stays out of it, the same reason
    // cpi_eff_x256 is not in it.
    struct scheduler_saved saved;

    // These four are the pacing governor's wall-clock smoothing.  They used
    // to be inside the checkpointed block, so every checkpoint carried one
    // host's timing state -- the restore then overwrote all four from
    // host_time_ms(), so nothing ever consumed them, but they still made save
    // files non-reproducible: two processes saving identical guest state
    // produced files differing in the mantissa of these doubles: host state
    // leaking into a save file.
    double previous_time; // previous time in seconds
    // On a restore, the CPI the checkpoint carries -- a scheduler.cpi
    // override included -- until scheduler_restore_events puts it back over
    // the machine's own, which its build sets after the scheduler (0: none).
    // Not checkpointed: outside the saved block.
    uint32_t restored_cpi;

    double vbl_acc_error; // accumulated VBL timing error (seconds)
    double host_secs_per_vbl; // smoothed host seconds per VBL
    double host_secs_per_loop; // smoothed host seconds per main loop iteration

    // Event type registry for checkpointing
    event_type_t event_types[MAX_EVENT_TYPES];
    int num_event_types;

    // The host's pacing this machine currently runs under, as last handed to
    // scheduler_apply_pacing by the run step.  Host policy, never machine
    // state: not checkpointed, and a new or restored machine starts from the
    // host's setting.
    host_pacing_t pacing;

    // Effective CPI, x256 fixed point: cpi << 8 in paced/unthrottled, lowered
    // (never raised) in accelerated mode. Derived by scheduler_update_cpi_eff
    // from (pacing, cpi, governor rung); deliberately outside the saved block
    // so it is never checkpointed — restore re-derives it.
    uint32_t cpi_eff_x256;
    // Sub-cycle remainder of sprint cycle accounting, 0..255 (x256 fractional
    // cycles). Carried across sprints so cycles advanced stay exact integers
    // and nothing is dropped; reset on mode/CPI/speed changes and on restore.
    uint32_t cycle_frac_x256;

    // Adaptive-governor state — live controller state, never checkpointed
    // (like the host-timing estimators outside the saved block): a restored
    // or mode-switched machine re-learns its speed from fresh measurements.
    int gov_rung; // current index into gov_ladder_x256 (0 = authentic 1x)
    double gov_util_ewma; // smoothed utilization (host secs per frame / VBL period)
    double gov_dwell_secs; // host seconds spent at the current rung
    double gov_holdoff_secs; // remaining post-back-off climb holdoff

    // Sprint execution counters (previously file-scope globals)
    uint32_t sprint_total; // instructions planned for current sprint
    uint32_t sprint_burndown; // instructions remaining in current sprint

    // The mode: who started the current run and why it stopped.  Live
    // run-loop state, so never checkpointed; a
    // restored machine starts with no mode open -- unless it was saved
    // running, when scheduler_init opens an unbounded one.  `mode_open` is
    // set by scheduler_run_with_budget and cleared by scheduler_run_frame when it
    // sees `running` down, which is where the mode_ended event goes out --
    // once per mode, whichever path dropped `running`.
    uint32_t run_owner;
    uint32_t mode_seq; // id of the current mode, counted from 1
    sched_stop_reason_t stop_reason;
    bool mode_open;
    bool mode_bounded; // the mode has an instruction budget
    uint32_t speed_reported_x256; // the last effective speed announced (speed event)
    bool in_frame; // inside scheduler_run_frame: a stop there is reported at its end

    // Pointers last
    sched_cpu_if_t cpu; // the main-CPU seam (copied at init; ctx outlives us)
    event_t *cpu_events; // priority queue sorted by timestamp

    uint32_t frequency;
    // The VIA E-clock period in CPU cycles x256, derived from `frequency`;
    // the sprint publishes it as g_sprint_io.esync_period_x256.
    uint32_t esync_period_x256;

    // Object-tree binding — lifetime tied to scheduler_init / scheduler_delete.
    struct object *object;
};

// Look up the registered names for a (source, callback) pair; NULL if none.
const event_type_t *scheduler_find_event_type(struct scheduler *s, void *source, event_callback_t cb);

// The accelerated-mode speed multiplier (x256) in force: the pinned setting,
// else the governor's rung, clamped to the user cap.
uint32_t scheduler_current_speed_x256(struct scheduler *s);

// Total events dispatched since process start (scheduler.events_fired).
extern uint64_t g_sched_events_fired;

// Create the `scheduler` node for `s` and attach it under the root
// (scheduler_class.c); NULL if the object could not be made.
struct object *scheduler_object_new(struct scheduler *s);
// Detach and free a node made by scheduler_object_new (NULL is a no-op).
void scheduler_object_delete(struct object *obj);

#endif // SCHEDULER_INTERNAL_H
