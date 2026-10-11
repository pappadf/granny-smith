// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_parts.h
// A machine's checkpoint is the list of its parts, in construction order.
//
// Every object whose state a checkpoint carries is a part, opened just before
// it is built and registered, with the function that writes its block, just
// after:
//
//     machine_part_begin(cfg, cp, "scc");
//     cfg->scc = scc_init(..., cp);
//     machine_part(cfg, cp, "scc", part_save_scc, cfg->scc);
//
// Saving walks the list; restoring is construction: each constructor reads
// its own block as it builds.  So the order a checkpoint is written in is, by
// construction, the order a restore reads it in -- there is no per-machine
// save list to keep in step with the build.
//
// Each part's name comes before its block.  Opening a part on a restore reads
// that name and checks it BEFORE the constructor reads anything, so a
// checkpoint that does not match the machine being built fails at the first
// part that differs, naming it, and no constructor is handed another part's
// bytes.  Once the checkpoint has an error, every later read returns zeros
// (system_read_checkpoint_data) and the restore fails at the end of the build.

#ifndef MACHINE_PARTS_H
#define MACHINE_PARTS_H

#include "checkpoint.h"
#include "common.h"

struct config;

typedef void (*machine_part_save_fn)(void *obj, checkpoint_t *cp);

// Open the part `name`: what the machine builds next reads its block.  With
// `cp` (a restore), read the part's name from the checkpoint and check it.
void machine_part_begin(struct config *cfg, checkpoint_t *cp, const char *name);

// Register `obj` as the open part `name`, saved by `save`.  The name must be
// the one machine_part_begin opened.
void machine_part(struct config *cfg, checkpoint_t *cp, const char *name, machine_part_save_fn save, void *obj);

// Abandon the open part: what it was building could not be built (a boot
// leaves that slot empty; a restore has already flagged its checkpoint).
void machine_part_cancel(struct config *cfg);

// Read the next part's name from `cp` and check that it is `name`, the part
// numbered `index`; flags the checkpoint and returns false when it is not.
// machine_part_begin's check, for the one reader that runs before there is a
// machine: system_restore reads the board's part to choose the model.
bool machine_part_expect(checkpoint_t *cp, const char *name, int index);

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
