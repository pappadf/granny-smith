// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue_io.c
// Shared GLUE-family I/O dispatcher — see mac030_glue_io.h.  Logic is the
// SE/30 / IIcx dispatcher verbatim (those were byte-identical); only the
// state access is now via mac030_glue_io_t instead of each machine's struct.

#include "mac030_glue_io.h"

#include "asc.h"
#include "floppy.h"
#include "scc.h"
#include "scsi.h"
#include "via.h"

// Mirror mask + window offsets (the canonical GLUE $50Fxxxxx island).
#define GLUE_IO_MIRROR    0x0001FFFFUL
#define IO_VIA1           0x00000
#define IO_VIA1_END       0x02000
#define IO_VIA2           0x02000
#define IO_VIA2_END       0x04000
#define IO_SCC            0x04000
#define IO_SCC_END        0x06000
#define IO_SCSI_DRQ       0x06000
#define IO_SCSI_DRQ_END   0x08000
#define IO_SCSI_REG       0x10000
#define IO_SCSI_REG_END   0x12000
#define IO_SCSI_BLIND     0x12000
#define IO_SCSI_BLIND_END 0x14000
#define IO_ASC            0x14000
#define IO_ASC_END        0x16000
#define IO_SWIM           0x16000
#define IO_SWIM_END       0x18000

// Per-window access penalties (cycles): VIA syncs to the E-clock (~16),
// everything else costs ~2 on top of the ~4 baseline.
#define GLUE_VIA_IO_PENALTY  16
#define GLUE_SCC_IO_PENALTY  2
#define GLUE_SCSI_IO_PENALTY 2
#define GLUE_ASC_IO_PENALTY  2
#define GLUE_SWIM_IO_PENALTY 2

// Cache the device interfaces for the dispatcher.
void mac030_glue_io_bind(mac030_glue_io_t *io, config_t *cfg, void *asc, void *floppy) {
    io->cfg = cfg;
    io->via1_iface = via_get_memory_interface(cfg->via1);
    io->via2_iface = via_get_memory_interface(cfg->via2);
    io->scc_iface = scc_get_memory_interface(cfg->scc);
    io->scsi_iface = scsi_get_memory_interface(cfg->scsi);
    io->asc_iface = asc_get_memory_interface((asc_t *)asc);
    io->floppy_iface = floppy_get_memory_interface((floppy_t *)floppy);
    io->asc = asc;
    io->floppy = floppy;
}

uint8_t mac030_glue_io_read_uint8(void *ctx, uint32_t addr) {
    mac030_glue_io_t *io = (mac030_glue_io_t *)ctx;
    config_t *cfg = io->cfg;
    // 6522 decodes only A9..A12 and rides lane 0, so mask A0 before handing
    // the offset to the VIA core (any byte in the 8 KB window aliases).
    uint32_t offset = addr & GLUE_IO_MIRROR;
    if (offset < IO_VIA1_END) {
        memory_io_penalty(GLUE_VIA_IO_PENALTY);
        return io->via1_iface->read_uint8(cfg->via1, (offset - IO_VIA1) & ~1u);
    }
    if (offset < IO_VIA2_END) {
        memory_io_penalty(GLUE_VIA_IO_PENALTY);
        return io->via2_iface->read_uint8(cfg->via2, (offset - IO_VIA2) & ~1u);
    }
    if (offset < IO_SCC_END) {
        memory_io_penalty(GLUE_SCC_IO_PENALTY);
        return io->scc_iface->read_uint8(cfg->scc, offset - IO_SCC);
    }
    if (offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(GLUE_SCSI_IO_PENALTY);
        return io->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(GLUE_SCSI_IO_PENALTY);
        return io->scsi_iface->read_uint8(cfg->scsi, offset - IO_SCSI_REG);
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(GLUE_SCSI_IO_PENALTY);
        return io->scsi_iface->read_uint8(cfg->scsi, 0);
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(GLUE_ASC_IO_PENALTY);
        return io->asc_iface->read_uint8(io->asc, offset - IO_ASC);
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(GLUE_SWIM_IO_PENALTY);
        return io->floppy_iface->read_uint8(io->floppy, offset - IO_SWIM);
    }
    return 0;
}

uint16_t mac030_glue_io_read_uint16(void *ctx, uint32_t addr) {
    return ((uint16_t)mac030_glue_io_read_uint8(ctx, addr) << 8) | mac030_glue_io_read_uint8(ctx, addr + 1);
}

uint32_t mac030_glue_io_read_uint32(void *ctx, uint32_t addr) {
    mac030_glue_io_t *io = (mac030_glue_io_t *)ctx;
    config_t *cfg = io->cfg;
    uint32_t offset = addr & GLUE_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(GLUE_SCSI_IO_PENALTY * 4);
        uint8_t b0 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(GLUE_SCSI_IO_PENALTY * 4);
        uint8_t b0 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = io->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
    }
    return ((uint32_t)mac030_glue_io_read_uint16(ctx, addr) << 16) | mac030_glue_io_read_uint16(ctx, addr + 2);
}

void mac030_glue_io_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    mac030_glue_io_t *io = (mac030_glue_io_t *)ctx;
    config_t *cfg = io->cfg;
    uint32_t offset = addr & GLUE_IO_MIRROR;
    if (offset < IO_VIA1_END) {
        memory_io_penalty(GLUE_VIA_IO_PENALTY);
        io->via1_iface->write_uint8(cfg->via1, (offset - IO_VIA1) & ~1u, value);
        return;
    }
    if (offset < IO_VIA2_END) {
        memory_io_penalty(GLUE_VIA_IO_PENALTY);
        io->via2_iface->write_uint8(cfg->via2, (offset - IO_VIA2) & ~1u, value);
        return;
    }
    if (offset < IO_SCC_END) {
        memory_io_penalty(GLUE_SCC_IO_PENALTY);
        io->scc_iface->write_uint8(cfg->scc, offset - IO_SCC, value);
        return;
    }
    if (offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(GLUE_SCSI_IO_PENALTY);
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(GLUE_SCSI_IO_PENALTY);
        io->scsi_iface->write_uint8(cfg->scsi, offset - IO_SCSI_REG, value);
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(GLUE_SCSI_IO_PENALTY);
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(GLUE_ASC_IO_PENALTY);
        io->asc_iface->write_uint8(io->asc, offset - IO_ASC, value);
        return;
    }
    if (offset >= IO_SWIM && offset < IO_SWIM_END) {
        memory_io_penalty(GLUE_SWIM_IO_PENALTY);
        io->floppy_iface->write_uint8(io->floppy, offset - IO_SWIM, value);
        return;
    }
}

void mac030_glue_io_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    mac030_glue_io_write_uint8(ctx, addr, (uint8_t)(value >> 8));
    mac030_glue_io_write_uint8(ctx, addr + 1, (uint8_t)(value & 0xFF));
}

void mac030_glue_io_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    mac030_glue_io_t *io = (mac030_glue_io_t *)ctx;
    config_t *cfg = io->cfg;
    uint32_t offset = addr & GLUE_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ && offset < IO_SCSI_DRQ_END) {
        memory_io_penalty(GLUE_SCSI_IO_PENALTY * 4);
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    if (offset >= IO_SCSI_BLIND && offset < IO_SCSI_BLIND_END) {
        memory_io_penalty(GLUE_SCSI_IO_PENALTY * 4);
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        io->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value));
        return;
    }
    mac030_glue_io_write_uint16(ctx, addr, (uint16_t)(value >> 16));
    mac030_glue_io_write_uint16(ctx, addr + 2, (uint16_t)(value & 0xFFFF));
}

// Fill an interface struct with the six dispatch entry-points.
void mac030_glue_io_fill_interface(memory_interface_t *iface) {
    iface->read_uint8 = mac030_glue_io_read_uint8;
    iface->read_uint16 = mac030_glue_io_read_uint16;
    iface->read_uint32 = mac030_glue_io_read_uint32;
    iface->write_uint8 = mac030_glue_io_write_uint8;
    iface->write_uint16 = mac030_glue_io_write_uint16;
    iface->write_uint32 = mac030_glue_io_write_uint32;
}
