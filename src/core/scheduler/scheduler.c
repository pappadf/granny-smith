// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scheduler.c
// Event scheduler and timing control for Granny Smith.

// ============================================================================
// Includes
// ============================================================================

#include "scheduler.h"

#include "cpu.h"
#include "shell.h"
#include "system.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

#define MAC_VBL_FREQUENCY     60.15 // 60 vertical blanking interrupts per second
#define MAC_VBL_PERIOD        (1.0 / MAC_VBL_FREQUENCY) // period of one VBL in seconds
#define CYCLES_PER_INSTR_HW   12 // cycles per instruction for hardware accuracy mode
#define CYCLES_PER_INSTR_FAST 4 // cycles per instruction for realtime and max speed modes
#define MAC_CPU_FREQUENCY     7833600.0
#define MAX_EVENT_TYPES       32
#define MAX_SANE_EVENTS       10000 // upper bound for event queue length sanity checks

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
    int tmp_num_events;
    event_as_checkpoint_t *tmp_events;

    uint32_t frequency;
};

// ============================================================================
// Forward Declarations
// ============================================================================

static uint64_t current_cpu_cycles(struct scheduler *s);
static int num_events_in_queue(struct scheduler *restrict s);

// ============================================================================
// Static Helpers
// ============================================================================

// Returns cycles per instruction based on current scheduler mode
static inline uint32_t avg_cycles_per_instr(struct scheduler *s) {
    GS_ASSERT(s != NULL);
    switch (s->mode) {
    case schedule_hw_accuracy:
        return CYCLES_PER_INSTR_HW;
    case schedule_real_time:
    case schedule_max_speed:
    default:
        return CYCLES_PER_INSTR_FAST;
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
            printf("WARNING [%s]: vbl_acc_error out of bounds (%f)\n", context, s->vbl_acc_error);
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

    event_t *event = (event_t *)malloc(sizeof(event_t));
    if (event == NULL)
        return NULL;

    memset(event, 0, sizeof(event_t));
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

// Shell command to start execution, optionally limiting the number of instructions to run
static uint64_t cmd_run(int argc, char *argv[]) {
    if (argc > 2) {
        printf("Usage: run [instructions]\n");
        return 0;
    }

    scheduler_t *s = system_scheduler();
    GS_ASSERT(s != NULL);

    int cpi = avg_cycles_per_instr(s);

    // Cancel any pending stop events from previous limited runs
    remove_event(s, run_stop_event, NULL);

    if (argc == 2) {
        char *endptr = NULL;
        unsigned long long instructions = strtoull(argv[1], &endptr, 0);
        if (endptr == argv[1] || *endptr != '\0') {
            printf("Invalid instruction count: %s\n", argv[1]);
            return 0;
        }
        if (instructions == 0) {
            printf("Instruction count must be greater than zero\n");
            return 0;
        }
        if (instructions > UINT64_MAX / cpi) {
            printf("Instruction count too large\n");
            return 0;
        }

        uint64_t cycles = instructions * cpi;
        scheduler_new_cpu_event(s, run_stop_event, s, 0, cycles, 0);
    }

    // Enter running state
    s->running = true;
    return 0;
}

// Shell command to stop execution
static uint64_t cmd_stop(int argc, char *argv[]) {
    if (argc != 1) {
        printf("Usage: stop\n");
        return 0;
    }

    scheduler_t *s = system_scheduler();
    GS_ASSERT(s != NULL);

    scheduler_stop(s);
    return 0;
}

// Shell command to get/set scheduler mode
static uint64_t cmd_schedule(int argc, char *argv[]) {
    scheduler_t *s = system_scheduler();
    GS_ASSERT(s != NULL);

    // Map mode enum to display string
    const char *mode_str = "?";
    switch (s->mode) {
    case schedule_max_speed:
        mode_str = "max";
        break;
    case schedule_real_time:
        mode_str = "real";
        break;
    case schedule_hw_accuracy:
        mode_str = "hw";
        break;
    default:
        break;
    }

    if (argc == 1) {
        printf("current scheduler mode: %s (cycles/instr: %u) (options: max, real, hw)\n", mode_str,
               avg_cycles_per_instr(s));
        return 0;
    }
    if (argc != 2) {
        printf("usage: schedule [max|real|hw]\n");
        return 0;
    }

    if (strcmp(argv[1], "max") == 0) {
        s->mode = schedule_max_speed;
    } else if (strcmp(argv[1], "real") == 0) {
        s->mode = schedule_real_time;
        s->vbl_acc_error = 0; // reset accumulated error when switching back
    } else if (strcmp(argv[1], "hw") == 0) {
        s->mode = schedule_hw_accuracy;
    } else {
        printf("unknown mode '%s' (valid: max, real, hw)\n", argv[1]);
        return 0;
    }

    printf("scheduler mode set to: %s (cycles/instr: %u)\n", argv[1], avg_cycles_per_instr(s));
    return 0;
}

// Shell command to check if scheduler is running
static uint64_t cmd_status(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    scheduler_t *s = system_scheduler();
    if (!s)
        return 0;
    return scheduler_is_running(s) ? 1 : 0;
}

// Shell command to print a readable view of the event queue
static uint64_t cmd_events(int argc, char *argv[]) {
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
    s->frequency = 7833600;
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
        uint32_t restored_cpi = (s->mode == schedule_hw_accuracy) ? CYCLES_PER_INSTR_HW : CYCLES_PER_INSTR_FAST;
        s->total_instructions = s->cpu_cycles / restored_cpi;
        s->sprint_total = 0;
        s->sprint_burndown = 0;

        GS_ASSERT(s->mode >= schedule_max_speed && s->mode <= schedule_hw_accuracy);
        GS_ASSERT(s->cpu_cycles < (1ULL << 60));

        // Save event data for deferred restoration (names must be resolved after device registration)
        system_read_checkpoint_data(checkpoint, &s->tmp_num_events, sizeof(s->tmp_num_events));
        s->tmp_events = (event_as_checkpoint_t *)malloc(s->tmp_num_events * sizeof(event_as_checkpoint_t));
        system_read_checkpoint_data(checkpoint, s->tmp_events, s->tmp_num_events * sizeof(event_as_checkpoint_t));
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

    // Register shell commands
    register_cmd("run", "Scheduler", "run [instructions] - start execution, optionally stop after N instructions",
                 cmd_run);
    register_cmd("stop", "Scheduler", "stop – stop execution", cmd_stop);
    register_cmd("schedule", "Scheduler", "schedule [max|real|hw] – set or show scheduler mode", cmd_schedule);
    register_cmd("status", "Scheduler", "status – return 1 if running, 0 if idle", cmd_status);
    register_cmd("events", "Scheduler", "events – show pending CPU event queue", cmd_events);

    scheduler_new_event_type(s, "Scheduler", s, "run_stop", run_stop_event);

    return s;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free all resources associated with a scheduler instance
void scheduler_delete(struct scheduler *scheduler) {
    if (!scheduler)
        return;

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
    event_as_checkpoint_t *events_to_save = (event_as_checkpoint_t *)calloc(num_events, sizeof(event_as_checkpoint_t));

    event_t *e = scheduler->cpu_events;
    for (unsigned int i = 0; i < num_events; i++) {
        GS_ASSERT(e != NULL);
        events_to_save[i].timestamp = e->timestamp;
        events_to_save[i].data = e->data;

        // Look up names by callback (source pointers are runtime-specific)
        bool found = false;
        for (int j = 0; j < scheduler->num_event_types; j++) {
            if (scheduler->event_types[j].callback == e->callback) {
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

    system_write_checkpoint_data(checkpoint, events_to_save, num_events * sizeof(event_as_checkpoint_t));
    free(events_to_save);
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
    for (int i = 0; i < s->tmp_num_events; i++) {
        event_as_checkpoint_t *saved = &s->tmp_events[i];

        // Find matching registered event type by name
        int found = -1;
        for (int j = 0; j < s->num_event_types; j++) {
            if (strncmp(s->event_types[j].source_name, saved->source_name, sizeof(s->event_types[j].source_name)) ==
                    0 &&
                strncmp(s->event_types[j].event_name, saved->event_name, sizeof(s->event_types[j].event_name)) == 0) {
                found = j;
                break;
            }
        }

        GS_ASSERTF(found >= 0, "cannot restore event '%s.%s' — type not registered", saved->source_name,
                   saved->event_name);
        GS_ASSERTF(saved->timestamp > s->cpu_cycles, "restored event timestamp (%llu) <= cpu_cycles (%llu) for '%s.%s'",
                   (unsigned long long)saved->timestamp, (unsigned long long)s->cpu_cycles, saved->source_name,
                   saved->event_name);

        // Recreate and insert event
        event_t *e = (event_t *)malloc(sizeof(event_t));
        GS_ASSERT(e != NULL);
        memset(e, 0, sizeof(event_t));

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

    // Skip if already registered (one type per callback function)
    for (int i = 0; i < scheduler->num_event_types; i++) {
        if (scheduler->event_types[i].callback == callback)
            return;
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
    return (double)cycles * (1e9 / MAC_CPU_FREQUENCY);
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

        // Execute sprint
        s->sprint_total = instr_to_exec;
        s->sprint_burndown = instr_to_exec;
        cpu_run_sprint(cpu, &s->sprint_burndown);

        // Account for executed instructions and cycles
        uint32_t executed_instr = s->sprint_total;
        s->sprint_total = 0;
        uint32_t executed_cycles = executed_instr * avg_cycles_per_instr(s);

        // Overshoot check: allow up to one instruction of overshoot
        if (s->cpu_events != NULL) {
            GS_ASSERT(executed_cycles <= cycles_to_execute + avg_cycles_per_instr(s));
        }

        s->total_instructions += executed_instr;
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

// Main loop iteration for real-time emulation with VBL-based timing
void scheduler_main_loop(struct config *restrict config, double now_msecs) {
    GS_ASSERT(config != NULL);
    GS_ASSERT(config->scheduler != NULL);

    struct scheduler *s = config->scheduler;

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
        // Fall through to hw_accuracy model when host loop deviates too much

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

    // Execute VBLs
    int executed_vbls = 0;
    for (int i = 0; i < vbls_to_execute; i++) {
        trigger_vbl(config);
        scheduler_run(s, MAC_VBL_PERIOD);
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
