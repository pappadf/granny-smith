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
#include "card.h"
#include "checkpoint.h"
#include "cpu.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "keyboard.h"
#include "machine_profile.h"
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
// Forward declaration — the PowerPC main-CPU core (src/core/cpu/ppc/).
struct ppc;

struct config {
    const hw_profile_t *machine; // active machine profile (set by system_create)
    uint32_t ram_size; // actual RAM size in bytes (from setup --ram or machine default)
    void *machine_context; // machine-specific state (e.g., plus_state_t)

    // Core CPU and memory subsystems.  The main CPU is a tagged handle
    // (PPC proposal §3.9a): cpu_arch discriminates, and exactly one of
    // cpu / ppc is non-NULL on a built machine.
    cpu_arch_t cpu_arch; // set by system_create from machine->cpu_model
    cpu_t *cpu; // 68K main CPU (NULL on PPC machines)
    struct ppc *ppc; // PowerPC main CPU (NULL on 68K machines)
    cpu_debug_if_t cpu_dbg; // main-CPU debug seam (populated by system_create)
    memory_map_t *mem_map;

    // VIA chips (via1 = primary; via2 = NULL on Plus)
    via_t *via1;
    via_t *via2; // secondary VIA (SE/30, IIcx); NULL for Plus

    // Other peripherals
    scc_t *scc;
    scsi_t *scsi;
    rtc_t *rtc;
    floppy_t *floppy; // floppy controller: IWM (Plus) or SWIM (SE/30)
    sound_t *sound; // PWM sound (Plus); NULL on SE/30 / IIcx (which use ASC)
    mouse_t *mouse;
    keyboard_t *keyboard;
    adb_t *adb; // ADB controller (SE/30, IIcx); NULL for Plus

    debug_t *debugger;

    // Disk images tracked for checkpoint/restore
    image_t *images[MAX_IMAGES];
    int n_images;

    scheduler_t *scheduler;
    uint32_t irq; // active interrupt bitmask

    // NuBus subsystem. NULL on machines without NuBus.
    nubus_bus_t *nubus;

    // PCI subsystem (one root, one bus per host bridge).  NULL on machines
    // without PCI.  Declared by struct tag: core/peripherals/pci/pci.h is
    // a machine-side include, and this header must not drag it in (its
    // sibling card.h would shadow the NuBus one included above).
    struct pci_root *pci;
};

#endif // SYSTEM_CONFIG_H
