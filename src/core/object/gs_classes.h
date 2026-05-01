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

// Detach and free every stub object the install path created. Safe to
// call when nothing is installed.
void gs_classes_uninstall(void);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_GS_CLASSES_H
