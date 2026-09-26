// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue_io.c
// The mac030 II-family I/O dispatch engine + the GLUE family's dispatch
// tables.  The engine (mac030_io_*) walks the ordered window table
// in a mac030_io_t; GLUE (here) and MDU+RBV (mdu_io.c) each install their own
// table + mirror + device set.  The decode, per-window bus penalties, and IRQ
// priority are the SE/30 / IIcx logic verbatim (those were byte- and
// behaviour-identical); only the dispatch is now table-driven.

#include "mac030_glue_io.h"

#include "mac030_glue.h" // mac030_irq_route_t + MAC030_GLUE_IRQ_*

#include "asc.h"
#include "floppy.h"
#include "log.h"
#include "scc.h"
#include "scsi.h"
#include "via.h"

#include <stdbool.h>

LOG_USE_CATEGORY_NAME("setup"); // the validation diagnostic is a setup-time check

// The runtime decode-miss diagnostic belongs with the board, not with setup,
// so it takes its own category through LOG_WITH.  Registered lazily on first
// use; log_register_category is idempotent.
static const log_category_t *io_board_category(void) {
    static log_category_t *cat;
    if (!cat)
        cat = log_register_category("board");
    return cat;
}

// Log the first access to each unimplemented register, once per masked
// offset, the way every hand-written ladder already does -- amic.c:1035
// ("write of unwired island offset"), grand_central.c:550, hammerhead.c:265,
// rbv.c:299, dafb.c:473-477 (log-once with a touched[] bitmap).
//
// The five families on this shared engine were the ones saying NOTHING about
// a decode miss, and they are precisely the ones where an unimplemented
// register is most likely -- so the project's stated RE workflow ("every
// first touch is logged so the boot ROM's access sequence becomes an RE
// artefact", mcu.c:57-58) was unavailable exactly where it was most
// wanted.
//
// The bitmaps live on the instance, not in a function-level static: two
// machines in one process (the boot-matrix rows do this) each get their own
// first touches, and a new machine starts over.
static void io_log_miss_once(mac030_io_t *io, uint32_t offset, bool is_write, uint32_t value) {
    uint64_t bit = 1ull << ((offset >> 12) & 63u);
    uint64_t *seen = is_write ? &io->miss_logged_write : &io->miss_logged_read;
    if (*seen & bit)
        return;
    *seen |= bit;
    if (is_write)
        LOG_WITH(io_board_category(), 2, "I/O island: write of unwired offset $%05X = $%02X (first touch in this 4K)",
                 offset, value);
    else
        LOG_WITH(io_board_category(), 2, "I/O island: read of unwired offset $%05X -> $%02X (first touch in this 4K)",
                 offset, io->unmapped_read);
}

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
    case MAC030_IO_STRIDE_512:
        return ((offset - r->base) >> 9) & 0x0Fu;
    case MAC030_IO_NORMAL:
    default:
        return offset - r->base;
    }
}

// Decode, with the page index and a hint.
//
// Two separate cuts at the same waste:
//
//  - The page index. A byte access used to walk the table from row 0 every
//    time.  Measured over a full suite-iici run: 322,597,183 byte accesses
//    at an average of 3.91 rows each.  page_first_row[] jumps straight to
//    the first row that can contain the offset, so the walk starts where it
//    would otherwise have arrived.
//  - The hint. A 16- or 32-bit access decodes once up front and hands the
//    row to each byte instead of decoding two or four times over.  Worth
//    1.8% of the steps on its own (1,260,931,726 -> 1,238,726,246 on the
//    same run): most I/O traffic on these machines is byte-wide, so the
//    index is the half that matters.
//
// The hint is still re-validated per byte -- two compares against a pointer
// that is already hot -- because nothing forbids an access straddling a
// window edge, and a straddle must keep decoding byte by byte.
static inline const mac030_io_range_t *io_find(const mac030_io_t *io, const mac030_io_range_t *hint, uint32_t offset) {
    if (hint && offset >= hint->base && offset < hint->end)
        return hint;
    if (!io->indexed) {
        // No index for this board (see io_build_page_index): walk the whole
        // table, first match wins, exactly as the engine always did.
        for (const mac030_io_range_t *r = io->ranges; r->end; r++) {
            if (offset >= r->base && offset < r->end)
                return r;
        }
        return NULL;
    }
    uint32_t page = offset >> 12;
    if (page >= io->page_count)
        return NULL;
    unsigned first = io->page_first_row[page];
    if (first == MAC030_IO_NO_ROW)
        return NULL;
    unsigned last = io->page_last_row[page];
    // Table order within the span, so first-match still means what it meant.
    for (const mac030_io_range_t *r = io->ranges + first; first <= last; first++, r++) {
        if (offset >= r->base && offset < r->end)
            return r;
    }
    return NULL;
}

const mac030_io_range_t *mac030_io_decode_indexed(const mac030_io_t *io, uint32_t addr) {
    return io_find(io, NULL, addr & io->mirror_mask);
}

static inline uint8_t io_read_byte(mac030_io_t *io, uint32_t addr, const mac030_io_range_t *hint) {
    uint32_t offset = addr & io->mirror_mask;
    const mac030_io_range_t *r = io_find(io, hint, offset);
    if (!r) {
        io_log_miss_once(io, offset, false, 0);
        return io->unmapped_read;
    }
    if (r->esync)
        memory_io_esync_penalty(); // 6522: stall to the next E boundary
    else
        memory_io_penalty(r->penalty);
    if (r->read_fn)
        return r->read_fn(io->cfg, io_sub_offset(r, offset, true), addr);
    // A device-row whose chip this model does not build: the window is
    // decoded (we got here) but unpopulated, so the cycle is still
    // acknowledged and the bus floats — see memory_signal_bus_error.
    if (!io->iface[r->device])
        return io->unmapped_read;
    return io->iface[r->device]->read_uint8(io->handle[r->device], io_sub_offset(r, offset, true));
}

static inline void io_write_byte(mac030_io_t *io, uint32_t addr, uint8_t value, const mac030_io_range_t *hint) {
    uint32_t offset = addr & io->mirror_mask;
    const mac030_io_range_t *r = io_find(io, hint, offset);
    if (!r) {
        io_log_miss_once(io, offset, true, value);
        return;
    }
    if (r->esync)
        memory_io_esync_penalty();
    else
        memory_io_penalty(r->penalty);
    if (r->write_fn)
        r->write_fn(io->cfg, io_sub_offset(r, offset, false), addr, value);
    else if (io->iface[r->device]) // unpopulated device-row: acknowledged, dropped
        io->iface[r->device]->write_uint8(io->handle[r->device], io_sub_offset(r, offset, false), value);
}

uint8_t mac030_io_read_uint8(void *ctx, uint32_t addr) {
    mac030_io_t *io = (mac030_io_t *)ctx;
    return io_read_byte(io, addr, NULL);
}

// 16/32-bit accesses decompose into bytes.  This reproduces the former
// explicit SCSI 32-bit "blind burst" exactly: a 4-byte read of a DRQ/BLIND
// window byte-decomposes to four reads of the same fixed register, and the
// bus penalty is identical (memory_io_penalty accumulation is split-
// invariant, so 4×2 == the old single ×4).  Only the decode is hoisted.
uint16_t mac030_io_read_uint16(void *ctx, uint32_t addr) {
    mac030_io_t *io = (mac030_io_t *)ctx;
    const mac030_io_range_t *hint = io_find(io, NULL, addr & io->mirror_mask);
    return ((uint16_t)io_read_byte(io, addr, hint) << 8) | io_read_byte(io, addr + 1, hint);
}

uint32_t mac030_io_read_uint32(void *ctx, uint32_t addr) {
    mac030_io_t *io = (mac030_io_t *)ctx;
    const mac030_io_range_t *hint = io_find(io, NULL, addr & io->mirror_mask);
    uint32_t v = (uint32_t)io_read_byte(io, addr, hint) << 24;
    v |= (uint32_t)io_read_byte(io, addr + 1, hint) << 16;
    v |= (uint32_t)io_read_byte(io, addr + 2, hint) << 8;
    return v | io_read_byte(io, addr + 3, hint);
}

void mac030_io_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    mac030_io_t *io = (mac030_io_t *)ctx;
    io_write_byte(io, addr, value, NULL);
}

void mac030_io_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    mac030_io_t *io = (mac030_io_t *)ctx;
    const mac030_io_range_t *hint = io_find(io, NULL, addr & io->mirror_mask);
    io_write_byte(io, addr, (uint8_t)(value >> 8), hint);
    io_write_byte(io, addr + 1, (uint8_t)(value & 0xFF), hint);
}

void mac030_io_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    mac030_io_t *io = (mac030_io_t *)ctx;
    const mac030_io_range_t *hint = io_find(io, NULL, addr & io->mirror_mask);
    io_write_byte(io, addr, (uint8_t)(value >> 24), hint);
    io_write_byte(io, addr + 1, (uint8_t)(value >> 16), hint);
    io_write_byte(io, addr + 2, (uint8_t)(value >> 8), hint);
    io_write_byte(io, addr + 3, (uint8_t)value, hint);
}

void mac030_io_fill_interface(memory_interface_t *iface) {
    iface->read_uint8 = mac030_io_read_uint8;
    iface->read_uint16 = mac030_io_read_uint16;
    iface->read_uint32 = mac030_io_read_uint32;
    iface->write_uint8 = mac030_io_write_uint8;
    iface->write_uint16 = mac030_io_write_uint16;
    iface->write_uint32 = mac030_io_write_uint32;
}

// Names for the validation diagnostic, indexed by mac030_dev_t.
static const char *const mac030_dev_names[MAC030_DEV_COUNT] = {
    [MAC030_DEV_VIA1] = "VIA1",         [MAC030_DEV_VIA2] = "VIA2", [MAC030_DEV_SCC] = "SCC",
    [MAC030_DEV_SCSI] = "SCSI",         [MAC030_DEV_ASC] = "ASC",   [MAC030_DEV_FLOPPY] = "FLOPPY",
    [MAC030_DEV_RBV] = "RBV",           [MAC030_DEV_VDAC] = "VDAC", [MAC030_DEV_SCC_IOP] = "SCC_IOP",
    [MAC030_DEV_SWIM_IOP] = "SWIM_IOP", [MAC030_DEV_OSS] = "OSS",
};

// Build the page index from the table: for each 4 KB page of the island, the
// span of row indices that touch it.
//
// A span rather than a single row because the table needs no particular
// order.  The first cut at this required ascending, non-overlapping rows and
// rejected anything else -- and the IIfx table is neither: it nests a
// 32-byte bus-error window (rpu_probe, $1E000-$1E020) inside the 16 KB
// oss_ext window ($1C000-$20000) and declares the narrow one first, so the
// linear walk's first-match semantics carve it out.  That is a correct
// table, not a broken one, and the index has to reproduce it rather than
// refuse it.  Scanning first..last in table order does exactly that, for any
// order and any overlap.
//
// `indexed` still goes false, with the plain walk behind it, for a board the
// index cannot address at all: a mirror mask that is not 2^n-1, an island
// wider than MAC030_IO_MAX_PAGES, or more windows than a byte can name.
static void io_build_page_index(mac030_io_t *io) {
    io->indexed = false;
    io->page_count = 0;
    for (unsigned p = 0; p < MAC030_IO_MAX_PAGES; p++)
        io->page_first_row[p] = io->page_last_row[p] = MAC030_IO_NO_ROW;

    uint32_t span = io->mirror_mask + 1u;
    if (span == 0 || (io->mirror_mask & span) != 0) {
        LOG(0, "mac030 I/O: mirror mask $%08X is not 2^n-1 -- decode falls back to a linear walk", io->mirror_mask);
        return;
    }
    uint32_t pages = span >> 12;
    if (pages == 0 || pages > MAC030_IO_MAX_PAGES) {
        LOG(0, "mac030 I/O: island of %u KB needs %u index pages (max %u) -- decode falls back to a linear walk",
            span >> 10, pages, (unsigned)MAC030_IO_MAX_PAGES);
        return;
    }

    unsigned n = 0;
    for (const mac030_io_range_t *r = io->ranges; r->end; r++, n++) {
        if (n >= MAC030_IO_NO_ROW) {
            LOG(0, "mac030 I/O: more than %u windows -- decode falls back to a linear walk", MAC030_IO_NO_ROW - 1);
            return;
        }
        if (r->end <= r->base || r->end > span) {
            LOG(0,
                "Error: mac030 I/O window '%s' ($%05X-$%05X) is empty or outside the $%05X island "
                "-- the decode index is disabled and the table falls back to a linear walk",
                r->debug_name ? r->debug_name : "(unnamed)", r->base, r->end, span - 1u);
            return;
        }
        for (uint32_t pg = r->base >> 12; pg <= (r->end - 1u) >> 12; pg++) {
            if (io->page_first_row[pg] == MAC030_IO_NO_ROW)
                io->page_first_row[pg] = (uint8_t)n;
            io->page_last_row[pg] = (uint8_t)n;
        }
    }
    io->page_count = (uint8_t)pages;
    io->indexed = true;
}

void mac030_io_install(mac030_io_t *io, config_t *cfg, const struct mac030_board_desc *desc) {
    for (int i = 0; i < MAC030_DEV_COUNT; i++) {
        io->handle[i] = NULL;
        io->iface[i] = NULL;
    }
    io->ranges = desc->io_ranges;
    io->mirror_mask = desc->io_mirror_mask;
    io->cfg = cfg;
    io->unmapped_read = desc->io_unmapped_read;
    io->miss_logged_read = io->miss_logged_write = 0;
    io_build_page_index(io);
}

int mac030_io_validate(const mac030_io_t *io, const char *machine_id) {
    if (!io || !io->ranges)
        return 0;
    int unbound = 0;
    for (const mac030_io_range_t *r = io->ranges; r->end; r++) {
        // Every window that completes a bus cycle charges for it.  The
        // handler rows were the ones that did not: av.c and mcu.c left the
        // field at its zero default at twenty-odd rows, so a PSC or 53C96
        // access on a Quadra was free while an SCC access two rows above it
        // cost 2.  The IIfx table had it right all
        // along -- its scsi_dma and oss_ext handler rows charge, and only
        // its two bus-error windows do not -- which is the evidence that
        // the penalty models the island's bus turnaround, not the part.
        if (r->penalty == 0 && !r->esync && !r->berr) {
            LOG(0,
                "Error: %s I/O window '%s' ($%05X-$%05X) declares no bus penalty -- set one, mark it .esync, "
                "or mark it .berr if the cycle never completes",
                machine_id, r->debug_name ? r->debug_name : "(unnamed)", r->base, r->end);
            unbound++;
        }
        if (r->read_fn || r->write_fn) // handler-row: no device slot to bind
            continue;
        if (io->iface[r->device])
            continue;
        const char *dev =
            (r->device < MAC030_DEV_COUNT && mac030_dev_names[r->device]) ? mac030_dev_names[r->device] : "?";
        LOG(0,
            "Error: %s I/O window '%s' ($%05X-$%05X) routes to %s, which this machine never bound "
            "-- the window will read $%02X forever",
            machine_id, r->debug_name ? r->debug_name : "(unnamed)", r->base, r->end, dev, io->unmapped_read);
        unbound++;
    }
    return unbound;
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
    {0x00000, 0x02000, MAC030_DEV_VIA1, GLUE_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x02000, 0x04000, MAC030_DEV_VIA2, GLUE_VIA_IO_PENALTY, MAC030_IO_MASK_A0, 0, 0, NULL, NULL, "via2", .esync = 1},
    {0x04000, 0x06000, MAC030_DEV_SCC, GLUE_SCC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x06000, 0x08000, MAC030_DEV_SCSI, GLUE_SCSI_IO_PENALTY, MAC030_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_drq"},
    {0x10000, 0x12000, MAC030_DEV_SCSI, GLUE_SCSI_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "scsi_reg"},
    {0x12000, 0x14000, MAC030_DEV_SCSI, GLUE_SCSI_IO_PENALTY, MAC030_IO_FIXED, 0, 0x201, NULL, NULL, "scsi_blind"},
    {0x14000, 0x16000, MAC030_DEV_ASC, GLUE_ASC_IO_PENALTY, MAC030_IO_NORMAL, 0, 0, NULL, NULL, "asc"},
    {0x16000, 0x18000, MAC030_DEV_FLOPPY, GLUE_SWIM_IO_PENALTY, MAC030_IO_STRIDE_512, 0, 0, NULL, NULL, "swim"},
    {0}, // sentinel: end == 0
};

const mac030_io_range_t *mac030_glue_io_ranges(void) {
    return glue_io_ranges;
}

void mac030_glue_io_bind(mac030_io_t *io, config_t *cfg, const struct mac030_board_desc *desc, void *asc,
                         void *floppy) {
    mac030_io_install(io, cfg, desc);
    mac030_io_bind_dev(io, MAC030_DEV_VIA1, cfg->via1, via_get_memory_interface(cfg->via1));
    mac030_io_bind_dev(io, MAC030_DEV_VIA2, cfg->via2, via_get_memory_interface(cfg->via2));
    mac030_io_bind_dev(io, MAC030_DEV_SCC, cfg->scc, scc_get_memory_interface(cfg->scc));
    mac030_io_bind_dev(io, MAC030_DEV_SCSI, cfg->scsi, scsi_get_memory_interface(cfg->scsi));
    mac030_io_bind_dev(io, MAC030_DEV_ASC, asc, asc_get_memory_interface((asc_t *)asc));
    mac030_io_bind_dev(io, MAC030_DEV_FLOPPY, floppy, floppy_get_memory_interface((floppy_t *)floppy));
}

// ============================================================
// IRQ→IPL routing (the GLUE family's second dispatch table)
// ============================================================

// Ordered highest-IPL-first: NMI→7, SCC→4, VIA2→2, VIA1→1.
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
// highest-priority active source, or 0 if none.  Pure; unit-tested.
int mac030_irq_resolve_ipl(const mac030_irq_route_t *routes, uint32_t irq) {
    for (; routes->source; routes++) {
        if (irq & (uint32_t)routes->source)
            return routes->ipl;
    }
    return 0;
}
