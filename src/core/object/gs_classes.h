// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_classes.h
// Stub classes for the object-model rollout. M2 wires these into the
// root tree so the resolver has something to traverse and the `eval`
// shell command has paths to read. Each stub is intentionally minimal:
// read-only attributes that wrap existing C state. Methods, setters,
// and indexed children land in M3–M7 per the milestone plan.

#ifndef GS_OBJECT_GS_CLASSES_H
#define GS_OBJECT_GS_CLASSES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
struct object;

// One-time installation of the stub class tree onto object_root().
// Looks up CPU/memory/scheduler/machine state from `cfg` and creates
// objects that wrap that state read-only. Idempotent: if called twice
// for the same config the second call is a no-op (existing objects
// remain attached).
//
// `cfg` must outlive the root population — typically the caller is
// system_create() and gs_classes_uninstall() runs from system_destroy().
void gs_classes_install(struct config *cfg);

// Install just the root class — the top-level methods (cp, peeler,
// rom_probe, …) that don't depend on a booted machine. Called early
// from shell_init() so JS tooling can call gsEval('rom_probe', …)
// before any machine is created. Safe to call multiple times.
void gs_classes_install_root(void);

// Detach and free every stub object the install path created. Safe to
// call when nothing is installed.
void gs_classes_uninstall(void);

// Conditional uninstall: only tears down when the stubs are still
// associated with `cfg`. Used by `system_destroy(old_cfg)` after a
// `checkpoint --load` has already swapped in stubs for the new cfg.
// See the comment above the install/uninstall block in gs_classes.c.
void gs_classes_uninstall_if(struct config *cfg);

// === lp (logpoint) synthetic class ==========================================
//
// `lp` exposes the per-fire context to ${...} interpolation in
// logpoint messages — `lp.value`, `lp.addr`, `lp.size`,
// `lp.instruction_pc`, `lp.pc` (proposal §5.3). The fields are valid
// only while a logpoint is being formatted; outside that window the
// getters return V_ERROR.
//
// debug.c brackets each format call with begin/end so the lp child
// reads the right values:
//
//   gs_lp_context_begin(addr, value, size);
//   expr_interpolate_string(msg, &ctx);   // getters see lp.* values
//   gs_lp_context_end();

void gs_lp_context_begin(uint32_t addr, uint32_t value, unsigned size);
void gs_lp_context_end(void);

// === M6 — debugger entry-object factories ===================================
//
// Each breakpoint / logpoint owns a per-entry object_t* exposed under
// debugger.breakpoints[id] / debugger.logpoints[id] (proposal §5.3,
// §9). debug.c calls these factories once per entry at set-time;
// object_delete fires the per-entry invalidator hooks (object.h) when
// the entry is removed. NULL is a valid return — object resolution
// falls back to "empty slot" semantics.

struct breakpoint;
struct logpoint;

struct object *gs_classes_make_breakpoint_object(struct breakpoint *bp);
struct object *gs_classes_make_logpoint_object(struct logpoint *lp);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_GS_CLASSES_H
