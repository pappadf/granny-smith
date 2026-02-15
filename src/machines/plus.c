// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// plus.c
// Macintosh Plus machine profile descriptor.
//
// This file defines the hw_profile_t for the Macintosh Plus.
// The actual init/teardown logic currently lives in system.c (setup_plus,
// setup_teardown). As the refactoring progresses, Plus-specific init code
// will migrate here.

#include "machine.h"

// Macintosh Plus hardware profile descriptor
const hw_profile_t machine_plus = {
    .model_name = "Macintosh Plus",
    .model_id = "plus",

    // 68000 at 7.8336 MHz (actual Plus clock)
    .cpu_model = 68000,
    .cpu_clock_hz = 7833600,
    .mmu_present = false,
    .fpu_present = false,

    // 24-bit address space
    .address_bits = 24,
    .ram_size_default = 0x400000, // 4 MB
    .ram_size_max = 0x400000, // 4 MB max
    .rom_size = 0x020000, // 128 KB

    // Single VIA, no ADB, no NuBus
    .via_count = 1,
    .has_adb = false,
    .has_nubus = false,
    .nubus_slot_count = 0,

    // Callbacks: currently NULL — system.c still owns init/teardown.
    // These will be populated when setup_plus() moves here.
    .init = NULL,
    .teardown = NULL,
    .checkpoint_save = NULL,
    .checkpoint_restore = NULL,
    .memory_layout_init = NULL,
    .update_ipl = NULL,
    .trigger_vbl = NULL,
};
