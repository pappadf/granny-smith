// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memory_iface_sized.h
// A memory_interface_t generated from one sized register handler pair.
//
// A device whose registers are decoded by one function taking the access
// width (and, for reads, whether the access is a debugger peek) used to spell
// out the nine width thunks by hand -- read8/16/32, peek8/16/32,
// write8/16/32 -- each a one-line cast and forward, identical between cards
// apart from the context type (the 24AC and the 8*24 GC carried the same 47
// lines).  MEMORY_IFACE_SIZED emits them and the interface in one line.
//
// The thunks are still one direct call each, as the hand-written ones were:
// these sit on the 68K MMIO path, so a single function with a width switch
// behind one pointer would be the slower shape.

#ifndef MEMORY_IFACE_SIZED_H
#define MEMORY_IFACE_SIZED_H

// === Includes ===

#include "memory.h"

#include <stdbool.h>
#include <stdint.h>

// === Macros ===

// Defines `static const memory_interface_t name` over a device context `ctx_t`:
//   uint32_t rd(ctx_t *ctx, uint32_t addr, unsigned width, bool peek)
//   void     wr(ctx_t *ctx, uint32_t addr, uint32_t value, unsigned width)
// with width 1, 2 or 4 and the value in the low `width` bytes.
#define MEMORY_IFACE_SIZED(name, ctx_t, rd, wr)                                                                        \
    static uint8_t name##_read8(void *dev, uint32_t addr) {                                                            \
        return (uint8_t)rd((ctx_t *)dev, addr, 1, false);                                                              \
    }                                                                                                                  \
    static uint16_t name##_read16(void *dev, uint32_t addr) {                                                          \
        return (uint16_t)rd((ctx_t *)dev, addr, 2, false);                                                             \
    }                                                                                                                  \
    static uint32_t name##_read32(void *dev, uint32_t addr) {                                                          \
        return rd((ctx_t *)dev, addr, 4, false);                                                                       \
    }                                                                                                                  \
    static uint8_t name##_peek8(void *dev, uint32_t addr) {                                                            \
        return (uint8_t)rd((ctx_t *)dev, addr, 1, true);                                                               \
    }                                                                                                                  \
    static uint16_t name##_peek16(void *dev, uint32_t addr) {                                                          \
        return (uint16_t)rd((ctx_t *)dev, addr, 2, true);                                                              \
    }                                                                                                                  \
    static uint32_t name##_peek32(void *dev, uint32_t addr) {                                                          \
        return rd((ctx_t *)dev, addr, 4, true);                                                                        \
    }                                                                                                                  \
    static void name##_write8(void *dev, uint32_t addr, uint8_t value) {                                               \
        wr((ctx_t *)dev, addr, value, 1);                                                                              \
    }                                                                                                                  \
    static void name##_write16(void *dev, uint32_t addr, uint16_t value) {                                             \
        wr((ctx_t *)dev, addr, value, 2);                                                                              \
    }                                                                                                                  \
    static void name##_write32(void *dev, uint32_t addr, uint32_t value) {                                             \
        wr((ctx_t *)dev, addr, value, 4);                                                                              \
    }                                                                                                                  \
    static const memory_interface_t name = {                                                                           \
        .read_uint8 = name##_read8,                                                                                    \
        .read_uint16 = name##_read16,                                                                                  \
        .read_uint32 = name##_read32,                                                                                  \
        .write_uint8 = name##_write8,                                                                                  \
        .write_uint16 = name##_write16,                                                                                \
        .write_uint32 = name##_write32,                                                                                \
        .peek_uint8 = name##_peek8,                                                                                    \
        .peek_uint16 = name##_peek16,                                                                                  \
        .peek_uint32 = name##_peek32,                                                                                  \
    }

#endif // MEMORY_IFACE_SIZED_H
