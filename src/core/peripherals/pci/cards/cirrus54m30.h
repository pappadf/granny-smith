// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cirrus54m30.h
// The one hook the Network Server's experimental PReP window needs from the
// 54M30 model (see cirrus54m30.c, "Presenting as a CL-GD5434").

#ifndef CIRRUS54M30_H
#define CIRRUS54M30_H

#include <stdbool.h>

struct pci_device;

// Make the part identify as a CL-GD5434 (CR27 $A8) and decode the legacy
// VGA memory window at PCI $A0000.  Idempotent.  False if `dev` is not a
// 54M30.
bool cirrus54m30_present_as_5434(struct pci_device *dev);

#endif // CIRRUS54M30_H
