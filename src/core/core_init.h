// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// core_init.h
// Process bootstrap: everything that must exist before the first request
// is served, independent of any machine.

#ifndef GS_CORE_INIT_H
#define GS_CORE_INIT_H

// Bring up the process-lifetime core: the thread latches, logging, the job
// layer, the shell, the object root with its top-level methods, and the
// singleton namespaces that have pre-boot surfaces (rom, machine, catalog,
// files, ...).  Call once, on the emulator thread, before any request;
// later calls do nothing.  Returns 0.
int core_init(void);

#endif // GS_CORE_INIT_H
