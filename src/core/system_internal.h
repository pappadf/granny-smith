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

#include "adb.h"
#include "card.h"
#include "checkpoint.h"
#include "cpu.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "keyboard.h"
#include "machine_build_opts.h"
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

// The most disk images one machine tracks (config_t.images); also the bound
// a restored checkpoint's image list is checked against.
#define MAX_IMAGES 10

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
    uint32_t irq; // active interrupt bitmask

    // NuBus subsystem. NULL on machines without NuBus.
    nubus_bus_t *nubus;

    // PCI subsystem (one root, one bus per host bridge).  NULL on machines
    // without PCI.  Declared by struct tag: core/peripherals/pci/pci.h is
    // a machine-side include, and this header must not drag it in (its
    // sibling card.h would shadow the NuBus one included above).
    struct pci_root *pci;
};

// The machine's image list as a construction argument (image_list_t): the
// controllers a restore builds resolve their saved media in it.
#define CONFIG_IMAGES(cfg) (&(const image_list_t){(cfg)->images, (cfg)->n_images})

#endif // SYSTEM_INTERNAL_H
