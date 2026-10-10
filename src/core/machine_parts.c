// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_parts.c
// The list of a machine's checkpoint parts (machine_parts.h).

#include "machine_parts.h"

#include "checkpoint.h"
#include "gs_assert.h"
#include "log.h"
#include "system_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("ckpt");

#define PART_NAME_MAX 32

struct machine_part_entry {
    char name[PART_NAME_MAX];
    machine_part_save_fn save;
    void *obj;
};

bool machine_part_expect(checkpoint_t *cp, const char *name, int index) {
    if (checkpoint_has_error(cp))
        return false;
    char got[PART_NAME_MAX];
    system_read_checkpoint_data(cp, got, sizeof got, "part");
    got[sizeof got - 1] = '\0';
    if (checkpoint_has_error(cp) || strncmp(got, name, sizeof got) != 0) {
        LOG(0, "Error: checkpoint does not match the machine: part %d is '%s' here, '%s' in the file", index, name,
            checkpoint_has_error(cp) ? "(unreadable)" : got);
        checkpoint_set_error(cp);
        return false;
    }
    return true;
}

void machine_part_begin(struct config *cfg, checkpoint_t *cp, const char *name) {
    GS_ASSERT(cfg && name && strlen(name) < PART_NAME_MAX);
    GS_ASSERTF(!cfg->part_open[0], "machine_part_begin('%s') while part '%s' is open", name, cfg->part_open);
    snprintf(cfg->part_open, sizeof cfg->part_open, "%s", name);
    if (cp)
        machine_part_expect(cp, name, cfg->n_parts);
}

void machine_part(struct config *cfg, checkpoint_t *cp, const char *name, machine_part_save_fn save, void *obj) {
    (void)cp; // the name was checked when the part was opened
    GS_ASSERT(cfg && name && save);
    GS_ASSERTF(strcmp(cfg->part_open, name) == 0, "machine_part('%s') closes part '%s'", name, cfg->part_open);
    cfg->part_open[0] = '\0';
    if (cfg->n_parts == cfg->cap_parts) {
        int cap = cfg->cap_parts ? cfg->cap_parts * 2 : 32;
        struct machine_part_entry *p = realloc(cfg->parts, (size_t)cap * sizeof(*p));
        if (!p) {
            LOG(0, "Error: out of memory registering checkpoint part '%s'", name);
            if (cp)
                checkpoint_set_error(cp);
            return;
        }
        cfg->parts = p;
        cfg->cap_parts = cap;
    }
    struct machine_part_entry *e = &cfg->parts[cfg->n_parts++];
    memset(e->name, 0, sizeof e->name);
    snprintf(e->name, sizeof e->name, "%s", name);
    e->save = save;
    e->obj = obj;
}

void machine_part_cancel(struct config *cfg) {
    cfg->part_open[0] = '\0';
}

void machine_parts_save(struct config *cfg, checkpoint_t *cp) {
    for (int i = 0; i < cfg->n_parts && !checkpoint_has_error(cp); i++) {
        struct machine_part_entry *e = &cfg->parts[i];
        system_write_checkpoint_data(cp, e->name, sizeof e->name, "part");
        e->save(e->obj, cp);
    }
}

void machine_parts_free(struct config *cfg) {
    free(cfg->parts);
    cfg->parts = NULL;
    cfg->n_parts = cfg->cap_parts = 0;
    cfg->part_open[0] = '\0';
}
