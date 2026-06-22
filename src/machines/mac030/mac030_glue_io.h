// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue_io.h
// The GLUE-family I/O dispatcher, shared by SE/30, IIcx and IIx.  These three
// decode the canonical $50Fxxxxx I/O island identically (VIA1 / VIA2 / SCC /
// SCSI {DRQ,REG,BLIND} / ASC / SWIM, 17-bit $20000 mirror, VIA penalty 16 and
// 2 elsewhere) and their dispatchers were byte-for-byte identical bar the
// state-struct accessor (proposal §1.1, "I/O dispatcher (~600)").
//
// The decode is expressed as DATA — an ordered, sentinel-terminated table of
// `mac030_io_range_t` windows walked by a generic engine — rather than a
// hand-written if-ladder (proposal §4.2.2).  This makes the address map
// directly unit-testable (§6.1) and is the same engine the MDU family reuses
// with its own table.
//
// The dispatcher is decoupled from each machine's private state struct: it
// reads everything it needs from a small mac030_glue_io_t the machine fills
// at init and registers as the I/O region's device context.

#ifndef GS_MACHINES_MAC030_GLUE_IO_H
#define GS_MACHINES_MAC030_GLUE_IO_H

#include "memory.h"
#include "system_config.h"

#include <stdint.h>

// The canonical GLUE $50Fxxxxx island repeats every 128 KB; the 6522s decode
// only A9..A12, so the live decode is the low 17 bits.
#define MAC030_GLUE_IO_MIRROR 0x0001FFFFUL

// Devices the GLUE dispatcher routes to.  Used to index the resolved
// (handle, interface) pair arrays filled by mac030_glue_io_bind().
typedef enum {
    MAC030_DEV_VIA1 = 0,
    MAC030_DEV_VIA2,
    MAC030_DEV_SCC,
    MAC030_DEV_SCSI,
    MAC030_DEV_ASC,
    MAC030_DEV_FLOPPY,
    MAC030_DEV_COUNT,
} mac030_dev_t;

// How a window maps a masked bus offset to the device's sub-register offset.
typedef enum {
    MAC030_IO_NORMAL = 0, // sub = offset - base
    MAC030_IO_MASK_A0, // sub = (offset - base) & ~1  — the 6522 ignores A0
    MAC030_IO_FIXED, // sub = read ? read_off : write_off — SCSI pseudo-DMA
} mac030_io_xform_t;

// One decoded I/O window.  The dispatcher walks an ordered, sentinel-terminated
// (end == 0) table of these.  `penalty` is the per-byte-access bus penalty in
// cycles; 16/32-bit accesses decompose into bytes, so each constituent byte
// pays it (memory_io_penalty accumulation is split-invariant, so this exactly
// reproduces the former per-window penalty arithmetic).
typedef struct mac030_io_range {
    uint32_t base, end; // [base, end) within the I/O mirror
    mac030_dev_t device; // which device handles this window
    uint16_t penalty; // memory_io_penalty cycles per byte access
    mac030_io_xform_t xform; // offset transform
    uint16_t read_off, write_off; // sub-offsets for MAC030_IO_FIXED windows
    const char *debug_name; // for the address-map unit test + tracing
} mac030_io_range_t;

// Device handles + cached memory interfaces the GLUE dispatcher routes to,
// indexed by mac030_dev_t.  Plus the ordered window table for this family.
// Filled by mac030_glue_io_bind(); used as the I/O region's device context.
typedef struct mac030_glue_io {
    void *handle[MAC030_DEV_COUNT]; // device-object pointers
    const memory_interface_t *iface[MAC030_DEV_COUNT]; // their memory interfaces
    const mac030_io_range_t *ranges; // ordered, sentinel-terminated
} mac030_glue_io_t;

// Cache the device interfaces from the (already constructed) peripherals and
// install the GLUE family window table.  Call after rtc/scc/via/scsi/asc/floppy
// are up, before installing the map.
void mac030_glue_io_bind(mac030_glue_io_t *io, config_t *cfg, void *asc, void *floppy);

// Pure decode: the window containing `offset` (already masked to the mirror by
// the caller, or not — this masks internally), or NULL if unmapped.  Exposed
// for the address-map unit test (§6.1).
const mac030_io_range_t *mac030_glue_io_decode(uint32_t offset);

// The GLUE family's ordered window table (sentinel-terminated).  Exposed for
// the address-map unit test.
const mac030_io_range_t *mac030_glue_io_ranges(void);

// The six dispatch entry-points.  `ctx` is a mac030_glue_io_t*.
uint8_t mac030_glue_io_read_uint8(void *ctx, uint32_t addr);
uint16_t mac030_glue_io_read_uint16(void *ctx, uint32_t addr);
uint32_t mac030_glue_io_read_uint32(void *ctx, uint32_t addr);
void mac030_glue_io_write_uint8(void *ctx, uint32_t addr, uint8_t value);
void mac030_glue_io_write_uint16(void *ctx, uint32_t addr, uint16_t value);
void mac030_glue_io_write_uint32(void *ctx, uint32_t addr, uint32_t value);

// Fill `iface` with the six dispatch function pointers (convenience).
void mac030_glue_io_fill_interface(memory_interface_t *iface);

#endif // GS_MACHINES_MAC030_GLUE_IO_H
