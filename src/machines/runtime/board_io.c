// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// board_io.c
// The I/O-island dispatch engine (board_io.h): decode through the page
// index, per-window bus penalties, handler and device rows, board guards,
// and the table validation.  The tables themselves live with their families
// (glue_io.c, mdu_io.c, iifx.c, mcu.c, av.c, and the PowerPC islands
// amic.c, grand_central.c, heathrow.c).

#include "board_io.h"

#include "log.h"

#include <stdbool.h>

LOG_USE_CATEGORY_NAME("system"); // the validation diagnostic is a setup-time check

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
// offset, the way every hand-written ladder already does -- hammerhead.c:265,
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
static void io_log_miss_once(board_io_t *io, uint32_t offset, bool is_write, uint32_t value) {
    unsigned page = (offset >> 12) & (BOARD_IO_MAX_PAGES - 1u);
    uint64_t bit = 1ull << (page & 63u);
    uint64_t *seen = is_write ? &io->miss_logged_write[page >> 6] : &io->miss_logged_read[page >> 6];
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

const board_io_range_t *board_io_decode(const board_io_range_t *ranges, uint32_t mirror, uint32_t offset) {
    offset &= mirror;
    for (const board_io_range_t *r = ranges; r->end; r++) {
        if (offset >= r->base && offset < r->end)
            return r;
    }
    return NULL;
}

// The device sub-register offset a window maps `offset` to, for read vs write.
static inline uint32_t io_sub_offset(const board_io_range_t *r, uint32_t offset, bool is_read) {
    switch (r->xform) {
    case BOARD_IO_MASK_A0:
        return (offset - r->base) & ~1u;
    case BOARD_IO_FIXED:
        return is_read ? r->read_off : r->write_off;
    case BOARD_IO_STRIDE_512:
        return ((offset - r->base) >> 9) & 0x0Fu;
    case BOARD_IO_STRIDE_16:
        return ((offset - r->base) >> 4) & 0x0Fu;
    case BOARD_IO_NORMAL:
    default:
        return offset - r->base;
    }
}

// Does `r` contain `offset` on this board?  The guard is asked only once the
// range has matched, and only on the few rows that carry one.
static inline bool io_row_hits(const board_io_t *io, const board_io_range_t *r, uint32_t offset) {
    return offset >= r->base && offset < r->end && (!r->present || r->present(io->cfg));
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
// window edge, and a straddle must keep decoding byte by byte.  (A hint has
// already passed its board guard, which does not change within an access.)
static inline const board_io_range_t *io_find(const board_io_t *io, const board_io_range_t *hint, uint32_t offset) {
    if (hint && offset >= hint->base && offset < hint->end)
        return hint;
    if (!io->indexed) {
        // No index for this board (see io_build_page_index): walk the whole
        // table, first match wins, exactly as the engine always did.
        for (const board_io_range_t *r = io->ranges; r->end; r++) {
            if (io_row_hits(io, r, offset))
                return r;
        }
        return NULL;
    }
    uint32_t page = offset >> 12;
    if (page >= io->page_count)
        return NULL;
    unsigned first = io->page_first_row[page];
    if (first == BOARD_IO_NO_ROW)
        return NULL;
    unsigned last = io->page_last_row[page];
    // Table order within the span, so first-match still means what it meant.
    for (const board_io_range_t *r = io->ranges + first; first <= last; first++, r++) {
        if (io_row_hits(io, r, offset))
            return r;
    }
    return NULL;
}

const board_io_range_t *board_io_decode_indexed(const board_io_t *io, uint32_t addr) {
    return io_find(io, NULL, addr & io->mirror_mask);
}

// True when `addr` (window-relative, before the mirror) lies in the board's
// undecoded span: nothing answers there, so the cycle times out.
static inline bool io_in_berr(const board_io_t *io, uint32_t addr) {
    return addr >= io->berr_lo && addr <= io->berr_hi;
}

static inline uint8_t io_read_byte(board_io_t *io, uint32_t addr, const board_io_range_t *hint) {
    if (io_in_berr(io, addr)) {
        memory_signal_bus_error(io->window_base + addr, false);
        return io->unmapped_read;
    }
    uint32_t offset = addr & io->mirror_mask;
    const board_io_range_t *r = io_find(io, hint, offset);
    if (!r) {
        io_log_miss_once(io, offset, false, 0);
        return io->unmapped_read;
    }
    if (r->berr) {
        // Nothing answers: the cycle aborts at the full bus address.
        memory_signal_bus_error(io->window_base + addr, false);
        return io->unmapped_read;
    }
    if (r->esync)
        memory_io_esync_penalty(); // 6522: stall to the next E boundary
    else if (r->penalty)
        memory_io_penalty(r->penalty); // (a zero penalty charges nothing; skip its divide)
    if (r->read_fn)
        return r->read_fn(io->cfg, io_sub_offset(r, offset, true), addr);
    // A device-row whose chip this model does not build: the window is
    // decoded (we got here) but unpopulated, so the cycle is still
    // acknowledged and the bus floats — see memory_signal_bus_error.
    if (!io->iface[r->device])
        return io->unmapped_read;
    return io->iface[r->device]->read_uint8(io->handle[r->device], io_sub_offset(r, offset, true));
}

// An inspection of one byte: what io_read_byte would return, without its bus
// penalty, its first-touch log, or the device's read side effects.
static inline uint8_t io_peek_byte(board_io_t *io, uint32_t addr, const board_io_range_t *hint) {
    uint32_t offset = addr & io->mirror_mask;
    const board_io_range_t *r = io_find(io, hint, offset);
    if (!r || r->berr) // a bus-error window: what the faulting read returns, without the fault
        return io->unmapped_read;
    if (r->read_fn)
        return (r->peek_fn ? r->peek_fn : r->read_fn)(io->cfg, io_sub_offset(r, offset, true), addr);
    if (!io->iface[r->device])
        return io->unmapped_read;
    return memory_iface_read8(io->iface[r->device], io->handle[r->device], io_sub_offset(r, offset, true), true);
}

static inline void io_write_byte(board_io_t *io, uint32_t addr, uint8_t value, const board_io_range_t *hint) {
    if (io_in_berr(io, addr)) {
        memory_signal_bus_error(io->window_base + addr, true);
        return;
    }
    uint32_t offset = addr & io->mirror_mask;
    const board_io_range_t *r = io_find(io, hint, offset);
    if (!r) {
        io_log_miss_once(io, offset, true, value);
        return;
    }
    if (r->berr) {
        memory_signal_bus_error(io->window_base + addr, true);
        return;
    }
    if (r->esync)
        memory_io_esync_penalty();
    else if (r->penalty)
        memory_io_penalty(r->penalty);
    if (r->write_fn)
        r->write_fn(io->cfg, io_sub_offset(r, offset, false), addr, value);
    else if (io->iface[r->device]) // unpopulated device-row: acknowledged, dropped
        io->iface[r->device]->write_uint8(io->handle[r->device], io_sub_offset(r, offset, false), value);
}

uint8_t board_io_read_uint8(void *ctx, uint32_t addr) {
    board_io_t *io = (board_io_t *)ctx;
    return io_read_byte(io, addr, NULL);
}

// 16/32-bit accesses decompose into bytes.  This reproduces the former
// explicit SCSI 32-bit "blind burst" exactly: a 4-byte read of a DRQ/BLIND
// window byte-decomposes to four reads of the same fixed register, and the
// bus penalty is identical (memory_io_penalty accumulation is split-
// invariant, so 4×2 == the old single ×4).  Only the decode is hoisted.
uint16_t board_io_read_uint16(void *ctx, uint32_t addr) {
    board_io_t *io = (board_io_t *)ctx;
    const board_io_range_t *hint = io_find(io, NULL, addr & io->mirror_mask);
    return ((uint16_t)io_read_byte(io, addr, hint) << 8) | io_read_byte(io, addr + 1, hint);
}

uint32_t board_io_read_uint32(void *ctx, uint32_t addr) {
    board_io_t *io = (board_io_t *)ctx;
    const board_io_range_t *hint = io_find(io, NULL, addr & io->mirror_mask);
    uint32_t v = (uint32_t)io_read_byte(io, addr, hint) << 24;
    v |= (uint32_t)io_read_byte(io, addr + 1, hint) << 16;
    v |= (uint32_t)io_read_byte(io, addr + 2, hint) << 8;
    return v | io_read_byte(io, addr + 3, hint);
}

uint8_t board_io_peek_uint8(void *ctx, uint32_t addr) {
    return io_peek_byte((board_io_t *)ctx, addr, NULL);
}

// Wide peeks decompose into bytes exactly as the reads do.
uint16_t board_io_peek_uint16(void *ctx, uint32_t addr) {
    board_io_t *io = (board_io_t *)ctx;
    const board_io_range_t *hint = io_find(io, NULL, addr & io->mirror_mask);
    return ((uint16_t)io_peek_byte(io, addr, hint) << 8) | io_peek_byte(io, addr + 1, hint);
}

uint32_t board_io_peek_uint32(void *ctx, uint32_t addr) {
    board_io_t *io = (board_io_t *)ctx;
    const board_io_range_t *hint = io_find(io, NULL, addr & io->mirror_mask);
    uint32_t v = (uint32_t)io_peek_byte(io, addr, hint) << 24;
    v |= (uint32_t)io_peek_byte(io, addr + 1, hint) << 16;
    v |= (uint32_t)io_peek_byte(io, addr + 2, hint) << 8;
    return v | io_peek_byte(io, addr + 3, hint);
}

void board_io_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    board_io_t *io = (board_io_t *)ctx;
    io_write_byte(io, addr, value, NULL);
}

void board_io_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    board_io_t *io = (board_io_t *)ctx;
    const board_io_range_t *hint = io_find(io, NULL, addr & io->mirror_mask);
    io_write_byte(io, addr, (uint8_t)(value >> 8), hint);
    io_write_byte(io, addr + 1, (uint8_t)(value & 0xFF), hint);
}

void board_io_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    board_io_t *io = (board_io_t *)ctx;
    const board_io_range_t *hint = io_find(io, NULL, addr & io->mirror_mask);
    io_write_byte(io, addr, (uint8_t)(value >> 24), hint);
    io_write_byte(io, addr + 1, (uint8_t)(value >> 16), hint);
    io_write_byte(io, addr + 2, (uint8_t)(value >> 8), hint);
    io_write_byte(io, addr + 3, (uint8_t)value, hint);
}

void board_io_fill_interface(memory_interface_t *iface) {
    iface->read_uint8 = board_io_read_uint8;
    iface->read_uint16 = board_io_read_uint16;
    iface->read_uint32 = board_io_read_uint32;
    iface->write_uint8 = board_io_write_uint8;
    iface->write_uint16 = board_io_write_uint16;
    iface->write_uint32 = board_io_write_uint32;
    iface->peek_uint8 = board_io_peek_uint8;
    iface->peek_uint16 = board_io_peek_uint16;
    iface->peek_uint32 = board_io_peek_uint32;
}

// Names for the validation diagnostic, indexed by board_dev_t.
static const char *const board_dev_names[BOARD_DEV_COUNT] = {
    [BOARD_DEV_VIA1] = "VIA1",
    [BOARD_DEV_VIA2] = "VIA2",
    [BOARD_DEV_SCC] = "SCC",
    [BOARD_DEV_SCSI] = "SCSI",
    [BOARD_DEV_ASC] = "ASC",
    [BOARD_DEV_FLOPPY] = "FLOPPY",
    [BOARD_DEV_RBV] = "RBV",
    [BOARD_DEV_VDAC] = "VDAC",
    [BOARD_DEV_SCC_IOP] = "SCC_IOP",
    [BOARD_DEV_SWIM_IOP] = "SWIM_IOP",
    [BOARD_DEV_OSS] = "OSS",
    [BOARD_DEV_SCSI96] = "SCSI96",
    [BOARD_DEV_SCSI96_EXT] = "SCSI96_EXT",
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
// wider than BOARD_IO_MAX_PAGES, or more windows than a byte can name.
static void io_build_page_index(board_io_t *io) {
    io->indexed = false;
    io->page_count = 0;
    for (unsigned p = 0; p < BOARD_IO_MAX_PAGES; p++)
        io->page_first_row[p] = io->page_last_row[p] = BOARD_IO_NO_ROW;

    uint32_t span = io->mirror_mask + 1u;
    if (span == 0 || (io->mirror_mask & span) != 0) {
        LOG(0, "board I/O: mirror mask $%08X is not 2^n-1 -- decode falls back to a linear walk", io->mirror_mask);
        return;
    }
    uint32_t pages = span >> 12;
    if (pages == 0 || pages > BOARD_IO_MAX_PAGES) {
        LOG(0, "board I/O: island of %u KB needs %u index pages (max %u) -- decode falls back to a linear walk",
            span >> 10, pages, (unsigned)BOARD_IO_MAX_PAGES);
        return;
    }

    unsigned n = 0;
    for (const board_io_range_t *r = io->ranges; r->end; r++, n++) {
        if (n >= BOARD_IO_NO_ROW) {
            LOG(0, "board I/O: more than %u windows -- decode falls back to a linear walk", BOARD_IO_NO_ROW - 1);
            return;
        }
        if (r->end <= r->base || r->end > span) {
            LOG(0,
                "Error: board I/O window '%s' ($%05X-$%05X) is empty or outside the $%05X island "
                "-- the decode index is disabled and the table falls back to a linear walk",
                r->debug_name ? r->debug_name : "(unnamed)", r->base, r->end, span - 1u);
            return;
        }
        for (uint32_t pg = r->base >> 12; pg <= (r->end - 1u) >> 12; pg++) {
            if (io->page_first_row[pg] == BOARD_IO_NO_ROW)
                io->page_first_row[pg] = (uint8_t)n;
            io->page_last_row[pg] = (uint8_t)n;
        }
    }
    io->page_count = (uint8_t)pages;
    io->indexed = true;
}

void board_io_install_table(board_io_t *io, config_t *cfg, const board_io_range_t *ranges, uint32_t mirror_mask,
                            uint8_t unmapped_read) {
    for (int i = 0; i < BOARD_DEV_COUNT; i++) {
        io->handle[i] = NULL;
        io->iface[i] = NULL;
    }
    io->ranges = ranges;
    io->mirror_mask = mirror_mask;
    io->cfg = cfg;
    io->unmapped_read = unmapped_read;
    io->untimed = false;
    io->direct = false;
    io->window_base = 0;
    io->berr_lo = 1; // lo > hi: no bus-error span
    io->berr_hi = 0;
    for (unsigned w = 0; w < 2; w++)
        io->miss_logged_read[w] = io->miss_logged_write[w] = 0;
    io_build_page_index(io);
}

// A page goes in the direct map only when the general engine would do
// nothing for it but the row's call: the page index names exactly one row
// for it, that row covers the whole page, and the row has no guard, penalty,
// esync, bus error or offset transform.  Every other page stays NULL and
// keeps the full decode.
void board_io_mark_untimed(board_io_t *io) {
    io->untimed = true;
    io->direct = false;
    for (unsigned pg = 0; pg < BOARD_IO_MAX_PAGES; pg++)
        io->page_row[pg] = NULL;
    if (!io->indexed || io->berr_lo <= io->berr_hi)
        return;
    for (unsigned pg = 0; pg < io->page_count; pg++) {
        unsigned first = io->page_first_row[pg];
        if (first == BOARD_IO_NO_ROW || first != io->page_last_row[pg])
            continue;
        const board_io_range_t *r = &io->ranges[first];
        if (r->base > (pg << 12) || r->end < ((pg + 1u) << 12))
            continue;
        if (r->present || r->penalty || r->esync || r->berr || r->xform != BOARD_IO_NORMAL)
            continue;
        if (!r->read_fn != !r->write_fn) // a half handler row: leave it to the engine
            continue;
        io->page_row[pg] = r;
    }
    io->direct = true;
}

int board_io_validate(const board_io_t *io, const char *machine_id) {
    if (!io || !io->ranges)
        return 0;
    int unbound = 0;
    for (const board_io_range_t *r = io->ranges; r->end; r++) {
        // Every window that completes a bus cycle charges for it.  The
        // handler rows were the ones that did not: av.c and mcu.c left the
        // field at its zero default at twenty-odd rows, so a PSC or 53C96
        // access on a Quadra was free while an SCC access two rows above it
        // cost 2.  The IIfx table had it right all
        // along -- its scsi_dma and oss_ext handler rows charge, and only
        // its two bus-error windows do not -- which is the evidence that
        // the penalty models the island's bus turnaround, not the part.
        if (!io->untimed && r->penalty == 0 && !r->esync && !r->berr) {
            LOG(0,
                "Error: %s I/O window '%s' ($%05X-$%05X) declares no bus penalty -- set one, mark it .esync, "
                "or mark it .berr if the cycle never completes",
                machine_id, r->debug_name ? r->debug_name : "(unnamed)", r->base, r->end);
            unbound++;
        }
        // A handler row decodes both directions itself; half a handler row
        // would send the other direction to device slot `device`.
        if ((r->read_fn != NULL) != (r->write_fn != NULL)) {
            LOG(0, "Error: %s I/O window '%s' ($%05X-$%05X) sets only one of read_fn / write_fn", machine_id,
                r->debug_name ? r->debug_name : "(unnamed)", r->base, r->end);
            unbound++;
            continue;
        }
        if (r->read_fn || r->berr) // handler-row or bus-error window: no device slot to bind
            continue;
        if (io->iface[r->device])
            continue;
        const char *dev =
            (r->device < BOARD_DEV_COUNT && board_dev_names[r->device]) ? board_dev_names[r->device] : "?";
        LOG(0,
            "Error: %s I/O window '%s' ($%05X-$%05X) routes to %s, which this machine never bound "
            "-- the window will read $%02X forever",
            machine_id, r->debug_name ? r->debug_name : "(unnamed)", r->base, r->end, dev, io->unmapped_read);
        unbound++;
    }
    return unbound;
}
