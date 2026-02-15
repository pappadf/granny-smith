// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memory_internal.h
// Page table entry definition for the memory subsystem.

#ifndef MEMORY_INTERNAL_H
#define MEMORY_INTERNAL_H

#include "memory.h"

// Internal page size helper (computed from public PAGE_SHIFT; underscore to avoid conflict with system PAGE_SIZE)
#define PAGE_SIZE_ (1 << PAGE_SHIFT) // 4096

// Number of pages for 24-bit address space (uses public PAGE_SHIFT from memory.h)
#define PAGE_COUNT_24BIT (1 << (24 - PAGE_SHIFT)) // 4096
#endif // MEMORY_INTERNAL_H
