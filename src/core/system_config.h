// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// system_config.h
// Private config_t struct definition for machine implementations.
//
// This header exposes the full `struct config` layout to machine code
// (src/machines/*.c) and system.c itself. Platform code (em_main.c,
// headless_main.c) must NOT include this header — they interact with
// config_t only through the opaque handle declared in system.h.

#ifndef SYSTEM_CONFIG_H
#define SYSTEM_CONFIG_H

#include "checkpoint.h"
#include "cpu.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "keyboard.h"
#include "machine.h"
#include "memory.h"
#include "mouse.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "system.h"
#include "via.h"

// Full definition of the opaque config_t handle.
// The forward declaration (`struct config;`) in system.h makes this type
// visible externally; this definition adds the fields for internal use.
struct config {
    const hw_profile_t *machine; // active machine profile (set by system_create)
    void *machine_context; // machine-specific state (e.g., plus_state_t)

    // Core CPU and memory subsystems
    cpu_t *cpu; // was: new_cpu
    memory_map_t *mem_map;
    memory_interface_t *mem_iface;

    // VIA chips (via1 = primary; via2 = NULL on Plus)
    via_t *via1; // was: new_via
    via_t *via2; // secondary VIA (SE/30, IIcx); NULL for Plus

    // Other peripherals
    scc_t *scc; // was: new_scc
    scsi_t *scsi; // was: new_scsi
    rtc_t *rtc; // was: new_rtc
    floppy_t *floppy; // primary floppy controller (IWM on Plus)
    mouse_t *mouse;
    keyboard_t *keyboard;

    debug_t *debugger;

    // Disk images tracked for checkpoint/restore
    image_t *images[MAX_IMAGES];
    int n_images;

    struct scheduler *scheduler;
    uint8_t *ram_vbuf; // pointer into RAM at current video framebuffer address
    uint32_t irq; // active interrupt bitmask
};

#endif // SYSTEM_CONFIG_H
