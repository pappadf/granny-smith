// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scheduler_class.c
// Object-model glue for the scheduler: the `scheduler` node (run, stop, the
// pacing attributes, the event queue, the cycle and instruction counters).
// Kept out of scheduler.c so the event queue and sprint loop sit in a TU of
// their own.  The pacing attributes are mirrors of the host's setting, whose
// reads, validation and writes live in pacing.c with the root `pacing` node.

#include "scheduler.h"
#include "scheduler_internal.h"

#include "object.h"
#include "system.h"
#include "value.h"

#include <stdlib.h>
#include <time.h>

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

static scheduler_t *sched_self_from(struct object *self) {
    return (scheduler_t *)object_data(self);
}

// `scheduler.events` — the pending queue as a list of maps.
//
// Replaces cmd_events(int argc, char *argv[]), which had ZERO callers: the
// exact argc/argv shape docs/internals/core/object/object-model.md says was retired, so
// the event queue was the one piece of scheduler state nothing could inspect.
// That mattered more than it sounds -- the teardown warning in
// machine_teardown.c reports a COUNT of leaked events and nothing could then
// say which.
static DEF_GETTER(sched_attr_events) {
    struct scheduler *s = sched_self_from(self);
    if (!s)
        return val_err("scheduler.events: no scheduler");

    value_t *items = NULL;
    size_t len = 0, cap = 0;
    for (event_t *e = s->cpu_events; e; e = e->next) {
        const event_type_t *t = scheduler_find_event_type(s, e->source, e->callback);
        int64_t delta = (int64_t)e->timestamp - (int64_t)s->saved.cpu_cycles;

        value_map_builder_t *b = val_map_new();
        val_map_put(b, "source", val_str(t ? t->source_name : "unknown"));
        val_map_put(b, "event", val_str(t ? t->event_name : "unknown"));
        val_map_put(b, "when", val_uint(8, e->timestamp));
        val_map_put(b, "delta", val_int(delta));
        val_map_put(b, "data", val_uint(8, e->data));
        value_t entry = val_map_finish(b);
        if (!val_list_push(&items, &len, &cap, entry)) {
            value_free(&entry);
            for (size_t i = 0; i < len; i++)
                value_free(&items[i]);
            free(items);
            return val_err("out of memory");
        }
    }
    return val_list(items, len);
}

static DEF_GETTER(sched_attr_running) {
    return val_bool(scheduler_is_running(sched_self_from(self)));
}

static DEF_GETTER(sched_attr_mode_get) {
    return pacing_read_mode("scheduler.mode");
}

static DEF_SETTER(sched_attr_mode_set) {
    return pacing_write_mode(sched_self_from(self), "scheduler.mode", in);
}

static DEF_GETTER(sched_attr_cpi) {
    return val_uint(4, sched_self_from(self)->saved.cpi);
}

// Debug override for the per-machine CPI constant. Mode-independent; changing
// it mid-run alters the guest timeline from that point on, so it is a tuning
// and experimentation tool, not something the UI exposes.
static DEF_SETTER(sched_attr_cpi_set) {
    if (in.u < 1 || in.u > 255)
        return val_err("scheduler.cpi: value %llu out of range (1..255)", (unsigned long long)in.u);
    scheduler_set_cpi(sched_self_from(self), (uint32_t)in.u);
    return val_none();
}

// Speed multiplier. Reads back the multiplier currently in force under paced
// or accelerated pacing — the pinned value, or the adaptive governor's live
// speed when the setting is auto (so it moves on its own; below 1 when the
// host cannot keep up). Writing 0 selects auto; 1.0..8.0 pins the
// accelerated multiplier. The setting is the host's: it outlives machines
// and is never checkpointed.
static DEF_GETTER(sched_attr_speed) {
    return val_float((double)scheduler_current_speed_x256(sched_self_from(self)) / 256.0);
}
static DEF_SETTER(sched_attr_speed_set) {
    return pacing_write_speed(sched_self_from(self), "scheduler.speed", in.f);
}

// Whether the adaptive governor is choosing the speed (scheduler.speed = 0)
static DEF_GETTER(sched_attr_speed_auto) {
    (void)self;
    return val_bool(platform_pacing()->speed_x256 == SPEED_X256_AUTO);
}

// User floor on the governor: how far it may slow the guest when the host
// cannot emulate a frame-unit per VBL period (1.0 = never). The host's
// setting, like the speed.
static DEF_GETTER(sched_attr_min_speed) {
    return pacing_read_min_speed();
}
static DEF_SETTER(sched_attr_min_speed_set) {
    return pacing_write_min_speed(sched_self_from(self), "scheduler.min_speed", in.f);
}

// User cap on the accelerated-mode multiplier (governor ceiling; also clamps
// a pinned speed). The host's setting, like the speed.
static DEF_GETTER(sched_attr_max_speed) {
    return pacing_read_max_speed();
}
static DEF_SETTER(sched_attr_max_speed_set) {
    return pacing_write_max_speed(sched_self_from(self), "scheduler.max_speed", in.f);
}

static DEF_GETTER(sched_attr_cycles) {
    return val_uint(8, scheduler_cpu_cycles(sched_self_from(self)));
}

static DEF_GETTER(sched_attr_events_fired) {
    return val_uint(8, g_sched_events_fired);
}

static DEF_GETTER(sched_attr_instr_count) {
    return val_uint(8, cpu_instr_count());
}

static DEF_GETTER(sched_attr_frequency) {
    return val_uint(4, sched_self_from(self)->frequency);
}

// Cumulative host user-mode CPU time consumed by this process across
// all threads, in nanoseconds, since process start.  Read from POSIX
// CLOCK_PROCESS_CPUTIME_ID on every query.  Sample the delta around a
// scheduler.run call and divide instr_count delta by it for emulator
// IPS that excludes OS scheduling jitter and I/O wait.
static DEF_GETTER(sched_attr_host_user_ns) {
    return val_uint(8, host_clock_ns(CLOCK_PROCESS_CPUTIME_ID));
}

// Host wall-clock time, in nanoseconds, since an unspecified monotonic
// epoch (typically boot).  Read from POSIX CLOCK_MONOTONIC on every
// query.  Sample the delta around a scheduler.run call and divide
// instr_count delta by it for perceived emulator IPS — what the user
// actually waits for.  Always >= host_user_ns delta by definition.
static DEF_GETTER(sched_attr_host_wall_ns) {
    return val_uint(8, host_clock_ns(CLOCK_MONOTONIC));
}

static DEF_METHOD(sched_method_run) {
    scheduler_t *s = sched_self_from(self);
    if (!s)
        return val_err("scheduler.run: scheduler not initialised");
    uint64_t instructions = (argc >= 1) ? argv[0].u : 0; // 0 = run-until-stopped
    if (!scheduler_run_with_budget(s, instructions))
        return val_err("scheduler.run: instruction count too large");
    return val_none();
}

static DEF_METHOD(sched_method_stop) {
    scheduler_t *s = sched_self_from(self);
    if (!s)
        return val_err("scheduler.stop: scheduler not initialised");
    scheduler_stop(s);
    return val_none();
}

static const arg_decl_t sched_run_args[] = {
    {.name = "instructions",
     .kind = VK_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Optional instruction budget; 0 / omitted = run until stopped"},
};

static const member_t scheduler_members[] = {
    {.kind = MK_ATTR,
     .name = "running",
     .doc = "True while the scheduler is executing instructions",
     .attr = {.type = VK_BOOL, .get = sched_attr_running, .set = NULL}},
    {.kind = MK_ATTR,
     .name = "mode",
     .doc = "Pacing mode: paced (real-time), accelerated (faster, adaptive) or turbo (flat out)",
     .flags = 0,
     .attr =
         {.type = VK_ENUM, .enum_values = pacing_mode_names, .get = sched_attr_mode_get, .set = sched_attr_mode_set}},
    {.kind = MK_ATTR,
     .name = "cpi",
     .doc = "Per-machine cycles per instruction (mode-independent; writable as a debug override, 1..255)",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = VK_UINT, .get = sched_attr_cpi, .set = sched_attr_cpi_set}},
    {.kind = MK_ATTR,
     .name = "speed",
     .doc = "CPU speed multiplier in force (live): below 1 when the host cannot keep up in paced or "
            "accelerated, above 1 in accelerated. Write 0 for auto (adaptive governor, between min_speed "
            "and max_speed) or 1.0..8.0 to pin a fixed accelerated multiplier. Timebase (VBL/VIA/sound) "
            "stays real-time regardless", .flags = 0,
     .attr = {.type = VK_FLOAT, .get = sched_attr_speed, .set = sched_attr_speed_set}},
    {.kind = MK_ATTR,
     .name = "speed_auto",
     .doc = "True while the adaptive governor is choosing the speed (scheduler.speed = 0)",
     .attr = {.type = VK_BOOL, .get = sched_attr_speed_auto, .set = NULL}},
    {.kind = MK_ATTR,
     .name = "min_speed",
     .doc = "Floor on the governor's multiplier (0.25..1.0): how far the CPU may slow when the host cannot "
            "emulate a frame per VBL period; 1.0 never slows it. The host's setting (pacing.min_speed)", .flags = 0,
     .attr = {.type = VK_FLOAT, .get = sched_attr_min_speed, .set = sched_attr_min_speed_set}},
    {.kind = MK_ATTR,
     .name = "max_speed",
     .doc = "Cap on the accelerated-mode multiplier (1.0..8.0): the adaptive governor's ceiling, and pinned "
            "speeds are clamped to it. The host's setting (pacing.max_speed)", .flags = 0,
     .attr = {.type = VK_FLOAT, .get = sched_attr_max_speed, .set = sched_attr_max_speed_set}},
    {.kind = MK_ATTR,
     .name = "cycles",
     .doc = "Total CPU cycles executed so far",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = VK_UINT, .get = sched_attr_cycles, .set = NULL}},
    {.kind = MK_ATTR,
     .name = "events_fired",
     .doc = "Total scheduler events dispatched since process start (diagnostic)",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = VK_UINT, .get = sched_attr_events_fired, .set = NULL}},
    {.kind = MK_ATTR,
     .name = "instr_count",
     .doc = "Total CPU instructions executed so far",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = VK_UINT, .get = sched_attr_instr_count, .set = NULL}},
    {.kind = MK_ATTR,
     .name = "frequency",
     .doc = "CPU clock frequency in Hz",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = VK_UINT, .get = sched_attr_frequency, .set = NULL}},
    {.kind = MK_ATTR,
     .name = "host_user_ns",
     .doc = "Process user-CPU time since daemon start, ns (POSIX CLOCK_PROCESS_CPUTIME_ID). "
            "Sample before+after scheduler.run; divide instr_count delta by the time delta "
            "and multiply by 1e9 for emulator throughput in instructions per CPU-second.", .flags = M_CAT_ADVANCED,
     .attr = {.type = VK_UINT, .get = sched_attr_host_user_ns, .set = NULL}},
    {.kind = MK_ATTR,
     .name = "host_wall_ns",
     .doc = "Host monotonic wall-clock time, ns (POSIX CLOCK_MONOTONIC). "
            "Sample before+after scheduler.run; divide instr_count delta by the time delta "
            "and multiply by 1e9 for perceived emulator throughput in instructions per real second.", .flags = M_CAT_ADVANCED,
     .attr = {.type = VK_UINT, .get = sched_attr_host_wall_ns, .set = NULL}},
    {.kind = MK_ATTR,
     .name = "events",
     .doc = "Pending event queue: {source, event, when, delta, data} per entry",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = VK_LIST, .presentation_flags = VFLAG_VOLATILE, .get = sched_attr_events}},
    {.kind = MK_METHOD,
     .name = "run",
     .examples = EXAMPLES("scheduler.run", "scheduler.run 20000000"),
     .doc = "Start execution; with an instruction budget, stop after that many",
     .method = {.args = sched_run_args, .nargs = 1, .result = VK_NONE, .fn = sched_method_run}},
    {.kind = MK_METHOD,
     .name = "stop",
     .examples = EXAMPLES("scheduler.stop"),
     .doc = "Interrupt execution",
     .method = {.args = NULL, .nargs = 0, .result = VK_NONE, .fn = sched_method_stop}},
};

static const class_desc_t scheduler_class = {
    .name = "scheduler",
    .members = scheduler_members,
    .n_members = sizeof(scheduler_members) / sizeof(scheduler_members[0]),
    .doc = "Runs the machine: start, stop, pacing mode and speed",
};

struct object *scheduler_object_new(struct scheduler *s) {
    struct object *obj = object_new(&scheduler_class, s, "scheduler");
    if (obj) {
        object_set_order(obj, 10); // root order: machine 0, scheduler 10, checkpoint 20, files 30, debug 40, log
                                   // 50, shell 60, catalog 70, appletalk 100
        object_attach(object_root(), obj);
    }
    return obj;
}

void scheduler_object_delete(struct object *obj) {
    if (!obj)
        return;
    object_detach(obj);
    object_delete(obj);
}
