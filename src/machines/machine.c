// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine.c
// Machine profile registry implementation.

#include "machine.h"

#include "object.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

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

const class_desc_t machine_class = {
    .name = "machine",
    .members = machine_members,
    .n_members = sizeof(machine_members) / sizeof(machine_members[0]),
};
