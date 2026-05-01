// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_classes.c
// M2 stub classes. Each class wraps existing C state read-only and
// exposes the minimum attributes needed to exercise the resolver and
// the `eval` shell command. Real classes (with setters, methods,
// children) land in M3–M7.

#include "gs_classes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "machine.h"
#include "memory.h"
#include "object.h"
#include "scheduler.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

// instance_data on these stubs is config_t*. The lifetime is bounded
// by gs_classes_install / gs_classes_uninstall — same scope as the
// owning emulator instance.

static cpu_t *cpu_from(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return cfg ? cfg->cpu : NULL;
}

// === CPU class ==============================================================

static value_t attr_cpu_pc(struct object *self) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_pc(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_sr(struct object *self) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(2, cpu_get_sr(cpu));
    v.flags |= VAL_HEX;
    return v;
}

#define CPU_DREG_GETTER(N)                                                                                             \
    static value_t attr_cpu_d##N(struct object *self) {                                                                \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        value_t v = val_uint(4, cpu_get_dn(cpu, N));                                                                   \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }
#define CPU_AREG_GETTER(N)                                                                                             \
    static value_t attr_cpu_a##N(struct object *self) {                                                                \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        value_t v = val_uint(4, cpu_get_an(cpu, N));                                                                   \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }

// clang-format off — these macros expand to function definitions
// without a trailing `;`, which clang-format mis-parses as expressions
// and reflows on every commit. Keep them on one line each.
CPU_DREG_GETTER(0)
CPU_DREG_GETTER(1)
CPU_DREG_GETTER(2)
CPU_DREG_GETTER(3)
CPU_DREG_GETTER(4)
CPU_DREG_GETTER(5)
CPU_DREG_GETTER(6)
CPU_DREG_GETTER(7)
CPU_AREG_GETTER(0)
CPU_AREG_GETTER(1)
CPU_AREG_GETTER(2)
CPU_AREG_GETTER(3)
CPU_AREG_GETTER(4)
CPU_AREG_GETTER(5)
CPU_AREG_GETTER(6)
CPU_AREG_GETTER(7)
// clang-format on

#define ATTR_RO_HEX(name_, get_)                                                                                       \
    {                                                                                                                  \
        .kind = M_ATTR, .name = name_, .flags = VAL_RO | VAL_HEX, .attr = {.type = V_UINT, .get = get_, .set = NULL }  \
    }

static const member_t cpu_members[] = {
    ATTR_RO_HEX("pc", attr_cpu_pc), ATTR_RO_HEX("sr", attr_cpu_sr), ATTR_RO_HEX("d0", attr_cpu_d0),
    ATTR_RO_HEX("d1", attr_cpu_d1), ATTR_RO_HEX("d2", attr_cpu_d2), ATTR_RO_HEX("d3", attr_cpu_d3),
    ATTR_RO_HEX("d4", attr_cpu_d4), ATTR_RO_HEX("d5", attr_cpu_d5), ATTR_RO_HEX("d6", attr_cpu_d6),
    ATTR_RO_HEX("d7", attr_cpu_d7), ATTR_RO_HEX("a0", attr_cpu_a0), ATTR_RO_HEX("a1", attr_cpu_a1),
    ATTR_RO_HEX("a2", attr_cpu_a2), ATTR_RO_HEX("a3", attr_cpu_a3), ATTR_RO_HEX("a4", attr_cpu_a4),
    ATTR_RO_HEX("a5", attr_cpu_a5), ATTR_RO_HEX("a6", attr_cpu_a6), ATTR_RO_HEX("a7", attr_cpu_a7),
};

static const class_desc_t cpu_class = {
    .name = "cpu",
    .members = cpu_members,
    .n_members = sizeof(cpu_members) / sizeof(cpu_members[0]),
};

// === Memory class ===========================================================

static value_t attr_mem_ram_size(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return val_uint(4, cfg ? cfg->ram_size : 0u);
}

static value_t attr_mem_rom_size(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return val_uint(4, (cfg && cfg->machine) ? cfg->machine->rom_size : 0u);
}

static const member_t memory_members[] = {
    {.kind = M_ATTR,
     .name = "ram_size",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_mem_ram_size, .set = NULL}},
    {.kind = M_ATTR,
     .name = "rom_size",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_mem_rom_size, .set = NULL}},
};

static const class_desc_t memory_class = {
    .name = "memory",
    .members = memory_members,
    .n_members = sizeof(memory_members) / sizeof(memory_members[0]),
};

// === Machine class ==========================================================

static value_t attr_machine_model_id(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return val_str((cfg && cfg->machine && cfg->machine->model_id) ? cfg->machine->model_id : "");
}

static value_t attr_machine_model_name(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return val_str((cfg && cfg->machine && cfg->machine->model_name) ? cfg->machine->model_name : "");
}

static value_t attr_machine_cpu_clock(struct object *self) {
    config_t *cfg = (config_t *)object_data(self);
    return val_uint(4, (cfg && cfg->machine) ? cfg->machine->cpu_clock_hz : 0u);
}

static const member_t machine_members[] = {
    {.kind = M_ATTR,
     .name = "model_id",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = attr_machine_model_id, .set = NULL}  },
    {.kind = M_ATTR,
     .name = "model_name",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = attr_machine_model_name, .set = NULL}},
    {.kind = M_ATTR,
     .name = "cpu_clock_hz",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_machine_cpu_clock, .set = NULL}   },
};

static const class_desc_t machine_class = {
    .name = "machine",
    .members = machine_members,
    .n_members = sizeof(machine_members) / sizeof(machine_members[0]),
};

// === Scheduler / Shell / Storage stubs ======================================
//
// Empty member tables — these classes exist purely so the path
// resolver sees them and `eval` can confirm they are present. Real
// content lands in later milestones.

static const class_desc_t scheduler_class_desc = {.name = "scheduler", .members = NULL, .n_members = 0};
static const class_desc_t shell_class_desc = {.name = "shell", .members = NULL, .n_members = 0};
static const class_desc_t storage_class_desc = {.name = "storage", .members = NULL, .n_members = 0};

// === Install / uninstall ====================================================

#define MAX_STUBS 8
static struct object *g_stubs[MAX_STUBS];
static int g_stub_count = 0;

static struct object *attach_stub(const class_desc_t *cls, void *data, const char *name) {
    if (g_stub_count >= MAX_STUBS)
        return NULL;
    char err[160];
    if (!object_validate_class(cls, err, sizeof(err))) {
        fprintf(stderr, "gs_classes: class '%s' invalid: %s\n", cls->name ? cls->name : "?", err);
        return NULL;
    }
    struct object *o = object_new(cls, data, name);
    if (!o)
        return NULL;
    object_attach(object_root(), o);
    g_stubs[g_stub_count++] = o;
    return o;
}

void gs_classes_install(struct config *cfg) {
    if (g_stub_count > 0)
        return; // idempotent
    attach_stub(&cpu_class, cfg, "cpu");
    attach_stub(&memory_class, cfg, "memory");
    attach_stub(&scheduler_class_desc, cfg, "scheduler");
    attach_stub(&machine_class, cfg, "machine");
    attach_stub(&shell_class_desc, cfg, "shell");
    attach_stub(&storage_class_desc, cfg, "storage");
}

void gs_classes_uninstall(void) {
    for (int i = g_stub_count - 1; i >= 0; i--) {
        struct object *o = g_stubs[i];
        if (o) {
            object_detach(o);
            object_delete(o);
        }
        g_stubs[i] = NULL;
    }
    g_stub_count = 0;
}
