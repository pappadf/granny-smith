// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scheduler.h
// Public interface for event scheduling and timing.

#ifndef SCHEDULE_H
#define SCHEDULE_H

// === Includes ===
#include "common.h"
#include "cpu.h"

// === Forward Declarations ===
struct config;
typedef struct config config_t;

// === Type Definitions ===

struct event;
typedef struct event event_t;

typedef void (*event_callback_t)(void *source, uint64_t data);

#define NS_PER_SEC 1000000000ULL

// Three pacing modes (see docs/core/scheduler/scheduler.md §10,
// local/gs-docs/completed/proposal-scheduler-two-modes.md and
// local/gs-docs/proposals/proposal-scheduler-accelerated-mode.md):
//   schedule_paced       — wall-clock accumulator; the guest tracks real time
//                          (web2 default)
//   schedule_unthrottled — as many frame-units as the host allows ("turbo")
//   schedule_accelerated — paced timebase (VBL/VIA/sound stay real-time) with
//                          a lowered *effective* CPI, so the CPU retires more
//                          instructions per frame-unit — "the same Mac with a
//                          CPU accelerator card"; scheduler.speed picks the
//                          multiplier (1x..8x)
// Pacing only affects how many frame-units a host tick batches. In paced and
// unthrottled the guest's execution timeline is a pure function of the
// frame-unit count: CPI is a per-machine constant and never depends on the
// mode. Accelerated deliberately gives that up (it is excluded from
// budget-pinned tests): the cycle timebase stays real-time, but instructions
// per frame-unit scale with the speed setting.
enum schedule_mode { schedule_paced, schedule_unthrottled, schedule_accelerated };

struct scheduler;
typedef struct scheduler scheduler_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Create and initialize a scheduler, optionally restoring from checkpoint
struct scheduler *scheduler_init(struct cpu *cpu, checkpoint_t *checkpoint);

// Free all resources associated with a scheduler instance
void scheduler_delete(struct scheduler *scheduler);

// Save scheduler state to a checkpoint
void scheduler_checkpoint(struct scheduler *restrict scheduler, checkpoint_t *checkpoint);

// === Operations ===

// Event management

// Check if an event with the given callback is currently scheduled
bool has_event(struct scheduler *restrict scheduler, event_callback_t callback);

// Schedule a new CPU event to fire after the specified cycles or nanoseconds
event_t *scheduler_new_cpu_event(struct scheduler *restrict scheduler, event_callback_t callback, void *source,
                                 uint64_t data, uint64_t cycles, uint64_t ns);

// Remove all events matching the given callback (and optionally source) from the queue
void remove_event(struct scheduler *restrict scheduler, event_callback_t callback, void *source);

// Remove events matching callback, source, and data value
void remove_event_by_data(struct scheduler *restrict scheduler, event_callback_t callback, void *source, uint64_t data);

// Register a new event type for checkpoint save/restore
void scheduler_new_event_type(struct scheduler *restrict scheduler, const char *source_name, void *source,
                              const char *event_name, event_callback_t callback);

// Time and cycle queries

// Returns the current cpu_cycles including in-progress sprint execution
extern uint64_t scheduler_cpu_cycles(struct scheduler *restrict scheduler);

// Get current emulated time in nanoseconds
extern double scheduler_time_ns(struct scheduler *restrict scheduler);

// Execution control

// Main loop iteration for real-time emulation with VBL-based timing.  The
// WASM/web2 RAF entry point: maps elapsed host time onto whole VBL frame-units
// and runs them via scheduler_run_frame().
void scheduler_main_loop(config_t *restrict config, double now_msecs);

// Run one VBL frame-unit: pulse the machine's VBL line (trigger_vbl) then run
// exactly one VBL period of emulated time.  This is the atomic unit shared by
// every target's run loop — web2's scheduler_main_loop() calls it once per
// host-clock VBL, the headless pump calls it once per synthetic tick — so the
// guest sees an identical [VBL, run-period, VBL, run-period, …] sequence on all
// targets, differing only in how fast the host issues the ticks.
void scheduler_run_frame(struct scheduler *restrict s, config_t *config);

// Run the scheduler for a specified number of instructions
void scheduler_run_instructions(struct scheduler *restrict s, uint64_t n);

// Run the scheduler for a specified number of microseconds
void scheduler_run_usecs(struct scheduler *restrict s, uint64_t usecs);

// Complete deferred checkpoint restore after all devices have registered event types
void scheduler_start(struct scheduler *restrict s);

// Stop the scheduler immediately, halting CPU execution
void scheduler_stop(struct scheduler *restrict scheduler);

// Set the scheduler running state
void scheduler_set_running(struct scheduler *restrict scheduler, bool running);

// Check if the scheduler is currently running
bool scheduler_is_running(struct scheduler *restrict s);

// Set scheduler pacing mode (paced/unthrottled/accelerated)
void scheduler_set_mode(struct scheduler *restrict s, enum schedule_mode mode);

// Set the accelerated-mode CPU speed multiplier: 0 = auto (the adaptive
// governor picks moment to moment, bounded by max_speed), any other value
// pins a fixed multiplier (clamped to [1.0, 8.0]; stored as x256 fixed
// point). Retained across mode switches and checkpoints, but only takes
// effect while the mode is schedule_accelerated — paced and unthrottled
// always run the authentic CPI. The governor needs the paced main loop's
// host-timing signal, so headless accelerated runs use a pinned speed.
void scheduler_set_speed(struct scheduler *restrict s, double multiplier);

// Set the user cap on the accelerated-mode multiplier (clamped to
// [1.0, 8.0]): the adaptive governor's ceiling, and pinned speeds are
// clamped to it at use. Persisted with the checkpoint prefix.
void scheduler_set_max_speed(struct scheduler *restrict s, double multiplier);

// Set the CPU clock frequency in Hz (e.g. 7833600 for Plus, 15667200 for SE/30)
void scheduler_set_frequency(struct scheduler *restrict s, uint32_t frequency_hz);

// Set the per-machine cycles-per-instruction constant. Guest-visible (it sets
// how many instructions the guest retires per emulated frame) and identical
// in every pacing mode — one guest timeline.
void scheduler_set_cpi(struct scheduler *restrict s, uint32_t cpi);

// Get the total number of CPU instructions executed so far
uint64_t cpu_instr_count(void);

// Reconcile sprint counters (called from IRQ handlers to stabilize accounting)
void cpu_reschedule(void);

// `events` argv handler — typed `info_events` calls this directly.
uint64_t cmd_events(int argc, char *argv[]);

#endif // SCHEDULE_H
