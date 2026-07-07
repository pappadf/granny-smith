// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue_io.c
// The mac030 II-family I/O dispatch engine + the GLUE family's dispatch tables
// (proposal §4.2.2).  The engine (mac030_io_*) walks the ordered window table
// in a mac030_io_t; GLUE (here) and MDU+RBV (mdu_io.c) each install their own
// table + mirror + device set.  The decode, per-window bus penalties, and IRQ
// priority are the SE/30 / IIcx logic verbatim (those were byte- and
// behaviour-identical); only the dispatch is now table-driven.

#include "mac030_glue_io.h"

#include "mac030_glue.h" // mac030_irq_route_t + MAC030_GLUE_IRQ_*

#include "asc.h"
#include "floppy.h"
#include "scc.h"
#include "scsi.h"
#include "via.h"

#include <stdbool.h>

// ============================================================
// The engine
// ============================================================

const mac030_io_range_t *mac030_io_decode(const mac030_io_range_t *ranges, uint32_t mirror, uint32_t offset) {
    offset &= mirror;
    for (const mac030_io_range_t *r = ranges; r->end; r++) {
        if (offset >= r->base && offset < r->end)
            return r;
    }
    return NULL;
}

// The device sub-register offset a window maps `offset` to, for read vs write.
static inline uint32_t io_sub_offset(const mac030_io_range_t *r, uint32_t offset, bool is_read) {
    switch (r->xform) {
    case MAC030_IO_MASK_A0:
        return (offset - r->base) & ~1u;
    case MAC030_IO_FIXED:
        return is_read ? r->read_off : r->write_off;
    case MAC030_IO_NORMAL:
    default:
        return offset - r->base;
    }
}

uint8_t mac030_io_read_uint8(void *ctx, uint32_t addr) {
    mac030_io_t *io = (mac030_io_t *)ctx;
    uint32_t offset = addr & io->mirror_mask;
    for (const mac030_io_range_t *r = io->ranges; r->end; r++) {
        if (offset >= r->base && offset < r->end) {
            memory_io_penalty(r->penalty);
            if (r->read_fn)
                return r->read_fn(io->cfg, addr);
            return io->iface[r->device]->read_uint8(io->handle[r->device], io_sub_offset(r, offset, true));
        }
    }
    return io->unmapped_read;
}

uint16_t mac030_io_read_uint16(void *ctx, uint32_t addr) {
    return ((uint16_t)mac030_io_read_uint8(ctx, addr) << 8) | mac030_io_read_uint8(ctx, addr + 1);
}

uint32_t mac030_io_read_uint32(void *ctx, uint32_t addr) {
    // 16/32-bit accesses decompose into bytes.  This reproduces the former
    // explicit SCSI 32-bit "blind burst" exactly: a 4-byte read of a DRQ/BLIND
    // window byte-decomposes to four reads of the same fixed register, and the
    // bus penalty is identical (memory_io_penalty accumulation is split-
    // invariant, so 4×2 == the old single ×4).
    return ((uint32_t)mac030_io_read_uint16(ctx, addr) << 16) | mac030_io_read_uint16(ctx, addr + 2);
}

void mac030_io_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    mac030_io_t *io = (mac030_io_t *)ctx;
    uint32_t offset = addr & io->mirror_mask;
    for (const mac030_io_range_t *r = io->ranges; r->end; r++) {
        if (offset >= r->base && offset < r->end) {
            memory_io_penalty(r->penalty);
            if (r->write_fn)
                r->write_fn(io->cfg, addr, value);
            else
                io->iface[r->device]->write_uint8(io->handle[r->device], io_sub_offset(r, offset, false), value);
            return;
        }
    }
}

void mac030_io_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    mac030_io_write_uint8(ctx, addr, (uint8_t)(value >> 8));
    mac030_io_write_uint8(ctx, addr + 1, (uint8_t)(value & 0xFF));
}

void mac030_io_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    mac030_io_write_uint16(ctx, addr, (uint16_t)(value >> 16));
    mac030_io_write_uint16(ctx, addr + 2, (uint16_t)(value & 0xFFFF));
}

void mac030_io_fill_interface(memory_interface_t *iface) {
    iface->read_uint8 = mac030_io_read_uint8;
    iface->read_uint16 = mac030_io_read_uint16;
    iface->read_uint32 = mac030_io_read_uint32;
    iface->write_uint8 = mac030_io_write_uint8;
    iface->write_uint16 = mac030_io_write_uint16;
    iface->write_uint32 = mac030_io_write_uint32;
}

// ============================================================
// GLUE family tables
// ============================================================

// Per-window access penalties (cycles): VIA syncs to the E-clock (~16),
// everything else costs ~2 on top of the ~4 baseline.
#define GLUE_VIA_IO_PENALTY  16
#define GLUE_SCC_IO_PENALTY  2
#define GLUE_SCSI_IO_PENALTY 2
#define GLUE_ASC_IO_PENALTY  2
#define GLUE_SWIM_IO_PENALTY 2

// The canonical GLUE $50Fxxxxx decode, expressed as data.  Windows are
// half-open [base, end); the gap [$08000,$10000) and anything past $18000 are
// unmapped (read 0 / write ignored).  SCSI {DRQ,BLIND} are the pseudo-DMA
// "blind" registers: every access hits one fixed NCR5380 register — reads pop
// the FIFO at reg 0, writes push at reg $201 — regardless of the byte offset
// within the window (MAC030_IO_FIXED).  The two VIA windows mask A0 because the
// 6522 rides lane 0 and ignores it (MAC030_IO_MASK_A0).
//
//   base     end      device            penalty               xform               rd  wr     name
const mac030_io_range_t glue_io_ranges[] = {
    {0x00000, 0x02000, MAC030_DEV_VIA1, GLUE_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via1"},
    {0x02000, 0x04000, MAC030_DEV_VIA2, GLUE_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via2"},
    {0x04000, 0x06000, MAC030_DEV_SCC, GLUE_SCC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x06000, 0x08000, MAC030_DEV_SCSI, GLUE_SCSI_IO_PENALTY, MAC030_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_drq"},
    {0x10000, 0x12000, MAC030_DEV_SCSI, GLUE_SCSI_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "scsi_reg"},
    {0x12000, 0x14000, MAC030_DEV_SCSI, GLUE_SCSI_IO_PENALTY, MAC030_IO_FIXED, 0, 0x201 | SCSI_BLIND_SEL, NULL, NULL,
     "scsi_blind"},
    {0x14000, 0x16000, MAC030_DEV_ASC, GLUE_ASC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "asc"},
    {0x16000, 0x18000, MAC030_DEV_FLOPPY, GLUE_SWIM_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "swim"},
    {0}, // sentinel: end == 0
};

const mac030_io_range_t *mac030_glue_io_ranges(void) {
    return glue_io_ranges;
}

void mac030_glue_io_bind(mac030_io_t *io, config_t *cfg, const struct mac030_board_desc *desc, void *asc,
                         void *floppy) {
    for (int i = 0; i < MAC030_DEV_COUNT; i++) {
        io->handle[i] = NULL;
        io->iface[i] = NULL;
    }
    io->handle[MAC030_DEV_VIA1] = cfg->via1;
    io->handle[MAC030_DEV_VIA2] = cfg->via2;
    io->handle[MAC030_DEV_SCC] = cfg->scc;
    io->handle[MAC030_DEV_SCSI] = cfg->scsi;
    io->handle[MAC030_DEV_ASC] = asc;
    io->handle[MAC030_DEV_FLOPPY] = floppy;

    io->iface[MAC030_DEV_VIA1] = via_get_memory_interface(cfg->via1);
    io->iface[MAC030_DEV_VIA2] = via_get_memory_interface(cfg->via2);
    io->iface[MAC030_DEV_SCC] = scc_get_memory_interface(cfg->scc);
    io->iface[MAC030_DEV_SCSI] = scsi_get_memory_interface(cfg->scsi);
    io->iface[MAC030_DEV_ASC] = asc_get_memory_interface((asc_t *)asc);
    io->iface[MAC030_DEV_FLOPPY] = floppy_get_memory_interface((floppy_t *)floppy);

    io->ranges = desc->io_ranges;
    io->mirror_mask = desc->io_mirror_mask;
    io->cfg = cfg;
    io->unmapped_read = desc->io_unmapped_read;
}

// ============================================================
// IRQ→IPL routing (the GLUE family's second dispatch table)
// ============================================================

// Ordered highest-IPL-first: NMI→7, SCC→4, VIA2→2, VIA1→1 (proposal §4.2.2).
static const mac030_irq_route_t glue_irq_routes[] = {
    {MAC030_GLUE_IRQ_NMI,  7},
    {MAC030_GLUE_IRQ_SCC,  4},
    {MAC030_GLUE_IRQ_VIA2, 2},
    {MAC030_GLUE_IRQ_VIA1, 1},
    {0,                    0}, // sentinel
};

const mac030_irq_route_t *mac030_glue_irq_routes(void) {
    return glue_irq_routes;
}

// Walk an ordered (high→low IPL) routing table; return the IPL of the
// highest-priority active source, or 0 if none.  Pure; unit-tested (§6.1).
int mac030_irq_resolve_ipl(const mac030_irq_route_t *routes, uint32_t irq) {
    for (; routes->source; routes++) {
        if (irq & (uint32_t)routes->source)
            return routes->ipl;
    }
    return 0;
}
