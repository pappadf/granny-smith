// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iicx.c
// Macintosh IIcx machine profile descriptor (stub).
//
// This file provides the hw_profile_t for a future Macintosh IIcx emulation.
// All callback pointers are NULL — this is a placeholder to validate the
// machine abstraction framework.
//
// Implementation notes for IIcx support:
// - 68030 CPU at 15.6672 MHz with PMMU and optional 68882 FPU
// - 32-bit clean address space
// - Two VIA chips (VIA1 for system, VIA2 for NuBus/SCSI interrupts)
// - Apple Desktop Bus (ADB) instead of VIA-based keyboard/mouse
// - Apple Sound Chip (ASC) instead of PWM sound
// - SWIM floppy controller instead of IWM
// - Three NuBus expansion slots
// - Separate video card (no on-board video)
// - RBV (RAM-Based Video) chip — manages VIA2 functions on some II-series
// - Interrupt routing: VIA1 drives /IPL0, VIA2 drives /IPL1,
//   SCC drives /IPL2 (active-low, accent on the PAL logic differences)

#include "machine.h"

// Macintosh IIcx hardware profile descriptor (stub — all callbacks NULL)
const hw_profile_t machine_iicx = {
    .model_name = "Macintosh IIcx",
    .model_id = "iicx",

    // 68030 at 15.6672 MHz
    .cpu_model = 68030,
    .cpu_clock_hz = 15667200,
    .mmu_present = true,
    .fpu_present = true, // 68882 FPU (optional but standard)

    // 32-bit address space
    .address_bits = 32,
    .ram_size_default = 0x100000, // 1 MB default
    .ram_size_max = 0x8000000, // 128 MB max (8x 16MB SIMMs)
    .rom_size = 0x040000, // 256 KB

    // Two VIAs, ADB present, three NuBus slots
    .via_count = 2,
    .has_adb = true,
    .has_nubus = true,
    .nubus_slot_count = 3,

    // All callbacks NULL — IIcx init/teardown not yet implemented.
    // When implementing:
    // - .init should create two VIA instances, ADB controller, ASC, SWIM,
    //   NuBus manager, and video subsystem
    // - .teardown should free all IIcx-specific resources
    // - .update_ipl should implement IIcx PAL interrupt priority logic:
    //     VIA1 → /IPL0, VIA2 → /IPL1, SCC → /IPL2
    // - .trigger_vbl should assert VIA1 CA1 (same as Plus) and also
    //   signal the video card's VBL interrupt via NuBus/VIA2
    // - .memory_layout_init should set up the 32-bit memory map with
    //   ROM at 0x40000000, I/O at 0x50000000, NuBus at 0xF0000000+
    // - .checkpoint_save/restore should handle two VIAs and IIcx peripherals
    .init = NULL,
    .teardown = NULL,
    .checkpoint_save = NULL,
    .checkpoint_restore = NULL,
    .memory_layout_init = NULL,
    .update_ipl = NULL,
    .trigger_vbl = NULL,
};
