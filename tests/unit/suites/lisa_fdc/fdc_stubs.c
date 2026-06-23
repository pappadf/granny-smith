// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Local stubs for the isolated lisa_fdc unit suite.
//
// lisa_fdc.c references a few symbols that belong to subsystems this suite does
// not link: the Sony GCR geometry helpers (floppy_gcr.c) and the guest-PC fetch
// used only by the level-1 floppy command trace (cpu.c).  None are reached on
// the empty-drive command paths these tests exercise — the geometry helpers are
// only used once media is attached, and the trace evaluates cpu_get_pc() only
// when the "floppy" log category is raised — so trivial stubs satisfy the linker
// without pulling in the CPU or the IWM.

#include "cpu.h"
#include "floppy_internal.h"

#include <stddef.h>
#include <stdint.h>

uint32_t cpu_get_pc(cpu_t *restrict cpu) {
    (void)cpu;
    return 0;
}

int iwm_sectors_per_track(int track) {
    (void)track;
    return 0;
}

size_t iwm_disk_image_offset(int track, int side, int num_sides) {
    (void)track;
    (void)side;
    (void)num_sides;
    return 0;
}
