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

#include "adb.h"
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
#include "sound.h"
#include "system.h"
#include "via.h"

// Full definition of the opaque config_t handle.
// The forward declaration (`struct config;`) in system.h makes this type
// visible externally; this definition adds the fields for internal use.
struct config {
    const hw_profile_t *machine; // active machine profile (set by system_create)
    uint32_t ram_size; // actual RAM size in bytes (from setup --ram or machine default)
    void *machine_context; // machine-specific state (e.g., plus_state_t)

    // Core CPU and memory subsystems
    cpu_t *cpu; // was: new_cpu
    memory_map_t *mem_map;

    // VIA chips (via1 = primary; via2 = NULL on Plus)
    via_t *via1; // was: new_via
    via_t *via2; // secondary VIA (SE/30, IIcx); NULL for Plus

    // Other peripherals
    scc_t *scc; // was: new_scc
    scsi_t *scsi; // was: new_scsi
    rtc_t *rtc; // was: new_rtc
    floppy_t *floppy; // floppy controller: IWM (Plus) or SWIM (SE/30)
    sound_t *sound; // PWM sound (Plus); NULL on SE/30 / IIcx (which use ASC)
    mouse_t *mouse;
    keyboard_t *keyboard;
    adb_t *adb; // ADB controller (SE/30, IIcx); NULL for Plus

    debug_t *debugger;

    // Disk images tracked for checkpoint/restore
    image_t *images[MAX_IMAGES];
    int n_images;

    struct scheduler *scheduler;
    uint32_t irq; // active interrupt bitmask

    // NuBus subsystem.  NULL on machines without NuBus (Plus today;
    // future 68000 family).  Set by glue030_init() once it lands; for
    // now stays NULL because no machine creates a bus yet.
    struct nubus_bus *nubus;
};

#endif // SYSTEM_CONFIG_H
