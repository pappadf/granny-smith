// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue_io.h
// The mac030 II-family I/O dispatch ENGINE plus the GLUE family's tables.
//
// The engine (mac030_io_*) walks an ordered, sentinel-terminated table of
// `mac030_io_range_t` windows — the address map expressed as DATA rather than a
// hand-written if-ladder (proposal §4.2.2).  It is family-neutral: GLUE (SE/30,
// IIcx, IIx) and MDU+RBV (IIci, IIsi) both use it, each installing its own
// range table + mirror mask + device set into a `mac030_io_t` at bind time.
// This makes the address map directly unit-testable (§6.1) and kills the
// formerly cloned ~600-line dispatchers (§1.1).
//
// The engine is decoupled from each machine's private state struct: it reads
// everything it needs from the small mac030_io_t the machine fills at init and
// registers as the I/O region's device context.

#ifndef GS_MACHINES_MAC030_GLUE_IO_H
#define GS_MACHINES_MAC030_GLUE_IO_H

#include "memory.h"
#include "system_config.h"

#include <stdint.h>

// The canonical GLUE $50Fxxxxx island repeats every 128 KB; the 6522s decode
// only A9..A12, so the live decode is the low 17 bits.  (MDU uses an 18-bit
// $40000 mirror — see mdu_io.c.)
#define MAC030_GLUE_IO_MIRROR 0x0001FFFFUL

// Devices the engine routes to — the union across all mac030 families.  Used to
// index the resolved (handle, interface) pair arrays filled at bind time.  A
// family leaves the slots it lacks NULL (GLUE has no RBV/VDAC; MDU no VIA2).
typedef enum {
    MAC030_DEV_VIA1 = 0,
    MAC030_DEV_VIA2,
    MAC030_DEV_SCC,
    MAC030_DEV_SCSI,
    MAC030_DEV_ASC,
    MAC030_DEV_FLOPPY,
    MAC030_DEV_RBV, // MDU only (VIA2 replacement)
    MAC030_DEV_VDAC, // MDU only (Bt450 fronting the built-in RBV video)
    MAC030_DEV_SCC_IOP, // OSS only (IIfx SCC I/O processor)
    MAC030_DEV_SWIM_IOP, // OSS only (IIfx SWIM I/O processor)
    MAC030_DEV_OSS, // OSS only (the OSS interrupt/glue controller)
    MAC030_DEV_COUNT,
} mac030_dev_t;

// How a window maps a masked bus offset to the device's sub-register offset.
typedef enum {
    MAC030_IO_NORMAL = 0, // sub = offset - base
    MAC030_IO_MASK_A0, // sub = (offset - base) & ~1  — the 6522 ignores A0
    MAC030_IO_FIXED, // sub = read ? read_off : write_off — SCSI pseudo-DMA
} mac030_io_xform_t;

// One decoded I/O window.  The engine walks an ordered, sentinel-terminated
// (end == 0) table of these.  `penalty` is the per-byte-access bus penalty in
// cycles; 16/32-bit accesses decompose into bytes, so each constituent byte
// pays it (memory_io_penalty accumulation is split-invariant, so this exactly
// reproduces the former per-window penalty arithmetic).
typedef struct mac030_io_range {
    uint32_t base, end; // [base, end) within the I/O mirror
    mac030_dev_t device; // which device handles this window (when read_fn/write_fn NULL)
    uint16_t penalty; // memory_io_penalty cycles per byte access
    mac030_io_xform_t xform; // offset transform
    uint16_t read_off, write_off; // sub-offsets for MAC030_IO_FIXED windows
    // Optional code hooks for windows a (device, offset) row can't express —
    // bus-error windows, DMA engines, stateful shift registers (proposal
    // §4.2.2 "the few things a table can't express are code hooks").  When set,
    // the engine calls them with the machine config + the FULL bus address
    // (so a handler can report the faulting address) instead of routing to a
    // device.  NULL on every GLUE/MDU row (those are pure device routes).
    uint8_t (*read_fn)(struct config *cfg, uint32_t addr);
    void (*write_fn)(struct config *cfg, uint32_t addr, uint8_t value);
    const char *debug_name; // for the address-map unit test + tracing
} mac030_io_range_t;

// Device handles + cached memory interfaces the engine routes to, indexed by
// mac030_dev_t.  Plus the ordered window table + mirror mask for this family.
// Filled by a family bind (mac030_glue_io_bind / mac030_mdu_io_bind /
// mac030_oss_io_bind); used as the I/O region's device context.
typedef struct mac030_io {
    void *handle[MAC030_DEV_COUNT]; // device-object pointers
    const memory_interface_t *iface[MAC030_DEV_COUNT]; // their memory interfaces
    const mac030_io_range_t *ranges; // ordered, sentinel-terminated
    uint32_t mirror_mask; // addr & mask before decode
    struct config *cfg; // for handler-row (read_fn/write_fn) dispatch
    uint8_t unmapped_read; // value returned on a no-match read (0 GLUE/MDU; 0xFF OSS)
} mac030_io_t;

// Backwards-compatible alias: the GLUE state struct calls its field's type
// mac030_glue_io_t.  (The engine is family-neutral; the name is historical.)
typedef mac030_io_t mac030_glue_io_t;

// --- The engine ------------------------------------------------------------

// Pure decode: the window in `ranges` containing `offset & mirror`, or NULL if
// unmapped.  Exposed for the address-map unit tests (§6.1).
const mac030_io_range_t *mac030_io_decode(const mac030_io_range_t *ranges, uint32_t mirror, uint32_t offset);

// The six dispatch entry-points (the shared engine).  `ctx` is a mac030_io_t*.
uint8_t mac030_io_read_uint8(void *ctx, uint32_t addr);
uint16_t mac030_io_read_uint16(void *ctx, uint32_t addr);
uint32_t mac030_io_read_uint32(void *ctx, uint32_t addr);
void mac030_io_write_uint8(void *ctx, uint32_t addr, uint8_t value);
void mac030_io_write_uint16(void *ctx, uint32_t addr, uint16_t value);
void mac030_io_write_uint32(void *ctx, uint32_t addr, uint32_t value);

// Fill `iface` with the six engine entry-points (shared by all families).
void mac030_io_fill_interface(memory_interface_t *iface);

// --- The GLUE family's tables ----------------------------------------------

// Cache the GLUE device interfaces and install the GLUE window table + mirror.
// Call after rtc/scc/via/scsi/asc/floppy are up, before installing the map.
void mac030_glue_io_bind(mac030_io_t *io, config_t *cfg, void *asc, void *floppy);

// The GLUE family's ordered window table (sentinel-terminated).  Exposed for
// the address-map unit test.
const mac030_io_range_t *mac030_glue_io_ranges(void);

#endif // GS_MACHINES_MAC030_GLUE_IO_H
