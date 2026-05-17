// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint_machine.h
// Process-global "current machine" identity used to root all per-machine
// checkpoint state under /opfs/checkpoints/<machine_id>-<created>/.

#ifndef CHECKPOINT_MACHINE_H
#define CHECKPOINT_MACHINE_H

#include <stdbool.h>

// Set the active machine for path derivation of state.checkpoint, the
// manifest, and writable image deltas.  Creates
// /opfs/checkpoints/<machine_id>-<created>/ if it does not exist.
//
// Called exactly once per process lifetime.  A second call is a programming
// error and returns failure; the C side does not support rotating to a
// different machine mid-process.  Rotation is a JS-driven page reload, which
// gives a fresh C context.
//
// `created` is "YYYYMMDDTHHMMSSZ" and is opaque to the C side.  Returns 0 on
// success, non-zero on failure.
int checkpoint_machine_set(const char *machine_id, const char *created);

// Returns the current machine directory, e.g.
// "/opfs/checkpoints/a3f1b8c204e7d59a-20260430T153045Z" — or NULL when no
// machine has been set.
const char *checkpoint_machine_dir(void);

// Returns the machine id (the first half of the directory name).  NULL when
// unset.
const char *checkpoint_machine_id(void);

// Returns the creation timestamp (the second half of the directory name).
// NULL when unset.
const char *checkpoint_machine_created(void);

// Override the parent directory used to host machine subdirectories.  Default
// is "/opfs/checkpoints".  Test harnesses use this to redirect the entire
// tree to a temp location.
void checkpoint_machine_set_root(const char *root);

// Set the active machine directory explicitly (no <machine_id>-<created>
// suffix).  Used by headless callers that pass --checkpoint-dir directly.
// Creates the directory if it does not exist.  Returns 0 on success.
int checkpoint_machine_set_dir(const char *dir);

// Sweep the parent /opfs/checkpoints/ directory, deleting every entry whose
// name does not exactly match the current machine directory.  Also deletes
// any *.tmp files left in the current machine directory by a crashed
// previous write.  Idempotent; safe to call before any images are opened.
// Returns 0 on success, non-zero on failure.
int checkpoint_machine_sweep_others(void);

// Write manifest.json in the current machine directory.  Called exactly once,
// at the end of machine creation, after the slot table is populated.  The
// manifest is informational only: failure is logged but not fatal.
int checkpoint_machine_write_manifest(void);

#endif // CHECKPOINT_MACHINE_H
