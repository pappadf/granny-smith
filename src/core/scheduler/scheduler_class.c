// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scheduler_class.c
// Object-model glue for the scheduler: the `scheduler` node (run, stop, the
// pacing attributes, the event queue, the cycle and instruction counters)
// and the root `pacing` node.  Kept out of scheduler.c so the event queue and
// sprint loop sit in a TU of their own.

#include "scheduler.h"
#include "scheduler_internal.h"

#include "object.h"
#include "system.h"
#include "value.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
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

// scheduler.mode values, in enum-index order.  The legacy aliases (real,
// realtime, hw, hardware → paced; accel → accelerated; max → turbo) are gone
// everywhere: the attribute, --speed= and ?speed= take these three only.
static const char *const sched_mode_names[] = {"paced", "accelerated", "turbo", NULL};
static const enum schedule_mode sched_mode_values[] = {schedule_paced, schedule_accelerated, schedule_unthrottled};

static DEF_GETTER(sched_attr_mode_get) {
    (void)self;
    enum schedule_mode mode = platform_pacing()->mode;
    for (size_t i = 0; i < 3; i++)
        if (sched_mode_values[i] == mode)
            return val_enum((int)i, sched_mode_names, 3);
    return val_err("scheduler.mode: unknown internal mode %d", (int)mode);
}

bool scheduler_mode_from_string(const char *name, enum schedule_mode *out) {
    for (size_t i = 0; name && i < 3; i++) {
        if (strcmp(name, sched_mode_names[i]) == 0) {
            *out = sched_mode_values[i];
            return true;
        }
    }
    return false;
}

static DEF_SETTER(sched_attr_mode_set) {
    // node_set has already coerced a name to V_ENUM against the table.
    if (in.kind != V_ENUM || in.enm.idx < 0 || in.enm.idx >= 3) {
        value_free(&in);
        return val_err("scheduler.mode: expected paced, accelerated or turbo");
    }
    host_pacing_t *pacing = platform_pacing();
    pacing->mode = sched_mode_values[in.enm.idx];
    scheduler_apply_pacing(sched_self_from(self), pacing); // shows at once, not at the next frame
    value_free(&in);
    return val_none();
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

// Accelerated-mode speed multiplier. Reads back the multiplier currently in
// force for accelerated mode — the pinned value, or the adaptive governor's
// live speed when the setting is auto (so it moves on its own). Writing 0
// selects auto; 1.0..8.0 pins. The setting is the host's: it outlives
// machines, is never checkpointed, and only applies in 'accelerated'.
static DEF_GETTER(sched_attr_speed) {
    return val_float((double)scheduler_current_speed_x256(sched_self_from(self)) / 256.0);
}
static DEF_SETTER(sched_attr_speed_set) {
    if (isnan(in.f) || (in.f != 0.0 && (in.f < 1.0 || in.f > 8.0)))
        return val_err("scheduler.speed: %g out of range (0 = auto, or 1.0 .. 8.0)", in.f);
    host_pacing_set_speed(platform_pacing(), in.f);
    scheduler_apply_pacing(sched_self_from(self), platform_pacing());
    return val_none();
}

// Whether the adaptive governor is choosing the speed (scheduler.speed = 0)
static DEF_GETTER(sched_attr_speed_auto) {
    (void)self;
    return val_bool(platform_pacing()->speed_x256 == SPEED_X256_AUTO);
}

// User cap on the accelerated-mode multiplier (governor ceiling; also clamps
// a pinned speed). The host's setting, like the speed.
static DEF_GETTER(sched_attr_max_speed) {
    (void)self;
    return val_float((double)platform_pacing()->max_speed_x256 / 256.0);
}
static DEF_SETTER(sched_attr_max_speed_set) {
    if (isnan(in.f) || in.f < 1.0 || in.f > 8.0)
        return val_err("scheduler.max_speed: %g out of range (1.0 .. 8.0)", in.f);
    host_pacing_set_max_speed(platform_pacing(), in.f);
    scheduler_apply_pacing(sched_self_from(self), platform_pacing());
    return val_none();
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
    return val_bool(true);
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
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Optional instruction budget; 0 / omitted = run until stopped"},
};

static const member_t scheduler_members[] = {
    {.kind = M_ATTR,
     .name = "running",
     .doc = "True while the scheduler is executing instructions",
     .attr = {.type = V_BOOL, .get = sched_attr_running, .set = NULL}},
    {.kind = M_ATTR,
     .name = "mode",
     .doc = "Pacing mode: paced (real-time), accelerated (faster, adaptive) or turbo (flat out)",
     .flags = 0,
     .attr = {.type = V_ENUM, .enum_values = sched_mode_names, .get = sched_attr_mode_get, .set = sched_attr_mode_set}},
    {.kind = M_ATTR,
     .name = "cpi",
     .doc = "Per-machine cycles per instruction (mode-independent; writable as a debug override, 1..255)",
     .flags = M_CAT_ADVANCED,
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
     .attr = {.type = V_BOOL, .get = sched_attr_speed_auto, .set = NULL}},
    {.kind = M_ATTR,
     .name = "max_speed",
     .doc = "Cap on the accelerated-mode multiplier (1.0..8.0): the adaptive governor's ceiling, and pinned "
            "speeds are clamped to it. The host's setting (pacing.max_speed)", .flags = 0,
     .attr = {.type = V_FLOAT, .get = sched_attr_max_speed, .set = sched_attr_max_speed_set}},
    {.kind = M_ATTR,
     .name = "cycles",
     .doc = "Total CPU cycles executed so far",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = V_UINT, .get = sched_attr_cycles, .set = NULL}},
    {.kind = M_ATTR,
     .name = "events_fired",
     .doc = "Total scheduler events dispatched since process start (diagnostic)",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = V_UINT, .get = sched_attr_events_fired, .set = NULL}},
    {.kind = M_ATTR,
     .name = "instr_count",
     .doc = "Total CPU instructions executed so far",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = V_UINT, .get = sched_attr_instr_count, .set = NULL}},
    {.kind = M_ATTR,
     .name = "frequency",
     .doc = "CPU clock frequency in Hz",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = V_UINT, .get = sched_attr_frequency, .set = NULL}},
    {.kind = M_ATTR,
     .name = "host_user_ns",
     .doc = "Process user-CPU time since daemon start, ns (POSIX CLOCK_PROCESS_CPUTIME_ID). "
            "Sample before+after scheduler.run; divide instr_count delta by the time delta "
            "and multiply by 1e9 for emulator throughput in instructions per CPU-second.", .flags = M_CAT_ADVANCED,
     .attr = {.type = V_UINT, .get = sched_attr_host_user_ns, .set = NULL}},
    {.kind = M_ATTR,
     .name = "host_wall_ns",
     .doc = "Host monotonic wall-clock time, ns (POSIX CLOCK_MONOTONIC). "
            "Sample before+after scheduler.run; divide instr_count delta by the time delta "
            "and multiply by 1e9 for perceived emulator throughput in instructions per real second.", .flags = M_CAT_ADVANCED,
     .attr = {.type = V_UINT, .get = sched_attr_host_wall_ns, .set = NULL}},
    {.kind = M_ATTR,
     .name = "events",
     .doc = "Pending event queue: {source, event, when, delta, data} per entry",
     .flags = M_CAT_ADVANCED,
     .attr = {.type = V_LIST, .presentation_flags = VAL_VOLATILE, .get = sched_attr_events}},
    {.kind = M_METHOD,
     .name = "run",
     .examples = EXAMPLES("scheduler.run", "scheduler.run 20000000"),
     .doc = "Start execution; with an instruction budget, stop after that many",
     .method = {.args = sched_run_args, .nargs = 1, .result = V_BOOL, .fn = sched_method_run}},
    {.kind = M_METHOD,
     .name = "stop",
     .examples = EXAMPLES("scheduler.stop"),
     .doc = "Interrupt execution",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = sched_method_stop}},
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

// === pacing =================================================================
//
// The host's pacing setting -- what the toolbar, ?speed= and --speed= choose
// -- as an object of its own, there with or without a machine: a page sets
// it before it boots anything, and every machine it builds or restores runs
// under it (system_swap_in).  scheduler.mode / speed / max_speed are the same
// setting, reached through the running machine.

// Apply the setting to the running machine, if there is one.
static void pacing_reaches_machine(void) {
    scheduler_t *running = system_running_scheduler();
    if (running)
        scheduler_apply_pacing(running, platform_pacing());
}

static DEF_GETTER(pacing_attr_mode_get) {
    (void)self;
    enum schedule_mode mode = platform_pacing()->mode;
    for (size_t i = 0; i < 3; i++)
        if (sched_mode_values[i] == mode)
            return val_enum((int)i, sched_mode_names, 3);
    return val_err("pacing.mode: unknown internal mode %d", (int)mode);
}

static DEF_SETTER(pacing_attr_mode_set) {
    (void)self;
    if (in.kind != V_ENUM || in.enm.idx < 0 || in.enm.idx >= 3) {
        value_free(&in);
        return val_err("pacing.mode: expected paced, accelerated or turbo");
    }
    platform_pacing()->mode = sched_mode_values[in.enm.idx];
    value_free(&in);
    pacing_reaches_machine();
    return val_none();
}

// The setting, not the governor's live pick: 0 is auto.
static DEF_GETTER(pacing_attr_speed_get) {
    (void)self;
    uint32_t x = platform_pacing()->speed_x256;
    return val_float(x == SPEED_X256_AUTO ? 0.0 : (double)x / 256.0);
}

static DEF_SETTER(pacing_attr_speed_set) {
    (void)self;
    if (isnan(in.f) || (in.f != 0.0 && (in.f < 1.0 || in.f > 8.0)))
        return val_err("pacing.speed: %g out of range (0 = auto, or 1.0 .. 8.0)", in.f);
    host_pacing_set_speed(platform_pacing(), in.f);
    pacing_reaches_machine();
    return val_none();
}

static DEF_GETTER(pacing_attr_max_speed_get) {
    (void)self;
    return val_float((double)platform_pacing()->max_speed_x256 / 256.0);
}

static DEF_SETTER(pacing_attr_max_speed_set) {
    (void)self;
    if (isnan(in.f) || in.f < 1.0 || in.f > 8.0)
        return val_err("pacing.max_speed: %g out of range (1.0 .. 8.0)", in.f);
    host_pacing_set_max_speed(platform_pacing(), in.f);
    pacing_reaches_machine();
    return val_none();
}

static const member_t pacing_members[] = {
    {.kind = M_ATTR,
     .name = "mode",
     .doc = "Pacing mode: paced (real-time), accelerated (faster, adaptive) or turbo (flat out)",
     .attr =
         {.type = V_ENUM, .enum_values = sched_mode_names, .get = pacing_attr_mode_get, .set = pacing_attr_mode_set}},
    {.kind = M_ATTR,
     .name = "speed",
     .doc = "Accelerated-mode speed: 0 for auto (the adaptive governor, capped by max_speed) or 1.0..8.0 pinned",
     .attr = {.type = V_FLOAT, .get = pacing_attr_speed_get, .set = pacing_attr_speed_set}                          },
    {.kind = M_ATTR,
     .name = "max_speed",
     .doc = "Cap on the accelerated-mode multiplier (1.0..8.0)",
     .attr = {.type = V_FLOAT, .get = pacing_attr_max_speed_get, .set = pacing_attr_max_speed_set}                  },
};

static const class_desc_t pacing_class = {
    .name = "pacing",
    .members = pacing_members,
    .n_members = sizeof(pacing_members) / sizeof(pacing_members[0]),
    .doc = "The host's pacing: how fast every machine runs, set with or without one",
};

static struct object *s_pacing_object = NULL;

void pacing_init(void) {
    if (s_pacing_object)
        return;
    s_pacing_object = object_new(&pacing_class, NULL, "pacing");
    if (s_pacing_object) {
        object_set_order(s_pacing_object, 21);
        // The toolbar's mode buttons are its everyday interface; in a tree it
        // is an advanced node, like the vrom and prom registries.
        object_set_category(s_pacing_object, M_CAT_ADVANCED);
        object_attach(object_root(), s_pacing_object);
    }
}
