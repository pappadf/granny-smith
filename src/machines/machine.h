// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine.h
// Machine descriptor and registry for multi-machine support.

#ifndef MACHINE_H
#define MACHINE_H

#include "common.h"
#include <stdbool.h>
#include <stdint.h>

// Forward declarations
struct config;

// Machine descriptor: static metadata and callbacks for each emulated machine model
typedef struct hw_profile {
    const char *model_name; // "Macintosh Plus"
    const char *model_id; // "plus"

    // Processor
    int cpu_model; // 68000, 68030
    uint32_t cpu_clock_hz;
    bool mmu_present;
    bool fpu_present;

    // Address space
    int address_bits; // 24 or 32
    uint32_t ram_size_default;
    uint32_t ram_size_max;
    uint32_t rom_size;

    // Peripheral counts
    int via_count; // 1 (Plus) or 2 (IIcx)
    bool has_adb;
    bool has_nubus;
    int nubus_slot_count;

    // Callbacks: machine-specific setup/teardown
    void (*init)(struct config *cfg, checkpoint_t *cp);
    void (*teardown)(struct config *cfg);
    void (*checkpoint_save)(struct config *cfg, checkpoint_t *cp);
    void (*checkpoint_restore)(struct config *cfg, checkpoint_t *cp);

    // Machine-specific memory layout setup
    void (*memory_layout_init)(struct config *cfg);

    // Machine-specific interrupt routing
    void (*update_ipl)(struct config *cfg, int source, bool active);

    // Machine-specific VBL handling
    void (*trigger_vbl)(struct config *cfg);
} hw_profile_t;

// Registry: find a machine profile by model id
const hw_profile_t *machine_find(const char *model_id);

// Registry: register a machine profile
void machine_register(const hw_profile_t *profile);

// Built-in machine profiles (defined in plus.c, iicx.c, se30.c, etc.)
extern const hw_profile_t machine_plus;
extern const hw_profile_t machine_se30;
// extern const hw_profile_t machine_iicx;  // TODO: IIcx stub

#endif // MACHINE_H
