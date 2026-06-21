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
struct nubus_slot_decl;
struct image;

// Floppy drive capabilities form a strict superset hierarchy:
//   FLOPPY_400K reads 400K only.
//   FLOPPY_800K reads 400K and 800K.
//   FLOPPY_HD   reads 400K, 800K, and 1.44 MB.
// `kind` names the *highest* format the drive supports; readers derive the
// rest from the table.  Wire form is the lowercase string returned by
// floppy_kind_to_string().
typedef enum floppy_kind {
    FLOPPY_400K = 0,
    FLOPPY_800K,
    FLOPPY_HD,
} floppy_kind_t;

// Convert a floppy_kind_t to its wire string ("400k" / "800k" / "hd").
const char *floppy_kind_to_string(floppy_kind_t kind);

// Kind of memory-management unit a machine carries.  Deliberately typed
// (not a bool): the debug UI must tell a 68030 PMMU (which has TC/CRP/SRP/
// TT0/TT1/MMUSR register views) apart from the Lisa's segment MMU (which
// does not) and from a machine with no MMU at all.  This is the single
// source of truth the capability probe exports as `mmu.kind`; it migrates
// onto the machine_substrate vtable when that lands (proposal §4.4).
typedef enum mmu_kind {
    MMU_NONE = 0, // no MMU (compact 68000 Macs)
    MMU_68030_PMMU, // Motorola 68030 integrated PMMU
    MMU_LISA_SEGMENT, // Apple Lisa custom segment MMU
} mmu_kind_t;

// Wire string for an mmu_kind_t ("none" / "68030_pmmu" / "lisa_segment").
const char *mmu_kind_to_string(mmu_kind_t kind);

// One floppy drive slot on a machine.  Sentinel-terminated arrays end at
// the first entry whose `label` is NULL.
struct floppy_slot {
    const char *label; // "Internal FD0"; NULL terminates the array
    floppy_kind_t kind;
};

// One SCSI bus slot wired up by the machine's profile (HD0, HD1, …).
struct scsi_slot {
    const char *label; // "SCSI HD0"; NULL terminates the array
    int id; // Conventional SCSI bus id for this slot
};

// Machine descriptor: static metadata and callbacks for each emulated machine model
typedef struct hw_profile {
    // Identity
    const char *name; // Human-readable, e.g. "Macintosh Plus"
    const char *id; // Short token, e.g. "plus"

    // Processor
    int cpu_model; // 68000, 68030
    uint32_t freq; // CPU clock in Hz
    bool mmu_present;
    bool fpu_present;
    // Typed MMU kind — the single source of truth behind the exported
    // `mmu.kind` capability (proposal §4.4).  `mmu_present` is the legacy
    // bool kept until phase 6 deletes it; new code reads mmu_kind.
    mmu_kind_t mmu_kind;

    // Address space
    int address_bits; // 24 or 32
    uint32_t ram_default; // Bytes
    uint32_t ram_max; // Bytes
    uint32_t rom_size;

    // RAM size choices for the configuration dialog, in KB.
    // Zero-terminated array; the last valid entry is followed by 0.
    const uint32_t *ram_options;

    // Floppy / SCSI slot tables.  Sentinel-terminated.
    const struct floppy_slot *floppy_slots;
    const struct scsi_slot *scsi_slots;

    // CD-ROM availability is a build-time flag, not a model-level UX policy:
    // every emulated Mac architecturally supports a SCSI CD bay; the flag
    // gates dialog display while individual models catch up driver-wise.
    bool has_cdrom;
    int cdrom_id; // SCSI bus id for the CD bay; conventionally 3.

    // SE/30 needs a 32 KB Apple SE/30 video ROM at machine-init time.  When
    // true, the configuration dialog adds a VROM row and gates Start on a
    // non-empty selection.  Dialog-only: machine init still pulls
    // vrom_pending_path() per-machine; this flag exists solely to drive UX.
    bool needs_vrom;

    // Peripheral counts
    int via_count; // 1 (Plus) or 2 (IIcx)
    bool has_adb;
    bool has_nubus;
    int nubus_slot_count;

    // NuBus slot declarations — sentinel-terminated array of
    // nubus_slot_decl_t (slot id, kind, builtin/available card list).
    // Used by machine.profile to enumerate cards per slot and build
    // the per-card video-mode catalog the configuration dialog needs.
    // NULL for non-NuBus machines (Plus, …).  The machine's `init`
    // callback passes this same pointer to nubus_init() so the
    // runtime view and the profile view are guaranteed identical.
    const struct nubus_slot_decl *nubus_slots;

    // Callbacks: machine-specific setup/teardown
    void (*init)(struct config *cfg, checkpoint_t *cp);
    void (*reset)(struct config *cfg); // hardware RESET line: re-init VIAs, overlay, MMU
    void (*teardown)(struct config *cfg);
    void (*checkpoint_save)(struct config *cfg, checkpoint_t *cp);
    void (*checkpoint_restore)(struct config *cfg, checkpoint_t *cp);

    // Machine-specific memory layout setup
    void (*memory_layout_init)(struct config *cfg);

    // Machine-specific interrupt routing
    void (*update_ipl)(struct config *cfg, int source, bool active);

    // Machine-specific VBL handling
    void (*trigger_vbl)(struct config *cfg);

    // Optional: per-machine floppy insertion for non-IWM controllers (the Lisa
    // intelligent FDC).  When cfg->floppy is NULL the framework routes floppy
    // insert/present queries here instead.  NULL on IWM/SWIM machines.
    int (*fd_insert)(struct config *cfg, int drive, struct image *disk);
    bool (*fd_present)(struct config *cfg, int drive);

    // Optional: per-machine host-input injection.  When present, the `keyboard`
    // and `mouse` object methods route here instead of the default Mac (ADB /
    // Toolbox-globals) path — the Lisa uses these to drive its COPS, which has
    // its own keycodes and a relative-delta mouse.  Return 0 on success, <0 if
    // unhandled (the caller then falls back to the default path).
    //   input_key:          `key` is a key name or "0xNN" keycode string.
    //   input_mouse_move:   move/position the cursor; `mode` as for mouse.move.
    //   input_mouse_button: press/release the button; `mode` as for mouse.click.
    int (*input_key)(struct config *cfg, const char *key, bool down);
    int (*input_mouse_move)(struct config *cfg, int x, int y, const char *mode);
    int (*input_mouse_button)(struct config *cfg, bool down, const char *mode);

    // Optional: per-machine primary display.  Used by system_display()
    // when cfg->nubus is NULL (Plus and any future non-NuBus machine);
    // glue030-family machines leave this NULL because their display
    // comes from a NuBus card via cfg->nubus.  Forward declaration of
    // display_t lives in system.h.
    struct display *(*display)(struct config *cfg);
} hw_profile_t;

// Registry: find a machine profile by id
const hw_profile_t *machine_find(const char *id);

// Registry: register a machine profile.  Profiles missing any required
// declarative field (name, id, freq, ram_default, ram_max, ram_options,
// floppy_slots, scsi_slots) are rejected.
void machine_register(const hw_profile_t *profile);

// Built-in machine profiles (defined in plus.c, iicx.c, iix.c, se30.c, etc.)
extern const hw_profile_t machine_plus;
extern const hw_profile_t machine_se30;
extern const hw_profile_t machine_iicx;
extern const hw_profile_t machine_iix;
extern const hw_profile_t machine_iifx;
extern const hw_profile_t machine_iici;
extern const hw_profile_t machine_lisa;
extern const hw_profile_t machine_macxl;
extern const hw_profile_t machine_iisi;

#endif // MACHINE_H
