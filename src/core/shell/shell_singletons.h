// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell_singletons.h
// The process singletons shell_init() installs, declared in one findable
// place.
//
// These were twelve `extern` declarations in shell_init()'s own body, plus a
// thirteenth further down.  A function-scope extern is invisible to the
// compiler when it checks the DEFINITION, so a signature change in any of
// them produced a silent ABI mismatch rather than an error -- and AGENTS.md
// says prototypes belong in headers.  The block was also the de facto list of
// what a process has installed, which is worth being able to find.
//
// Each of these is defined in its own module.  They are gathered rather than
// each module's header being included because several of those headers pull
// in machine/config types shell.c has no other reason to see.

#ifndef GS_SHELL_SINGLETONS_H
#define GS_SHELL_SINGLETONS_H

#ifdef __cplusplus
extern "C" {
#endif

struct config;

// Object-tree root: the top-level method table, and the per-cfg stubs.
void root_install_class(void);
void root_install(struct config *cfg);

// Subsystem singletons, each attaching its own node at shell_init time.
void rom_init(void);
void vrom_init(void);
void prom_init(void);
void machine_init(void);
void checkpoint_init(void);
void archive_init(void);

// Class registrations for singletons whose node is attached from elsewhere.
void mouse_class_register(void);
void screen_class_register(void);
void vfs_class_register(void);
void find_class_register(void);
void scsi_class_register(void);

#ifdef __cplusplus
}
#endif

#endif // GS_SHELL_SINGLETONS_H
