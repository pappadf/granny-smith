// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scheduler.c
// Event scheduler and timing control for Granny Smith.

// ============================================================================
// Includes
// ============================================================================

#include "scheduler.h"
#include "scheduler_internal.h"

#include "cpu.h"
#include "debug.h"
#include "gs_assert.h"
#include "log.h"
#include "memory.h"
#include "object.h"
#include "shell.h"
#include "system.h"
#include "value.h"
#include "event/gs_event.h"

LOG_USE_CATEGORY_NAME("scheduler");

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

// MAC_VBL_FREQUENCY / MAC_VBL_PERIOD live in scheduler.h -- the display
// producers that raise their own frame event need the same number.
// Default cycles per instruction: the authentic average for the original
// 68000 Macs. Machines override this with one per-machine constant via
// scheduler_set_cpi(); CPI never depends on the pacing mode.
#define CYCLES_PER_INSTR_DEFAULT 12
// Fallback clock, used only until a machine calls scheduler_set_frequency().
// Every substrate now does so from hw_profile_t.freq -- five call sites, three
// direct and the rest through mac030_build_core -- so no machine's timing
// depends on this value any more.  It stays for the unit-test harness, which
// builds a scheduler with no profile behind it.
#define MAC_CPU_FREQUENCY 7833600.0
#define MAX_SANE_EVENTS   10000 // upper bound for event queue length sanity checks
// Bounds for plain-data fields restored from a checkpoint.  Real machines use
// a cpi of 1, 2, 4 or 10 (pdm, tnt, mac030/lisa, plus), so 255 is the same
// ceiling scheduler.cpi already enforces on the writable attribute; the cycle
// bound is the one the old assert used.
#define MAX_SANE_CPI        255
#define MAX_SANE_CPU_CYCLES (1ULL << 60)

// Paced mode: hard cap on frame-units executed per host tick. A slow or
// stalled host makes vbl_acc_error grow; without a cap, each oversized burst
// lengthens the next tick's period — a positive-feedback catch-up spiral
// (up to ~60 frame-units piling into one tick before the >1 s guard resets).
// With the cap, a sustained-slow host simply lags real time by a bounded
// amount instead of freezing the UI in a wall of frames.
#define PACED_MAX_CATCHUP 4

// Unthrottled ("turbo") mode: fraction of the host tick period to fill with
// emulation, leaving the rest as idle headroom for the browser (rendering,
// input, GC). Raised from 0.5: with WebGL rendering and the SAB audio ring
// the browser work per tick is small, and reserving half of every slice was
// measured to cost exactly its share of turbo throughput; 0.7 keeps the UI
// responsive (in-browser bench terminal probes still answer promptly) while
// returning ~40% more turbo speed.
#define TURBO_HOST_HEADROOM 0.7

// Adaptive governor (accelerated mode with scheduler.speed = auto). AIMD on a
// quantized speed ladder: additive rung-up only after dwelling at the current
// speed (the slew limit — instruction-counted guest delays calibrated at
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

// Host-timing estimators (paced main loop): weight of the newest sample in
// the exponentially weighted moving averages of the host loop period and the
// host cost of one frame-unit.  A per-sample weight, not a time constant: at a
// 120 Hz host the averages settle in half the wall time they take at 60 Hz.
// Host-side only -- nothing guest-visible depends on it.
#define HOST_EWMA_ALPHA 0.1
// Seed for the host-loop-period estimator: one 60 Hz display frame, the
// common browser requestAnimationFrame rate.  Only a starting point -- the
// EWMA converges on the real period within a few dozen ticks at any rate.
#define HOST_LOOP_PERIOD_SEED (1.0 / 60.0)

// ============================================================================
// Type Definitions
// ============================================================================
// struct event, event_type_t and struct scheduler live in scheduler_internal.h,
// shared with the object-model glue in scheduler_class.c.

// Checkpoint-friendly representation of an event (names instead of pointers)
typedef struct {
    uint64_t timestamp;
    char source_name[64];
    char event_name[64];
    uint64_t data;
    // Without these a restored periodic fires once and stops -- the machine
    // would come back with its timers dead and nothing would say so.
    uint64_t interval_cycles;
    uint8_t periodic;
    uint8_t pad[7];
} event_as_checkpoint_t;

// ============================================================================
// Static Helpers
// ============================================================================

// Smaller of two values; typed functions, not a macro, so each argument is
// evaluated once.
static inline uint64_t min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}
static inline uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

// Returns the per-machine cycles-per-instruction constant (mode-independent).
// This is the *authentic* CPI — invariant tolerances and event-restore checks
// key off it because the effective CPI below never exceeds it.
static inline uint32_t avg_cycles_per_instr(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    return s->saved.cpi;
}

// The speed multiplier (x256) accelerated mode runs at right now: the pinned
// setting if one is set, else the governor's current rung — clamped to the
// user cap either way.
uint32_t scheduler_current_speed_x256(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->gov_rung >= 0 && s->gov_rung < GOV_NUM_RUNGS);
    uint32_t sp = (s->pacing.speed_x256 != SPEED_X256_AUTO) ? s->pacing.speed_x256 : gov_ladder_x256[s->gov_rung];
    if (sp > s->pacing.max_speed_x256)
        sp = s->pacing.max_speed_x256;
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
// for free.
//
// Only between sprints: current_cpu_cycles derives mid-sprint "now" from the
// slots run so far times cpi_eff_x256 plus cycle_frac_x256, so changing
// either inside a sprint would re-date the cycles already run (a reconcile
// does not help -- it keeps the executed slots in the product).  Every caller
// (machine build, restore, the pacing setters, the governor) runs between
// frames, and the assert holds them to it.
static void scheduler_update_cpi_eff(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->saved.cpi > 0);
    GS_ASSERTF(s->sprint_total == 0, "effective CPI changed mid-sprint (%u slots planned)", s->sprint_total);
    uint32_t eff = s->saved.cpi << 8;
    if (s->pacing.mode == schedule_accelerated) {
        uint32_t sp = scheduler_current_speed_x256(s);
        if (sp > SPEED_X256_ONE) {
            eff = (uint32_t)(((uint64_t)s->saved.cpi << 16) / sp);
            if (eff == 0)
                eff = 1; // never a zero divisor (cpi 1 at the 8x cap is still 32)
        }
    }
    s->cpi_eff_x256 = eff;
    s->cycle_frac_x256 = 0;
    // The effective speed changed (a governor step, a pin, a mode switch):
    // say so once, here, where every path that changes it passes.
    if (scheduler_effective_speed_x256(s) != s->speed_reported_x256)
        scheduler_announce_speed(s);
}

void scheduler_announce_speed(struct scheduler *s) {
    if (!s)
        return;
    s->speed_reported_x256 = scheduler_effective_speed_x256(s);
    gs_event_emitf(GS_EVENT_STATE, "{\"event\":\"speed\",\"x256\":%u}", (unsigned)s->speed_reported_x256);
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
//     next rung stays under the target — the slew limit required so
//     instruction-counted guest delays see a stable speed, plus headroom.
// The utilization EWMA is rescaled on every step (utilization is proportional
// to instructions per frame), so the estimator stays meaningful across steps.
static void scheduler_governor_tick(struct scheduler *s, double host_secs_this_vbl, double elapsed_secs) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->pacing.mode == schedule_accelerated && s->pacing.speed_x256 == SPEED_X256_AUTO);

    // Utilization of the real-time frame budget, smoothed. Pressure registers
    // fast (protect the deadline); optimism accumulates slowly.
    double u = host_secs_this_vbl / MAC_VBL_PERIOD;
    double alpha = (u > s->gov_util_ewma) ? 0.35 : 0.10;
    s->gov_util_ewma += alpha * (u - s->gov_util_ewma);

    s->gov_dwell_secs += elapsed_secs;
    if (s->gov_holdoff_secs > 0.0)
        s->gov_holdoff_secs -= elapsed_secs;

    // Optional audio feedback: the platform reports the host ring's
    // fill fraction against its target depth, or <0 where the signal doesn't
    // exist (headless, audio idle). A draining ring means the deadline is
    // already being missed where it hurts first.
    double fill = platform_audio_ring_fill();
    bool audio_pressure = (fill >= 0.0 && fill < GOV_AUDIO_LOW);

    // Highest rung the user cap allows
    int max_rung = 0;
    while (max_rung + 1 < GOV_NUM_RUNGS && gov_ladder_x256[max_rung + 1] <= s->pacing.max_speed_x256)
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

// Returns the current cpu_cycles including in-progress sprint execution
static inline uint64_t current_cpu_cycles(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->sprint_burndown <= s->sprint_total);
    GS_ASSERT(s->saved.cpu_cycles < (1ULL << 60));

    // Base cycles plus cycles consumed in the current sprint. Fixed-point x256
    // with the carried sub-cycle remainder — exactly consistent with the
    // sprint's final accounting, so mid-sprint reads (VIA timers, event
    // scheduling) and the boundary bookkeeping agree to the cycle.
    uint64_t in_sprint = s->sprint_total - s->sprint_burndown;
    uint64_t result = s->saved.cpu_cycles + ((in_sprint * s->cpi_eff_x256 + s->cycle_frac_x256) >> 8);

    // Overflow check
    GS_ASSERT(result >= s->saved.cpu_cycles);
    return result;
}

// Validate all critical scheduler invariants at key transition points.
// GS_FAST (production profile): the full-queue walk itself is the cost — it
// runs at entry and exit of scheduler_run/scheduler_run_instructions — so the
// whole function compiles to nothing, not just its asserts.
#ifdef GS_FAST
static inline void scheduler_check_invariants(struct scheduler *s, const char *context) {
    (void)s;
    (void)context;
}
#else
static void scheduler_check_invariants(struct scheduler *s, const char *context) {
    if (s == NULL)
        return;

    // Sprint counter: burndown can never exceed total
    GS_ASSERTF(s->sprint_burndown <= s->sprint_total, "[%s] sprint_burndown(%u) > sprint_total(%u)", context,
               s->sprint_burndown, s->sprint_total);

    // cpu_cycles sanity: must be less than ~4.7M years at 7.8MHz
    GS_ASSERTF(s->saved.cpu_cycles < (1ULL << 60), "[%s] cpu_cycles overflow (%llu)", context,
               (unsigned long long)s->saved.cpu_cycles);

    // Mode must be valid
    GS_ASSERTF(s->pacing.mode == schedule_paced || s->pacing.mode == schedule_unthrottled ||
                   s->pacing.mode == schedule_accelerated,
               "[%s] invalid mode (%d)", context, s->pacing.mode);

    // Effective CPI: derived, nonzero, and never above the authentic CPI
    GS_ASSERTF(s->cpi_eff_x256 > 0 && s->cpi_eff_x256 <= (s->saved.cpi << 8), "[%s] cpi_eff_x256 out of range (%u)",
               context, s->cpi_eff_x256);
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

    // CPU interface must remain valid
    GS_ASSERTF(s->cpu.run_sprint != NULL, "[%s] cpu interface is NULL", context);
}
#endif // GS_FAST

// Convenience macro to check invariants with automatic context
#define CHECK_INVARIANTS(s) scheduler_check_invariants((s), __func__)

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
// Idempotent, and a no-op outside a sprint (sprint_total == burndown == 0),
// so callers that may or may not be inside one call it unconditionally.
static void reconcile_sprint(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->sprint_burndown <= s->sprint_total);

    uint32_t executed = s->sprint_total - s->sprint_burndown;
    s->sprint_total = executed;
    s->sprint_burndown = 0;
}

// Validate that the CPU event queue is properly ordered.
// GS_FAST: compiled out — this walk runs on every event insertion (22,257×
// per emulated second during ASC playback).
#ifdef GS_FAST
static inline void validate_cpu_events(struct scheduler *s) {
    (void)s;
}
#else
static void validate_cpu_events(struct scheduler *s) {
    GS_ASSERT(s != NULL);

    uint32_t max_past = avg_cycles_per_instr(s);
    uint64_t prev_ts = 0;
    int count = 0;

    // Only the head can lie in the past: the ordering asserted below bounds
    // every later entry by it.
    if (s->cpu_events != NULL)
        GS_ASSERTF(s->cpu_events->timestamp + max_past >= s->saved.cpu_cycles,
                   "timestamp too far in past (%llu vs cpu_cycles %llu)", (unsigned long long)s->cpu_events->timestamp,
                   (unsigned long long)s->saved.cpu_cycles);

    for (event_t *e = s->cpu_events; e != NULL; e = e->next) {
        count++;
        GS_ASSERTF(count <= MAX_SANE_EVENTS, "event queue too long or infinite loop (count=%d)", count);
        GS_ASSERT(e->callback != NULL);
        GS_ASSERTF(e->timestamp >= prev_ts, "timestamp not increasing (%llu < %llu)", (unsigned long long)e->timestamp,
                   (unsigned long long)prev_ts);
        prev_ts = e->timestamp;
    }
}
#endif // GS_FAST

// Look up registered event type names for a given source+callback pair
const event_type_t *scheduler_find_event_type(struct scheduler *s, void *source, event_callback_t cb) {
    if (!s)
        return NULL;
    for (int i = 0; i < s->num_event_types; i++) {
        if (s->event_types[i].source == source && s->event_types[i].callback == cb)
            return &s->event_types[i];
    }
    return NULL;
}

// ============================================================================
// Event allocation pool
// ============================================================================

// Events churn at the emulated-sample rate — the ASC FIFO drain re-arms one
// event per sample (22,257/s during audio playback), each a calloc at insert
// plus a free at fire.  Recycle them through a
// small LIFO free list instead.  The pool is process-global (event_t carries
// no per-scheduler state) and bounded; the live queue stays ~5 entries deep,
// so the cap covers any realistic burst.
#define EVENT_POOL_MAX 64
static event_t *g_event_pool = NULL; // LIFO free list, linked via ->next
static int g_event_pool_count = 0; // entries currently pooled

// Pop a recycled event (zeroed, like calloc) or fall back to the allocator.
static event_t *event_alloc(void) {
    event_t *e = g_event_pool;
    if (e != NULL) {
        g_event_pool = e->next;
        g_event_pool_count--;
        memset(e, 0, sizeof(*e));
        return e;
    }
    return (event_t *)calloc(1, sizeof(event_t));
}

// Return an event to the pool (or to the allocator once the pool is full).
static void event_free(event_t *e) {
    if (g_event_pool_count < EVENT_POOL_MAX) {
        e->next = g_event_pool;
        g_event_pool = e;
        g_event_pool_count++;
        return;
    }
    free(e);
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
                                   uint64_t cycles, uint64_t ns, bool periodic) {
    // Exactly one of cycles/ns must be non-zero
    GS_ASSERTF(cycles != 0 || ns != 0, "both cycles and ns are 0");
    GS_ASSERTF(!(cycles != 0 && ns != 0), "both cycles and ns are set");

    // A delay given in nanoseconds becomes cycles.  Split into whole seconds
    // and the sub-second rest so the multiply cannot overflow (the rest times
    // a 32-bit frequency stays under 2^62); the result is exactly
    // floor(ns * frequency / NS_PER_SEC).  A delay shorter than one cycle
    // rounds UP to one: the scheduler has no finer resolution, and truncating
    // to zero would fire the event at "now", ahead of the next instruction,
    // as if no delay had been asked for (and a periodic would spin).
    if (ns != 0) {
        GS_ASSERTF(ns / NS_PER_SEC <= UINT64_MAX / s->frequency, "event delay of %llu ns overflows the cycle count",
                   (unsigned long long)ns);
        cycles = (ns / NS_PER_SEC) * s->frequency + (ns % NS_PER_SEC) * s->frequency / NS_PER_SEC;
        if (cycles == 0)
            cycles = 1;
    }

    event_t *event = event_alloc();
    if (event == NULL)
        return NULL;

    event->callback = callback;
    event->source = source;
    event->data = data;
    event->periodic = periodic;
    event->interval_cycles = periodic ? cycles : 0;
    GS_ASSERTF(!periodic || cycles != 0, "periodic event with a zero interval would spin");

    // Timestamp relative to current time including in-sprint progress
    uint64_t now = current_cpu_cycles(s);
    event->timestamp = cycles + now;
    GS_ASSERT(event->timestamp >= now);

    s->cpu_events = insert_event_queue(s->cpu_events, event);
    return event;
}

// Total events dispatched since process start (diagnostic; exposed as
// scheduler.events_fired).  Process-global like the event pool.
uint64_t g_sched_events_fired = 0;

// Process all events in the queue that are due at or before current_time
static void process_event_queue(event_t **queue, uint64_t current_time) {
    GS_ASSERT(queue != NULL);

    while (*queue != NULL && (*queue)->timestamp <= current_time) {
        event_t *e = *queue;
        *queue = e->next;
        g_sched_events_fired++;

        if (e->periodic) {
            // Re-arm BEFORE the callback runs, and that ordering is
            // load-bearing.  The event is unlinked above, so a handler that
            // cancels itself with remove_event() or scheduler_forget_source()
            // would not find it -- and re-arming afterwards would then
            // silently reinstate what the handler just cancelled.  Inserting
            // first means both cancel paths see the next occurrence and
            // remove it, with no new API and no change to the callback
            // signature.
            //
            // The next deadline comes from the SCHEDULED time, not from
            // current_time, so a periodic does not drift the way a handler
            // re-arming itself from "now" does.
            event_t *next = event_alloc();
            if (next) {
                *next = *e;
                next->next = NULL;
                next->timestamp = e->timestamp + e->interval_cycles;
                *queue = insert_event_queue(*queue, next);
            }
        }

        (e->callback)(e->source, e->data);
        event_free(e);
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

// Event callback used by the run command to stop execution after a fixed
// instruction budget.  The scheduler is its own event source: the type is
// registered with source = the scheduler (scheduler_init) and the event is
// armed with source = the scheduler (scheduler_run_with_budget), which is
// what both the checkpoint name lookup and this cast rely on.
static void run_stop_event(void *source, uint64_t data) {
    (void)data;
    scheduler_t *s = (scheduler_t *)source;
    GS_ASSERT(s != NULL);
    scheduler_stop_reason(s, SCHED_STOP_BUDGET);
}

// Opens a mode: the run belongs to whoever is being served right now.
static void open_mode(struct scheduler *s, uint64_t instructions) {
    s->run_owner = gs_current_client();
    s->stop_reason = SCHED_STOP_NONE;
    s->mode_seq++;
    s->mode_open = true;
    s->mode_bounded = instructions != 0;
    s->saved.running = true;
    gs_event_emitf(GS_EVENT_STATE, "{\"event\":\"mode_started\",\"mode\":%u,\"owner\":%u,\"budget\":%llu}",
                   (unsigned)s->mode_seq, (unsigned)s->run_owner, (unsigned long long)instructions);
}

// The main CPU's pc for the mode_ended event, 0 when no debug seam exists.
static uint32_t mode_pc(void) {
    const struct cpu_debug_if *d = system_cpu_debug_if();
    return d && d->get_pc ? d->get_pc(d->ctx) : 0;
}

// Reports the end of an open mode once `running` has dropped.
static void close_mode_if_stopped(struct scheduler *s) {
    if (!s->mode_open || s->saved.running)
        return;
    s->mode_open = false;
    gs_event_emitf(GS_EVENT_STATE,
                   "{\"event\":\"mode_ended\",\"mode\":%u,\"owner\":%u,\"reason\":\"%s\",\"pc\":%u,"
                   "\"instr_count\":%llu}",
                   (unsigned)s->mode_seq, (unsigned)s->run_owner, sched_stop_reason_name(s->stop_reason),
                   (unsigned)mode_pc(), (unsigned long long)cpu_instr_count());
}

// Schedule a stop after `instructions` more instructions of execution.
// Returns false on overflow / zero-count / scheduler not initialised.
bool scheduler_run_with_budget(scheduler_t *s, uint64_t instructions) {
    GS_ASSERT(s != NULL);
    uint32_t eff_x256 = s->cpi_eff_x256;

    // Cancel any pending stop events from previous limited runs
    remove_event(s, run_stop_event, NULL);

    if (instructions == 0) {
        // Run indefinitely (caller wants to step until externally stopped).
        open_mode(s, 0);
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
    open_mode(s, instructions);
    return true;
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// Create and initialize a scheduler instance, optionally restoring from checkpoint
// Sanity-check the saved block a checkpoint just wrote into `s`.
//
// Everything `system_read_checkpoint_data` fills below is attacker-controlled:
// a checkpoint is a file the user supplies -- `checkpoint --load <path>`, a
// browser drag-and-drop, or the quick checkpoint written to OPFS every 15
// seconds -- and the build-ID gate is not a defence, because the build ID sits
// in the file and copies from any legitimate checkpoint.
//
// These checks were GS_ASSERT / GS_ASSERTF.  That was never a guard.
// gs_assert_fail() prints, pauses the scheduler and RETURNS, so even in a
// debug build execution continued into the operation the assert was standing
// in front of; and GS_FAST -- the wasm release profile and MODE=fast --
// compiles the call out entirely.  There was no build in which they protected
// anything.
//
// The cpi case was a live crash rather than a latent one.  `cpi` sits inside
// the restored block, and the next statement was
// `total_instructions = cpu_cycles / cpi`.  On WebAssembly -- the shipping
// target -- i64.div_u TRAPS when the divisor is zero, exactly as i32.div_s
// traps on INT_MIN / -1.  A
// crafted checkpoint therefore killed the browser tab, with no log line in the
// release build.  A zero cpi also poisons cpi_eff_x256, which two more divides
// depend on (:449, :1564).
//
// Returns false with the checkpoint flagged; scheduler_init then falls back to
// fresh-boot values so nothing runs on half-validated state in the window
// before system_restore observes the error.
static bool scheduler_restore_saved_ok(const struct scheduler *s, checkpoint_t *checkpoint) {
    // What does not depend on the machine.  The remainders are checked
    // against the machine's own clock once its build has set it
    // (scheduler_restore_events).
    const char *bad = NULL;
    if (s->saved.cpi == 0 || s->saved.cpi > MAX_SANE_CPI)
        bad = "cycles-per-instruction out of range";
    else if (s->saved.cpu_cycles >= MAX_SANE_CPU_CYCLES)
        bad = "cycle counter out of range";

    if (!bad)
        return true;

    LOG(0, "Error: corrupt scheduler state in checkpoint (%s); refusing the restore", bad);
    checkpoint_set_error(checkpoint);
    return false;
}

struct scheduler *scheduler_init(const sched_cpu_if_t *cpu, checkpoint_t *checkpoint) {
    GS_ASSERT(cpu != NULL);
    GS_ASSERT(cpu->run_sprint != NULL && cpu->is_stopped != NULL && cpu->poll_interrupt != NULL);

    // Zero-initialize to avoid uninitialized padding bytes
    struct scheduler *s = (struct scheduler *)calloc(1, sizeof(struct scheduler));
    if (s == NULL)
        return NULL;

    s->cpu = *cpu;
    s->cpu_events = NULL;
    s->saved.running = false;
    s->vbl_acc_error = 0;
    s->previous_time = host_time();
    s->host_secs_per_vbl = NAN;
    s->host_secs_per_loop = HOST_LOOP_PERIOD_SEED;
    s->frequency = (uint32_t)MAC_CPU_FREQUENCY;
    s->saved.cpi = CYCLES_PER_INSTR_DEFAULT;
    // The host's pacing reaches the machine when it becomes the active one
    // (system_swap_in) and with every frame after; until then it is built at
    // the default.
    s->pacing = HOST_PACING_DEFAULT;
    s->num_event_types = 0;
    memset(s->event_types, 0, sizeof(s->event_types));

    // Initialize sprint counters
    s->saved.total_instructions = 0;
    s->sprint_total = 0;
    s->sprint_burndown = 0;

    if (checkpoint != NULL) {
        // Restore the saved block from the checkpoint
        system_read_checkpoint_data(checkpoint, &s->saved, sizeof(s->saved));

        // The four wall-clock fields used to be restored here and then
        // immediately overwritten.  They are outside the saved block now, so the
        // values set above still stand and there is nothing to undo.

        if (!scheduler_restore_saved_ok(s, checkpoint)) {
            // Put the whole block back to the fresh-boot values set above, so
            // every later derivation runs on known-good numbers.  The checkpoint is already flagged; system_restore
            // unwinds when it looks.
            s->saved.cpu_cycles = 0;
            s->saved.cpi = CYCLES_PER_INSTR_DEFAULT;
            s->saved.io_penalty_remainder = 0;
            s->saved.io_stall_slots = 0;
            s->saved.frame_cycles_left = 0;
            s->saved.total_instructions = 0;
        } else {
            s->restored_cpi = s->saved.cpi;
        }

        // A machine saved while running comes back running, yet the mode is
        // not checkpointed: open one (unbounded, owned by nobody) so the
        // stop that ends this run reports mode_ended like any other.  No
        // mode_started goes out -- the restore may still be refused, and the
        // page reads the run state after every restore.
        if (s->saved.running) {
            s->run_owner = 0;
            s->stop_reason = SCHED_STOP_NONE;
            s->mode_seq++;
            s->mode_open = true;
            s->mode_bounded = false;
        }

        s->sprint_total = 0;
        s->sprint_burndown = 0;

    } else {
        // Fresh boot
        s->saved.cpu_cycles = 0;
        s->saved.total_instructions = 0;
        s->sprint_total = 0;
        s->sprint_burndown = 0;
    }

    // Derive the transient governor + effective-CPI state (fresh boot and
    // restore alike — neither is checkpointed) before anything can size a
    // sprint or read cycles.
    scheduler_governor_reset(s);
    scheduler_update_cpi_eff(s);

    // Source = the scheduler itself; run_stop_event depends on it.
    scheduler_new_event_type(s, "scheduler", s, "run_stop", run_stop_event);
    // VBL is no longer a scheduler event: every target injects it imperatively
    // via scheduler_run_frame() (see scheduler_main_loop / the headless pump),
    // so no 'vbl_tick' event type is registered and none is ever checkpointed.

    // Object-tree binding — instance_data is the scheduler itself.
    s->object = scheduler_object_new(s);

    return s;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free all resources associated with a scheduler instance
void scheduler_delete(struct scheduler *s) {
    if (!s)
        return;

    // Object-tree teardown — fires invalidators before any internal state goes.
    scheduler_object_delete(s->object);
    s->object = NULL;

    // Free pending CPU events (through the pool so a follow-up scheduler
    // instance can recycle them)
    event_t *e = s->cpu_events;
    while (e) {
        event_t *next = e->next;
        event_free(e);
        e = next;
    }

    free(s);
}

// ============================================================================
// Lifecycle: Checkpointing
// ============================================================================

// Save scheduler state to a checkpoint
void scheduler_checkpoint(struct scheduler *restrict s, checkpoint_t *checkpoint) {
    GS_ASSERT(s != NULL && checkpoint != NULL);
    GS_ASSERT(s->cpu.run_sprint != NULL);
    GS_ASSERT(s->pacing.mode == schedule_paced || s->pacing.mode == schedule_unthrottled ||
              s->pacing.mode == schedule_accelerated);
    GS_ASSERT(s->saved.cpu_cycles < (1ULL << 60));
    GS_ASSERT(s->num_event_types >= 0 && s->num_event_types <= MAX_EVENT_TYPES);

    validate_cpu_events(s);

    // Save the checkpointed block
    system_write_checkpoint_data(checkpoint, &s->saved, sizeof(s->saved));
}

// Save the event queue: the last block of a machine checkpoint, so its
// restore runs once every event source exists (scheduler_restore_events).
void scheduler_checkpoint_events(struct scheduler *restrict s, checkpoint_t *checkpoint) {
    GS_ASSERT(s != NULL && checkpoint != NULL);
    validate_cpu_events(s);

    // Convert event queue to checkpoint-friendly format (names instead of pointers)
    unsigned int num_events = num_events_in_queue(s);
    system_write_checkpoint_data(checkpoint, &num_events, sizeof(num_events));
    // calloc(0, …) is implementation-defined (some libc return NULL, some a 1-byte sentinel).
    // Skip the alloc entirely on an empty queue.
    event_as_checkpoint_t *events_to_save =
        num_events ? (event_as_checkpoint_t *)calloc(num_events, sizeof(event_as_checkpoint_t)) : NULL;
    if (num_events && !events_to_save) {
        // The write loop below indexed this unconditionally.  Flag the
        // checkpoint rather than writing through NULL; the count is already on
        // the stream, so the restore will refuse it as a short read.
        LOG(0, "Error: out of memory saving %u scheduler events", num_events);
        checkpoint_set_error(checkpoint);
        return;
    }

    event_t *e = s->cpu_events;
    for (unsigned int i = 0; i < num_events; i++) {
        GS_ASSERT(e != NULL);
        events_to_save[i].timestamp = e->timestamp;
        events_to_save[i].data = e->data;
        events_to_save[i].interval_cycles = e->interval_cycles;
        events_to_save[i].periodic = e->periodic ? 1u : 0u;
        memset(events_to_save[i].pad, 0, sizeof(events_to_save[i].pad));

        // Look up names by source+callback pair.  A linear scan per event
        // (O(events x types)); both are a few dozen at most and this is the
        // cold save path, so no index is kept.
        const event_type_t *t = scheduler_find_event_type(s, e->source, e->callback);
        GS_ASSERTF(t != NULL, "event at timestamp %llu has no registered type", (unsigned long long)e->timestamp);
        if (t) {
            memcpy(events_to_save[i].source_name, t->source_name, sizeof(events_to_save[i].source_name));
            memcpy(events_to_save[i].event_name, t->event_name, sizeof(events_to_save[i].event_name));
        }

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

// Restore the event queue saved by scheduler_checkpoint_events.  The caller
// reads it after constructing the whole machine, so every event source has
// registered its types: each saved event binds as it is read.
void scheduler_restore_events(struct scheduler *restrict s, checkpoint_t *checkpoint) {
    GS_ASSERT(s != NULL && checkpoint != NULL);

    // The build is done: the machine has set its clock and CPI.  The CPI the
    // checkpoint carries wins (an override is part of the guest's timeline),
    // and the remainders are checked against the clock the machine runs.
    if (s->restored_cpi) {
        s->saved.cpi = s->restored_cpi;
        s->restored_cpi = 0;
        scheduler_update_cpi_eff(s);
    }
    const char *bad = NULL;
    if (s->saved.io_penalty_remainder >= (s->saved.cpi << 8))
        bad = "I/O penalty remainder out of range"; // always under one CPI
    else if (s->saved.frame_cycles_left > (uint64_t)(MAC_VBL_PERIOD * (double)s->frequency) + 2)
        bad = "VBL frame remainder out of range"; // at most one frame's cycles (scheduler_run_frame)
    if (bad) {
        LOG(0, "Error: corrupt scheduler state in checkpoint (%s); refusing the restore", bad);
        checkpoint_set_error(checkpoint);
        s->saved.io_penalty_remainder = 0;
        s->saved.io_stall_slots = 0;
        s->saved.frame_cycles_left = 0;
        return;
    }

    unsigned int num_events = 0;
    system_read_checkpoint_data(checkpoint, &num_events, sizeof(num_events));
    // An on-disk count drives the reads below, so it is checked before it is
    // used rather than asserted after.  MAX_SANE_EVENTS bounds it at ~10k
    // entries; the largest queue the corpus produces is orders of magnitude
    // smaller.
    if (checkpoint_has_error(checkpoint))
        return;
    if (num_events > MAX_SANE_EVENTS) {
        LOG(0, "Error: checkpoint claims %u pending events (cap %d); refusing the restore", num_events,
            MAX_SANE_EVENTS);
        checkpoint_set_error(checkpoint);
        return;
    }

    // Everything below comes off disk, so it is checked, not asserted: a
    // release build compiles GS_ASSERT out.  A saved event whose type nothing
    // registered -- a checkpoint from a different build -- or whose time is
    // already past fails the load, and the machine that was running stays.
    if (num_events == 0)
        return;
    event_as_checkpoint_t *all = (event_as_checkpoint_t *)malloc((size_t)num_events * sizeof(event_as_checkpoint_t));
    if (!all) {
        LOG(0, "Error: out of memory restoring %u scheduler events", num_events);
        checkpoint_set_error(checkpoint);
        return;
    }
    system_read_checkpoint_data(checkpoint, all, (size_t)num_events * sizeof(event_as_checkpoint_t));
    for (unsigned int i = 0; i < num_events && !checkpoint_has_error(checkpoint); i++) {
        event_as_checkpoint_t saved = all[i];
        // Null-terminate defensively: the names came off disk.
        saved.source_name[sizeof(saved.source_name) - 1] = '\0';
        saved.event_name[sizeof(saved.event_name) - 1] = '\0';

        int found = -1;
        for (int j = 0; j < s->num_event_types; j++) {
            if (strcmp(s->event_types[j].source_name, saved.source_name) == 0 &&
                strcmp(s->event_types[j].event_name, saved.event_name) == 0) {
                found = j;
                break;
            }
        }
        if (found < 0) {
            LOG(0, "Error: checkpoint holds a pending '%s.%s' event, and no such event type is registered",
                saved.source_name, saved.event_name);
            checkpoint_set_error(checkpoint);
            break;
        }
        // The documented invariant: timestamp + CPI >= cpu_cycles (so an
        // event that legitimately fired on the same cycle the checkpoint was
        // taken can still be restored).
        if (saved.timestamp + avg_cycles_per_instr(s) < s->saved.cpu_cycles) {
            LOG(0, "Error: checkpoint's '%s.%s' event is due at cycle %llu, before the saved clock (%llu)",
                saved.source_name, saved.event_name, (unsigned long long)saved.timestamp,
                (unsigned long long)s->saved.cpu_cycles);
            checkpoint_set_error(checkpoint);
            break;
        }

        event_t *e = event_alloc();
        GS_ASSERT(e != NULL);
        e->timestamp = saved.timestamp;
        e->callback = s->event_types[found].callback;
        e->source = s->event_types[found].source;
        e->data = saved.data;
        // A periodic with a zero interval would spin, so refuse it rather
        // than restore it -- this value came off disk like the rest.
        e->periodic = saved.periodic != 0 && saved.interval_cycles != 0;
        e->interval_cycles = e->periodic ? saved.interval_cycles : 0;
        s->cpu_events = insert_event_queue(s->cpu_events, e);
    }
    free(all);

    CHECK_INVARIANTS(s);
}

// Register a new event type for checkpoint save/restore
void scheduler_new_event_type(struct scheduler *s, const char *source_name, void *source, const char *event_name,
                              event_callback_t callback) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(source_name != NULL && source_name[0] != '\0');
    GS_ASSERT(event_name != NULL && event_name[0] != '\0');
    GS_ASSERT(callback != NULL);
    GS_ASSERT(s->num_event_types < MAX_EVENT_TYPES);

    // Update names if already registered (match on both callback AND source to allow
    // the same callback with different source pointers, e.g. VIA1 vs VIA2)
    for (int i = 0; i < s->num_event_types; i++) {
        if (s->event_types[i].callback == callback && s->event_types[i].source == source) {
            // Update names (allows via_set_instance_name to relabel entries).
            // strncpy doesn't null-terminate on full fill; do it explicitly so a
            // shorter previous name doesn't leak past a longer new name.
            strncpy(s->event_types[i].source_name, source_name, sizeof(s->event_types[i].source_name) - 1);
            s->event_types[i].source_name[sizeof(s->event_types[i].source_name) - 1] = '\0';
            strncpy(s->event_types[i].event_name, event_name, sizeof(s->event_types[i].event_name) - 1);
            s->event_types[i].event_name[sizeof(s->event_types[i].event_name) - 1] = '\0';
            return;
        }
    }

    event_type_t *et = &s->event_types[s->num_event_types];
    strncpy(et->source_name, source_name, sizeof(et->source_name) - 1);
    et->source_name[sizeof(et->source_name) - 1] = '\0';
    et->source = source;
    strncpy(et->event_name, event_name, sizeof(et->event_name) - 1);
    et->event_name[sizeof(et->event_name) - 1] = '\0';
    et->callback = callback;
    s->num_event_types++;
}

// Schedule a new CPU event to fire after the specified number of cycles or nanoseconds
event_t *scheduler_new_cpu_event_ex(struct scheduler *s, event_callback_t callback, void *source, uint64_t data,
                                    uint64_t cycles, uint64_t ns, bool periodic) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->cpu.run_sprint != NULL);
    GS_ASSERT(callback != NULL);

    // The (callback, source) pair MUST have been registered with
    // scheduler_new_event_type beforehand. Without this, the gap only
    // surfaces ~30s later at the next checkpoint save when
    // scheduler_checkpoint walks the event queue and fails to name the
    // pending entry — by which time the original call site is lost.
    // Asserting here fingers the offending caller directly. Cost is
    // O(num_event_types), typically <30 entries; trivial vs. the bug
    // class it prevents.
    //
    // Looked up BEFORE the cycles/ns checks so those can name the culprit
    // too: a bare "both cycles and ns are 0" identifies the scheduler, which
    // is never the buggy component, and leaves you grepping ~30 call sites
    // for the one whose delay computed to zero.
    //
    // Only the asserts read the result, so GS_FAST (which compiles them out)
    // skips the lookup too.
#ifndef GS_FAST
    const char *event_name = "<unregistered>";
    bool registered = false;
    for (int i = 0; i < s->num_event_types; i++) {
        if (s->event_types[i].callback == callback && s->event_types[i].source == source) {
            registered = true;
            event_name = s->event_types[i].event_name;
            break;
        }
    }
    GS_ASSERTF(registered, "scheduler_new_cpu_event: event type not registered "
                           "(call scheduler_new_event_type first for this (callback, source) pair)");

    GS_ASSERTF(cycles != 0 || ns != 0, "scheduler_new_cpu_event(%s): both cycles and ns are 0", event_name);
    GS_ASSERTF(!(cycles != 0 && ns != 0), "scheduler_new_cpu_event(%s): both cycles and ns are set (%llu, %llu)",
               event_name, (unsigned long long)cycles, (unsigned long long)ns);
#endif

    CHECK_INVARIANTS(s);
    validate_cpu_events(s);
    reconcile_sprint(s);

    event_t *result = add_event_internal(s, callback, source, data, cycles, ns, periodic);
    CHECK_INVARIANTS(s);
    return result;
}

// Unlink and free every queued event with this callback and source (NULL:
// any source) -- and, when match_data is set, this data value.  The shared
// body of remove_event and remove_event_by_data.
static void remove_matching_events(struct scheduler *restrict s, event_callback_t callback, void *source,
                                   bool match_data, uint64_t data) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(callback != NULL);

    event_t **ev = &s->cpu_events;
    while (*ev != NULL) {
        event_t *e = *ev;
        if (e->callback == callback && (e->source == source || source == NULL) && (!match_data || e->data == data)) {
            *ev = e->next;
            event_free(e);
        } else {
            ev = &e->next;
        }
    }
    CHECK_INVARIANTS(s);
}

// Remove all events matching the given callback (and optionally source) from the queue
void remove_event(struct scheduler *restrict s, event_callback_t callback, void *source) {
    remove_matching_events(s, callback, source, false, 0);
}

// Remove events matching callback, source, AND data value
void remove_event_by_data(struct scheduler *restrict s, event_callback_t callback, void *source, uint64_t data) {
    remove_matching_events(s, callback, source, true, data);
}

// Drop everything the scheduler still holds for `source`: every queued event
// whatever its callback, and the event-type registration row.
//
// This exists because remove_event() matches on callback AND source, so a
// device with N callbacks needs N calls to clean up, and the convention could
// not be kept even by people trying: a survey found 27 of 38 device destructors
// leaking at least one queued event -- appletalk.c schedules 5 and removes 2,
// adb.c 4 and 3, floppy.c 3 and 0, via.c 2 and 0.  Keyed on the source alone,
// the call is one line per destructor and cannot be half-done, which makes
// "every *_delete that owns a scheduler-visible object calls
// scheduler_forget_source before free" a rule a reviewer or a lint can check.
//
// remove_event() stays: cancelling ONE pending thing on a live device is a
// different operation from "this object is going away".
//
// DESTRUCTORS ONLY -- see the header.  Removing the type registrations is what
// makes this unsafe on a live device, and also what makes it complete on a
// dying one.
//
// Safe to call with a source the scheduler has never seen.
void scheduler_forget_source(struct scheduler *restrict s, void *source) {
    GS_ASSERT(s != NULL);
    if (source == NULL)
        return; // NULL means "any source" to remove_event; refuse it here

    // Queued events first -- these are the dangling pointers that matter.
    event_t **ev = &s->cpu_events;
    while (*ev != NULL) {
        if ((*ev)->source == source) {
            event_t *to_remove = *ev;
            *ev = to_remove->next;
            event_free(to_remove);
        } else {
            ev = &(*ev)->next;
        }
    }

    // Then the registration rows, which no per-callback remove_event can
    // reach.  Compact rather than tombstone: the table is scanned linearly by
    // find_event_type and by both checkpoint paths, and a hole would have to
    // be skipped in all three.
    int out = 0;
    for (int i = 0; i < s->num_event_types; i++) {
        if (s->event_types[i].source == source)
            continue;
        if (out != i)
            s->event_types[out] = s->event_types[i];
        out++;
    }
    s->num_event_types = out;
}

// Number of events currently queued.
int scheduler_pending_events(const struct scheduler *s) {
    GS_ASSERT(s != NULL);
    int n = 0;
    for (const event_t *e = s->cpu_events; e != NULL; e = e->next)
        n++;
    return n;
}

// Number of queued events that belong to an OBJECT, i.e. carry a non-NULL
// source -- what the teardown backstop counts.
int scheduler_pending_device_events(const struct scheduler *s) {
    GS_ASSERT(s != NULL);
    int n = 0;
    for (const event_t *e = s->cpu_events; e != NULL; e = e->next)
        if (e->source != NULL)
            n++;
    return n;
}

// Number of registered event types.
int scheduler_event_type_count(const struct scheduler *s) {
    GS_ASSERT(s != NULL);
    return s->num_event_types;
}

// Check if an event with the given callback is currently scheduled
bool has_event(struct scheduler *restrict s, event_callback_t callback) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(callback != NULL);

    for (event_t *e = s->cpu_events; e != NULL; e = e->next) {
        if (e->callback == callback)
            return true;
    }
    return false;
}

// When the LAST event still queued for this callback is due, in emulated
// nanoseconds on the same clock as scheduler_time_ns; 0 if none is queued.
// Read-only, and the same walk has_event does.
//
// keyboard.type needs it: each call paces its transitions one spacing apart
// and has to start after whatever a previous call left in flight.  That
// instant used to be a shadow field in adb_t, which meant the answer lived in
// two places and only one of them was per-machine.  Asking the queue cannot
// drift, and it survives a checkpoint restore for free -- the events are
// restored, so the answer is too.
double scheduler_last_event_ns(struct scheduler *restrict s, event_callback_t callback) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(callback != NULL);

    uint64_t last = 0;
    for (event_t *e = s->cpu_events; e != NULL; e = e->next)
        if (e->callback == callback && e->timestamp > last)
            last = e->timestamp;
    return (double)last * (1e9 / (double)s->frequency);
}

// Returns the current cpu_cycles including in-progress sprint execution
uint64_t scheduler_cpu_cycles(struct scheduler *restrict s) {
    GS_ASSERT(s != NULL);
    return current_cpu_cycles(s);
}

// Get current emulated time in nanoseconds
double scheduler_time_ns(struct scheduler *restrict s) {
    uint64_t cycles = scheduler_cpu_cycles(s);
    return (double)cycles * (1e9 / (double)s->frequency);
}

uint64_t scheduler_instr_count(struct scheduler *s) {
    if (s == NULL)
        return 0;
    GS_ASSERT(s->sprint_burndown <= s->sprint_total);
    return s->saved.total_instructions + s->sprint_total - s->sprint_burndown;
}

// Get the total number of CPU instructions executed so far
uint64_t cpu_instr_count(void) {
    return scheduler_instr_count(system_scheduler());
}

// Reconcile sprint counters (public API for external callers like IRQ handlers)
void cpu_reschedule(struct scheduler *s) {
    if (s == NULL)
        return;
    reconcile_sprint(s);
}

const char *sched_stop_reason_name(sched_stop_reason_t reason) {
    switch (reason) {
    case SCHED_STOP_NONE:
        return "none";
    case SCHED_STOP_BUDGET:
        return "budget";
    case SCHED_STOP_BREAKPOINT:
        return "breakpoint";
    case SCHED_STOP_REQUEST:
        return "stop_request";
    case SCHED_STOP_CANCELLED:
        return "cancelled";
    case SCHED_STOP_ASSERT:
        return "assert";
    }
    return "?";
}

// Stop the scheduler immediately, halting CPU execution
void scheduler_stop_reason(struct scheduler *restrict s, sched_stop_reason_t reason) {
    GS_ASSERT(s != NULL);
    if (s->saved.running)
        s->stop_reason = reason;
    s->saved.running = false;
    reconcile_sprint(s);
    // A stop from outside a frame (a scheduler.stop leaf between ticks, a
    // signal) is reported here; one from inside (budget, breakpoint) at the
    // end of the frame, once the sprint has settled.
    if (!s->in_frame)
        close_mode_if_stopped(s);
}

void scheduler_stop(struct scheduler *restrict s) {
    scheduler_stop_reason(s, SCHED_STOP_REQUEST);
}

bool scheduler_stop_owned(struct scheduler *restrict s, uint32_t owner) {
    GS_ASSERT(s != NULL);
    if (!s->saved.running || (owner != 0 && s->run_owner != owner))
        return false;
    scheduler_stop_reason(s, SCHED_STOP_REQUEST);
    return true;
}

uint32_t scheduler_run_owner(struct scheduler *restrict s) {
    return s ? s->run_owner : 0;
}

uint32_t scheduler_mode_id(struct scheduler *restrict s) {
    return s ? s->mode_seq : 0;
}

bool scheduler_mode_bounded(struct scheduler *restrict s) {
    return s && s->mode_bounded;
}

// Check if the scheduler is currently running
bool scheduler_is_running(struct scheduler *restrict s) {
    if (!s)
        return false;
    return s->saved.running;
}

// The pacing a build with no run loop of its own runs under (the unit
// suites); every platform defines platform_pacing over its run loop's setting.
__attribute__((weak)) host_pacing_t *platform_pacing(void) {
    static host_pacing_t pacing = HOST_PACING_DEFAULT;
    return &pacing;
}

// A speed multiplier clamped to [1x, 8x], as x256 fixed point.
static uint32_t speed_x256_clamped(double multiplier) {
    double clamped = multiplier;
    if (clamped < (double)SPEED_X256_ONE / 256.0)
        clamped = (double)SPEED_X256_ONE / 256.0;
    if (clamped > (double)SPEED_X256_MAX / 256.0)
        clamped = (double)SPEED_X256_MAX / 256.0;
    return (uint32_t)(clamped * 256.0 + 0.5);
}

// Pin the accelerated speed (0 = auto, the governor picks).
void host_pacing_set_speed(host_pacing_t *p, double multiplier) {
    if (!p || isnan(multiplier))
        return;
    p->speed_x256 = (multiplier == 0.0) ? SPEED_X256_AUTO : speed_x256_clamped(multiplier);
}

// Cap the accelerated speed (the governor's ceiling; a pin is clamped to it).
void host_pacing_set_max_speed(host_pacing_t *p, double multiplier) {
    if (!p || isnan(multiplier))
        return;
    p->max_speed_x256 = speed_x256_clamped(multiplier);
}

// Run under the host's pacing from now on.  Nothing to do when it is what
// the scheduler already runs under; otherwise re-derive.
void scheduler_apply_pacing(struct scheduler *restrict s, const host_pacing_t *pacing) {
    if (!s || !pacing)
        return;
    if (s->pacing.mode == pacing->mode && s->pacing.speed_x256 == pacing->speed_x256 &&
        s->pacing.max_speed_x256 == pacing->max_speed_x256)
        return;
    if (s->pacing.mode != pacing->mode) {
        // Estimator hygiene: reset the pacing estimators on every mode switch
        // so burst-shaped estimates from one mode don't leak into the first
        // ticks of the other (e.g. turbo's host_secs_per_vbl into paced
        // catch-up math).
        s->vbl_acc_error = 0.0;
        s->host_secs_per_vbl = NAN;
        s->host_secs_per_loop = HOST_LOOP_PERIOD_SEED;
    }
    s->pacing = *pacing;
    // Entering or leaving accelerated, or changing its speed, symmetrically
    // re-derives the effective CPI and clears the sub-cycle remainder, so no
    // accelerated-mode speed leaks into paced/unthrottled (or vice versa).
    // The governor restarts from the authentic floor with fresh estimators.
    scheduler_governor_reset(s);
    scheduler_update_cpi_eff(s);
}

// The speed multiplier actually applied to the CPU right now, x256 fixed point
// (256 = 1x = authentic). This is 1x in paced/unthrottled and, in accelerated
// mode, the live multiplier (pinned or the governor's current rung). It is
// what a UI should display as "how much faster than the original this CPU is
// running" — a slowly-varying, core-driven signal (the governor steps it on a
// ≥2 s dwell), suited to a push-on-change notification rather than polling.
uint32_t scheduler_effective_speed_x256(struct scheduler *restrict s) {
    if (!s || s->pacing.mode != schedule_accelerated)
        return SPEED_X256_ONE;
    return scheduler_current_speed_x256(s);
}

// Set the CPU clock frequency in Hz
void scheduler_set_frequency(struct scheduler *restrict s, uint32_t frequency_hz) {
    if (!s)
        return;
    GS_ASSERT(frequency_hz > 0);
    s->frequency = frequency_hz;
    // The VIA E-clock period (783.360 kHz) in CPU cycles x256 for the
    // E-synchronized I/O penalty (memory_io_esync_penalty), published per
    // sprint. Machines without esync-flagged I/O ranges simply never read it.
    s->esync_period_x256 = (uint32_t)(((uint64_t)frequency_hz * 256 + 783360 / 2) / 783360);
}

// Set the per-machine cycles-per-instruction constant (mode-independent)
void scheduler_set_cpi(struct scheduler *restrict s, uint32_t cpi) {
    if (!s)
        return;
    GS_ASSERT(cpi > 0);
    s->saved.cpi = cpi;
    // The effective CPI is derived from the authentic one; keep it in step
    scheduler_update_cpi_eff(s);
}

// Run the scheduler for a specified number of instructions
void scheduler_run_instructions(struct scheduler *restrict s, uint64_t n) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->cpu.run_sprint != NULL);
    GS_ASSERT(s->pacing.mode == schedule_paced || s->pacing.mode == schedule_unthrottled ||
              s->pacing.mode == schedule_accelerated);

    CHECK_INVARIANTS(s);

    if (s->cpu_events != NULL)
        GS_ASSERT(s->cpu_events->timestamp >= s->saved.cpu_cycles);

    const sched_cpu_if_t *cpu = &s->cpu;
    s->saved.running = true;

    // Check once at loop entry whether debugger is engaged
    debug_t *debugger = system_debug();
    bool debugger_active = debug_active(debugger);

    // Cycle budget at the effective CPI (exactly n * cpi when it is the
    // authentic integer CPI — paced/turbo budgets stay bit-identical).  A
    // count whose product would overflow saturates: it means "run on", and
    // the run ends long before 2^56 cycles for some other reason.
    uint64_t remaining_cycles = (n > UINT64_MAX / s->cpi_eff_x256) ? (UINT64_MAX >> 8) : ((n * s->cpi_eff_x256) >> 8);

    while (remaining_cycles > 0) {
        GS_ASSERT(s->sprint_burndown <= s->sprint_total);

        // If the CPU executed STOP (e.g. the Lisa OS scheduler's Pause), it has
        // suspended instruction fetch until an interrupt.  Don't burn billions
        // of instructions spinning — advance emulated time straight to the next
        // scheduled event so its interrupt can wake the CPU.  Idle time consumes
        // the cycle budget but executes no instructions (the correct behaviour).
        if (cpu->is_stopped(cpu->ctx)) { // Take any interrupt already pending at entry FIRST — e.g. the VBL,
            // which trigger_vbl asserts just before this loop for only a short
            // retrace window.  Advancing to the next event before checking could
            // skip past (and clear) that window, dropping the heartbeat.
            cpu->poll_interrupt(cpu->ctx);
            if (cpu->is_stopped(cpu->ctx)) { // still halted: sleep until the next event
                if (s->cpu_events == NULL)
                    break; // nothing scheduled can ever wake it; end the run
                uint64_t cte = (s->cpu_events->timestamp > s->saved.cpu_cycles)
                                   ? (s->cpu_events->timestamp - s->saved.cpu_cycles)
                                   : 0;
                uint64_t advance = min_u64(cte, remaining_cycles);
                remaining_cycles -= advance;
                s->saved.cpu_cycles += advance;
                process_event_queue(&s->cpu_events, s->saved.cpu_cycles);
                cpu->poll_interrupt(cpu->ctx); // event may have raised the IPL → take it (clears stopped)
            }
            continue;
        }

        uint64_t cycles_to_execute = remaining_cycles;

        // Clamp to next event if one exists
        if (s->cpu_events != NULL) {
            GS_ASSERT(s->cpu_events->timestamp >= s->saved.cpu_cycles);

            uint64_t cycles_to_event =
                (s->cpu_events->timestamp > s->saved.cpu_cycles) ? s->cpu_events->timestamp - s->saved.cpu_cycles : 0;
            cycles_to_execute = min_u64(cycles_to_event, remaining_cycles);
        }

        // Convert cycles to instructions for sprint (at least one whenever
        // any cycles remain -- cycles_to_instructions rounds that case up)
        uint32_t instr_to_exec = (uint32_t)cycles_to_instructions(s, cycles_to_execute);

        // Single-step when debugger is active
        if (debugger_active) {
            instr_to_exec = 1;
            // The probes run after each instruction, at the PC about to
            // execute.  A pending interrupt is taken at the sprint's entry,
            // so the handler's first instruction executed before any probe
            // saw its address: a breakpoint or PC logpoint there almost
            // never fired (#173).  Take the interrupt here and, when it
            // moved the PC, probe the handler's entry before it runs.
            const cpu_debug_if_t *dif = system_cpu_debug_if();
            uint32_t pc_before = dif ? dif->get_pc(dif->ctx) : 0;
            cpu->poll_interrupt(cpu->ctx);
            if (dif && dif->get_pc(dif->ctx) != pc_before && debug_break_and_trace()) {
                remaining_cycles = 0;
                if (s->saved.running)
                    s->stop_reason = SCHED_STOP_BREAKPOINT;
                s->saved.running = false;
                break;
            }
        }

        // Execute sprint — expose burndown pointer and CPI for I/O penalty mechanism
        s->sprint_total = instr_to_exec;
        s->sprint_burndown = instr_to_exec;
        g_sprint_burndown_ptr = &s->sprint_burndown;
        g_io_cpi_x256 = s->cpi_eff_x256;
        g_io_phantom_instructions = 0;
        g_sprint_unrun_slots = 0;
        // Sprint timebase for E-synchronized penalties: mid-sprint "now" =
        // base cycles + (slots consumed x effective CPI + carried fraction),
        // mirroring current_cpu_cycles (see memory_io_esync_penalty)
        g_sprint_base_cycles = s->saved.cpu_cycles;
        g_sprint_frac_x256 = s->cycle_frac_x256;
        g_sprint_total_slots = instr_to_exec;
        g_esync_period_x256 = s->esync_period_x256;
        // The penalty remainder lives in the scheduler; the sprint runs on its alias.
        g_io_penalty_remainder = s->saved.io_penalty_remainder;
        // A stall carried over from the previous sprint comes first: the CPU
        // is still waiting on that bus cycle.
        uint32_t owed = min_u32(s->saved.io_stall_slots, s->sprint_burndown);
        s->saved.io_stall_slots -= owed;
        s->sprint_burndown -= owed;
        g_io_phantom_instructions = owed;
        g_io_stall_owed = s->saved.io_stall_slots;
        if (s->sprint_burndown)
            cpu->run_sprint(cpu->ctx, &s->sprint_burndown);
        s->saved.io_stall_slots = g_io_stall_owed;
        s->saved.io_penalty_remainder = g_io_penalty_remainder;
        g_sprint_burndown_ptr = NULL; // no longer valid outside sprint

        // Account for executed instructions and cycles.
        // sprint_total includes both real instructions and phantom instructions
        // (burned by I/O penalties).  Phantom instructions represent bus stall
        // time: (real + phantom) * CPI = real_cycles + penalty_cycles.
        // Slots the sprint planned but never ran come off it: an exception,
        // a STOP or a trace step ended it early (memory_end_sprint).  They
        // are not time the CPU spent, so neither the clock nor the
        // instruction count advances for them; a CPU left stopped sleeps to
        // the next event on the idle path above.  Booking the whole plan
        // instead froze the CPU at the faulting instruction until whatever
        // event came next -- the length of the stall set by how far away
        // that event happened to be (scheduler.md §6.4).
        uint32_t unrun = min_u32(g_sprint_unrun_slots, s->sprint_total);
        g_sprint_unrun_slots = 0;
        uint32_t executed_slots = s->sprint_total - unrun;
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

        // Overshoot check.  The sprint can only run what it planned
        // (reconcile_sprint and an early end shrink it, nothing grows it) --
        // that is the bound that matters, so it is checked on the slots
        // themselves.  The plan is at most one instruction (the round-up of a
        // sub-instruction remainder, or a debugger single-step) past the
        // clamped cycle budget, so cycles overshoot by under one CPI.
        GS_ASSERTF(executed_slots <= instr_to_exec, "sprint ran %u slots, planned %u", executed_slots, instr_to_exec);
        if (s->cpu_events != NULL) {
            GS_ASSERT(executed_cycles <= cycles_to_execute + avg_cycles_per_instr(s));
        }

        s->saved.total_instructions += (executed_slots > phantom) ? (executed_slots - phantom) : 0;
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
        s->saved.cpu_cycles += executed_cycles;

        // Verify event queue integrity after advancing cycles.  Relaxed by
        // one CPI: the sprint may have overshot a due event by part of an
        // instruction, and that event has not fired yet.
        if (s->cpu_events != NULL) {
            GS_ASSERT(s->cpu_events->timestamp + avg_cycles_per_instr(s) >= s->saved.cpu_cycles);
        }

        // Handle debugger single-step
        if (debugger_active) {
            if (debug_break_and_trace()) {
                remaining_cycles = 0;
                if (s->saved.running)
                    s->stop_reason = SCHED_STOP_BREAKPOINT;
                s->saved.running = false;
                break;
            }
        }

        // Fire any events that are now due
        process_event_queue(&s->cpu_events, s->saved.cpu_cycles);

        // Strict now, not relaxed as above: process_event_queue fires every
        // event due at or before cpu_cycles -- including any a callback or a
        // periodic re-arm queued at or before it -- so whatever is left is
        // in the future, and a callback cannot have scheduled into the past.
        if (s->cpu_events != NULL)
            GS_ASSERT(s->cpu_events->timestamp >= s->saved.cpu_cycles);

        if (!s->saved.running) {
            reconcile_sprint(s);
            break;
        }
    }

    GS_ASSERT(s->sprint_burndown <= s->sprint_total);
    if (s->cpu_events != NULL)
        GS_ASSERT(s->cpu_events->timestamp >= s->saved.cpu_cycles);

    CHECK_INVARIANTS(s);
}

// Run one VBL frame-unit: pulse the VBL line then run exactly one VBL period.
// The atomic step every target's run loop is built from (see the header).  The
// caller decides how many frame-units to run and at what pace; this just does
// one.  The run clamps to the next event, so a run_stop_event or a breakpoint
// inside the period stops the frame early (running goes false), and the
// caller's loop sees it.
//
// A frame stopped early is RESUMED by the next call, not restarted: the VBL
// line is pulsed once per VBL period of *emulated* time, never once per call.
// Pulsing per call is what a run loop naturally does when every frame runs to
// completion, but `scheduler.run N` (small N), a breakpoint and a daemon
// client sending mid-run all end a frame early — and re-pulsing then hands the
// guest a 60 Hz tick it never earned.  Driving the emulator in 1000-instruction
// steps used to run guest time ~600x fast, which perturbs exactly the timing-
// sensitive guest code an instruction-stepped debug session is trying to
// observe.  frame_cycles_left carries the unfinished remainder.
void scheduler_run_frame(struct scheduler *restrict s, config_t *config, const host_pacing_t *pacing) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(config != NULL);
    scheduler_apply_pacing(s, pacing);

    if (s->saved.frame_cycles_left == 0) {
        trigger_vbl(config);
        // Open a new frame: one VBL period's worth of whole instructions at
        // the effective CPI, converted back to cycles, so a frame that runs
        // to completion consumes exactly that instruction budget's cycles.
        uint64_t frame_instructions =
            (uint64_t)(MAC_VBL_PERIOD * (double)s->frequency * 256.0 / (double)s->cpi_eff_x256);
        s->saved.frame_cycles_left = (frame_instructions * s->cpi_eff_x256) >> 8;
        if (s->saved.frame_cycles_left == 0)
            s->saved.frame_cycles_left = 1; // degenerate config: still make progress
    }

    // Cycles back to instructions, rounding up so the budget never falls short
    // of the remainder (a resumed tail may overshoot by under one instruction).
    uint64_t instructions = ((s->saved.frame_cycles_left << 8) + s->cpi_eff_x256 - 1) / s->cpi_eff_x256;

    uint64_t before = scheduler_cpu_cycles(s);
    s->in_frame = true;
    scheduler_run_instructions(s, instructions);
    s->in_frame = false;
    uint64_t advanced = scheduler_cpu_cycles(s) - before;

    s->saved.frame_cycles_left = (advanced < s->saved.frame_cycles_left) ? s->saved.frame_cycles_left - advanced : 0;
    close_mode_if_stopped(s);
}

// Main loop iteration for real-time emulation with VBL-based timing
void scheduler_main_loop(config_t *restrict config, double now_msecs, const host_pacing_t *pacing) {
    GS_ASSERT(config != NULL);
    GS_ASSERT(system_scheduler() != NULL);

    struct scheduler *s = system_scheduler();
    scheduler_apply_pacing(s, pacing);

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
    s->host_secs_per_loop = s->host_secs_per_loop * (1.0 - HOST_EWMA_ALPHA) + current_period * HOST_EWMA_ALPHA;
    GS_ASSERT(!isnan(s->host_secs_per_loop));

    int vbls_to_execute = 0;
    s->previous_time = now;

    switch (s->pacing.mode) {
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
            // banking unbounded catch-up work (the death spiral).
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
        scheduler_run_frame(s, config, pacing);
        executed_vbls++;
        if (!s->saved.running)
            break;
    }

    // Update smoothed host-seconds-per-VBL estimate
    double delta = host_time() - now;
    int denom = executed_vbls > 0 ? executed_vbls : 1;
    if (isnan(s->host_secs_per_vbl))
        s->host_secs_per_vbl = delta / denom;
    else
        s->host_secs_per_vbl = s->host_secs_per_vbl * (1.0 - HOST_EWMA_ALPHA) + (delta * HOST_EWMA_ALPHA) / denom;

    // Adaptive governor: accelerated mode with speed = auto learns the host's
    // headroom from the measured per-frame emulation cost. Evaluated only on
    // ticks that actually ran frame-units (no new cost sample otherwise) —
    // and never on the headless path, which doesn't come through this loop.
    if (s->pacing.mode == schedule_accelerated && s->pacing.speed_x256 == SPEED_X256_AUTO && executed_vbls > 0)
        scheduler_governor_tick(s, delta / denom, current_period);
}
