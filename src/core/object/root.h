// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// root.h
// The `emu` root class plus the install/uninstall lifecycle that
// attaches the few cfg-scoped stubs (storage, shell.alias) under it.
// Declarations for the per-entry debug-object factories live here too
// because they are used by debug.c at breakpoint/logpoint set-time.

#ifndef GS_OBJECT_ROOT_H
#define GS_OBJECT_ROOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
struct object;

// Full root install: attaches the `emu` class onto object_root() and
// wires up the cfg-scoped stubs (storage, shell.alias). Idempotent for
// the same config — a second call with the same `cfg` is a no-op. When
// `cfg` differs from the previous install (e.g., after a checkpoint
// load), the old stubs are torn down before new ones are attached.
//
// `cfg` must outlive the root population — typically the caller is
// system_create() and root_uninstall_if() runs from system_destroy().
void root_install(struct config *cfg);

// Attach just the `emu` class onto object_root(). Called early from
// shell_init() so JS tooling can resolve the top-level methods before
// any machine is created. Safe to call multiple times.
void root_install_class(void);

// Detach and free every stub object the install path created. Safe to
// call when nothing is installed.
void root_uninstall(void);

// Conditional uninstall: only tears down when the stubs are still
// associated with `cfg`. Used by `system_destroy(old_cfg)` after a
// `checkpoint --load` has already swapped in stubs for the new cfg.
void root_uninstall_if(struct config *cfg);

// === Debug entry-object factories ===========================================
//
// Each breakpoint / logpoint owns a per-entry object_t* exposed under
// debug.breakpoints[id] / debug.logpoints[id]. debug.c calls these
// factories once per entry at set-time; object_delete fires the per-entry
// invalidator hooks (object.h) when the entry is removed. NULL is a
// valid return — object resolution falls back to "empty slot" semantics.

struct breakpoint;
struct logpoint;

struct object *gs_classes_make_breakpoint_object(struct breakpoint *bp);
struct object *gs_classes_make_logpoint_object(struct logpoint *lp);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_ROOT_H
