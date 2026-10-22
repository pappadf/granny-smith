// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pacing.c
// The host's pacing setting -- the mode, the pinned speed, the governor's
// floor and cap -- in one place: the clamps every write goes through, the
// validated writes the `pacing` node and the scheduler.* mirrors share, and
// the root `pacing` object itself.  The setting is what the toolbar, ?speed=
// and --speed= choose; it is there with or without a machine (a page sets it
// before it boots anything, and every machine it builds or restores runs
// under it: system_swap_in), outlives every machine and is never in a
// checkpoint.  The platform's run loop owns the one instance
// (platform_pacing); a weak default serves a build with no run loop.

#include "scheduler.h"
#include "scheduler_internal.h"

#include "object.h"
#include "system.h"
#include "value.h"

#include <math.h>
#include <string.h>

// ============================================================================
// The setting
// ============================================================================

// The pacing a build with no run loop of its own runs under (the unit
// suites); every platform defines platform_pacing over its run loop's setting.
__attribute__((weak)) host_pacing_t *platform_pacing(void) {
    static host_pacing_t pacing = HOST_PACING_DEFAULT;
    return &pacing;
}

// A speed multiplier clamped to [lo, hi] (x256), as x256 fixed point.
static uint32_t speed_x256_clamped(double multiplier, uint32_t lo, uint32_t hi) {
    double clamped = multiplier;
    if (clamped < (double)lo / 256.0)
        clamped = (double)lo / 256.0;
    if (clamped > (double)hi / 256.0)
        clamped = (double)hi / 256.0;
    return (uint32_t)(clamped * 256.0 + 0.5);
}

// Pin the accelerated speed (0 = auto, the governor picks), 1x..8x.
void host_pacing_set_speed(host_pacing_t *p, double multiplier) {
    if (!p || isnan(multiplier))
        return;
    p->speed_x256 =
        (multiplier == 0.0) ? SPEED_X256_AUTO : speed_x256_clamped(multiplier, SPEED_X256_ONE, SPEED_X256_MAX);
}

// Floor the governor (how far it may slow an overloaded host's guest),
// 0.25x..1x; 1x keeps the guest at the authentic speed whatever the host does.
void host_pacing_set_min_speed(host_pacing_t *p, double multiplier) {
    if (!p || isnan(multiplier))
        return;
    p->min_speed_x256 = speed_x256_clamped(multiplier, SPEED_X256_MIN, SPEED_X256_ONE);
}

// Cap the accelerated speed (the governor's ceiling; a pin is clamped to it),
// 1x..8x.
void host_pacing_set_max_speed(host_pacing_t *p, double multiplier) {
    if (!p || isnan(multiplier))
        return;
    p->max_speed_x256 = speed_x256_clamped(multiplier, SPEED_X256_ONE, SPEED_X256_MAX);
}

// ============================================================================
// Modes by name
// ============================================================================

// Mode names, in enum-index order of pacing_mode_values.  The legacy aliases
// (real, realtime, hw, hardware → paced; accel → accelerated; max → turbo)
// are gone everywhere: the attributes, --speed= and ?speed= take these three
// only.
const char *const pacing_mode_names[] = {"paced", "accelerated", "turbo", NULL};
static const enum schedule_mode pacing_mode_values[] = {schedule_paced, schedule_accelerated, schedule_unthrottled};

bool scheduler_mode_from_string(const char *name, enum schedule_mode *out) {
    for (size_t i = 0; name && i < PACING_NUM_MODES; i++) {
        if (strcmp(name, pacing_mode_names[i]) == 0) {
            *out = pacing_mode_values[i];
            return true;
        }
    }
    return false;
}

// ============================================================================
// Reads and validated writes, shared by `pacing` and scheduler.*
// ============================================================================

// Apply the setting to a machine, if there is one to apply it to.
static void pacing_reaches(scheduler_t *s) {
    if (s)
        scheduler_apply_pacing(s, platform_pacing());
}

value_t pacing_read_mode(const char *attr) {
    enum schedule_mode mode = platform_pacing()->mode;
    for (size_t i = 0; i < PACING_NUM_MODES; i++)
        if (pacing_mode_values[i] == mode)
            return val_enum((int)i, pacing_mode_names, PACING_NUM_MODES);
    return val_err("%s: unknown internal mode %d", attr, (int)mode);
}

value_t pacing_read_min_speed(void) {
    return val_float((double)platform_pacing()->min_speed_x256 / 256.0);
}

value_t pacing_read_max_speed(void) {
    return val_float((double)platform_pacing()->max_speed_x256 / 256.0);
}

value_t pacing_write_mode(scheduler_t *s, const char *attr, value_t in) {
    // node_set has already coerced a name to VK_ENUM against the table.
    if (in.kind != VK_ENUM || in.enm.idx < 0 || in.enm.idx >= PACING_NUM_MODES) {
        value_free(&in);
        return val_err("%s: expected paced, accelerated or turbo", attr);
    }
    platform_pacing()->mode = pacing_mode_values[in.enm.idx];
    value_free(&in);
    pacing_reaches(s); // shows at once, not at the next frame
    return val_none();
}

value_t pacing_write_speed(scheduler_t *s, const char *attr, double multiplier) {
    if (isnan(multiplier) || (multiplier != 0.0 && (multiplier < 1.0 || multiplier > 8.0)))
        return val_err("%s: %g out of range (0 = auto, or 1.0 .. 8.0)", attr, multiplier);
    host_pacing_set_speed(platform_pacing(), multiplier);
    pacing_reaches(s);
    return val_none();
}

value_t pacing_write_min_speed(scheduler_t *s, const char *attr, double multiplier) {
    if (isnan(multiplier) || multiplier < 0.25 || multiplier > 1.0)
        return val_err("%s: %g out of range (0.25 .. 1.0)", attr, multiplier);
    host_pacing_set_min_speed(platform_pacing(), multiplier);
    pacing_reaches(s);
    return val_none();
}

value_t pacing_write_max_speed(scheduler_t *s, const char *attr, double multiplier) {
    if (isnan(multiplier) || multiplier < 1.0 || multiplier > 8.0)
        return val_err("%s: %g out of range (1.0 .. 8.0)", attr, multiplier);
    host_pacing_set_max_speed(platform_pacing(), multiplier);
    pacing_reaches(s);
    return val_none();
}

// ============================================================================
// The root `pacing` object
// ============================================================================
//
// The setting as an object of its own, reachable without a machine.
// scheduler.mode / speed / min_speed / max_speed (scheduler_class.c) are the
// same setting, reached through the running machine.

static DEF_GETTER(pacing_attr_mode_get) {
    return pacing_read_mode("pacing.mode");
}

static DEF_SETTER(pacing_attr_mode_set) {
    return pacing_write_mode(system_running_scheduler(), "pacing.mode", in);
}

// The setting, not the governor's live pick: 0 is auto.
static DEF_GETTER(pacing_attr_speed_get) {
    uint32_t x = platform_pacing()->speed_x256;
    return val_float(x == SPEED_X256_AUTO ? 0.0 : (double)x / 256.0);
}

static DEF_SETTER(pacing_attr_speed_set) {
    return pacing_write_speed(system_running_scheduler(), "pacing.speed", in.f);
}

static DEF_GETTER(pacing_attr_min_speed_get) {
    return pacing_read_min_speed();
}

static DEF_SETTER(pacing_attr_min_speed_set) {
    return pacing_write_min_speed(system_running_scheduler(), "pacing.min_speed", in.f);
}

static DEF_GETTER(pacing_attr_max_speed_get) {
    return pacing_read_max_speed();
}

static DEF_SETTER(pacing_attr_max_speed_set) {
    return pacing_write_max_speed(system_running_scheduler(), "pacing.max_speed", in.f);
}

static const member_t pacing_members[] = {
    {.kind = MK_ATTR,
     .name = "mode",
     .doc = "Pacing mode: paced (real-time), accelerated (faster, adaptive) or turbo (flat out)",
     .attr =
         {.type = VK_ENUM, .enum_values = pacing_mode_names, .get = pacing_attr_mode_get, .set = pacing_attr_mode_set}},
    {.kind = MK_ATTR,
     .name = "speed",
     .doc = "Speed: 0 for auto (the adaptive governor, between min_speed and max_speed) or 1.0..8.0 pinned",
     .attr = {.type = VK_FLOAT, .get = pacing_attr_speed_get, .set = pacing_attr_speed_set}                           },
    {.kind = MK_ATTR,
     .name = "min_speed",
     .doc = "Floor on the governor's multiplier (0.25..1.0; 1.0 never slows the CPU on an overloaded host)",
     .attr = {.type = VK_FLOAT, .get = pacing_attr_min_speed_get, .set = pacing_attr_min_speed_set}                   },
    {.kind = MK_ATTR,
     .name = "max_speed",
     .doc = "Cap on the accelerated-mode multiplier (1.0..8.0)",
     .attr = {.type = VK_FLOAT, .get = pacing_attr_max_speed_get, .set = pacing_attr_max_speed_set}                   },
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
