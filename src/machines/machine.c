// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine.c
// Machine profile registry plus the machine.* object-model surface.

#include "machine.h"

#include "object.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MACHINES 8

// Registry of known machine profiles
static const hw_profile_t *g_machines[MAX_MACHINES];
static int g_machine_count = 0;

// Register a machine profile with the registry
void machine_register(const hw_profile_t *profile) {
    if (!profile || g_machine_count >= MAX_MACHINES)
        return;
    g_machines[g_machine_count++] = profile;
}

// Find a machine profile by its model_id string
const hw_profile_t *machine_find(const char *model_id) {
    if (!model_id)
        return NULL;
    for (int i = 0; i < g_machine_count; ++i) {
        if (g_machines[i] && strcmp(g_machines[i]->model_id, model_id) == 0)
            return g_machines[i];
    }
    return NULL;
}

// === Object-model class descriptor =========================================
//
// instance_data on the machine stub is config_t*. The lifetime is bounded
// by gs_classes_install / gs_classes_uninstall.

static value_t attr_machine_model_id(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_str((cfg && cfg->machine && cfg->machine->model_id) ? cfg->machine->model_id : "");
}

static value_t attr_machine_model_name(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_str((cfg && cfg->machine && cfg->machine->model_name) ? cfg->machine->model_name : "");
}

static value_t attr_machine_cpu_clock(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_uint(4, (cfg && cfg->machine) ? cfg->machine->cpu_clock_hz : 0u);
}

static value_t attr_machine_ram_kb(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_uint(4, cfg ? cfg->ram_size / 1024u : 0u);
}

static value_t attr_machine_created(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_bool(cfg && cfg->machine != NULL);
}

// machine.profiles → list of registered model_ids
static value_t attr_machine_profiles(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    int n = g_machine_count;
    if (n <= 0)
        return val_list(NULL, 0);
    value_t *items = (value_t *)calloc((size_t)n, sizeof(value_t));
    if (!items)
        return val_err("machine.profiles: out of memory");
    for (int i = 0; i < n; i++)
        items[i] = val_str(g_machines[i] && g_machines[i]->model_id ? g_machines[i]->model_id : "");
    return val_list(items, (size_t)n);
}

extern config_t *global_emulator;

// machine.boot(model, [ram_kb]) — atomic machine creation. Tears down any
// existing machine and creates a fresh one with no ROM loaded. Caller must
// follow up with rom.load(path) to actually run anything.
static value_t machine_method_boot(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s || !*argv[0].s)
        return val_err("machine.boot: expected (model, [ram_kb])");
    const char *model_id = argv[0].s;
    const hw_profile_t *profile = machine_find(model_id);
    if (!profile)
        return val_err("machine.boot: unknown model '%s'", model_id);

    uint32_t ram_kb = 0;
    if (argc >= 2) {
        bool ok = false;
        int64_t v = val_as_i64(&argv[1], &ok);
        if (!ok && argv[1].kind == V_UINT)
            v = (int64_t)argv[1].u;
        else if (!ok)
            return val_err("machine.boot: ram_kb must be an integer");
        if (v <= 0)
            return val_err("machine.boot: ram_kb must be positive");
        ram_kb = (uint32_t)v;
        uint32_t max_kb = profile->ram_size_max / 1024u;
        if (ram_kb > max_kb)
            return val_err("machine.boot: %u KB exceeds max %u KB for %s", ram_kb, max_kb, profile->model_name);
        system_set_pending_ram_kb(ram_kb);
    }

    if (global_emulator) {
        system_destroy(global_emulator);
        global_emulator = NULL;
    }
    config_t *cfg = system_create(profile, NULL);
    if (!cfg)
        return val_err("machine.boot: failed to create %s", model_id);
    printf("Machine created: %s (%s), RAM: %u KB\n", profile->model_name, profile->model_id, cfg->ram_size / 1024u);
    return val_bool(true);
}

// machine.register(id, created) — record the active machine identity for
// checkpointing. Routes to the platform's gs_register_machine.
static value_t machine_method_register(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING || argv[1].kind != V_STRING)
        return val_err("machine.register: expected (id, created)");
    return val_bool(gs_register_machine(argv[0].s ? argv[0].s : "", argv[1].s ? argv[1].s : "") == 0);
}

static const arg_decl_t machine_boot_args[] = {
    {.name = "model", .kind = V_STRING, .doc = "Machine model id (plus / se30 / iicx)"},
    {.name = "ram_kb", .kind = V_UINT, .flags = OBJ_ARG_OPTIONAL, .doc = "Optional RAM override (KB)"},
};

static const arg_decl_t machine_register_args[] = {
    {.name = "id",      .kind = V_STRING, .doc = "Machine identity (UUID-like)"},
    {.name = "created", .kind = V_STRING, .doc = "Creation timestamp"          },
};

static const member_t machine_members[] = {
    {.kind = M_ATTR,
     .name = "model_id",
     .doc = "Active machine's model id (\"plus\" / \"se30\" / …)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = attr_machine_model_id, .set = NULL}},
    {.kind = M_ATTR,
     .name = "model_name",
     .doc = "Active machine's human-readable name",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = attr_machine_model_name, .set = NULL}},
    {.kind = M_ATTR,
     .name = "cpu_clock_hz",
     .doc = "CPU clock in Hz",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_machine_cpu_clock, .set = NULL}},
    {.kind = M_ATTR,
     .name = "ram_kb",
     .doc = "Active RAM size in KB",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = attr_machine_ram_kb, .set = NULL}},
    {.kind = M_ATTR,
     .name = "created",
     .doc = "True if a machine has been booted",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = attr_machine_created, .set = NULL}},
    {.kind = M_ATTR,
     .name = "profiles",
     .doc = "List of registered machine model_ids",
     .flags = VAL_RO,
     .attr = {.type = V_LIST, .get = attr_machine_profiles, .set = NULL}},
    {.kind = M_METHOD,
     .name = "boot",
     .doc = "Tear down any active machine and create a new one (no ROM loaded yet)",
     .method = {.args = machine_boot_args, .nargs = 2, .result = V_BOOL, .fn = machine_method_boot}},
    {.kind = M_METHOD,
     .name = "register",
     .doc = "Record the active machine identity for checkpointing",
     .method = {.args = machine_register_args, .nargs = 2, .result = V_BOOL, .fn = machine_method_register}},
};

const class_desc_t machine_class = {
    .name = "machine",
    .members = machine_members,
    .n_members = sizeof(machine_members) / sizeof(machine_members[0]),
};
