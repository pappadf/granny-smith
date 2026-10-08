// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// system_internal.h
// Private config_t struct definition for machine implementations.
//
// This header exposes the full `struct config` layout to machine code
// (src/machines/*.c) and system.c itself. Platform code (em_main.c,
// headless_main.c) must NOT include this header — they interact with
// config_t only through the opaque handle declared in system.h.

#ifndef SYSTEM_INTERNAL_H
#define SYSTEM_INTERNAL_H

// Only the headers whose types config_t embeds by value: the profile and
// storage-device table (machine_profile.h), the build options
// (machine_build_opts.h), cpu_debug_if_t (debug.h) and the handle typedefs
// (system.h).  The devices config_t points at are forward-declared -- in
// system.h, or below -- so a TU that dereferences one includes its header.
#include "debug.h"
#include "machine_build_opts.h"
#include "machine_profile.h"
#include "system.h"

#include <stdint.h>

struct adb;
typedef struct adb adb_t;

struct nubus_bus;
typedef struct nubus_bus nubus_bus_t;

// The most disk images one machine tracks (config_t.images); also the bound
// a restored checkpoint's image list is checked against.
#define MAX_IMAGES 10

// The machine's mutable runtime state, kept apart from the fields that say
// what the machine is made of: written by interrupt sources and read on
// dispatch while the composition above it stays fixed after construction.
typedef struct system_runtime {
    // Active interrupt-source bitmask; the bits are the family's own
    // (PLUS_IRQ_*, MAC030_GLUE_IRQ_*, AV_IRQ_*, the Lisa's levels, OSS).
    uint32_t irq;
} system_runtime_t;

// Full definition of the opaque config_t handle.
// The forward declaration (`struct config;`) in system.h makes this type
// visible externally; this definition adds the fields for internal use.
// Forward declaration — the PowerPC main-CPU core (src/core/cpu/ppc/).
struct ppc;

struct config {
    const hw_profile_t *machine; // active machine profile (set by system_create)
    // Construction's arguments: what the caller asked for, filled by
    // system_create and read by whoever needs it during construction, then
    // cleared once the machine is built -- nothing reads them afterwards
    // (machine_build_opts.h).
    machine_build_opts_t build_opts;
    // The storage devices the machine was built with -- its document's, or
    // its model's default configuration's: the positions an image can be
    // attached to (machine.storage).  Kept for the machine's life and in its
    // checkpoint.
    int n_storage;
    machine_storage_dev_t storage[MACHINE_STORAGE_MAX];
    uint32_t ram_size; // actual RAM size in bytes
    // The checkpoint parts, in construction order (machine_parts.h).
    struct machine_part_entry *parts;
    int n_parts, cap_parts;
    char part_open[32]; // the part being built (machine_part_begin), "" between parts
    void *machine_context; // machine-specific state (e.g., plus_state_t)

    // Core CPU and memory subsystems.  The main CPU is a tagged handle:
    // cpu_arch discriminates, and exactly one of
    // cpu / ppc is non-NULL on a built machine.
    cpu_arch_t cpu_arch; // set by system_create from machine->cpu_model
    cpu_t *cpu; // 68K main CPU (NULL on PPC machines)
    struct ppc *ppc; // PowerPC main CPU (NULL on 68K machines)
    cpu_debug_if_t cpu_dbg; // main-CPU debug seam (populated by system_create)
    memory_map_t *memory_map;

    // VIA chips (via1 = primary; via2 = NULL on Plus)
    via_t *via1;
    via_t *via2; // secondary VIA (SE/30, IIcx); NULL for Plus

    // Other peripherals
    scc_t *scc;
    // The machine's connection to the AppleTalk network (appletalk.h),
    // plugged into the SCC's LocalTalk channel; NULL on a machine without one.
    struct atalk_conn *atalk;
    // The machine's ImageWriter (iw_printer.h), on a serial port or LocalTalk;
    // NULL on a machine without an SCC.
    struct iw_printer *imagewriter;
    scsi_t *scsi;
    rtc_t *rtc;
    floppy_t *floppy; // floppy controller: IWM (Plus) or SWIM (SE/30)
    sound_t *sound; // PWM sound (Plus); NULL on SE/30 / IIcx (which use ASC)
    mouse_t *mouse;
    keyboard_t *keyboard;
    adb_t *adb; // ADB controller (SE/30, IIcx); NULL for Plus
    // The machine.adb.keyboard object and its paced typing (host_input.h).
    // Per machine on every family, including the ones with no adb_t.
    struct host_input *host_input;

    debug_t *debugger;

    // Disk images tracked for checkpoint/restore
    image_t *images[MAX_IMAGES];
    int n_images;

    scheduler_t *scheduler;

    // Runtime state (system_runtime_t above)
    system_runtime_t rt;

    // NuBus subsystem. NULL on machines without NuBus.
    nubus_bus_t *nubus;

    // PCI subsystem (one root, one bus per host bridge).  NULL on machines
    // without PCI.  Declared by struct tag: core/peripherals/pci/pci.h is
    // a machine-side include, and this header must not drag it in (its
    // sibling card.h would shadow the NuBus one).
    struct pci_root *pci;
};

// The machine's image list as a construction argument (image_list_t): the
// controllers a restore builds resolve their saved media in it.
#define CONFIG_IMAGES(cfg) (&(const image_list_t){(cfg)->images, (cfg)->n_images})

#endif // SYSTEM_INTERNAL_H
