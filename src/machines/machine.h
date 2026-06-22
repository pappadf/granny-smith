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

#include "machine_profile.h" // public descriptor + registry API

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

#endif // MACHINE_H
