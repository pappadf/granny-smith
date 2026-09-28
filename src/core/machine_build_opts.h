// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_build_opts.h
// Options that must be known BEFORE a device exists, delivered as an argument.

#ifndef MACHINE_BUILD_OPTS_H
#define MACHINE_BUILD_OPTS_H

#include <stdbool.h>
#include <stdint.h>

// Some choices cannot be made by writing a device after the machine is built,
// because they decide what the device IS.  The monitor sense is the clearest
// case: at construction it fixes the display geometry and format, the
// generated declaration-ROM bytes for the generic card kinds, the seeded slot
// PRAM, and on the SE/30 which host regions get mapped.  Setting it afterwards
// is a card rebuild, not an attribute write.
//
// Such an option used to reach its device through a per-module process global
// that the constructor read and cleared -- jmfb_pending_sense_set,
// dafb_pending_sense_set, pdm_pending_monitor_set.  Three problems with that:
// the value was invisible to anything but the two modules that agreed on it,
// the consume was destructive so a second construction silently got the
// default, and the channel was per-KIND while the thing it configures is
// per-SLOT, so two cards of one kind would collide (issue #156).
//
// This carries the same information explicitly, as an argument to
// substrate->init.  Both callers already have it at the right moment:
// machine_boot_apply from the boot document, and system.c's restore path from
// the checkpoint's machine record.
//
// NOT here, deliberately: the per-slot card and video-mode picks
// (machine.nubus.slot[N].card_id and friends).  Those travel through a
// per-slot table that is the boot document's visible extension rather than a
// hidden store, and that is legitimate.  What was wrong was the hidden
// per-kind channel, not the visible per-slot one.
typedef struct machine_build_opts {
    // Monitor sense code for the machine's video, or MACHINE_SENSE_UNSET when
    // the caller did not choose one and the board default applies.
    int video_sense;
} machine_build_opts_t;

// "No sense chosen" -- distinct from every legal 3-bit code, including 0.
#define MACHINE_SENSE_UNSET (-1)

// A build with nothing chosen: every field at its "caller said nothing" value.
static inline machine_build_opts_t machine_build_opts_default(void) {
    machine_build_opts_t o;
    o.video_sense = MACHINE_SENSE_UNSET;
    return o;
}

#endif // MACHINE_BUILD_OPTS_H
