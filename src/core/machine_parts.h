// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_parts.h
// A machine's checkpoint is the list of its parts, in construction order.
//
// Every object whose state a checkpoint carries registers itself as a part
// where it is built, with the function that writes its block.  Saving walks
// the list; restoring is construction: each constructor reads its own block
// as it builds, and registers right after.  So the order a checkpoint is
// written in is, by construction, the order a restore reads it in -- there
// is no per-machine save list to keep in step with the build.
//
// Each part's block is followed by its name.  Registering on a restore reads
// that name and checks it, so a checkpoint that does not match the machine
// being built fails at the first part that differs, naming it, rather than
// feeding one device's state to the next.

#ifndef MACHINE_PARTS_H
#define MACHINE_PARTS_H

#include "common.h"

struct config;

typedef void (*machine_part_save_fn)(void *obj, checkpoint_t *cp);

// Register `obj` as the part `name`, saved by `save`.  With `cp` (a restore),
// the part has just read its block: read and check the name that follows.
void machine_part(struct config *cfg, checkpoint_t *cp, const char *name, machine_part_save_fn save, void *obj);

// Write every part, in the order they were registered.
void machine_parts_save(struct config *cfg, checkpoint_t *cp);

// Forget the parts (machine teardown).
void machine_parts_free(struct config *cfg);

// A part-save function for an object whose checkpoint function takes its
// own type: MACHINE_PART_SAVE(scc_checkpoint, scc_t) defines
// scc_checkpoint_part(void *, checkpoint_t *).
#define MACHINE_PART_SAVE(fn, type)                                                                                    \
    static void fn##_part(void *obj, checkpoint_t *cp) {                                                               \
        fn((type *)obj, cp);                                                                                           \
    }

#endif // MACHINE_PARTS_H
