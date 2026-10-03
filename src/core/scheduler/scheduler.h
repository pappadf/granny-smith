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

// The Macintosh vertical-blanking rate: 60.15 Hz, the tick that paces Ticks,
// the Time Manager and every VBL task, on every machine from the 128K to the
// PowerPC models.  It is ONE number, so it lives in one place -- display
// producers that raise their own frame event were each carrying a private
// nanosecond literal derived from it (Civic 16625103, DAFB 16625000, the
// latter 103 ns short per frame), which is three chances to disagree about a
// constant.  A card whose timing registers give a real refresh
// rate should derive the period from THOSE and not use this; this is the
// rate of the machine's own retrace.
#define MAC_VBL_FREQUENCY 60.15
#define MAC_VBL_PERIOD    (1.0 / MAC_VBL_FREQUENCY) // seconds
#define MAC_VBL_PERIOD_NS 16625103ULL // = 1e9 / 60.15, truncated

// Three pacing modes (see docs/internals/core/scheduler/scheduler.md §10,
// docs/internals/core/scheduler/scheduler.md):
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

// Accelerated mode: bounds for the fixed speed multiplier, x256 fixed point.
// Floor 1x = authentic (the mode never runs the guest slower than real
// hardware); the 8x cap keeps instruction-timed guest code (TimeDBRA-derived
// busy-waits) from drifting absurdly far on fast hosts.
#define SPEED_X256_ONE 256
#define SPEED_X256_MAX (8 * 256)
// scheduler.speed == 0 means "auto": the adaptive governor picks the speed.
#define SPEED_X256_AUTO 0

// Pacing: how the HOST runs the guest timeline -- the mode, the pinned
// accelerated speed (SPEED_X256_AUTO lets the governor pick) and the cap on
// it.  Host policy: it lives in the platform's run loop (em_main, the
// headless daemon), outlives every machine and is never in a checkpoint.  It
// reaches a machine only as an argument of the scheduler's run step, so a new
// or restored machine simply runs under whatever the host says.
typedef struct host_pacing {
    enum schedule_mode mode;
    uint32_t speed_x256; // pinned accelerated multiplier, or SPEED_X256_AUTO
    uint32_t max_speed_x256; // cap on the accelerated multiplier
} host_pacing_t;

// The host's pacing as a process starts: paced, governor auto, 8x cap.
#define HOST_PACING_DEFAULT                                                                                            \
    ((host_pacing_t){.mode = schedule_paced, .speed_x256 = SPEED_X256_AUTO, .max_speed_x256 = SPEED_X256_MAX})

// The platform's pacing setting (its run loop owns the one instance).  A weak
// default serves a build with no run loop (the unit suites).  scheduler.mode,
// scheduler.speed and scheduler.max_speed read and write it.
host_pacing_t *platform_pacing(void);

// Set the pinned accelerated speed (0 = auto: the adaptive governor picks,
// bounded by the cap; otherwise clamped to 1x..8x) or the cap (clamped to
// 1x..8x) of a pacing setting.  Only accelerated mode uses either; paced and
// turbo always run the authentic CPI.  The governor needs the paced main
// loop's host-timing signal, so headless accelerated runs pin a speed.
void host_pacing_set_speed(host_pacing_t *p, double multiplier);
void host_pacing_set_max_speed(host_pacing_t *p, double multiplier);

struct scheduler;
typedef struct scheduler scheduler_t;

// What the scheduler needs from "the main CPU" — the four-entry seam that
// lets a future main-CPU architecture replace the 68K without touching the
// scheduler.  The struct is
// copied at scheduler_init; the ctx outlives the scheduler.
typedef struct sched_cpu_if {
    void *ctx; // the core instance
    void (*run_sprint)(void *ctx, uint32_t *instructions); // burn-down sprint
    bool (*is_stopped)(void *ctx); // halted awaiting an interrupt
    void (*poll_interrupt)(void *ctx); // service a now-eligible interrupt
} sched_cpu_if_t;

// The 68K adapter for the seam (implemented in cpu.c).
sched_cpu_if_t cpu_sched_if(struct cpu *cpu);

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Create and initialize a scheduler, optionally restoring from checkpoint.
// The interface struct is copied; every entry must be non-NULL.
struct scheduler *scheduler_init(const sched_cpu_if_t *cpu, checkpoint_t *checkpoint);

// Free all resources associated with a scheduler instance
void scheduler_delete(struct scheduler *scheduler);

// Save scheduler state to a checkpoint
void scheduler_checkpoint(struct scheduler *restrict scheduler, checkpoint_t *checkpoint);

// === Operations ===

// Event management

// Check if an event with the given callback is currently scheduled
bool has_event(struct scheduler *restrict scheduler, event_callback_t callback);

// When the last event still queued for this callback is due, in emulated
// nanoseconds (the scheduler_time_ns clock); 0 if none is queued.  See the
// definition for why keyboard.type wants this rather than a shadow copy of
// the same instant.
double scheduler_last_event_ns(struct scheduler *restrict scheduler, event_callback_t callback);

// Schedule a new CPU event to fire after the specified cycles or nanoseconds
// Arm an event.  Exactly one of `cycles` / `ns` must be non-zero; the other
// unit is derived.  The (callback, source) pair must already be registered
// with scheduler_new_event_type.
//
// An optional SEVENTH argument makes the event periodic: it re-arms itself
// inside the scheduler at timestamp + the interval it was armed with, until
// something cancels it.
//
//     scheduler_new_cpu_event(s, cb, dev, 0, 0, ns);        // one-shot
//     scheduler_new_cpu_event(s, cb, dev, 0, 0, ns, true);  // every ns
//
// There is no separate periodic API and no handle type, deliberately.  The
// units question is already answered here -- a scheduler_periodic_arm() would
// have to duplicate the cycles/ns handling or pick one and be wrong for half
// its callers -- and cancellation is already answered too: remove_event() and
// scheduler_forget_source() cancel a periodic exactly as they cancel a
// one-shot, so a repeating event adds no second lifetime to get wrong.
//
// The interval is the initial delay, and the next deadline is computed from
// the SCHEDULED time rather than from the dispatch time, so a periodic does
// not drift -- which a handler re-arming itself from "now" does.
//
// A handler MAY cancel its own event: the next occurrence is inserted before
// the callback runs, so remove_event() from inside the handler finds it.
event_t *scheduler_new_cpu_event_ex(struct scheduler *scheduler, event_callback_t callback, void *source, uint64_t data,
                                    uint64_t cycles, uint64_t ns, bool periodic);

#define SCHED_EV_SELECT_7(_1, _2, _3, _4, _5, _6, _7, NAME, ...) NAME
#define SCHED_EV_PERIODIC(s, cb, src, d, cyc, ns, per)                                                                 \
    scheduler_new_cpu_event_ex((s), (cb), (src), (d), (cyc), (ns), (per))
#define SCHED_EV_ONESHOT(s, cb, src, d, cyc, ns) scheduler_new_cpu_event_ex((s), (cb), (src), (d), (cyc), (ns), false)
#define scheduler_new_cpu_event(...)             SCHED_EV_SELECT_7(__VA_ARGS__, SCHED_EV_PERIODIC, SCHED_EV_ONESHOT)(__VA_ARGS__)

// Remove all events matching the given callback (and optionally source) from the queue
// Drop every queued event and the event-type registration held for `source`.
// One call per destructor, keyed on the object alone, so the cleanup cannot be
// half-done the way N-callbacks-N-remove_event calls repeatedly was.  Call it
// from any *_delete that owns a scheduler-visible object, before free().
//
// DESTRUCTORS ONLY.  It also removes the event-TYPE registrations, so a device
// that is still alive and schedules again afterwards trips
// scheduler_new_cpu_event's "event type not registered" assert.  To cancel one
// pending thing on a live device -- scsi_cancel_drq_service, phase_free,
// sym53c8xx_chip_reset -- use remove_event, which leaves the registration
// standing.  (Learned the hard way: routing scsi_cancel_drq_service through
// here broke iici-format-hd on the first integration run.)
void scheduler_forget_source(struct scheduler *restrict scheduler, void *source);

// Counts, for tests and introspection: queued events, and registered event
// types.  The scheduler object nodes will want both; scheduler_forget_source
// is untestable without them, since `struct scheduler` is opaque.
int scheduler_pending_events(const struct scheduler *scheduler);
int scheduler_pending_device_events(const struct scheduler *scheduler);
int scheduler_event_type_count(const struct scheduler *scheduler);

void remove_event(struct scheduler *restrict scheduler, event_callback_t callback, void *source);

// Remove events matching callback, source, and data value
void remove_event_by_data(struct scheduler *restrict scheduler, event_callback_t callback, void *source, uint64_t data);

// Register a new event type for checkpoint save/restore
void scheduler_new_event_type(struct scheduler *scheduler, const char *source_name, void *source,
                              const char *event_name, event_callback_t callback);

// Time and cycle queries

// Returns the current cpu_cycles including in-progress sprint execution
extern uint64_t scheduler_cpu_cycles(struct scheduler *restrict scheduler);

// Get current emulated time in nanoseconds
extern double scheduler_time_ns(struct scheduler *restrict scheduler);

// Execution control

// Main loop iteration for real-time emulation with VBL-based timing.  The
// WASM/web2 RAF entry point: maps elapsed host time onto whole VBL frame-units
// and runs them via scheduler_run_frame(), under the host's `pacing`.
void scheduler_main_loop(config_t *restrict config, double now_msecs, const host_pacing_t *pacing);

// Run one VBL frame-unit: pulse the machine's VBL line (trigger_vbl) then run
// exactly one VBL period of emulated time.  This is the atomic unit shared by
// every target's run loop — web2's scheduler_main_loop() calls it once per
// host-clock VBL, the headless pump calls it once per synthetic tick — so the
// guest sees an identical [VBL, run-period, VBL, run-period, …] sequence on all
// targets, differing only in how fast the host issues the ticks.
// A frame cut short (instruction budget, breakpoint, daemon client input) is
// resumed by the next call rather than restarted, so the VBL line is pulsed
// once per VBL period of emulated time however finely the caller steps.
// `pacing` is the host's setting the frame runs under (scheduler_apply_pacing).
void scheduler_run_frame(struct scheduler *restrict s, config_t *config, const host_pacing_t *pacing);

// Run under the host's `pacing` from now on: re-derives the effective CPI and
// resets the governor and pacing estimators when it differs from what the
// scheduler last ran under.  Every run step calls it; so do the scheduler.*
// pacing setters, so a change shows at once.
void scheduler_apply_pacing(struct scheduler *restrict s, const host_pacing_t *pacing);

// Run the scheduler for a specified number of instructions
void scheduler_run_instructions(struct scheduler *restrict s, uint64_t n);

// Run the scheduler for a specified number of microseconds
void scheduler_run_usecs(struct scheduler *restrict s, uint64_t usecs);

// Complete deferred checkpoint restore after all devices have registered event types
void scheduler_start(struct scheduler *restrict s);

// Why a run ended.  A mode (a run started by scheduler_run_with_budget)
// carries the reason it stopped and whose it was; scheduler_run_frame
// reports both in a mode_ended event (gs_event.h) at the point where
// `running` drops.
typedef enum sched_stop_reason {
    SCHED_STOP_NONE = 0, // still running, or never ran
    SCHED_STOP_BUDGET, // the instruction budget ran out
    SCHED_STOP_BREAKPOINT, // the debugger broke in (breakpoint, trace, watch)
    SCHED_STOP_REQUEST, // scheduler.stop, a client's stop, a signal
    SCHED_STOP_CANCELLED, // the owner's job was cancelled
    SCHED_STOP_ASSERT, // a failed assertion halted the machine
} sched_stop_reason_t;

const char *sched_stop_reason_name(sched_stop_reason_t reason);

// Stop the scheduler immediately, halting CPU execution (reason: request)
void scheduler_stop(struct scheduler *restrict scheduler);

// scheduler_stop with the reason the mode_ended event will carry.
void scheduler_stop_reason(struct scheduler *restrict scheduler, sched_stop_reason_t reason);

// Stops only a mode owned by `owner` (0: any owner).  Returns whether it
// stopped anything -- a client's stop must not end another's run.
bool scheduler_stop_owned(struct scheduler *restrict scheduler, uint32_t owner);

// The client that started the current (or last) mode, 0 for none.
uint32_t scheduler_run_owner(struct scheduler *restrict scheduler);

// The id of the current (or last) mode, counted from 1; 0 before any.
uint32_t scheduler_mode_id(struct scheduler *restrict scheduler);

// Whether the current mode has an instruction budget (it ends by itself).
// A job waits for a bounded mode it started; an unbounded run returns at
// once -- there is nothing to wait for.
bool scheduler_mode_bounded(struct scheduler *restrict scheduler);

// Start running with a stop scheduled after `instructions` more instructions
// (0 = until stopped).  scheduler_run_frame does the executing: the
// platform's loop for scheduler.run N, a loop inside the call for debug.step N.
// Returns false if the count overflows.
bool scheduler_run_with_budget(struct scheduler *s, uint64_t instructions);

// Set the scheduler running state
void scheduler_set_running(struct scheduler *restrict scheduler, bool running);

// Check if the scheduler is currently running
bool scheduler_is_running(struct scheduler *restrict s);

// The one parser of a pacing-mode name, for scheduler.mode and headless
// --speed alike: "paced", "accelerated", "turbo".  False for anything else.
bool scheduler_mode_from_string(const char *name, enum schedule_mode *out);

// The speed multiplier actually applied to the CPU right now, x256 fixed point
// (256 = 1x). 1x in paced/unthrottled; in accelerated mode the live pinned or
// governed multiplier. Divide by 256.0 for the display value.
uint32_t scheduler_effective_speed_x256(struct scheduler *restrict s);

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

#endif // SCHEDULE_H
