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

// The wide slots of a byte-only chip (the 5380, the IWM, the SWIM, the SCC):
// nothing decodes a 16- or 32-bit access there, so a read floats the bus to
// all-ones and a write is dropped, each with a log line -- the guest is a
// buggy driver or a probe -- while a peek reads the same all-ones silently,
// so a debugger dump over the chip prints nothing.  Defines name##_read16/32,
// name##_peek16/32 and name##_write16/32 in the device's own translation
// unit, so the log lines keep the device's LOG category; `chip` names the
// part in them.  The expanding file includes log.h.
#define MEMORY_IFACE_OPEN_BUS_WIDE(name, chip)                                                                         \
    static uint16_t name##_read16(void *dev, uint32_t addr) {                                                          \
        (void)dev;                                                                                                     \
        LOG(1, "%s: 16-bit read at 0x%08X is not decoded; reading open bus", chip, addr);                              \
        return 0xFFFF;                                                                                                 \
    }                                                                                                                  \
    static uint32_t name##_read32(void *dev, uint32_t addr) {                                                          \
        (void)dev;                                                                                                     \
        LOG(1, "%s: 32-bit read at 0x%08X is not decoded; reading open bus", chip, addr);                              \
        return 0xFFFFFFFFu;                                                                                            \
    }                                                                                                                  \
    static uint16_t name##_peek16(void *dev, uint32_t addr) {                                                          \
        (void)dev, (void)addr;                                                                                         \
        return 0xFFFF;                                                                                                 \
    }                                                                                                                  \
    static uint32_t name##_peek32(void *dev, uint32_t addr) {                                                          \
        (void)dev, (void)addr;                                                                                         \
        return 0xFFFFFFFFu;                                                                                            \
    }                                                                                                                  \
    static void name##_write16(void *dev, uint32_t addr, uint16_t value) {                                             \
        (void)dev;                                                                                                     \
        LOG(1, "%s: 16-bit write at 0x%08X = 0x%04X is not decoded; dropped", chip, addr, value);                      \
    }                                                                                                                  \
    static void name##_write32(void *dev, uint32_t addr, uint32_t value) {                                             \
        (void)dev;                                                                                                     \
        LOG(1, "%s: 32-bit write at 0x%08X = 0x%08X is not decoded; dropped", chip, addr, value);                      \
    }

// Point an interface's six wide slots at MEMORY_IFACE_OPEN_BUS_WIDE(name, ...).
#define MEMORY_IFACE_SET_OPEN_BUS_WIDE(iface, name)                                                                    \
    do {                                                                                                               \
        (iface)->read_uint16 = name##_read16;                                                                          \
        (iface)->read_uint32 = name##_read32;                                                                          \
        (iface)->peek_uint16 = name##_peek16;                                                                          \
        (iface)->peek_uint32 = name##_peek32;                                                                          \
        (iface)->write_uint16 = name##_write16;                                                                        \
        (iface)->write_uint32 = name##_write32;                                                                        \
    } while (0)

// The same six slots as designated initializers, for a `static const`
// memory_interface_t: { .read_uint8 = ..., MEMORY_IFACE_OPEN_BUS_WIDE_FIELDS(name) }.
#define MEMORY_IFACE_OPEN_BUS_WIDE_FIELDS(name)                                                                        \
    .read_uint16 = name##_read16, .read_uint32 = name##_read32, .peek_uint16 = name##_peek16,                          \
    .peek_uint32 = name##_peek32, .write_uint16 = name##_write16, .write_uint32 = name##_write32

#endif // MEMORY_IFACE_SIZED_H
