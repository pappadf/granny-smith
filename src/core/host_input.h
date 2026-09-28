// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// host_input.h
// The `machine.adb.keyboard` object and the paced typing behind it.
//
// This is a PER-MACHINE object, unlike the process-lifetime facade it
// replaces.  It has to be: keyboard.type paces its key transitions as
// scheduler events, and a scheduler event needs a source that is constructed
// with the machine, registered as an event type before scheduler_start so a
// checkpoint with keys in flight can restore, and forgotten in the machine's
// teardown.  The old facade had no such anchor, which is why type() reached
// past the substrate into adb_t and so worked only on ADB Macs — the Plus and
// the Lisa, which have no adb_t, got "the machine has no keyboard".
//
// Nothing here is machine-specific.  Key identity is the ADB virtual keycode
// (machine_profile.h), so a paced transition carries a keycode and goes out
// through system_input_key like any other; the only per-machine number is how
// many key-transition bytes the machine's queue can hold before it starts
// dropping, which the substrate declares.

#ifndef HOST_INPUT_H
#define HOST_INPUT_H

#include "common.h"

struct config;
struct scheduler;

typedef struct host_input host_input_t;

// Build the keyboard object for this machine and attach it under
// `machine.adb`.  Called once per machine from system_create, after the
// substrate has built the scheduler and before scheduler_start.
host_input_t *host_input_init(struct config *cfg, struct scheduler *scheduler);

// Detach and destroy it, and drop any typing still in flight.
void host_input_delete(host_input_t *hi);

#endif // HOST_INPUT_H
