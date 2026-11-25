// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// board_io.h
// The I/O-island dispatch ENGINE: an ordered, sentinel-terminated table of
// `board_io_range_t` windows -- the address map expressed as DATA rather than
// a hand-written if-ladder or switch -- walked through a per-board page index.
//
// It is family-neutral.  The 68k II families (GLUE, MDU+RBV, OSS, MCU, AV)
// install their tables through m68k_board_io_install() and a board descriptor
// (m68k_board_io.h); the PowerPC I/O islands (PDM's AMIC, TNT's Grand
// Central, Gossamer's Heathrow) install theirs with board_io_install_table().
// Each register block is declared once, with its read, peek and write side
// on the same row, so the read and write decodes cannot drift apart.
//
// The engine is decoupled from each machine's private state struct: it reads
// everything it needs from the small board_io_t the machine fills at init and
// registers (or calls through) as the island's device context.

#ifndef GS_MACHINES_RUNTIME_BOARD_IO_H
#define GS_MACHINES_RUNTIME_BOARD_IO_H

#include "memory.h"
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

// Devices the engine routes to — the union across all families.  Used to
// index the resolved (handle, interface) pair arrays filled at bind time.  A
// family leaves the slots it lacks NULL (GLUE has no RBV/VDAC; MDU no VIA2;
// the PowerPC islands bind VIA1 and SCC only).
typedef enum {
    BOARD_DEV_VIA1 = 0,
    BOARD_DEV_VIA2,
    BOARD_DEV_SCC,
    BOARD_DEV_SCSI,
    BOARD_DEV_ASC,
    BOARD_DEV_FLOPPY,
    BOARD_DEV_RBV, // MDU only (VIA2 replacement)
    BOARD_DEV_VDAC, // MDU only (Bt450 fronting the built-in RBV video)
    BOARD_DEV_SCC_IOP, // OSS only (IIfx SCC I/O processor)
    BOARD_DEV_SWIM_IOP, // OSS only (IIfx SWIM I/O processor)
    BOARD_DEV_OSS, // OSS only (the OSS interrupt/glue controller)
    BOARD_DEV_SCSI96, // MCU/AV: the (first) NCR 53C96, registers + pseudo-DMA
    BOARD_DEV_SCSI96_EXT, // Q900/Q950 only: the second (external-bus) 53C96
    BOARD_DEV_COUNT,
} board_dev_t;

// How a window maps a masked bus offset to the device's sub-register offset.
typedef enum {
    BOARD_IO_NORMAL = 0, // sub = offset - base
    BOARD_IO_MASK_A0, // sub = (offset - base) & ~1  — the 6522 ignores A0
    BOARD_IO_FIXED, // sub = read ? read_off : write_off — SCSI pseudo-DMA
    BOARD_IO_STRIDE_512, // sub = ((offset - base) >> 9) & 0x0F — the SWIM/IWM
                         // window: the chip's A0-A3 are wired to A9-A12.  A
                         // wiring fact, declared as data, exactly like
                         // BOARD_IO_MASK_A0 above.
    BOARD_IO_STRIDE_16, // sub = ((offset - base) >> 4) & 0x0F — the 53C96
                        // register window: A3-A0 wired to A7-A4.
} board_io_xform_t;

// One decoded I/O window.  The engine walks an ordered, sentinel-terminated
// (end == 0) table of these.  `penalty` is the per-byte-access bus penalty in
// cycles; 16/32-bit accesses decompose into bytes, so each constituent byte
// pays it (memory_io_penalty accumulation is split-invariant, so this exactly
// reproduces the former per-window penalty arithmetic).
typedef struct board_io_range {
    uint32_t base, end; // [base, end) within the I/O mirror
    board_dev_t device; // which device handles this window (when read_fn/write_fn NULL)
    uint16_t penalty; // memory_io_penalty cycles per byte access (ignored when esync set)
    board_io_xform_t xform; // offset transform
    uint16_t read_off, write_off; // sub-offsets for BOARD_IO_FIXED windows
    // Optional code hooks for windows a (device, offset) row can't express —
    // DMA engines, stateful shift registers, faults a row decides itself.  When set,
    // the engine calls them with the machine config, the window-relative
    // sub-offset it has ALREADY decoded, and `addr`: the access's offset
    // within the memory region the I/O interface is mapped at (not masked
    // to the island) -- a handler reporting a fault adds the board_io's
    // `window_base` to it for the bus address.  NULL on every GLUE/MDU row
    // (those are pure device routes).
    //
    // They used to get `addr` alone, so each handler re-derived the offset by
    // hand -- and the AV family's island mirror mask ($0003FFFF), declared
    // once as data in q840av.c/q660av.c, was written out six more times
    // inside psc.c and new_age.c as `(addr & 0x3FFFFu) - <window base>`.
    // Changing io_mirror_mask on a new board would silently have broken all
    // six.  Device rows never had this problem: they
    // have always been handed io_sub_offset().
    //
    // A handler row sets BOTH read_fn and write_fn: a NULL write_fn on a
    // handler row would route the write to device slot `device` (slot 0,
    // VIA1, by default).
    uint8_t (*read_fn)(struct config *cfg, uint32_t win_off, uint32_t addr);
    void (*write_fn)(struct config *cfg, uint32_t win_off, uint32_t addr, uint8_t value);
    const char *debug_name; // for the address-map unit test + tracing
    // 1 = 6522 window: charge the phase-accurate E-clock sync penalty
    // instead of the fixed `penalty` (each byte access completes at the
    // next 783.360 kHz E boundary — see memory_io_esync_penalty). Last so
    // positional initializers stay valid; flagged rows set `.esync = 1`.
    uint16_t esync;
    // 1 = this window signals a bus error instead of completing a cycle
    // (the IIfx RPU probe and FMC windows, the MEMCjr's empty communication
    // slot).  The engine raises it itself, at the full bus address
    // (window_base + addr), and a read returns `unmapped_read`; such a row
    // names no device and no handlers.  There is no bus turnaround to
    // charge, so `penalty` is legitimately 0 -- and saying so here is what
    // lets board_io_validate insist that every OTHER window declares one.
    uint16_t berr;
    // An inspection of a handler row (memory_interface_t.peek_*): the value
    // read_fn would return, with none of its side effects.  Set on every
    // row whose read_fn has one (a FIFO pop, a flag cleared on read, a bus
    // error); NULL means read_fn is pure and serves inspections too.
    uint8_t (*peek_fn)(struct config *cfg, uint32_t win_off, uint32_t addr);
    // Board guard: a window only some boards of a family populate (Grand
    // Central's GBUS blocks exist on the Network Servers only, its MESH
    // aperture on the Macintosh boards only).  When set and false the row
    // decodes as if absent and the access falls to the next matching row or
    // the unmapped value.  Asked at access time, so one table serves every
    // board of the family; NULL (every 68k row) means always present.
    bool (*present)(struct config *cfg);
} board_io_range_t;

// Handler-row adapters: stamp out a read_fn / write_fn named NAME around a
// chip face that takes (cfg, window offset[, value]) and has no use for the
// raw bus address -- the PowerPC islands' chip faces all look like that.
#define BOARD_IO_ROW_READ(NAME, FN)                                                                                    \
    static uint8_t NAME(struct config *cfg, uint32_t win_off, uint32_t addr) {                                         \
        (void)addr;                                                                                                    \
        return FN(cfg, win_off);                                                                                       \
    }
#define BOARD_IO_ROW_WRITE(NAME, FN)                                                                                   \
    static void NAME(struct config *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {                             \
        (void)addr;                                                                                                    \
        FN(cfg, win_off, value);                                                                                       \
    }

// The island is at most 512 KB (Heathrow's BAR0; the widest 68k
// io_mirror_mask is $0003FFFF), so 128 pages of 4 KB index all of it.
#define BOARD_IO_MAX_PAGES 128
#define BOARD_IO_NO_ROW    0xFFu

// Device handles + cached memory interfaces the engine routes to, indexed by
// board_dev_t.  Plus the ordered window table + mirror mask for this family.
// Filled by a family bind (glue_io_bind / mdu_io_bind /
// iifx_io_bind / the PowerPC island binds); used as the I/O region's
// device context.
typedef struct board_io {
    void *handle[BOARD_DEV_COUNT]; // device-object pointers
    const memory_interface_t *iface[BOARD_DEV_COUNT]; // their memory interfaces
    const board_io_range_t *ranges; // ordered, sentinel-terminated
    uint32_t mirror_mask; // addr & mask before decode
    struct config *cfg; // for handler-row (read_fn/write_fn) dispatch
    uint8_t unmapped_read; // value returned on a no-match read (0 GLUE/MDU; 0xFF OSS)
    // The island models no bus turnaround: every window's penalty is 0 by
    // design, so board_io_validate does not insist on one.  The PowerPC
    // islands: their timing lives in the CPU core, not in the I/O decode.
    bool untimed;
    uint32_t window_base; // physical base of the I/O window (bus-error addresses)
    uint32_t berr_lo, berr_hi; // window-relative bus-error span (desc->io_berr_*); lo > hi = none
    // Decode-miss log-once bitmaps, one bit per 4 KB of island (128 bits, the
    // BOARD_IO_MAX_PAGES island).  Diagnostic state only, and full of host
    // pointers besides -- this whole struct is rebuilt at init and never
    // checkpointed.
    uint64_t miss_logged_read[2], miss_logged_write[2];
    // Decode index: for each 4 KB page of the island, the span of rows that
    // touch it -- first..last inclusive, BOARD_IO_NO_ROW when no row does.
    // Built by board_io_install_table from the table itself.  A span, not
    // a single row, because the table needs no particular order: the IIfx
    // nests a 32-byte bus-error window inside the 16 KB oss_ext window and
    // declares the narrow one FIRST so the linear walk's first-match gives
    // it priority.  Scanning the span in table order reproduces that
    // exactly, whatever the order and whatever overlaps.
    uint8_t page_first_row[BOARD_IO_MAX_PAGES];
    uint8_t page_last_row[BOARD_IO_MAX_PAGES];
    uint8_t page_count;
    bool indexed;
    // Direct page map (board_io_mark_untimed): for each 4 KB page, the one
    // row that owns all of it and needs nothing but a call -- no guard, no
    // penalty, no esync/berr, a plain offset -- or NULL.  `direct` says the
    // map is live; the board_io_direct_* fast paths consult it before the
    // general engine.  Only the PowerPC islands opt in.
    bool direct;
    const board_io_range_t *page_row[BOARD_IO_MAX_PAGES];
} board_io_t;

// --- The engine ------------------------------------------------------------

// Pure decode: the window in `ranges` containing `offset & mirror`, or NULL if
// unmapped.  Exposed for the address-map unit tests.  Board guards are not
// consulted (there is no machine to ask); see board_io_decode_indexed.
const board_io_range_t *board_io_decode(const board_io_range_t *ranges, uint32_t mirror, uint32_t offset);

// The same decode the dispatch path actually takes, through this board's page
// index and its board guards.  Must agree with board_io_decode() on every
// offset of the island for every board without guarded rows -- which is what
// the unit test sweeps.
const board_io_range_t *board_io_decode_indexed(const board_io_t *io, uint32_t addr);

// The dispatch entry-points (the shared engine).  `ctx` is a board_io_t*.
uint8_t board_io_read_uint8(void *ctx, uint32_t addr);
uint8_t board_io_peek_uint8(void *ctx, uint32_t addr);
uint16_t board_io_peek_uint16(void *ctx, uint32_t addr);
uint32_t board_io_peek_uint32(void *ctx, uint32_t addr);
uint16_t board_io_read_uint16(void *ctx, uint32_t addr);
uint32_t board_io_read_uint32(void *ctx, uint32_t addr);
void board_io_write_uint8(void *ctx, uint32_t addr, uint8_t value);
void board_io_write_uint16(void *ctx, uint32_t addr, uint16_t value);
void board_io_write_uint32(void *ctx, uint32_t addr, uint32_t value);

// Fill `iface` with the engine entry-points (shared by all families).
void board_io_fill_interface(memory_interface_t *iface);

// Install a window table, mirror mask and unmapped-read value, clear every
// device slot, and build the page index.  No bus-error span, window base 0,
// timed.  The board-descriptor form (m68k_board_io_install, m68k_board_io.h)
// opens with this; the PowerPC islands call it directly.
void board_io_install_table(board_io_t *io, struct config *cfg, const board_io_range_t *ranges, uint32_t mirror_mask,
                            uint8_t unmapped_read);

// Declare the island untimed (no bus-turnaround model: the PowerPC islands,
// whose timing lives in the CPU core) and build its direct page map, so the
// board_io_direct_* fast paths below serve every page owned by one plain
// row.  Call after board_io_install_table; the island must have no
// bus-error span (none of the PowerPC islands do).
void board_io_mark_untimed(board_io_t *io);

// Byte fast paths for an island marked untimed: one masked page lookup and
// the row's call, which is what a hand-written switch over the island's 4 KB
// blocks costs.  Anything the map does not cover -- a page shared by several
// rows, a guarded row, an unbound device slot, an unmapped page -- takes the
// general engine, which decides it exactly as before.  `offset` is the
// island offset, as for board_io_read_uint8.
static inline uint8_t board_io_direct_read_uint8(board_io_t *io, uint32_t offset) {
    uint32_t off = offset & io->mirror_mask;
    const board_io_range_t *r = io->direct ? io->page_row[off >> 12] : NULL;
    if (r) {
        if (r->read_fn)
            return r->read_fn(io->cfg, off - r->base, offset);
        if (io->iface[r->device])
            return io->iface[r->device]->read_uint8(io->handle[r->device], off - r->base);
    }
    return board_io_read_uint8(io, offset);
}

static inline uint8_t board_io_direct_peek_uint8(board_io_t *io, uint32_t offset) {
    uint32_t off = offset & io->mirror_mask;
    const board_io_range_t *r = io->direct ? io->page_row[off >> 12] : NULL;
    if (r) {
        if (r->read_fn)
            return (r->peek_fn ? r->peek_fn : r->read_fn)(io->cfg, off - r->base, offset);
        if (io->iface[r->device])
            return memory_iface_read8(io->iface[r->device], io->handle[r->device], off - r->base, true);
    }
    return board_io_peek_uint8(io, offset);
}

static inline void board_io_direct_write_uint8(board_io_t *io, uint32_t offset, uint8_t value) {
    uint32_t off = offset & io->mirror_mask;
    const board_io_range_t *r = io->direct ? io->page_row[off >> 12] : NULL;
    if (r) {
        if (r->write_fn) {
            r->write_fn(io->cfg, off - r->base, offset, value);
            return;
        }
        if (io->iface[r->device]) {
            io->iface[r->device]->write_uint8(io->handle[r->device], off - r->base, value);
            return;
        }
    }
    board_io_write_uint8(io, offset, value);
}

// Bind one device slot: its handle and the memory interface that decodes it,
// together.  The two arrays are indexed by the same board_dev_t and are
// meaningless apart -- a handle without its interface decodes to NULL, and an
// interface without its handle dispatches on NULL -- so setting them in one
// call is what stops them drifting.  Four families were writing the pair by
// hand at twenty sites.
static inline void board_io_bind_dev(board_io_t *io, board_dev_t dev, void *handle, const memory_interface_t *iface) {
    io->handle[dev] = handle;
    io->iface[dev] = iface;
}

// Check that every device row in the installed table has a bound interface.
// A row naming a chip nobody built is not a crash any more (the dispatcher
// falls back to unmapped_read), which makes it silent: the machine boots and
// the window just reads $FF forever.  This is the thing that says so.  Called
// from m68k_board_finish once a 68k machine is fully built, and from each
// PowerPC island's bind -- and it takes the engine as a parameter precisely
// so a new family cannot forget to run it.  Returns the number of unbound
// rows found, logging each.
int board_io_validate(const board_io_t *io, const char *machine_id);

#endif // GS_MACHINES_RUNTIME_BOARD_IO_H
