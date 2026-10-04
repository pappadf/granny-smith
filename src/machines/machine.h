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
#include "machine_config.h"
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

// Resolve the boot document's expansion-slot configuration -- slots= and the
// sugar (video_card= / video_mode= / custom_mode= for the first NuBus socket,
// pci_card= / pci_option= for the first PCI socket, vrom= / prom= for every
// slot whose card the file provides) -- into validated per-slot entries in
// out->slots / out->n_slots (machine_slots.c).  Every check runs here, before
// the running machine is touched; V_ERROR names the slot on a rejection.
value_t machine_slots_resolve(const hw_profile_t *profile, const boot_config_t *doc, machine_build_opts_t *out);

#endif // MACHINE_H
