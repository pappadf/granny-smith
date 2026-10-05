// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine.h
// Machine-subsystem umbrella header (implementation side).  The PUBLIC
// descriptor + capability types live in core/machine_profile.h (which the
// platform-agnostic core includes); this header pulls those in and adds the
// implementation-only surface: the extern profile objects each family's
// machine file defines.  Core must NOT include this header — only
// machine_profile.h (enforced by the core-layering check).

#ifndef MACHINE_H
#define MACHINE_H

#include "machine_build_opts.h"
#include "machine_profile.h" // public descriptor + registry API
#include "value.h"

// Built-in machine profiles (defined in each family's machine file:
// compact/plus.c, glue/{se30,iicx,iix}.c, mdu/{iici,iisi}.c, oss/iifx.c,
// lisa/lisa.c).  Registered as a static const array in machine.c.
extern const hw_profile_t machine_plus;
extern const hw_profile_t machine_se30;
extern const hw_profile_t machine_iicx;
extern const hw_profile_t machine_iix;
extern const hw_profile_t machine_iifx;
extern const hw_profile_t machine_iici;
extern const hw_profile_t machine_lisa;
extern const hw_profile_t machine_macxl;
extern const hw_profile_t machine_iisi;
extern const hw_profile_t machine_q700;
extern const hw_profile_t machine_q900;
extern const hw_profile_t machine_q950;
extern const hw_profile_t machine_q840av;
extern const hw_profile_t machine_q660av;
extern const hw_profile_t machine_pm6100;
extern const hw_profile_t machine_pm7100;
extern const hw_profile_t machine_pm8100;
extern const hw_profile_t machine_pm7500;
extern const hw_profile_t machine_pm8500;
extern const hw_profile_t machine_pm9500;
extern const hw_profile_t machine_ans500;
extern const hw_profile_t machine_ans700;
extern const hw_profile_t machine_pmg3dt;
extern const hw_profile_t machine_pmg3mt;

// The in-flight boot document: pointers borrow from the caller; NULL/0/-1
// mean "not given" (the model's own defaults fill them; model and rom are
// required).
typedef struct boot_config {
    const char *model;
    uint32_t ram_kb; // 0 = the profile's ram_default
    const char *rom;
    const char *rom2;
    const char *vrom;
    const char *video_card;
    int video_sense; // -1 = not given
    const char *video_mode;
    const char *custom_mode; // "WxHxD" custom resolution (NULL = none)
    // Which monitor is strapped to the machine's BUILT-IN video port.
    // "none" leaves it unconnected, which switches built-in video off and
    // hands the screen to a NuBus card (machine_profile_t.builtin_video).
    const char *monitor; // NULL = machine default
    // Card id for the machine's FIRST PCI socket, mirroring video_card= for
    // NuBus.  Other slots are configured through slots=.
    const char *pci_card;
    // PCI expansion-ROM file, the sibling of vrom= for FCode cards: the ROM
    // of every slot whose card it provides.  NULL resolves from the offered
    // .prom files.
    const char *prom;
    // Options for that same card, as "key=value" pairs separated by commas
    // ("vram=4m", "vram=4m,monitor=15in_multi").  Which keys mean anything
    // is the CARD's business: the kind's accepts_option() hook accepts or
    // rejects each one before the boot begins, so no card identity leaks
    // into the boot path.
    const char *pci_option;
    // Per-slot configuration: "SLOT=CARD[,key=value]*;..." (machine_slots.c).
    // The sugar above (video_card=, video_mode=, custom_mode=, pci_card=,
    // pci_option=, vrom=, prom=) resolves into the same per-slot entries.
    const char *slots;
    // The configuration document (JSON, machine_config.h): every node the
    // arguments above rewrite into, and the rest -- options, floppies,
    // storage, the startup device, displays.  NULL: the model's defaults.
    const char *config;
    // Headless shorthand: drives added to the storage the document (or the
    // model's default) has, "bus:unit:type[;...]" (type hd or cd), and the
    // floppy positions a caller is about to use (fd1= wants a second drive
    // even where the default configuration leaves that position empty).
    const char *drives;
    int floppies_wanted;
} boot_config_t;

// Apply one boot document: validate, build, swap the new machine in
// (machine.c; shared by machine.boot and headless startup).  Returns V_NONE
// on success; V_ERROR — with the old machine still running — on rejection.
value_t machine_boot_apply(const boot_config_t *doc);

// Resolve the boot document's expansion-slot configuration -- slots= and the
// sugar (video_card= / video_mode= / custom_mode= for the first NuBus socket,
// pci_card= / pci_option= for the first PCI socket, vrom= / prom= for every
// slot whose card the file provides) -- into validated per-slot entries in
// out->slots / out->n_slots (machine_slots.c).  Every check runs here, before
// the running machine is touched; V_ERROR names the slot on a rejection.
value_t machine_slots_resolve(const hw_profile_t *profile, const boot_config_t *doc, machine_build_opts_t *out);

#endif // MACHINE_H
