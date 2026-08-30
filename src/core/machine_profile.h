// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_profile.h
// PUBLIC machine descriptor + capability types.  This is the one machine-
// subsystem header the platform-agnostic core is allowed to include
// (system.c / system_config.h store and read a `const hw_profile_t *`).  The
// machine *implementation* headers (mac030/…, glue/…, mdu/…, oss/…, lisa/…,
// runtime/…, the per-machine _internal.h) are off-limits to core — a CI
// layering check enforces that (proposal §4.3).
//
// machines/machine.h includes this and adds the implementation-side surface
// (the extern profile objects + the registry/object-model entry points).

#ifndef GS_CORE_MACHINE_PROFILE_H
#define GS_CORE_MACHINE_PROFILE_H

#include "common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations
struct config;
struct nubus_slot_decl;
struct image;
struct object;

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
// source of truth the capability probe exports as `mmu.kind`.
typedef enum mmu_kind {
    MMU_NONE = 0, // no MMU (compact 68000 Macs)
    MMU_68030_PMMU, // Motorola 68030 integrated PMMU
    MMU_LISA_SEGMENT, // Apple Lisa custom segment MMU
    MMU_68040, // Motorola 68040 integrated MMU (URP/SRP, fixed 3-level walk)
    MMU_PPC_601, // MPC601 integrated MMU (BAT + segment + hashed page table)
    MMU_PPC_604, // MPC604 integrated MMU (architected split I/D BATs + hashed page table)
} mmu_kind_t;

// Wire string for an mmu_kind_t ("none" / "68030_pmmu" / "lisa_segment" /
// "68040" / "ppc_601" / "ppc_604").
const char *mmu_kind_to_string(mmu_kind_t kind);

// Main-CPU architecture of a machine (PPC proposal §3.9a).  The tagged
// discriminator for config_t's main-CPU handle: exactly one of the per-arch
// core pointers is non-NULL, selected by this tag.  Derived from
// hw_profile_t.cpu_model — never stored in the profile separately.
typedef enum cpu_arch {
    CPU_ARCH_M68K = 0, // Motorola 68000/68030/68040 (src/core/cpu/)
    CPU_ARCH_PPC, // PowerPC — MPC601 (src/core/cpu/ppc/)
} cpu_arch_t;

// PowerPC model ids for hw_profile_t.cpu_model (the 68K ids live in cpu.h).
// Both are models of the one `ppc` module (src/core/cpu/ppc/), discriminated
// by ppc_t.cpu_model the way cpu.c discriminates 68000/030/040.
#define CPU_MODEL_PPC601 601
#define CPU_MODEL_PPC604 604

// Main-CPU architecture implied by a profile's cpu_model.
static inline cpu_arch_t cpu_arch_for_model(int cpu_model) {
    return (cpu_model == CPU_MODEL_PPC601 || cpu_model == CPU_MODEL_PPC604) ? CPU_ARCH_PPC : CPU_ARCH_M68K;
}

// How a machine attaches a hard-disk image.  Every Mac hangs its HD off the
// SCSI bus (scsi.attach_hd(path, id)); the Lisa 2 / Macintosh XL use the
// parallel-port ProFile instead (profile.attach(path, writable)).  This is the
// typed fact the config UI reads to label the HD row and pick the attach call —
// no model-name guessing (proposal §4.4).  Default 0 = SCSI, so every existing
// profile keeps its behavior without an explicit field.
typedef enum hd_bus {
    HD_BUS_SCSI = 0, // SCSI bus: scsi.attach_hd
    HD_BUS_PROFILE, // Lisa/XL parallel-port ProFile: profile.attach
} hd_bus_t;

// Wire string for an hd_bus_t ("scsi" / "profile").
const char *hd_bus_to_string(hd_bus_t bus);

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
    // The bay the firmware boots from by default, when that is not the
    // first one listed (the Network Server's Open Firmware boots
    // `disk2:aix`, bay 2).  The configuration dialog preselects it.
    bool boot;
};

// One auxiliary CPU core on a machine (heterogeneous multi-CPU): a
// peripheral processor executing real guest code on the main timeline
// (docs/core/cpu/cores.md).  Exported as `capabilities.aux_cpus`.
struct aux_cpu_slot {
    const char *name; // instance name — the machine.<name> node; NULL terminates
    const char *arch; // ISA token, e.g. "dsp3210"
    uint32_t freq; // core clock in Hz
};

// Which bus a medium in transit is attached through (media_slot_t below).
typedef enum media_bus {
    MEDIA_BUS_FLOPPY = 0, // unit = drive index
    MEDIA_BUS_SCSI, // unit = SCSI id; the device-identity fields apply
    // A machine's SECOND SCSI bus, where it has one.  The Apple Network
    // Servers carry two fast/wide channels and a SCSI id names a device on
    // neither without saying which channel it is on, so the bus has to
    // ride along with the medium.
    MEDIA_BUS_SCSI2,
    MEDIA_BUS_PROFILE, // Lisa/XL parallel-port ProFile (unit unused)
} media_bus_t;

// One mounted medium in transit across a machine.restart power-cycle
// (proposal-boot-vs-reset §3.3): the OPEN image handle — never a captured
// path, so delta writes and blank images survive by construction — plus the
// attachment coordinates needed to hand the handle back to the rebuilt
// machine.
typedef struct media_slot {
    media_bus_t bus;
    int unit; // floppy drive index / SCSI id
    struct image *img; // open handle; ownership is in transit
    // SCSI device identity (MEDIA_BUS_SCSI only), captured from the dying
    // device so the rebuilt one presents the same drive to the guest.
    int scsi_type; // enum scsi_device_type value (1 = hd, 2 = cdrom)
    uint16_t block_size;
    bool read_only;
    char vendor[9];
    char product[17];
    char revision[5];
} media_slot_t;

// Transfer capacity: 2 floppy drives + 8 SCSI ids on each of two buses +
// 1 ProFile.
#define MEDIA_SLOTS_MAX 20

// Machine lifecycle + host-input vtable.  The behavior half of a machine
// (proposal §4.4): hw_profile_t is pure descriptor DATA and points at one of
// these.  system.c / nubus.c dispatch through it; every hook is NULL-safe.
// (memory_layout_init and checkpoint_restore are deliberately absent — they
// were never dispatched: each init runs its own layout directly and restore
// is folded into init.)
typedef struct machine_substrate {
    void (*init)(struct config *cfg, checkpoint_t *cp);
    void (*reset)(struct config *cfg); // hardware RESET line
    void (*teardown)(struct config *cfg);
    void (*checkpoint_save)(struct config *cfg, checkpoint_t *cp);

    void (*update_ipl)(struct config *cfg, int source, bool active); // NuBus IRQ routing
    void (*trigger_vbl)(struct config *cfg);

    // Drive NuBus slot `slot` ($9..$E) /NMRQ active/inactive.  `umbrella_edge`
    // is true when this transition flips the "any slot asserted" aggregate.
    // Every NuBus machine implements it (GLUE → VIA2 port-A bit + CA1 on the
    // umbrella edge; MDU/OSS → the chipset's own IRQ controller via update_ipl);
    // keeps nubus.c machine-agnostic — no cfg->via2 poke (proposal §4.4).  NULL
    // on non-NuBus machines (Plus / Lisa), which never reach it.
    void (*nubus_slot_irq)(struct config *cfg, int slot, bool active, bool umbrella_edge);

    // Drive PCI slot `slot`'s strapped INTA-D line active/inactive.  The
    // PCI slot lines are level-sensitive and have no umbrella (each has
    // its own source bit in the machine's interrupt controller), so there
    // is no edge argument.  Implemented by every PCI machine (TNT → Grand
    // Central externals 23-25 / 27-29); keeps pci.c machine-agnostic.
    // NULL on machines without PCI slots.
    void (*pci_slot_irq)(struct config *cfg, int slot, bool active);

    // Floppy insertion + host-input injection + primary display, implemented by
    // EVERY substrate (Macs route to the shared mac_* helpers / NuBus video;
    // Lisa to its FDC / COPS) — one uniform path, no NULL-and-fallback
    // (proposal §4.4).  `display` may still be NULL on machines that surface
    // their framebuffer through the NuBus primary-display path instead.
    int (*fd_insert)(struct config *cfg, int drive, struct image *disk);
    bool (*fd_present)(struct config *cfg, int drive);
    int (*input_key)(struct config *cfg, const char *key, bool down);
    int (*input_mouse_move)(struct config *cfg, int x, int y, const char *mode);
    int (*input_mouse_button)(struct config *cfg, bool down, const char *mode);
    struct display *(*display)(struct config *cfg);

    // machine.restart media transfer (proposal-boot-vs-reset §3.3).
    // media_detach hands every mounted medium's open image handle (plus its
    // attachment coordinates) to `out`, removing them from whatever would
    // close them during teardown, and returns the count; media_attach hands
    // one such handle back to the freshly built machine (0 = attached, the
    // callee now owns the handle; <0 = the caller must close it).  Macs bind
    // the shared cfg->floppy/cfg->scsi implementation in system.c
    // (system_media_detach_std / system_media_attach_std); the Lisa
    // implements its own (parallel FDC + ProFile).
    int (*media_detach)(struct config *cfg, media_slot_t *out, int max);
    int (*media_attach)(struct config *cfg, const media_slot_t *slot);
} machine_substrate_t;

// Machine descriptor: static metadata for each emulated machine model.  The
// behavior (lifecycle + host input) lives on the bound machine_substrate_t.
typedef struct hw_profile {
    // Identity
    const char *name; // Human-readable, e.g. "Macintosh Plus"
    const char *id; // Short token, e.g. "plus"

    // Processor
    int cpu_model; // 68000, 68030
    uint32_t freq; // CPU clock in Hz
    // Typed MMU kind — the single source of truth behind the exported
    // `mmu.kind` capability (proposal §4.4).  FPU presence is derived from
    // cpu_model via cpu_has_fpu(); neither is a separate descriptor field.
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

    // Hard-disk attach interface (see hd_bus_t).  Default HD_BUS_SCSI (0): the
    // HD row attaches via scsi.attach_hd and takes its label from scsi_slots.
    // The Lisa/XL set HD_BUS_PROFILE — their parallel ProFile is not on the
    // SCSI bus (scsi_slots stays empty) and the UI attaches via profile.attach.
    hd_bus_t hd_bus;

    // CD-ROM availability is a build-time flag, not a model-level UX policy:
    // every emulated Mac architecturally supports a SCSI CD bay; the flag
    // gates dialog display while individual models catch up driver-wise.
    bool has_cdrom;
    int cdrom_id; // SCSI bus id for the CD bay; conventionally 3.

    // On-board video digitizer (the AV family's DMSD/VDC capture path).
    // Drives the exported `video_in` capability, which gates the frontend's
    // camera control the same way `fpu`/`mmu.kind` gate the debug panels.
    bool has_video_in;

    // On-board audio input (the AV family's Singer codec microphone path).
    // Drives the exported `audio_in` capability, which gates the frontend's
    // microphone control exactly as `video_in` gates the camera control.
    // Separate from `has_video_in` on purpose: the two seams are
    // independent, and a machine could plausibly have one without the other.
    bool has_audio_in;

    // Auxiliary CPU cores (heterogeneous multi-CPU, cores.md): peripheral
    // processors that execute real guest code but do not own time — the AV
    // family's DSP3210.  Sentinel-terminated (name == NULL); NULL when the
    // machine has none.  Drives the exported `capabilities.aux_cpus` list.
    const struct aux_cpu_slot *aux_cpus;

    // NuBus slot declarations — sentinel-terminated array of
    // nubus_slot_decl_t (slot id, kind, builtin card / default card).
    // Topology only: which cards FIT a configurable slot is computed from
    // the card registry (nubus_card_fits_socket), not listed here.
    // Used by machine.profile to enumerate cards per slot and build
    // the per-card video-mode catalog the configuration dialog needs.
    // NULL for non-NuBus machines (Plus, …).  The machine's `init`
    // callback passes this same pointer to nubus_init() so the
    // runtime view and the profile view are guaranteed identical.
    const struct nubus_slot_decl *nubus_slots;

    // PCI slot declarations — sentinel-terminated array of pci_slot_decl_t
    // (logical slot, kind, label, bus/device/interrupt line, builtin or
    // default card).  Topology only: which cards FIT a socket is computed
    // from the card registry (pci_card_fits_socket), not listed here.  The
    // machine's `init` hands this same pointer to pci_init(), so the
    // machine.profile view and the runtime view are identical by
    // construction.  NULL for non-PCI machines.
    const struct pci_slot_decl *pci_slots;

    // Built-in video that is NOT a NuBus pseudo-card: the display the
    // machine's own substrate publishes (the PDM family's Ariel scanout).
    // Non-NULL means the configuration dialog must offer it beside the
    // NuBus display cards, AND that it can be switched off by leaving its
    // monitor port unconnected — which is what makes a NuBus card the only
    // screen.  NULL on machines whose built-in video is a BUILTIN slot
    // (SE/30, IIci) or which have none at all.
    const char *builtin_video;

    // Behavior: the lifecycle + host-input vtable for this machine.  Machines
    // of the same chipset family SHARE one substrate (glue_substrate /
    // mdu_substrate; iifx is bespoke).
    const machine_substrate_t *substrate;

    // Per-machine board descriptor — chipset-family data the shared substrate
    // interprets (proposal §4.2.2/§4.4).  Typed by convention: the family
    // substrate casts it to its concrete type (mac030_glue_board_t for
    // GLUE/MDU).  NULL for families whose substrate needs no board (Plus,
    // Lisa, and the bespoke IIfx, which carry their data directly).
    const void *board;
} hw_profile_t;

// Registry: find a machine profile by id (NULL if unknown).
const hw_profile_t *machine_find(const char *id);

// Registry: enumerate the built-in profiles.  *out_count receives the count.
const hw_profile_t *const *machine_list(size_t *out_count);

// === Object-model topology (proposal-system-object-model.md §5.1) ==========
// The single `machine` container node — all emulated hardware nests under it
// (machine.cpu, machine.scsi.device[0].image, …).  Defined in
// machines/machine.c but declared here (the one machine header core may
// include) so subsystem *_init code can attach hardware to it instead of the
// root.  Lazily created; a process-singleton that outlives every cfg.
struct object *machine_object(void);

// Set the machine node's display label to the active model name
// ("Macintosh IIcx"), or NULL to clear it back to the bare "machine" segment.
// Called from system_create / system_destroy.
void machine_set_active_label(const char *name);

#endif // GS_CORE_MACHINE_PROFILE_H
