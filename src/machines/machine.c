// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine.c
// Machine profile registry implementation.

#include "machine.h"

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
