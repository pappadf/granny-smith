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

#define MAC_VBL_FREQUENCY             60.15 // 60 vertical blanking interrupts per second
#define MAC_VBL_PERIOD                (1.0 / MAC_VBL_FREQUENCY) // period of one VBL in seconds
#define CYCLES_PER_INSTR_HW_DEFAULT   12 // default cycles per instruction for hardware accuracy mode
#define CYCLES_PER_INSTR_FAST_DEFAULT 4 // default cycles per instruction for realtime and max speed modes
#define MAC_CPU_FREQUENCY             7833600.0
#define MAX_EVENT_TYPES               32
#define MAX_SANE_EVENTS               10000 // upper bound for event queue length sanity checks

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

    // Per-machine cycles-per-instruction tuning
    uint32_t cpi_hw; // cycles per instruction for hardware accuracy mode
    uint32_t cpi_fast; // cycles per instruction for realtime and max speed modes

    // Event type registry for checkpointing
    event_type_t event_types[MAX_EVENT_TYPES];
    int num_event_types;

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

// Returns cycles per instruction based on current scheduler mode
static inline uint32_t avg_cycles_per_instr(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    switch (s->mode) {
    case schedule_hw_accuracy:
        return s->cpi_hw;
    case schedule_real_time:
    case schedule_max_speed:
    default:
        return s->cpi_fast;
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
    GS_ASSERTF(s->mode >= schedule_max_speed && s->mode <= schedule_hw_accuracy, "[%s] invalid mode (%d)", context,
               s->mode);

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

    // Base cycles plus cycles consumed in the current sprint
    uint64_t in_sprint = s->sprint_total - s->sprint_burndown;
    uint64_t result = s->cpu_cycles + in_sprint * avg_cycles_per_instr(s);

    // Overflow check
    GS_ASSERT(result >= s->cpu_cycles);
    return result;
}

// Convert CPU cycles to instruction count using average cycles per instruction
static uint64_t cycles_to_instructions(struct scheduler *restrict s, uint64_t cycles) {
    GS_ASSERT(avg_cycles_per_instr(s) > 0);

    // At least 1 instruction if any cycles remain
    if (cycles < avg_cycles_per_instr(s) && cycles > 0)
        return 1;

    return cycles / avg_cycles_per_instr(s);
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
    int cpi = avg_cycles_per_instr(s);

    // Cancel any pending stop events from previous limited runs
    remove_event(s, run_stop_event, NULL);

    if (instructions == 0) {
        // Run indefinitely (caller wants to step until externally stopped).
        s->running = true;
        return true;
    }
    if (instructions > UINT64_MAX / cpi)
        return false;

    uint64_t cycles = instructions * cpi;
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
    s->cpi_hw = CYCLES_PER_INSTR_HW_DEFAULT;
    s->cpi_fast = CYCLES_PER_INSTR_FAST_DEFAULT;
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

        // Reconstruct instruction count from restored cycle counter.
        // Must use restored mode's CPI since global scheduler pointer is not yet updated.
        uint32_t restored_cpi = (s->mode == schedule_hw_accuracy) ? s->cpi_hw : s->cpi_fast;
        s->total_instructions = s->cpu_cycles / restored_cpi;
        s->sprint_total = 0;
        s->sprint_burndown = 0;

        GS_ASSERT(s->mode >= schedule_max_speed && s->mode <= schedule_hw_accuracy);
        GS_ASSERT(s->cpu_cycles < (1ULL << 60));

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
        s->mode = schedule_real_time;
        s->cpu_cycles = 0;
        s->total_instructions = 0;
        s->sprint_total = 0;
        s->sprint_burndown = 0;
        s->tmp_num_events = 0;
        s->tmp_events = NULL;
    }

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
    GS_ASSERT(scheduler->mode >= schedule_max_speed && scheduler->mode <= schedule_hw_accuracy);
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

// Set the scheduler mode (max_speed, real_time, or hw_accuracy)
void scheduler_set_mode(struct scheduler *restrict s, enum schedule_mode mode) {
    if (!s)
        return;
    if (s->mode == mode)
        return;
    s->mode = mode;
    if (mode == schedule_real_time)
        s->vbl_acc_error = 0.0; // reset accumulated timing error
}

// Set the CPU clock frequency in Hz
void scheduler_set_frequency(struct scheduler *restrict s, uint32_t frequency_hz) {
    if (!s)
        return;
    GS_ASSERT(frequency_hz > 0);
    s->frequency = frequency_hz;
}

// Set per-machine cycles-per-instruction for each scheduler mode
void scheduler_set_cpi(struct scheduler *restrict s, uint32_t cpi_hw, uint32_t cpi_fast) {
    if (!s)
        return;
    GS_ASSERT(cpi_hw > 0);
    GS_ASSERT(cpi_fast > 0);
    s->cpi_hw = cpi_hw;
    s->cpi_fast = cpi_fast;
}

// Run the scheduler for a specified number of instructions
void scheduler_run_instructions(struct scheduler *restrict s, uint64_t n) {
    GS_ASSERT(s != NULL);
    GS_ASSERT(s->cpu != NULL);
    GS_ASSERT(s->mode >= schedule_max_speed && s->mode <= schedule_hw_accuracy);

    CHECK_INVARIANTS(s);

    if (s->cpu_events != NULL)
        GS_ASSERT(s->cpu_events->timestamp >= s->cpu_cycles);

    cpu_t *cpu = s->cpu;
    s->running = true;

    // Check once at loop entry whether debugger is engaged
    debug_t *debugger = system_debug();
    bool debugger_active = debug_active(debugger);

    uint64_t remaining_cycles = n * avg_cycles_per_instr(s);

    while (remaining_cycles > 0) {
        GS_ASSERT(s->sprint_burndown <= s->sprint_total);

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
        g_io_cpi = avg_cycles_per_instr(s);
        g_io_phantom_instructions = 0;
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
        uint32_t executed_cycles = executed_slots * avg_cycles_per_instr(s);

        // Overshoot check: allow up to one instruction of overshoot
        if (s->cpu_events != NULL) {
            GS_ASSERT(executed_cycles <= cycles_to_execute + avg_cycles_per_instr(s));
        }

        s->total_instructions += (executed_slots > phantom) ? (executed_slots - phantom) : 0;
        remaining_cycles -= executed_cycles;
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

    uint64_t instructions = (uint64_t)(time * (double)s->frequency / avg_cycles_per_instr(s));
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

    uint64_t instructions = (usecs * s->frequency) / (1000000ULL * avg_cycles_per_instr(s));
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
    case schedule_max_speed:
        // Execute as many VBLs as fit in ~50% of the host loop period
        vbls_to_execute = (int)(s->host_secs_per_loop * 0.5 / s->host_secs_per_vbl);
        if (vbls_to_execute < 1)
            vbls_to_execute = 1;
        break;

    case schedule_real_time:
        // One-to-one mapping if host loop is close to VBL period
        if (current_period >= MAC_VBL_PERIOD * 0.5 && current_period <= MAC_VBL_PERIOD * 1.5) {
            vbls_to_execute = 1;
            break;
        }
        // Fall through to hw_accuracy model when host loop deviates too much.
        __attribute__((fallthrough));

    case schedule_hw_accuracy:
        // Accumulate elapsed time and execute VBLs when enough has accumulated
        s->vbl_acc_error += current_period;
        if (s->vbl_acc_error >= MAC_VBL_PERIOD * 0.1) {
            vbls_to_execute = (int)(s->vbl_acc_error / MAC_VBL_PERIOD);
            s->vbl_acc_error -= vbls_to_execute * MAC_VBL_PERIOD;
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
}

// ============================================================================
// Object-model class descriptor
// ============================================================================

static scheduler_t *sched_self_from(struct object *self) {
    return (scheduler_t *)object_data(self);
}

static const char *mode_label(enum schedule_mode m) {
    switch (m) {
    case schedule_max_speed:
        return "max";
    case schedule_real_time:
        return "real";
    case schedule_hw_accuracy:
        return "hw";
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
    if (strcmp(in.s, "max") == 0)
        mode = schedule_max_speed;
    else if (strcmp(in.s, "real") == 0)
        mode = schedule_real_time;
    else if (strcmp(in.s, "hw") == 0)
        mode = schedule_hw_accuracy;
    else {
        value_t e = val_err("scheduler.mode: unknown mode '%s' (valid: max, real, hw)", in.s);
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
     .doc = "Scheduler mode ('max' | 'real' | 'hw')",
     .flags = 0,
     .attr = {.type = V_STRING, .get = sched_attr_mode_get, .set = sched_attr_mode_set}},
    {.kind = M_ATTR,
     .name = "cpi",
     .doc = "Cycles per instruction for the current mode",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = sched_attr_cpi, .set = NULL}},
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
