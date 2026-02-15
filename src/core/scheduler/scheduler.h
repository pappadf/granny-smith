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

enum schedule_mode { schedule_max_speed, schedule_real_time, schedule_hw_accuracy };

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

// Main loop iteration for real-time emulation with VBL-based timing
void scheduler_main_loop(struct config *restrict config, double now_msecs);

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

// Set scheduler execution mode (max/realtime/hardware accuracy)
void scheduler_set_mode(struct scheduler *restrict s, enum schedule_mode mode);

// Get the total number of CPU instructions executed so far
uint64_t cpu_instr_count(void);

// Reconcile sprint counters (called from IRQ handlers to stabilize accounting)
void cpu_reschedule(void);

#endif // SCHEDULE_H
