// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_parts.c
// The list of a machine's checkpoint parts (machine_parts.h).

#include "machine_parts.h"

#include "checkpoint.h"
#include "log.h"
#include "system_config.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("ckpt");

#define PART_NAME_MAX 32

struct machine_part_entry {
    char name[PART_NAME_MAX];
    machine_part_save_fn save;
    void *obj;
};

void machine_part(struct config *cfg, checkpoint_t *cp, const char *name, machine_part_save_fn save, void *obj) {
    GS_ASSERT(cfg && name && save && strlen(name) < PART_NAME_MAX);
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
    strncpy(e->name, name, sizeof e->name - 1);
    e->save = save;
    e->obj = obj;

    if (cp && !checkpoint_has_error(cp)) {
        char got[PART_NAME_MAX];
        system_read_checkpoint_data(cp, got, sizeof got, "part");
        got[sizeof got - 1] = '\0';
        if (checkpoint_has_error(cp) || strcmp(got, e->name) != 0) {
            LOG(0, "Error: checkpoint does not match the machine: part %d is '%s' here, '%s' in the file",
                cfg->n_parts - 1, e->name, checkpoint_has_error(cp) ? "(unreadable)" : got);
            checkpoint_set_error(cp);
        }
    }
}

void machine_parts_save(struct config *cfg, checkpoint_t *cp) {
    for (int i = 0; i < cfg->n_parts && !checkpoint_has_error(cp); i++) {
        struct machine_part_entry *e = &cfg->parts[i];
        e->save(e->obj, cp);
        system_write_checkpoint_data(cp, e->name, sizeof e->name, "part");
    }
}

void machine_parts_free(struct config *cfg) {
    free(cfg->parts);
    cfg->parts = NULL;
    cfg->n_parts = cfg->cap_parts = 0;
}
