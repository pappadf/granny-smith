// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scheduler.c
// Event scheduler and timing control for Granny Smith.

// ============================================================================
// Includes
// ============================================================================

#include "scheduler.h"

#include "cpu.h"
#include "log.h"
#include "memory.h"
#include "object.h"
#include "shell.h"
#include "system.h"
#include "value.h"

LOG_USE_CATEGORY_NAME("scheduler");

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// Constants and Macros
// ============================================================================

#define MAC_VBL_FREQUENCY 60.15 // 60 vertical blanking interrupts per second
#define MAC_VBL_PERIOD    (1.0 / MAC_VBL_FREQUENCY) // period of one VBL in seconds
// Default cycles per instruction: the authentic average for the original
// 68000 Macs. Machines override this with one per-machine constant via
// scheduler_set_cpi(); CPI never depends on the pacing mode.
#define CYCLES_PER_INSTR_DEFAULT 12
#define MAC_CPU_FREQUENCY        7833600.0
#define MAX_EVENT_TYPES          32
#define MAX_SANE_EVENTS          10000 // upper bound for event queue length sanity checks

// Paced mode: hard cap on frame-units executed per host tick. A slow or
// stalled host makes vbl_acc_error grow; without a cap, each oversized burst
// lengthens the next tick's period — a positive-feedback catch-up spiral
// (up to ~60 frame-units piling into one tick before the >1 s guard resets).
// With the cap, a sustained-slow host simply lags real time by a bounded
// amount instead of freezing the UI in a wall of frames.
#define PACED_MAX_CATCHUP 4

// Unthrottled ("turbo") mode: fraction of the host tick period to fill with
// emulation, leaving the rest as idle headroom for the browser (rendering,
// input, GC). Previously a hard-coded 0.5.
#define TURBO_HOST_HEADROOM 0.5

// Accelerated mode: bounds for the fixed speed multiplier, x256 fixed point.
// Floor 1x = authentic (the mode never runs the guest slower than real
// hardware); the 8x cap keeps instruction-timed guest code (TimeDBRA-derived
// busy-waits) from drifting absurdly far on fast hosts
// (proposal-scheduler-accelerated-mode.md §4.3/§4.4).
#define SPEED_X256_ONE 256
#define SPEED_X256_MAX (8 * 256)
// scheduler.speed == 0 means "auto": the adaptive governor picks the speed.
#define SPEED_X256_AUTO 0

// Adaptive governor (accelerated mode with scheduler.speed = auto). AIMD on a
// quantized speed ladder: additive rung-up only after dwelling at the current
// speed (the §4.4 slew limit — instruction-counted guest delays calibrated at
// one speed must not be replayed at a glided-away one), multiplicative
// rung-down (each rung is ~x1.5) as soon as sustained utilization threatens
// the real-time deadline. Utilization = host seconds spent emulating one
// frame-unit / MAC_VBL_PERIOD; overrunning it stalls the paced accumulator,
// which surfaces as audio underruns and stutter — hence the headroom target.
#define GOV_UTIL_TARGET  0.80 // climb only if the *projected* post-climb utilization stays below this
#define GOV_UTIL_CEILING 0.90 // sustained above → back off one rung
#define GOV_DWELL_SECS   2.0 // minimum residence at a rung before climbing
#define GOV_HOLDOFF_SECS 1.0 // after a back-off, no climb attempts for this long
#define GOV_AUDIO_LOW    0.5 // audio ring below half its target depth = pressure (optional signal)

// Quantized speed steps (x256): 1x, 1.5x, 2x, 3x, 4x, 6x, 8x. Rung 0 is the
// authentic floor — the mode never runs the guest slower than real hardware.
static const uint32_t gov_ladder_x256[] = {256, 384, 512, 768, 1024, 1536, 2048};
#define GOV_NUM_RUNGS ((int)(sizeof(gov_ladder_x256) / sizeof(gov_ladder_x256[0])))

#define MIN(a, b) ((a) < (b) ? (a) : (b))

// ============================================================================
// Type Definitions
// ============================================================================

// A single scheduled event in the priority queue
struct event {
    event_t *next;
    uint64_t timestamp;
    event_callback_t callback;
    void *source;
    uint64_t data;
};

// Checkpoint-friendly representation of an event (names instead of pointers)
typedef struct {
    uint64_t timestamp;
    char source_name[64];
    char event_name[64];
    uint64_t data;
} event_as_checkpoint_t;

// Maps a source/callback pair to human-readable names for checkpointing
typedef struct {
    char source_name[64];
    void *source;
    char event_name[64];
    event_callback_t callback;
} event_type_t;

// Core scheduler state
struct scheduler {
    // Plain-data first
    enum schedule_mode mode;
    bool running;
    uint64_t cpu_cycles; // authoritative cycle counter, updated at sprint boundaries
    double previous_time; // previous time in seconds
    double vbl_acc_error; // accumulated VBL timing error (seconds)
    double host_secs_per_vbl; // smoothed host seconds per VBL
    double host_secs_per_loop; // smoothed host seconds per main loop iteration

    // Per-machine cycles-per-instruction constant — guest-visible, identical
    // in every pacing mode (one guest timeline)
    uint32_t cpi;

    // Accelerated-mode pinned speed multiplier, x256 fixed point (256 = 1x),
    // or SPEED_X256_AUTO (0) to let the adaptive governor pick. Lives in the
    // plain-data checkpoint prefix on purpose: the user's speed *setting*
    // persists; the governor's live speed below is transient. Inert unless
    // mode == schedule_accelerated.
    uint32_t speed_x256;

    // User cap on the accelerated-mode multiplier (governor ceiling; a pinned
    // speed is clamped to it too), x256 fixed point. Persisted with the
    // prefix; default 8x.
    uint32_t max_speed_x256;

    // Event type registry for checkpointing
    event_type_t event_types[MAX_EVENT_TYPES];
    int num_event_types;

    // Effective CPI, x256 fixed point: cpi << 8 in paced/unthrottled, lowered
    // (never raised) in accelerated mode. Derived by scheduler_update_cpi_eff
    // from (mode, cpi, speed setting, governor rung); deliberately after
    // event_types so it is never checkpointed — restore re-derives it.
    uint32_t cpi_eff_x256;
    // Sub-cycle remainder of sprint cycle accounting, 0..255 (x256 fractional
    // cycles). Carried across sprints so cycles advanced stay exact integers
    // and nothing is dropped; reset on mode/CPI/speed changes and on restore.
    uint32_t cycle_frac_x256;

    // Adaptive-governor state — live controller state, never checkpointed
    // (like the host-timing estimators above the prefix boundary): a restored
    // or mode-switched machine re-learns its speed from fresh measurements.
    int gov_rung; // current index into gov_ladder_x256 (0 = authentic 1x)
    double gov_util_ewma; // smoothed utilization (host secs per frame / VBL period)
    double gov_dwell_secs; // host seconds spent at the current rung
    double gov_holdoff_secs; // remaining post-back-off climb holdoff

    // Sprint execution counters (previously file-scope globals)
    uint64_t total_instructions; // accumulated instructions from completed sprints
    uint32_t sprint_total; // instructions planned for current sprint
    uint32_t sprint_burndown; // instructions remaining in current sprint

    // Pointers last
    struct cpu *cpu;
    event_t *cpu_events; // priority queue sorted by timestamp

    // Temporary storage used during checkpoint restore
    unsigned int tmp_num_events;
    event_as_checkpoint_t *tmp_events;

    uint32_t frequency;

    // Object-tree binding — lifetime tied to scheduler_init / scheduler_delete.
    struct object *object;
};

// Read a POSIX clock as nanoseconds.  CLOCK_PROCESS_CPUTIME_ID measures
// pure user-mode CPU time consumed by this process across all threads;
// CLOCK_MONOTONIC measures wall time.  On platforms that don't expose
// CLOCK_PROCESS_CPUTIME_ID, we fall back to CLOCK_MONOTONIC for both.
static inline uint64_t host_clock_ns(clockid_t clk) {
    struct timespec ts;
    if (clock_gettime(clk, &ts) != 0) {
        // Fallback if the requested clock isn't available
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
            return 0;
    }
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

// ============================================================================
// Forward Declarations
// ============================================================================

static uint64_t current_cpu_cycles(struct scheduler *s);
static int num_events_in_queue(struct scheduler *restrict s);

extern const class_desc_t scheduler_class;

// ============================================================================
// Static Helpers
// ============================================================================

// Returns the per-machine cycles-per-instruction constant (mode-independent).
// This is the *authentic* CPI — invariant tolerances and event-restore checks
// key off it because the effective CPI below never exceeds it.
static inline uint32_t avg_cycles_per_instr(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    return s->cpi;
}

// The speed multiplier (x256) accelerated mode runs at right now: the pinned
// setting if one is set, else the governor's current rung — clamped to the
// user cap either way.
static uint32_t scheduler_current_speed_x256(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->gov_rung >= 0 && s->gov_rung < GOV_NUM_RUNGS);
    uint32_t sp = (s->speed_x256 != SPEED_X256_AUTO) ? s->speed_x256 : gov_ladder_x256[s->gov_rung];
    if (sp > s->max_speed_x256)
        sp = s->max_speed_x256;
    if (sp < SPEED_X256_ONE)
        sp = SPEED_X256_ONE;
    return sp;
}

// Re-derive the effective CPI (x256) from mode, authentic CPI and the current
// speed (pinned or governed), and clear the sub-cycle remainder so no
// fractional carry leaks across a mode/CPI/speed change. The effective CPI
// equals the authentic CPI everywhere except accelerated mode, where it is
// lowered — never raised — so more instructions fit in the same (real-time)
// cycle budget. Holding the cycle rate and tuning CPI is what keeps every
// cycle-derived peripheral clock (VIA φ2, sound scan, SCC fallback) real-time
// for free (proposal §3).
static void scheduler_update_cpi_eff(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->cpi > 0);
    uint32_t eff = s->cpi << 8;
    if (s->mode == schedule_accelerated) {
        uint32_t sp = scheduler_current_speed_x256(s);
        if (sp > SPEED_X256_ONE) {
            eff = (uint32_t)(((uint64_t)s->cpi << 16) / sp);
            if (eff == 0)
                eff = 1; // never a zero divisor (cpi 1 at the 8x cap is still 32)
        }
    }
    s->cpi_eff_x256 = eff;
    s->cycle_frac_x256 = 0;
}

// Reset the adaptive governor to the authentic floor with fresh estimators.
// Called wherever its measurements go stale: mode switches, pin/unpin, cap
// changes, init and checkpoint restore — the controller re-learns the host's
// headroom from scratch rather than acting on stale utilization.
static void scheduler_governor_reset(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    s->gov_rung = 0;
    s->gov_util_ewma = 0.0;
    s->gov_dwell_secs = 0.0;
    s->gov_holdoff_secs = 0.0;
}

// One adaptive-governor evaluation (accelerated mode, speed = auto). Fed from
// the paced main loop with the measured host cost of one frame-unit and the
// elapsed host time since the previous tick. AIMD on the quantized ladder:
//   - back off one rung immediately when sustained utilization crosses the
//     ceiling or the (optional) audio ring drains below half target — the
//     multiplicative decrease that keeps a spike from becoming a spiral;
//   - climb one rung only after GOV_DWELL_SECS of residence, outside the
//     post-back-off holdoff, and only if the utilization *projected* at the
//     next rung stays under the target — the slew limit §4.4 requires so
//     instruction-counted guest delays see a stable speed, plus headroom.
// The utilization EWMA is rescaled on every step (utilization is proportional
// to instructions per frame), so the estimator stays meaningful across steps.
static void scheduler_governor_tick(struct scheduler *s, double host_secs_this_vbl, double elapsed_secs) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->mode == schedule_accelerated && s->speed_x256 == SPEED_X256_AUTO);

    // Utilization of the real-time frame budget, smoothed. Pressure registers
    // fast (protect the deadline); optimism accumulates slowly.
    double u = host_secs_this_vbl / MAC_VBL_PERIOD;
    double alpha = (u > s->gov_util_ewma) ? 0.35 : 0.10;
    s->gov_util_ewma += alpha * (u - s->gov_util_ewma);

    s->gov_dwell_secs += elapsed_secs;
    if (s->gov_holdoff_secs > 0.0)
        s->gov_holdoff_secs -= elapsed_secs;

    // Optional audio feedback (§11.3): the platform reports the host ring's
    // fill fraction against its target depth, or <0 where the signal doesn't
    // exist (headless, audio idle). A draining ring means the deadline is
    // already being missed where it hurts first.
    double fill = platform_audio_ring_fill();
    bool audio_pressure = (fill >= 0.0 && fill < GOV_AUDIO_LOW);

    // Highest rung the user cap allows
    int max_rung = 0;
    while (max_rung + 1 < GOV_NUM_RUNGS && gov_ladder_x256[max_rung + 1] <= s->max_speed_x256)
        max_rung++;

    int new_rung = s->gov_rung;
    if ((s->gov_util_ewma > GOV_UTIL_CEILING || audio_pressure) && new_rung > 0) {
        new_rung--; // back off fast
        s->gov_holdoff_secs = GOV_HOLDOFF_SECS;
    } else if (new_rung < max_rung && s->gov_holdoff_secs <= 0.0 && s->gov_dwell_secs >= GOV_DWELL_SECS) {
        // Climb only with headroom at the *next* rung, not just the current one
        double projected = s->gov_util_ewma * (double)gov_ladder_x256[new_rung + 1] / gov_ladder_x256[new_rung];
        if (projected < GOV_UTIL_TARGET)
            new_rung++;
    }
    if (new_rung > max_rung)
        new_rung = max_rung; // cap lowered mid-run

    if (new_rung != s->gov_rung) {
        // Rescale the estimator to the new speed and require a fresh dwell
        s->gov_util_ewma *= (double)gov_ladder_x256[new_rung] / gov_ladder_x256[s->gov_rung];
        s->gov_rung = new_rung;
        s->gov_dwell_secs = 0.0;
        scheduler_update_cpi_eff(s);
    }
}

// Validate all critical scheduler invariants at key transition points
static void scheduler_check_invariants(struct scheduler *s, const char *context) {
    if (s == NULL)
        return;

    // Sprint counter: burndown can never exceed total
    GS_ASSERTF(s->sprint_burndown <= s->sprint_total, "[%s] sprint_burndown(%u) > sprint_total(%u)", context,
               s->sprint_burndown, s->sprint_total);

    // cpu_cycles sanity: must be less than ~4.7M years at 7.8MHz
    GS_ASSERTF(s->cpu_cycles < (1ULL << 60), "[%s] cpu_cycles overflow (%llu)", context,
               (unsigned long long)s->cpu_cycles);

    // Mode must be valid
    GS_ASSERTF(s->mode == schedule_paced || s->mode == schedule_unthrottled || s->mode == schedule_accelerated,
               "[%s] invalid mode (%d)", context, s->mode);

    // Effective CPI: derived, nonzero, and never above the authentic CPI
    GS_ASSERTF(s->cpi_eff_x256 > 0 && s->cpi_eff_x256 <= (s->cpi << 8), "[%s] cpi_eff_x256 out of range (%u)", context,
               s->cpi_eff_x256);
    GS_ASSERTF(s->cycle_frac_x256 < 256, "[%s] cycle_frac_x256 out of range (%u)", context, s->cycle_frac_x256);

    // Event queue: first event must not be too far in the past (allow CPI overshoot)
    if (s->cpu_events != NULL) {
        uint64_t now = current_cpu_cycles(s);
        uint32_t max_past = avg_cycles_per_instr(s);
        GS_ASSERTF(s->cpu_events->timestamp + max_past >= now, "[%s] first event too far in past: ts=%llu now=%llu",
                   context, (unsigned long long)s->cpu_events->timestamp, (unsigned long long)now);
    }

    // Event queue internal ordering: timestamps must be non-decreasing, callbacks non-NULL
    if (s->cpu_events != NULL) {
        uint64_t prev_ts = 0;
        int idx = 0;
        for (event_t *e = s->cpu_events; e != NULL; e = e->next) {
            GS_ASSERTF(e->timestamp >= prev_ts, "[%s] event queue not sorted at index %d", context, idx);
            GS_ASSERTF(e->callback != NULL, "[%s] NULL callback at index %d", context, idx);
            prev_ts = e->timestamp;
            idx++;
            // Guard against corrupted list
            GS_ASSERTF(idx <= MAX_SANE_EVENTS, "[%s] event queue too long or loop detected", context);
        }
    }

    // Timing accumulator sanity (warning only, not a hard assert)
    if (!isnan(s->vbl_acc_error)) {
        if (s->vbl_acc_error > 10.0 || s->vbl_acc_error < -10.0) {
            LOG(1, "[%s] vbl_acc_error out of bounds (%f)", context, s->vbl_acc_error);
        }
    }

    // CPU pointer must remain valid
    GS_ASSERTF(s->cpu != NULL, "[%s] cpu pointer is NULL", context);
}

// Convenience macro to check invariants with automatic context
#define CHECK_INVARIANTS(s) scheduler_check_invariants((s), __func__)

// Returns the current cpu_cycles including in-progress sprint execution
static uint64_t current_cpu_cycles(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->sprint_burndown <= s->sprint_total);
    GS_ASSERT(s->cpu_cycles < (1ULL << 60));

    // Base cycles plus cycles consumed in the current sprint. Fixed-point x256
    // with the carried sub-cycle remainder — exactly consistent with the
    // sprint's final accounting, so mid-sprint reads (VIA timers, event
    // scheduling) and the boundary bookkeeping agree to the cycle.
    uint64_t in_sprint = s->sprint_total - s->sprint_burndown;
    uint64_t result = s->cpu_cycles + ((in_sprint * s->cpi_eff_x256 + s->cycle_frac_x256) >> 8);

    // Overflow check
    GS_ASSERT(result >= s->cpu_cycles);
    return result;
}

// Convert CPU cycles to instruction count using the effective CPI (x256)
static uint64_t cycles_to_instructions(struct scheduler *restrict s, uint64_t cycles) {
    GS_ASSERT(s->cpi_eff_x256 > 0);

    uint64_t n = (cycles << 8) / s->cpi_eff_x256;

    // At least 1 instruction if any cycles remain
    if (n == 0 && cycles > 0)
        return 1;

    return n;
}

// Reconcile sprint counters so that cpu_instr_count() returns a stable value.
// Must be called before scheduling events mid-sprint.
// Does NOT update cpu_cycles — that happens at sprint boundaries.
static void reconcile_sprint(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->sprint_burndown <= s->sprint_total);

    uint32_t executed = s->sprint_total - s->sprint_burndown;
    s->sprint_total = executed;
    s->sprint_burndown = 0;
}

// Validate that the CPU event queue is properly ordered
static void validate_cpu_events(struct scheduler *s) {
    GS_ASSERT(s != NULL);

    uint32_t max_past = avg_cycles_per_instr(s);
    uint64_t prev_ts = 0;
    int count = 0;

    for (event_t *e = s->cpu_events; e != NULL; e = e->next) {
        count++;
        GS_ASSERTF(count <= MAX_SANE_EVENTS, "event queue too long or infinite loop (count=%d)", count);
        GS_ASSERT(e->callback != NULL);
        GS_ASSERTF(e->timestamp >= prev_ts, "timestamp not increasing (%llu < %llu)", (unsigned long long)e->timestamp,
                   (unsigned long long)prev_ts);
        GS_ASSERTF(e->timestamp + max_past >= s->cpu_cycles, "timestamp too far in past (%llu vs cpu_cycles %llu)",
                   (unsigned long long)e->timestamp, (unsigned long long)s->cpu_cycles);
        prev_ts = e->timestamp;
    }
}

// Look up registered event type names for a given source+callback pair
static const event_type_t *find_event_type(struct scheduler *s, void *source, event_callback_t cb) {
    if (!s)
        return NULL;
    for (int i = 0; i < s->num_event_types; i++) {
        if (s->event_types[i].source == source && s->event_types[i].callback == cb)
            return &s->event_types[i];
    }
    return NULL;
}

// Insert an event into the queue, maintaining timestamp order
static event_t *insert_event_queue(event_t *queue, event_t *new_event) {
    GS_ASSERT(new_event != NULL);
    GS_ASSERT(new_event->callback != NULL);
    GS_ASSERT(new_event->next == NULL);

    // Insert at head if queue is empty or new event fires first
    if (queue == NULL || new_event->timestamp < queue->timestamp) {
        new_event->next = queue;
        return new_event;
    }

    // Walk to insertion point
    event_t *cur = queue;
    while (cur->next != NULL && cur->next->timestamp <= new_event->timestamp)
        cur = cur->next;

    new_event->next = cur->next;
    cur->next = new_event;
    return queue;
}

// Create and insert a new event into the scheduler queue
static event_t *add_event_internal(struct scheduler *restrict s, event_callback_t callback, void *source, uint64_t data,
                                   uint64_t cycles, uint64_t ns) {
    // Exactly one of cycles/ns must be non-zero
    GS_ASSERTF(cycles != 0 || ns != 0, "both cycles and ns are 0");
    GS_ASSERTF(!(cycles != 0 && ns != 0), "both cycles and ns are set");

    // Convert between cycles and nanoseconds
    if (ns == 0)
        ns = cycles * NS_PER_SEC / s->frequency;
    else
        cycles = ns * s->frequency / NS_PER_SEC;

    event_t *event = (event_t *)calloc(1, sizeof(event_t));
    if (event == NULL)
        return NULL;

    event->callback = callback;
    event->source = source;
    event->data = data;

    // Timestamp relative to current time including in-sprint progress
    uint64_t now = current_cpu_cycles(s);
    event->timestamp = cycles + now;
    GS_ASSERT(event->timestamp >= now);

    s->cpu_events = insert_event_queue(s->cpu_events, event);
    return event;
}

// Process all events in the queue that are due at or before current_time
static void process_event_queue(event_t **queue, uint64_t current_time) {
    GS_ASSERT(queue != NULL);

    while (*queue != NULL && (*queue)->timestamp <= current_time) {
        event_t *e = *queue;
        *queue = e->next;
        (e->callback)(e->source, e->data);
        free(e);
    }
}

// Count events in the queue
static int num_events_in_queue(struct scheduler *restrict s) {
    int count = 0;
    for (event_t *e = s->cpu_events; e != NULL; e = e->next)
        count++;
    return count;
}

// ============================================================================
// Shell Commands
// ============================================================================

// Event callback used by the run command to stop execution after a fixed instruction budget
static void run_stop_event(void *source, uint64_t data) {
    scheduler_t *s = (scheduler_t *)source;
    GS_ASSERT(s != NULL);
    scheduler_stop(s);
}

// Schedule a stop after `instructions` more instructions of execution.
// Returns false on overflow / zero-count / scheduler not initialised.
static bool scheduler_run_with_budget(scheduler_t *s, uint64_t instructions) {
    GS_ASSERT(s != NULL);
    uint32_t eff_x256 = s->cpi_eff_x256;

    // Cancel any pending stop events from previous limited runs
    remove_event(s, run_stop_event, NULL);

    if (instructions == 0) {
        // Run indefinitely (caller wants to step until externally stopped).
        s->running = true;
        return true;
    }
    if (instructions > UINT64_MAX / eff_x256)
        return false;

    // Effective-CPI conversion: exact (bit-identical to instructions * cpi)
    // whenever the effective CPI is the authentic integer one (paced/turbo)
    uint64_t cycles = (instructions * eff_x256) >> 8;
    if (cycles == 0)
        cycles = 1; // sub-cycle budget (accelerated, tiny N) still needs a nonzero delay
    scheduler_new_cpu_event(s, run_stop_event, s, 0, cycles, 0);
    s->running = true;
    return true;
}

// Shell command to print a readable view of the event queue
uint64_t cmd_events(int argc, char *argv[]) {
    struct scheduler *s = system_scheduler();
    GS_ASSERT(s != NULL);

    int count = num_events_in_queue(s);

    printf("Event queue: %d pending | cpu_cycles=%llu | instr=%llu | freq=%llu Hz\n", count,
           (unsigned long long)s->cpu_cycles, (unsigned long long)cpu_instr_count(), (unsigned long long)s->frequency);

    if (count == 0)
        return 0;

    printf("#   when(cyc)      +Δcyc     +Δµs    source.event                       data\n");

    int idx = 0;
    for (event_t *e = s->cpu_events; e; e = e->next) {
        int64_t delta_cycles = (int64_t)e->timestamp - (int64_t)s->cpu_cycles;
        uint64_t abs_delta = (delta_cycles >= 0) ? (uint64_t)delta_cycles : (uint64_t)(-delta_cycles);

        // Convert cycle delta to microseconds for display
        double delta_us = 0.0;
        if (s->frequency != 0)
            delta_us = (double)(abs_delta * 1000000000ULL / s->frequency) / 1000.0;

        // Look up human-readable name
        const event_type_t *t = find_event_type(s, e->source, e->callback);
        char namebuf[96];
        if (t)
            snprintf(namebuf, sizeof(namebuf), "%s.%s", t->source_name, t->event_name);
        else
            snprintf(namebuf, sizeof(namebuf), "unknown.unknown");

        printf("%-3d %-13llu %-9lld %8.3f  %-30s  0x%016llx\n", idx, (unsigned long long)e->timestamp,
               (long long)delta_cycles, delta_us, namebuf, (unsigned long long)e->data);
        idx++;
    }

    return 0;
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// Create and initialize a scheduler instance, optionally restoring from checkpoint
struct scheduler *scheduler_init(struct cpu *cpu, checkpoint_t *checkpoint) {
    GS_ASSERT(cpu != NULL);

    // Zero-initialize to avoid uninitialized padding bytes
    struct scheduler *s = (struct scheduler *)calloc(1, sizeof(struct scheduler));
    if (s == NULL)
        return NULL;

    s->cpu = cpu;
    s->cpu_events = NULL;
    s->running = false;
    s->vbl_acc_error = 0;
    s->previous_time = host_time();
    s->host_secs_per_vbl = NAN;
    s->host_secs_per_loop = 1.0 / 60.0;
    s->frequency = (uint32_t)MAC_CPU_FREQUENCY;
    s->cpi = CYCLES_PER_INSTR_DEFAULT;
    s->speed_x256 = SPEED_X256_AUTO;
    s->max_speed_x256 = SPEED_X256_MAX;
    s->num_event_types = 0;
    memset(s->event_types, 0, sizeof(s->event_types));

    // Initialize sprint counters
    s->total_instructions = 0;
    s->sprint_total = 0;
    s->sprint_burndown = 0;

    if (checkpoint != NULL) {
        // Restore plain-data portion of struct from checkpoint
        system_read_checkpoint_data(checkpoint, s, offsetof(struct scheduler, event_types));

        // Reset host-timing fields that are relative to wall-clock time
        s->previous_time = host_time();
        s->vbl_acc_error = 0.0;
        s->host_secs_per_vbl = NAN;
        s->host_secs_per_loop = 1.0 / 60.0;

        // Reconstruct instruction count from restored cycle counter. Cycles
        // are the sole ground truth for time; total_instructions is
        // display-only. The mapping is exact for a timeline that never ran
        // accelerated; time spent at a lowered effective CPI makes it an
        // underestimate — acceptable for a display counter, and the reason
        // nothing derives timing from it.
        GS_ASSERT(s->cpi > 0);
        s->total_instructions = s->cpu_cycles / s->cpi;
        s->sprint_total = 0;
        s->sprint_burndown = 0;

        GS_ASSERT(s->mode == schedule_paced || s->mode == schedule_unthrottled || s->mode == schedule_accelerated);
        GS_ASSERT(s->cpu_cycles < (1ULL << 60));

        // Checkpoints are build-ID-gated so the fields are always present,
        // but corrupt values must not poison the effective-CPI derivation.
        // speed: 0 (auto) or a pinned multiplier in [1x, 8x].
        if (s->speed_x256 != SPEED_X256_AUTO && (s->speed_x256 < SPEED_X256_ONE || s->speed_x256 > SPEED_X256_MAX))
            s->speed_x256 = SPEED_X256_AUTO;
        if (s->max_speed_x256 < SPEED_X256_ONE || s->max_speed_x256 > SPEED_X256_MAX)
            s->max_speed_x256 = SPEED_X256_MAX;

        // Save event data for deferred restoration (names must be resolved after device registration).
        system_read_checkpoint_data(checkpoint, &s->tmp_num_events, sizeof(s->tmp_num_events));
        // Reject negative-decoded / absurdly-large counts on a corrupt checkpoint.
        GS_ASSERTF(s->tmp_num_events <= MAX_SANE_EVENTS, "checkpoint claims %u pending events (cap %d)",
                   s->tmp_num_events, MAX_SANE_EVENTS);
        if (s->tmp_num_events == 0) {
            s->tmp_events = NULL;
        } else {
            s->tmp_events = (event_as_checkpoint_t *)malloc((size_t)s->tmp_num_events * sizeof(event_as_checkpoint_t));
            GS_ASSERT(s->tmp_events != NULL);
            system_read_checkpoint_data(checkpoint, s->tmp_events,
                                        (size_t)s->tmp_num_events * sizeof(event_as_checkpoint_t));
        }
    } else {
        // Fresh boot
        s->mode = schedule_paced;
        s->cpu_cycles = 0;
        s->total_instructions = 0;
        s->sprint_total = 0;
        s->sprint_burndown = 0;
        s->tmp_num_events = 0;
        s->tmp_events = NULL;
    }

    // Derive the transient governor + effective-CPI state (fresh boot and
    // restore alike — neither is checkpointed) before anything can size a
    // sprint or read cycles.
    scheduler_governor_reset(s);
    scheduler_update_cpi_eff(s);

    scheduler_new_event_type(s, "scheduler", s, "run_stop", run_stop_event);
    // VBL is no longer a scheduler event: every target injects it imperatively
    // via scheduler_run_frame() (see scheduler_main_loop / the headless pump),
    // so no 'vbl_tick' event type is registered and none is ever checkpointed.

    // Object-tree binding — instance_data is the scheduler itself.
    s->object = object_new(&scheduler_class, s, "scheduler");
    if (s->object)
        object_attach(object_root(), s->object);

    return s;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free all resources associated with a scheduler instance
void scheduler_delete(struct scheduler *scheduler) {
    if (!scheduler)
        return;

    // Object-tree teardown — fires invalidators before any internal state goes.
    if (scheduler->object) {
        object_detach(scheduler->object);
        object_delete(scheduler->object);
        scheduler->object = NULL;
    }

    // Free pending CPU events
    event_t *e = scheduler->cpu_events;
    while (e) {
        event_t *next = e->next;
        free(e);
        e = next;
    }

    // Free temporary checkpoint restore data
    if (scheduler->tmp_events) {
        free(scheduler->tmp_events);
        scheduler->tmp_events = NULL;
    }

    free(scheduler);
}

// ============================================================================
// Lifecycle: Checkpointing
// ============================================================================

// Save scheduler state to a checkpoint
void scheduler_checkpoint(struct scheduler *restrict scheduler, checkpoint_t *checkpoint) {
    GS_ASSERT(scheduler != NULL && checkpoint != NULL);
    GS_ASSERT(scheduler->cpu != NULL);
    GS_ASSERT(scheduler->mode == schedule_paced || scheduler->mode == schedule_unthrottled ||
              scheduler->mode == schedule_accelerated);
    GS_ASSERT(scheduler->cpu_cycles < (1ULL << 60));
    GS_ASSERT(scheduler->num_event_types >= 0 && scheduler->num_event_types <= MAX_EVENT_TYPES);

    validate_cpu_events(scheduler);

    // Save plain-data portion of struct
    system_write_checkpoint_data(checkpoint, scheduler, offsetof(struct scheduler, event_types));

    // Convert event queue to checkpoint-friendly format (names instead of pointers)
    unsigned int num_events = num_events_in_queue(scheduler);
    system_write_checkpoint_data(checkpoint, &num_events, sizeof(num_events));
    // calloc(0, …) is implementation-defined (some libc return NULL, some a 1-byte sentinel).
    // Skip the alloc entirely on an empty queue.
    event_as_checkpoint_t *events_to_save =
        num_events ? (event_as_checkpoint_t *)calloc(num_events, sizeof(event_as_checkpoint_t)) : NULL;

    event_t *e = scheduler->cpu_events;
    for (unsigned int i = 0; i < num_events; i++) {
        GS_ASSERT(e != NULL);
        events_to_save[i].timestamp = e->timestamp;
        events_to_save[i].data = e->data;

        // Look up names by source+callback pair
        bool found = false;
        for (int j = 0; j < scheduler->num_event_types; j++) {
            if (scheduler->event_types[j].callback == e->callback && scheduler->event_types[j].source == e->source) {
                memcpy(events_to_save[i].source_name, scheduler->event_types[j].source_name,
                       sizeof(events_to_save[i].source_name));
                memcpy(events_to_save[i].event_name, scheduler->event_types[j].event_name,
                       sizeof(events_to_save[i].event_name));
                found = true;
                break;
            }
        }
        GS_ASSERTF(found, "event at timestamp %llu has no registered type", (unsigned long long)e->timestamp);

        e = e->next;
    }

    if (num_events) {
        system_write_checkpoint_data(checkpoint, events_to_save, num_events * sizeof(event_as_checkpoint_t));
        free(events_to_save);
    }
}

// ============================================================================
// Operations
// ============================================================================

// Complete deferred checkpoint restore after all devices have registered event types
void scheduler_start(struct scheduler *restrict s) {
    GS_ASSERT(s != NULL);

    // Nothing to restore if not from checkpoint
    if (s->tmp_events == NULL)
        return;

    // Resolve saved event names to live pointers and rebuild the event queue
    for (unsigned int i = 0; i < s->tmp_num_events; i++) {
        event_as_checkpoint_t *saved = &s->tmp_events[i];

        // Find matching registered event type by name. The name buffers are
        // null-terminated on both sides, so strcmp catches differences past
        // the buffer length too (a 64-byte strncmp would alias collisions).
        int found = -1;
        for (int j = 0; j < s->num_event_types; j++) {
            if (strcmp(s->event_types[j].source_name, saved->source_name) == 0 &&
                strcmp(s->event_types[j].event_name, saved->event_name) == 0) {
                found = j;
                break;
            }
        }

        GS_ASSERTF(found >= 0, "cannot restore event '%s.%s' — type not registered", saved->source_name,
                   saved->event_name);
        // Match the documented invariant: timestamp + CPI >= cpu_cycles (so an
        // event that legitimately fired on the same cycle the checkpoint was
        // taken can still be restored).
        GS_ASSERTF(saved->timestamp + avg_cycles_per_instr(s) >= s->cpu_cycles,
                   "restored event timestamp (%llu) too far past cpu_cycles (%llu) for '%s.%s'",
                   (unsigned long long)saved->timestamp, (unsigned long long)s->cpu_cycles, saved->source_name,
                   saved->event_name);

        // Recreate and insert event
        event_t *e = (event_t *)calloc(1, sizeof(event_t));
        GS_ASSERT(e != NULL);

        e->timestamp = saved->timestamp;
        e->callback = s->event_types[found].callback;
        e->source = s->event_types[found].source;
        e->data = saved->data;

        s->cpu_events = insert_event_queue(s->cpu_events, e);
    }

    // Cleanup temporary storage
    free(s->tmp_events);
    s->tmp_events = NULL;
    s->tmp_num_events = 0;

    CHECK_INVARIANTS(s);
}

// Register a new event type for checkpoint save/restore
void scheduler_new_event_type(struct scheduler *restrict scheduler, const char *source_name, void *source,
                              const char *event_name, event_callback_t callback) {
    GS_ASSERT(scheduler != NULL);
    GS_ASSERT(source_name != NULL && source_name[0] != '\0');
    GS_ASSERT(event_name != NULL && event_name[0] != '\0');
    GS_ASSERT(callback != NULL);
    GS_ASSERT(scheduler->num_event_types < MAX_EVENT_TYPES);

    // Update names if already registered (match on both callback AND source to allow
    // the same callback with different source pointers, e.g. VIA1 vs VIA2)
    for (int i = 0; i < scheduler->num_event_types; i++) {
        if (scheduler->event_types[i].callback == callback && scheduler->event_types[i].source == source) {
            // Update names (allows via_set_instance_name to relabel entries).
            // strncpy doesn't null-terminate on full fill; do it explicitly so a
            // shorter previous name doesn't leak past a longer new name.
            strncpy(scheduler->event_types[i].source_name, source_name,
                    sizeof(scheduler->event_types[i].source_name) - 1);
            scheduler->event_types[i].source_name[sizeof(scheduler->event_types[i].source_name) - 1] = '\0';
            strncpy(scheduler->event_types[i].event_name, event_name, sizeof(scheduler->event_types[i].event_name) - 1);
            scheduler->event_types[i].event_name[sizeof(scheduler->event_types[i].event_name) - 1] = '\0';
            return;
        }
    }

    event_type_t *et = &scheduler->event_types[scheduler->num_event_types];
    strncpy(et->source_name, source_name, sizeof(et->source_name) - 1);
    et->source_name[sizeof(et->source_name) - 1] = '\0';
    et->source = source;
    strncpy(et->event_name, event_name, sizeof(et->event_name) - 1);
    et->event_name[sizeof(et->event_name) - 1] = '\0';
    et->callback = callback;
    scheduler->num_event_types++;
}

// Schedule a new CPU event to fire after the specified number of cycles or nanoseconds
event_t *scheduler_new_cpu_event(struct scheduler *restrict scheduler, event_callback_t callback, void *source,
                                 uint64_t data, uint64_t cycles, uint64_t ns) {
    GS_ASSERT(scheduler != NULL);
    GS_ASSERT(scheduler->cpu != NULL);
    GS_ASSERT(callback != NULL);
    GS_ASSERT(cycles != 0 || ns != 0);
    GS_ASSERT(!(cycles != 0 && ns != 0));

    // The (callback, source) pair MUST have been registered with
    // scheduler_new_event_type beforehand. Without this, the gap only
    // surfaces ~30s later at the next checkpoint save when
    // scheduler_checkpoint walks the event queue and fails to name the
    // pending entry — by which time the original call site is lost.
    // Asserting here fingers the offending caller directly. Cost is
    // O(num_event_types), typically <30 entries; trivial vs. the bug
    // class it prevents.
    bool registered = false;
    for (int i = 0; i < scheduler->num_event_types; i++) {
        if (scheduler->event_types[i].callback == callback && scheduler->event_types[i].source == source) {
            registered = true;
            break;
        }
    }
    GS_ASSERTF(registered, "scheduler_new_cpu_event: event type not registered "
                           "(call scheduler_new_event_type first for this (callback, source) pair)");

    CHECK_INVARIANTS(scheduler);
    validate_cpu_events(scheduler);
    reconcile_sprint(scheduler);

    event_t *result = add_event_internal(scheduler, callback, source, data, cycles, ns);
    CHECK_INVARIANTS(scheduler);
    return result;
}

// Remove all events matching the given callback (and optionally source) from the queue
void remove_event(struct scheduler *restrict scheduler, event_callback_t callback, void *source) {
    GS_ASSERT(scheduler != NULL);
    GS_ASSERT(callback != NULL);

    event_t **ev = &scheduler->cpu_events;
    while (*ev != NULL) {
        if ((*ev)->callback == callback && ((*ev)->source == source || source == NULL)) {
            event_t *to_remove = *ev;
            *ev = to_remove->next;
            free(to_remove);
        } else {
            ev = &(*ev)->next;
        }
    }
}

// Remove events matching callback, source, AND data value
void remove_event_by_data(struct scheduler *restrict scheduler, event_callback_t callback, void *source,
                          uint64_t data) {
    GS_ASSERT(scheduler != NULL);
    GS_ASSERT(callback != NULL);

    event_t **ev = &scheduler->cpu_events;
    while (*ev != NULL) {
        if ((*ev)->callback == callback && ((*ev)->source == source || source == NULL) && (*ev)->data == data) {
            event_t *to_remove = *ev;
            *ev = to_remove->next;
            free(to_remove);
        } else {
            ev = &(*ev)->next;
        }
    }
}

// Check if an event with the given callback is currently scheduled
bool has_event(struct scheduler *restrict scheduler, event_callback_t callback) {
    GS_ASSERT(scheduler != NULL);
    GS_ASSERT(callback != NULL);

    for (event_t *e = scheduler->cpu_events; e != NULL; e = e->next) {
        if (e->callback == callback)
            return true;
    }
    return false;
}

// Returns the current cpu_cycles including in-progress sprint execution
uint64_t scheduler_cpu_cycles(struct scheduler *restrict scheduler) {
    GS_ASSERT(scheduler != NULL);
    return current_cpu_cycles(scheduler);
}

// Get current emulated time in nanoseconds
double scheduler_time_ns(struct scheduler *restrict scheduler) {
    uint64_t cycles = scheduler_cpu_cycles(scheduler);
    return (double)cycles * (1e9 / (double)scheduler->frequency);
}

// Get the total number of CPU instructions executed so far
uint64_t cpu_instr_count(void) {
    struct scheduler *s = system_scheduler();
    if (s == NULL)
        return 0;
    GS_ASSERT(s->sprint_burndown <= s->sprint_total);
    return s->total_instructions + s->sprint_total - s->sprint_burndown;
}

// Reconcile sprint counters (public API for external callers like IRQ handlers)
void cpu_reschedule(void) {
    struct scheduler *s = system_scheduler();
    if (s == NULL)
        return;
    reconcile_sprint(s);
}

// Stop the scheduler immediately, halting CPU execution
void scheduler_stop(struct scheduler *restrict scheduler) {
    GS_ASSERT(scheduler != NULL);
    scheduler->running = false;
    reconcile_sprint(scheduler);
}

// Set the scheduler running state
void scheduler_set_running(struct scheduler *restrict scheduler, bool running) {
    if (!scheduler)
        return;
    scheduler->running = running;
}

// Check if the scheduler is currently running
bool scheduler_is_running(struct scheduler *restrict s) {
    if (!s)
        return false;
    return s->running;
}

// Set the scheduler pacing mode (paced, unthrottled or accelerated)
void scheduler_set_mode(struct scheduler *restrict s, enum schedule_mode mode) {
    if (!s)
        return;
    if (s->mode == mode)
        return;
    s->mode = mode;
    // Estimator hygiene: reset the pacing estimators on every mode switch so
    // burst-shaped estimates from one mode don't leak into the first ticks of
    // the other (e.g. turbo's host_secs_per_vbl into paced catch-up math).
    s->vbl_acc_error = 0.0;
    s->host_secs_per_vbl = NAN;
    s->host_secs_per_loop = 1.0 / 60.0;
    // Entering or leaving accelerated symmetrically re-derives the effective
    // CPI and clears the sub-cycle remainder, so no accelerated-mode speed
    // leaks into paced/unthrottled (or vice versa). The governor restarts
    // from the authentic floor with fresh estimators.
    scheduler_governor_reset(s);
    scheduler_update_cpi_eff(s);
}

// Set the accelerated-mode speed multiplier: 0 = auto (the adaptive governor
// picks, bounded by max_speed), any other value pins a fixed multiplier
// (clamped to [1x, 8x] — the §4.4 correctness-safe configuration, and the
// only way to accelerate headless runs, which have no host-timing signal for
// the governor). Retained across mode switches/checkpoints but inert outside
// schedule_accelerated.
void scheduler_set_speed(struct scheduler *restrict s, double multiplier) {
    if (!s)
        return;
    if (isnan(multiplier))
        return;
    if (multiplier == 0.0) {
        s->speed_x256 = SPEED_X256_AUTO;
    } else {
        double clamped = multiplier;
        if (clamped < (double)SPEED_X256_ONE / 256.0)
            clamped = (double)SPEED_X256_ONE / 256.0;
        if (clamped > (double)SPEED_X256_MAX / 256.0)
            clamped = (double)SPEED_X256_MAX / 256.0;
        s->speed_x256 = (uint32_t)(clamped * 256.0 + 0.5);
    }
    scheduler_governor_reset(s);
    scheduler_update_cpi_eff(s);
}

// Set the user cap on the accelerated-mode multiplier (governor ceiling; a
// pinned speed is clamped to it at use). Clamped to [1x, 8x]; persisted.
void scheduler_set_max_speed(struct scheduler *restrict s, double multiplier) {
    if (!s)
        return;
    if (isnan(multiplier))
        return;
    double clamped = multiplier;
    if (clamped < (double)SPEED_X256_ONE / 256.0)
        clamped = (double)SPEED_X256_ONE / 256.0;
    if (clamped > (double)SPEED_X256_MAX / 256.0)
        clamped = (double)SPEED_X256_MAX / 256.0;
    s->max_speed_x256 = (uint32_t)(clamped * 256.0 + 0.5);
    scheduler_governor_reset(s);
    scheduler_update_cpi_eff(s);
}

// Set the CPU clock frequency in Hz
void scheduler_set_frequency(struct scheduler *restrict s, uint32_t frequency_hz) {
    if (!s)
        return;
    GS_ASSERT(frequency_hz > 0);
    s->frequency = frequency_hz;
    // Publish the VIA E-clock period (783.360 kHz) in CPU cycles x256 for the
    // E-synchronized I/O penalty (memory_io_esync_penalty). Machines without
    // esync-flagged I/O ranges simply never read it.
    g_esync_period_x256 = (uint32_t)(((uint64_t)frequency_hz * 256 + 783360 / 2) / 783360);
}

// Set the per-machine cycles-per-instruction constant (mode-independent)
void scheduler_set_cpi(struct scheduler *restrict s, uint32_t cpi) {
    if (!s)
        return;
    GS_ASSERT(cpi > 0);
    s->cpi = cpi;
    // The effective CPI is derived from the authentic one; keep it in step
    scheduler_update_cpi_eff(s);
}

// Run the scheduler for a specified number of instructions
void scheduler_run_instructions(struct scheduler *restrict s, uint64_t n) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->cpu != NULL);
    GS_ASSERT(s->mode == schedule_paced || s->mode == schedule_unthrottled || s->mode == schedule_accelerated);

    CHECK_INVARIANTS(s);

    if (s->cpu_events != NULL)
        GS_ASSERT(s->cpu_events->timestamp >= s->cpu_cycles);

    cpu_t *cpu = s->cpu;
    s->running = true;

    // Check once at loop entry whether debugger is engaged
    debug_t *debugger = system_debug();
    bool debugger_active = debug_active(debugger);

    // Cycle budget at the effective CPI (exactly n * cpi when it is the
    // authentic integer CPI — paced/turbo budgets stay bit-identical)
    uint64_t remaining_cycles = (n * s->cpi_eff_x256) >> 8;

    while (remaining_cycles > 0) {
        GS_ASSERT(s->sprint_burndown <= s->sprint_total);

        // If the CPU executed STOP (e.g. the Lisa OS scheduler's Pause), it has
        // suspended instruction fetch until an interrupt.  Don't burn billions
        // of instructions spinning — advance emulated time straight to the next
        // scheduled event so its interrupt can wake the CPU.  Idle time consumes
        // the cycle budget but executes no instructions (the correct behaviour).
        if (cpu_is_stopped(cpu)) { // Take any interrupt already pending at entry FIRST — e.g. the VBL,
            // which trigger_vbl asserts just before this loop for only a short
            // retrace window.  Advancing to the next event before checking could
            // skip past (and clear) that window, dropping the heartbeat.
            cpu_poll_interrupt(cpu);
            if (cpu_is_stopped(cpu)) { // still halted: sleep until the next event
                if (s->cpu_events == NULL)
                    break; // nothing scheduled can ever wake it; end the run
                uint64_t cte =
                    (s->cpu_events->timestamp > s->cpu_cycles) ? (s->cpu_events->timestamp - s->cpu_cycles) : 0;
                uint64_t advance = MIN(cte, remaining_cycles);
                remaining_cycles -= advance;
                s->cpu_cycles += advance;
                process_event_queue(&s->cpu_events, s->cpu_cycles);
                cpu_poll_interrupt(cpu); // event may have raised the IPL → take it (clears stopped)
            }
            continue;
        }

        uint64_t cycles_to_execute = remaining_cycles;

        // Clamp to next event if one exists
        if (s->cpu_events != NULL) {
            GS_ASSERT(s->cpu_events->timestamp >= s->cpu_cycles);

            uint64_t cycles_to_event =
                (s->cpu_events->timestamp > s->cpu_cycles) ? s->cpu_events->timestamp - s->cpu_cycles : 0;
            cycles_to_execute = MIN(cycles_to_event, remaining_cycles);
        }

        // Convert cycles to instructions for sprint
        uint32_t instr_to_exec = (uint32_t)cycles_to_instructions(s, cycles_to_execute);
        if (cycles_to_execute > 0 && instr_to_exec == 0)
            instr_to_exec = 1;

        // Single-step when debugger is active
        if (debugger_active)
            instr_to_exec = 1;

        // Execute sprint — expose burndown pointer and CPI for I/O penalty mechanism
        s->sprint_total = instr_to_exec;
        s->sprint_burndown = instr_to_exec;
        g_sprint_burndown_ptr = &s->sprint_burndown;
        g_io_cpi_x256 = s->cpi_eff_x256;
        g_io_phantom_instructions = 0;
        // Sprint timebase for E-synchronized penalties: mid-sprint "now" =
        // base cycles + (slots consumed x effective CPI + carried fraction),
        // mirroring current_cpu_cycles (see memory_io_esync_penalty)
        g_sprint_base_cycles = s->cpu_cycles;
        g_sprint_frac_x256 = s->cycle_frac_x256;
        g_sprint_total_slots = instr_to_exec;
        // Note: g_io_penalty_remainder is NOT reset — it carries across sprints
        cpu_run_sprint(cpu, &s->sprint_burndown);
        g_sprint_burndown_ptr = NULL; // no longer valid outside sprint

        // Account for executed instructions and cycles.
        // sprint_total includes both real instructions and phantom instructions
        // (burned by I/O penalties).  Phantom instructions represent bus stall
        // time: (real + phantom) * CPI = real_cycles + penalty_cycles.
        uint32_t executed_slots = s->sprint_total;
        uint32_t phantom = g_io_phantom_instructions;
        g_io_phantom_instructions = 0;
        s->sprint_total = 0;
        // Fixed-point x256: whole cycles advance the clock, the sub-cycle
        // remainder carries in scheduler state so nothing is ever dropped —
        // cycle-timestamped events and the peripheral divisors observe an
        // exact integer cycle timeline at every effective CPI.
        uint64_t advance_x256 = (uint64_t)executed_slots * s->cpi_eff_x256 + s->cycle_frac_x256;
        uint32_t executed_cycles = (uint32_t)(advance_x256 >> 8);
        s->cycle_frac_x256 = (uint32_t)(advance_x256 & 0xFF);

        // Overshoot check: allow up to one (authentic-CPI) instruction of overshoot
        if (s->cpu_events != NULL) {
            GS_ASSERT(executed_cycles <= cycles_to_execute + avg_cycles_per_instr(s));
        }

        s->total_instructions += (executed_slots > phantom) ? (executed_slots - phantom) : 0;
        // The final sprint can overshoot the remaining budget by under one
        // instruction — a fractional effective CPI makes this routine, but it
        // already happened at integer CPI whenever a STOP'd-CPU advance (raw
        // event-delta cycles) left the budget a non-multiple of CPI and the
        // tail needed the min-1-instruction bump. Saturate: the frame ends
        // having overshot by <1 instruction. (Before the x256 change this
        // subtraction wrapped the unsigned budget, silently running the
        // "frame" on to the next stop event with no VBL injection — visible
        // as an A/UX timing-phase shift when fixed; see se30-aux3-boot.)
        remaining_cycles = (executed_cycles < remaining_cycles) ? remaining_cycles - executed_cycles : 0;
        s->cpu_cycles += executed_cycles;

        // Verify event queue integrity after advancing cycles
        if (s->cpu_events != NULL) {
            GS_ASSERT(s->cpu_events->timestamp + avg_cycles_per_instr(s) >= s->cpu_cycles);
        }

        // Handle debugger single-step
        if (debugger_active) {
            if (debug_break_and_trace()) {
                remaining_cycles = 0;
                s->running = false;
                break;
            }
        }

        // Fire any events that are now due
        process_event_queue(&s->cpu_events, s->cpu_cycles);

        // Verify event callbacks didn't schedule events in the past
        if (s->cpu_events != NULL)
            GS_ASSERT(s->cpu_events->timestamp >= s->cpu_cycles);

        if (!s->running) {
            reconcile_sprint(s);
            break;
        }
    }

    GS_ASSERT(s->sprint_burndown <= s->sprint_total);
    if (s->cpu_events != NULL)
        GS_ASSERT(s->cpu_events->timestamp >= s->cpu_cycles);

    CHECK_INVARIANTS(s);
}

// Run the scheduler for a specified amount of time (in seconds)
void scheduler_run(struct scheduler *restrict s, double time) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->cpu != NULL);

    CHECK_INVARIANTS(s);

    if (s->cpu_events != NULL)
        GS_ASSERT(s->cpu_events->timestamp >= s->cpu_cycles);

    s->running = true;

    // Effective-CPI sizing. The x256 scaling is by powers of two, which is
    // exact in doubles — so with the authentic integer CPI (paced/turbo) this
    // is bit-identical to time * frequency / cpi and pinned budgets hold.
    uint64_t instructions = (uint64_t)(time * (double)s->frequency * 256.0 / (double)s->cpi_eff_x256);
    scheduler_run_instructions(s, instructions);

    CHECK_INVARIANTS(s);
}

// Run the scheduler for a specified number of microseconds
void scheduler_run_usecs(struct scheduler *restrict s, uint64_t usecs) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->cpu != NULL);

    CHECK_INVARIANTS(s);

    if (s->cpu_events != NULL)
        GS_ASSERT(s->cpu_events->timestamp >= s->cpu_cycles);

    s->running = true;

    // a*256 / (b*256) == a/b in integer math, so the authentic-CPI result is
    // unchanged; accelerated lowers the effective CPI for more instructions
    uint64_t instructions = (usecs * s->frequency * 256ULL) / (1000000ULL * s->cpi_eff_x256);
    scheduler_run_instructions(s, instructions);
}

// Run one VBL frame-unit: pulse the VBL line then run exactly one VBL period.
// The atomic step every target's run loop is built from (see the header).  The
// caller decides how many frame-units to run and at what pace; this just does
// one.  scheduler_run() clamps to the next event, so a run_stop_event or a
// breakpoint inside the period stops the frame early (running goes false), and
// the caller's loop sees it.
void scheduler_run_frame(struct scheduler *restrict s, config_t *config) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(config != NULL);
    trigger_vbl(config);
    scheduler_run(s, MAC_VBL_PERIOD);
}

// Main loop iteration for real-time emulation with VBL-based timing
void scheduler_main_loop(config_t *restrict config, double now_msecs) {
    GS_ASSERT(config != NULL);
    GS_ASSERT(system_scheduler() != NULL);

    struct scheduler *s = system_scheduler();

    CHECK_INVARIANTS(s);
    GS_ASSERT(!isnan(s->vbl_acc_error));
    GS_ASSERT(!isnan(s->previous_time));
    GS_ASSERT(now_msecs >= 0.0 && !isnan(now_msecs));

    // Convert milliseconds to seconds
    double now = now_msecs / 1000.0;

    double current_period = now - s->previous_time;

    // Skip if too much time has elapsed (e.g. tab was backgrounded)
    if (current_period > 1.0) {
        s->previous_time = now;
        return;
    }

    // Exponentially weighted moving average of loop period
    s->host_secs_per_loop = s->host_secs_per_loop * 0.9 + current_period * 0.1;
    GS_ASSERT(!isnan(s->host_secs_per_loop));

    int vbls_to_execute = 0;
    s->previous_time = now;

    switch (s->mode) {
    case schedule_unthrottled:
        // Execute as many VBLs as fit in TURBO_HOST_HEADROOM of the host loop
        // period. host_secs_per_vbl starts NAN (and is re-NAN'd on checkpoint
        // restore and mode switch); guard the first tick explicitly — a
        // float→int conversion of NaN is undefined behaviour.
        if (isnan(s->host_secs_per_vbl) || s->host_secs_per_vbl <= 0.0)
            vbls_to_execute = 1;
        else
            vbls_to_execute = (int)(s->host_secs_per_loop * TURBO_HOST_HEADROOM / s->host_secs_per_vbl);
        if (vbls_to_execute < 1)
            vbls_to_execute = 1;
        break;

    case schedule_paced:
    case schedule_accelerated:
        // Wall-clock accumulator: run one frame-unit per accumulated VBL
        // period. At a ~60 Hz host this is 1 per tick in steady state (one
        // repeat/skip every ~7 s of oscillator drift); on 59.94/75/120/144 Hz
        // and VRR displays the long-term rate converges to 60.147 Hz exactly.
        // Accelerated shares this branch by design: it differs from paced
        // only in the effective CPI *inside* each frame-unit, never in how
        // many frame-units a host tick earns — that is what keeps its
        // timebase (VBL/VIA/sound) locked to real time.
        s->vbl_acc_error += current_period;
        if (s->vbl_acc_error >= MAC_VBL_PERIOD) {
            vbls_to_execute = (int)(s->vbl_acc_error / MAC_VBL_PERIOD);
            if (vbls_to_execute > PACED_MAX_CATCHUP)
                vbls_to_execute = PACED_MAX_CATCHUP;
            s->vbl_acc_error -= vbls_to_execute * MAC_VBL_PERIOD;
            // Saturate the debt a sustained-slow host can accumulate: the
            // emulator lags real time by at most one capped burst instead of
            // banking unbounded catch-up work (§8.1 death-spiral).
            if (s->vbl_acc_error > PACED_MAX_CATCHUP * MAC_VBL_PERIOD)
                s->vbl_acc_error = PACED_MAX_CATCHUP * MAC_VBL_PERIOD;
        }
        break;

    default:
        GS_ASSERT(0);
    }

    GS_ASSERT(!isnan(s->vbl_acc_error));

    now = host_time();

    // Execute the VBL frame-units the host clock earned this tick.  Each is a
    // trigger_vbl + one-VBL-period run (scheduler_run_frame) — the same unit the
    // headless pump runs, so the guest execution sequence is identical across
    // targets; only how many units a single host tick batches differs.
    int executed_vbls = 0;
    for (int i = 0; i < vbls_to_execute; i++) {
        scheduler_run_frame(s, config);
        executed_vbls++;
        if (!s->running)
            break;
    }

    // Update smoothed host-seconds-per-VBL estimate
    double delta = host_time() - now;
    int denom = executed_vbls > 0 ? executed_vbls : 1;
    if (isnan(s->host_secs_per_vbl))
        s->host_secs_per_vbl = delta / denom;
    else
        s->host_secs_per_vbl = s->host_secs_per_vbl * 0.9 + (delta * 0.1) / denom;

    // Adaptive governor: accelerated mode with speed = auto learns the host's
    // headroom from the measured per-frame emulation cost. Evaluated only on
    // ticks that actually ran frame-units (no new cost sample otherwise) —
    // and never on the headless path, which doesn't come through this loop.
    if (s->mode == schedule_accelerated && s->speed_x256 == SPEED_X256_AUTO && executed_vbls > 0)
        scheduler_governor_tick(s, delta / denom, current_period);
}

// ============================================================================
// Object-model class descriptor
// ============================================================================

static scheduler_t *sched_self_from(struct object *self) {
    return (scheduler_t *)object_data(self);
}

static const char *mode_label(enum schedule_mode m) {
    switch (m) {
    case schedule_paced:
        return "paced";
    case schedule_unthrottled:
        return "turbo";
    case schedule_accelerated:
        return "accelerated";
    default:
        return "?";
    }
}

static value_t sched_attr_running(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(scheduler_is_running(sched_self_from(self)));
}

static value_t sched_attr_mode_get(struct object *self, const member_t *m) {
    (void)m;
    return val_str(mode_label(sched_self_from(self)->mode));
}
static value_t sched_attr_mode_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    scheduler_t *s = sched_self_from(self);
    enum schedule_mode mode;
    // Legacy three-mode names stay accepted as aliases so existing scripts
    // keep working: 'real'/'hw' were the wall-clock modes → paced; 'max' was
    // the run-flat-out mode → turbo.
    if (strcmp(in.s, "paced") == 0 || strcmp(in.s, "real") == 0 || strcmp(in.s, "hw") == 0)
        mode = schedule_paced;
    else if (strcmp(in.s, "turbo") == 0 || strcmp(in.s, "max") == 0)
        mode = schedule_unthrottled;
    else if (strcmp(in.s, "accelerated") == 0 || strcmp(in.s, "accel") == 0)
        mode = schedule_accelerated;
    else {
        value_t e = val_err("scheduler.mode: unknown mode '%s' (valid: paced, accelerated, turbo)", in.s);
        value_free(&in);
        return e;
    }
    value_free(&in);
    scheduler_set_mode(s, mode);
    return val_none();
}

static value_t sched_attr_cpi(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, avg_cycles_per_instr(sched_self_from(self)));
}

// Debug override for the per-machine CPI constant. Mode-independent; changing
// it mid-run alters the guest timeline from that point on, so it is a tuning
// and experimentation tool, not something the UI exposes.
static value_t sched_attr_cpi_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    if (in.u < 1 || in.u > 255)
        return val_err("scheduler.cpi: value %llu out of range (1..255)", (unsigned long long)in.u);
    scheduler_set_cpi(sched_self_from(self), (uint32_t)in.u);
    return val_none();
}

// Accelerated-mode speed multiplier. Reads back the multiplier currently in
// force for accelerated mode — the pinned value, or the adaptive governor's
// live speed when the setting is auto (so it moves on its own; "RO while
// adaptive" in the proposal's terms). Writing 0 selects auto; 1.0..8.0 pins.
// The setting is retained (and checkpointed) in every mode but only applies
// in 'accelerated'; the governor's live speed is transient.
static value_t sched_attr_speed(struct object *self, const member_t *m) {
    (void)m;
    return val_float((double)scheduler_current_speed_x256(sched_self_from(self)) / 256.0);
}
static value_t sched_attr_speed_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    if (isnan(in.f) || (in.f != 0.0 && (in.f < 1.0 || in.f > 8.0)))
        return val_err("scheduler.speed: %g out of range (0 = auto, or 1.0 .. 8.0)", in.f);
    scheduler_set_speed(sched_self_from(self), in.f);
    return val_none();
}

// Whether the adaptive governor is choosing the speed (scheduler.speed = 0)
static value_t sched_attr_speed_auto(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(sched_self_from(self)->speed_x256 == SPEED_X256_AUTO);
}

// User cap on the accelerated-mode multiplier (governor ceiling; also clamps
// a pinned speed). Persisted.
static value_t sched_attr_max_speed(struct object *self, const member_t *m) {
    (void)m;
    return val_float((double)sched_self_from(self)->max_speed_x256 / 256.0);
}
static value_t sched_attr_max_speed_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    if (isnan(in.f) || in.f < 1.0 || in.f > 8.0)
        return val_err("scheduler.max_speed: %g out of range (1.0 .. 8.0)", in.f);
    scheduler_set_max_speed(sched_self_from(self), in.f);
    return val_none();
}

static value_t sched_attr_cycles(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(8, scheduler_cpu_cycles(sched_self_from(self)));
}

static value_t sched_attr_instr_count(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(8, cpu_instr_count());
}

static value_t sched_attr_frequency(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, sched_self_from(self)->frequency);
}

// Cumulative host user-mode CPU time consumed by this process across
// all threads, in nanoseconds, since process start.  Read from POSIX
// CLOCK_PROCESS_CPUTIME_ID on every query.  Sample the delta around a
// scheduler.run call and divide instr_count delta by it for emulator
// IPS that excludes OS scheduling jitter and I/O wait.
static value_t sched_attr_host_user_ns(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(8, host_clock_ns(CLOCK_PROCESS_CPUTIME_ID));
}

// Host wall-clock time, in nanoseconds, since an unspecified monotonic
// epoch (typically boot).  Read from POSIX CLOCK_MONOTONIC on every
// query.  Sample the delta around a scheduler.run call and divide
// instr_count delta by it for perceived emulator IPS — what the user
// actually waits for.  Always >= host_user_ns delta by definition.
static value_t sched_attr_host_wall_ns(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(8, host_clock_ns(CLOCK_MONOTONIC));
}

static value_t sched_method_run(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    scheduler_t *s = sched_self_from(self);
    if (!s)
        return val_err("scheduler.run: scheduler not initialised");
    uint64_t instructions = (argc >= 1) ? argv[0].u : 0; // 0 = run-until-stopped
    if (!scheduler_run_with_budget(s, instructions))
        return val_err("scheduler.run: instruction count too large");
    return val_bool(true);
}

static value_t sched_method_stop(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    scheduler_t *s = sched_self_from(self);
    if (!s)
        return val_err("scheduler.stop: scheduler not initialised");
    scheduler_stop(s);
    return val_none();
}

static const arg_decl_t sched_run_args[] = {
    {.name = "instructions",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Optional instruction budget; 0 / omitted = run until stopped"},
};

static const member_t scheduler_members[] = {
    {.kind = M_ATTR,
     .name = "running",
     .doc = "True while the scheduler is executing instructions",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = sched_attr_running, .set = NULL}},
    {.kind = M_ATTR,
     .name = "mode",
     .doc = "Pacing mode ('paced' | 'accelerated' | 'turbo'; legacy aliases real/hw → paced, max → turbo, "
            "accel → accelerated)", .flags = 0,
     .attr = {.type = V_STRING, .get = sched_attr_mode_get, .set = sched_attr_mode_set}},
    {.kind = M_ATTR,
     .name = "cpi",
     .doc = "Per-machine cycles per instruction (mode-independent; writable as a debug override, 1..255)",
     .flags = 0,
     .attr = {.type = V_UINT, .get = sched_attr_cpi, .set = sched_attr_cpi_set}},
    {.kind = M_ATTR,
     .name = "speed",
     .doc = "Accelerated-mode CPU speed multiplier in force (live). Write 0 for auto (adaptive governor, "
            "capped by max_speed) or 1.0..8.0 to pin a fixed multiplier. Only takes effect while mode is "
            "'accelerated'; timebase (VBL/VIA/sound) stays real-time regardless", .flags = 0,
     .attr = {.type = V_FLOAT, .get = sched_attr_speed, .set = sched_attr_speed_set}},
    {.kind = M_ATTR,
     .name = "speed_auto",
     .doc = "True while the adaptive governor is choosing the accelerated-mode speed (scheduler.speed = 0)",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = sched_attr_speed_auto, .set = NULL}},
    {.kind = M_ATTR,
     .name = "max_speed",
     .doc = "Cap on the accelerated-mode multiplier (1.0..8.0): the adaptive governor's ceiling, and pinned "
            "speeds are clamped to it. Persisted", .flags = 0,
     .attr = {.type = V_FLOAT, .get = sched_attr_max_speed, .set = sched_attr_max_speed_set}},
    {.kind = M_ATTR,
     .name = "cycles",
     .doc = "Total CPU cycles executed so far",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = sched_attr_cycles, .set = NULL}},
    {.kind = M_ATTR,
     .name = "instr_count",
     .doc = "Total CPU instructions executed so far",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = sched_attr_instr_count, .set = NULL}},
    {.kind = M_ATTR,
     .name = "frequency",
     .doc = "CPU clock frequency in Hz",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = sched_attr_frequency, .set = NULL}},
    {.kind = M_ATTR,
     .name = "host_user_ns",
     .doc = "Process user-CPU time since daemon start, ns (POSIX CLOCK_PROCESS_CPUTIME_ID). "
            "Sample before+after scheduler.run; divide instr_count delta by the time delta "
            "and multiply by 1e9 for emulator throughput in instructions per CPU-second.", .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = sched_attr_host_user_ns, .set = NULL}},
    {.kind = M_ATTR,
     .name = "host_wall_ns",
     .doc = "Host monotonic wall-clock time, ns (POSIX CLOCK_MONOTONIC). "
            "Sample before+after scheduler.run; divide instr_count delta by the time delta "
            "and multiply by 1e9 for perceived emulator throughput in instructions per real second.", .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = sched_attr_host_wall_ns, .set = NULL}},
    {.kind = M_METHOD,
     .name = "run",
     .doc = "Start execution; with an instruction budget, stop after that many",
     .method = {.args = sched_run_args, .nargs = 1, .result = V_BOOL, .fn = sched_method_run}},
    {.kind = M_METHOD,
     .name = "stop",
     .doc = "Interrupt execution",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = sched_method_stop}},
};

const class_desc_t scheduler_class = {
    .name = "scheduler",
    .members = scheduler_members,
    .n_members = sizeof(scheduler_members) / sizeof(scheduler_members[0]),
};
