// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// root.h
// The `emu` root class plus the install/uninstall lifecycle that
// attaches the cfg-scoped stubs: the shell namespace here, and each
// subsystem's own nodes through its registered install hook.

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
// wires up the cfg-scoped stubs (the shell namespace, then every registered
// install hook). Idempotent for
// the same `cfg`; if `cfg` differs from the previous install (e.g.,
// after a checkpoint load), the old stubs are torn down before new
// ones are attached. `cfg` must outlive the root population.
void root_install(struct config *cfg);

// Attach just the `emu` class onto object_root(). Called early from
// core_init() so the top-level methods resolve before any machine is
// created. Safe to call multiple times.
void root_install_class(void);

// Detach and free every stub object the install path created. Safe to
// call when nothing is installed.
void root_uninstall(void);

// A subsystem's cfg-scoped nodes (files.images, machine.nubus, machine.pci):
// `install` runs on every root_install and attaches its nodes with
// root_attach_stub; `uninstall` (may be NULL) runs on root_uninstall, after
// the stubs are freed, to drop what the subsystem kept about them.
// Registering the same install twice is a no-op, so an init path may call
// this every time it runs.
typedef void (*root_install_fn)(struct config *cfg);
typedef void (*root_uninstall_fn)(void);
void root_register_install(root_install_fn install, root_uninstall_fn uninstall);

// Attach `o` under `parent` (NULL = the root) as a cfg-scoped stub, freed by
// root_uninstall.  Returns `o`, or NULL -- having freed it -- when its class
// is invalid or the stub table is full.
struct object *root_attach_stub(struct object *parent, struct object *o);

// Tear down only when the installed stubs are still associated with
// `cfg`. Lets `system_destroy(old)` no-op after a `checkpoint --load`
// has already swapped in stubs for the new cfg.
void root_uninstall_if(struct config *cfg);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_ROOT_H
