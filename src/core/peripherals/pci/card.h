// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// card.h
// PCI device abstraction — the analogue of nubus/card.h, and the only
// header a pluggable card driver under pci/cards/ needs.  See
// proposal-pci-architecture.md §5.1.
//
// Three things live here:
//   * pci_config_decl_t — everything the GENERIC config header
//     (config_space.c) needs to answer a probe and size the device's BARs
//     without ever calling into the driver: identity, class, the writable
//     command bits, the BAR geometry, the expansion-ROM size.
//   * pci_device_ops_t / pci_device_t — one seated device, its vtable and
//     its live header state.  A device is a device: the Bandit's own
//     device-11 header, Grand Central's config presence, Control on the
//     display bus and every future slot card are all instances of this,
//     which is what lets "absent devices read all-ones" be "no device
//     registered here" instead of a hard-coded literal.
//   * pci_card_kind_t — the per-driver descriptor + factory, collected in
//     one explicit registry array in pci.c (the NuBus rule: no linker
//     constructors).  Machines declare topology, cards declare
//     attachment, compatibility is COMPUTED (pci_card_fits_socket).

#ifndef PCI_CARD_H
#define PCI_CARD_H

#include "common.h"
#include "config_space.h"
#include "memory.h" // memory_interface_t (a BAR's device-handler backing)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct config;
struct checkpoint;
struct display;
struct object;
struct nubus_monitor; // display fact, shared with the NuBus profile encoder
typedef struct config config_t;
typedef struct checkpoint checkpoint_t;
typedef struct display display_t;

struct pci_bus;
struct pci_device;
typedef struct pci_bus pci_bus_t;
typedef struct pci_device pci_device_t;

// Per-device vtable.  Every hook is optional; NULL hooks are skipped by
// the bus controller.
typedef struct pci_device_ops {
    // Construction / destruction.  Returns 0 on success.
    int (*init)(pci_device_t *dev, config_t *cfg, checkpoint_t *cp);
    void (*teardown)(pci_device_t *dev, config_t *cfg);
    void (*reset)(pci_device_t *dev, config_t *cfg); // PCI RST#

    // Config space BEYOND the generic header.  The generic layer answers
    // IDs, class, command/status, the BARs, the expansion-ROM BAR and
    // $3C..$3F; a device that owns more (Bandit's $48/$50, a capability
    // block) implements these and returns true to claim the access.
    // Returning false falls through to the generic behaviour.
    bool (*cfg_read)(pci_device_t *dev, uint32_t reg, uint32_t *out);
    bool (*cfg_write)(pci_device_t *dev, uint32_t reg, uint32_t byte, uint8_t value);

    // A decoded region changed: BAR `bar` (PCI_ROM_BAR_INDEX for the
    // expansion ROM) is now at `base`, or is no longer decoded (`on` =
    // false).  The generic layer installs/removes the declared backing
    // before calling this; the hook is for devices that must react
    // (re-derive a view, log, invalidate a cache).
    void (*bar_map)(pci_device_t *dev, int bar, uint32_t base, bool on);

    void (*on_vbl)(pci_device_t *dev, config_t *cfg);
    display_t *(*display)(pci_device_t *dev);
    void (*checkpoint_save)(pci_device_t *dev, checkpoint_t *cp);
    void (*checkpoint_restore)(pci_device_t *dev, checkpoint_t *cp);
    const char *(*name)(const pci_device_t *dev);
} pci_device_ops_t;

// How a device backs one decoded region.  Set once at init through
// pci_bar_backing_iface(); the generic layer decides WHERE and WHEN the
// region appears (proposal §5.4 — the region-registration helper NuBus
// never had).  v1 has one backing kind: the device's own handler,
// dispatched by the owning bridge window.  See pci.h for why the
// host-memory overlay fast path is not here yet.
typedef enum pci_backing_kind {
    PCI_BACKING_NONE = 0, // the BAR decodes nothing (probe hole)
    PCI_BACKING_IFACE, // device handler, dispatched by the bus window
} pci_backing_kind_t;

typedef struct pci_bar_backing {
    pci_backing_kind_t kind;
    const memory_interface_t *iface;
    void *ctx;
    bool mapped; // currently decoded at `base`
    uint32_t base; // where it is decoded (valid while mapped)
} pci_bar_backing_t;

// Which address space a bridge window forwards / a region decodes.  Lives
// here rather than in pci.h because a DEVICE's regions are described in
// this header and pci.h includes it (so `#include "pci.h"` still sees it).
typedef enum pci_space {
    PCI_SPACE_MEM = 0,
    PCI_SPACE_IO,
} pci_space_t;

// A region whose address is NOT BAR-derived: legacy or strapped decode, as
// on parts that predate BAR-based I/O.  Decodes `pci_addr` iff it lies in
// [base, base+span) AND (pci_addr & match_mask) == match_value.
//
// The match pair is what makes ISA-style SPARSE decoding expressible.  A
// mach64 answers only the 64 addresses ((sel << 10) | $2EC) scattered
// through the 64 KB I/O space — it compares just the low 10 bits and uses
// bits 15:10 as a register select — so a plain "claim base..base+size"
// region would have it swallow all 64 KB including addresses it does not
// drive.  Here that is match_mask $3FF, match_value $2EC, span $10000.
// A mask of 0 makes the match vacuous, which is an ordinary contiguous
// claim.
//
// Gated by the command register's space-enable bit exactly like a BAR, so
// a card software has not enabled decodes nothing.  The gate is derived
// from cfg.command, so there is no new checkpointed state.
#define PCI_FIXED_REGIONS 2

typedef struct pci_fixed_region {
    const memory_interface_t *iface; // NULL = unused entry
    void *ctx;
    pci_space_t space;
    uint32_t base; // PCI address the region starts at
    uint32_t span; // bytes it covers
    uint32_t match_mask; // 0 = contiguous claim, no sparse match
    uint32_t match_value;
    bool mapped; // command register currently enables this space
} pci_fixed_region_t;

// One seated device.  Per-device private state hangs off `priv`; the bus
// controller never reads it.
struct pci_device {
    const pci_device_ops_t *ops;
    const pci_config_decl_t *decl;
    pci_bus_t *bus; // owning bus (back-pointer, set by pci_bus_add_device)
    int device_num; // IDSEL AD line (11..31); -1 until seated
    int slot_index; // profile slot number; 0 for non-slot devices
    pci_cfg_state_t cfg; // live generic header state (config_space.h)
    void *priv;
    uint8_t *rom; // expansion-ROM image (FCode), or NULL
    size_t rom_size;
    pci_bar_backing_t backing[PCI_BAR_SLOTS]; // BARs 0..5 + the ROM BAR
    pci_fixed_region_t fixed[PCI_FIXED_REGIONS]; // non-BAR strapped decode
};

// Per-card constructor.  The bus controller calls this once per populated
// slot during pci_seat_slots().  Returns the new device (the bus takes
// ownership) or NULL on failure.
typedef pci_device_t *(*pci_card_factory_fn)(int slot_index, config_t *cfg, checkpoint_t *cp);

// What a card kind physically attaches through.  BUILTIN is deliberately
// 0 so a kind that forgets to declare its attachment is conservatively
// excluded from every socket rather than wrongly offered everywhere (the
// NuBus rule, restated).
typedef enum pci_attach {
    PCI_ATTACH_BUILTIN = 0, // soldered-down device the machine names (Control)
    PCI_ATTACH_PCI, // standard 33 MHz 5 V slot — universal
} pci_attach_t;

// Per-driver descriptor — one static instance per registered driver,
// listed in pci.c's explicit registry array.
// One user-selectable card option, and the values it takes.  `values` is a
// NULL-terminated list of ids; `labels` runs alongside it (same length) and
// may be NULL, in which case the ids are shown.  `default_value` is the id
// the card uses when nothing is staged, so a dialog can show what "leave it
// alone" means rather than inventing a blank entry.
typedef struct pci_card_option {
    const char *key; // "vram" — what pci_option= carries
    const char *label; // "Video Memory"
    const char *const *values; // {"2m", "4m", NULL}
    const char *const *labels; // {"2 MB", "4 MB (expansion module)", NULL}
    const char *default_value;
} pci_card_option_t;

typedef struct pci_card_kind {
    const char *id; // "spinnaker"
    const char *display_name; // "Apple Accelerated PCI Graphics Card"
    pci_attach_t attach; // drives socket matching (pci_card_fits_socket)
    bool requires_prom; // needs a real FCode expansion-ROM image
    // UI grouping hint: "display" / "scsi" / "network" / "other".  A
    // driver property, not a machine one, so the config dialog can group
    // its pickers without carrying card knowledge.  NULL falls back to
    // "display" when the kind advertises monitors, "other" otherwise.
    // (The device's own class_code lives in its pci_config_decl_t; on
    // Control that field carries the documented Chaos conflation, which
    // is exactly why the UI hint is separate.)
    const char *card_class;
    const struct nubus_monitor *monitors; // display cards only; NULL otherwise
    pci_card_factory_fn factory;

    // What stage_option() will accept, DECLARED so a frontend can render a
    // control for it without knowing what card this is.  Sentinel-
    // terminated (an entry whose key is NULL ends the list); NULL means the
    // card takes no options a user should be offered.  The card is still
    // free to accept keys it does not advertise — this is the offered set,
    // not the accepted set.
    const struct pci_card_option *options;

    // The two seams NuBus lacked (proposal §5.1): the generic layer routes
    // staged options and attaches extra object children through the KIND,
    // never by testing card identity.  Both optional.
    bool (*stage_option)(const char *key, const char *value);
    void (*attach_objects)(pci_device_t *dev, struct object *card_node);

    // Is the kind OFFERED on this host right now?  NULL means always.  A
    // kind that needs a host facility (the Voodoo2's WebGPU variant needs
    // a WebGPU device) answers false without it: machine.profile then
    // leaves it out of the socket's card list, so a frontend never offers
    // a choice it cannot honour.  The kind stays REGISTERED regardless —
    // a boot document or a checkpoint may still name it, and the factory
    // falls back on its own.
    bool (*offered)(void);
} pci_card_kind_t;

// Registry accessors.  The registry is an explicit list in pci.c.
const pci_card_kind_t *pci_card_find(const char *id);
const pci_card_kind_t *const *pci_card_registry(void);

// For unknown-id error messages: the registered id that differs from `id`
// only by underscores, or NULL.
const char *pci_card_suggest(const char *id);

#endif // PCI_CARD_H
