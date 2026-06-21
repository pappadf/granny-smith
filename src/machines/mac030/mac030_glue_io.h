// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue_io.h
// The GLUE-family I/O dispatcher, shared by SE/30, IIcx and IIx.  These three
// decode the canonical $50Fxxxxx I/O island identically (VIA1 / VIA2 / SCC /
// SCSI {DRQ,REG,BLIND} / ASC / SWIM, 17-bit $20000 mirror, VIA penalty 16 and
// 2 elsewhere) and their dispatchers were byte-for-byte identical bar the
// state-struct accessor (proposal §1.1, "I/O dispatcher (~600)").
//
// The dispatcher is decoupled from each machine's private state struct: it
// reads everything it needs from a small mac030_glue_io_t the machine fills
// at init and registers as the I/O region's device context.

#ifndef GS_MACHINES_MAC030_GLUE_IO_H
#define GS_MACHINES_MAC030_GLUE_IO_H

#include "memory.h"
#include "system_config.h"

#include <stdint.h>

// Device handles + cached memory interfaces the GLUE dispatcher routes to.
// Filled by mac030_glue_io_bind(); used as the I/O region's device context.
typedef struct mac030_glue_io {
    config_t *cfg; // for cfg->via1 / via2 / scc / scsi
    const memory_interface_t *via1_iface;
    const memory_interface_t *via2_iface;
    const memory_interface_t *scc_iface;
    const memory_interface_t *scsi_iface;
    const memory_interface_t *asc_iface;
    const memory_interface_t *floppy_iface;
    void *asc; // asc_t*
    void *floppy; // floppy_t*
} mac030_glue_io_t;

// Cache the device interfaces from the (already constructed) peripherals.
// Call after rtc/scc/via/scsi/asc/floppy are up, before installing the map.
void mac030_glue_io_bind(mac030_glue_io_t *io, config_t *cfg, void *asc, void *floppy);

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
