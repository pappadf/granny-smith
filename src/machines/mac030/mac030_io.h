// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_io.h
// The I/O-island dispatch ENGINE: an ordered, sentinel-terminated table of
// `mac030_io_range_t` windows -- the address map expressed as DATA rather than
// a hand-written if-ladder or switch -- walked through a per-board page index.
//
// It is family-neutral.  The 68k II families (GLUE, MDU+RBV, OSS, MCU, AV)
// install their tables through mac030_io_install() and a board descriptor
// (mac030_glue_io.h); the PowerPC I/O islands (PDM's AMIC, TNT's Grand
// Central, Gossamer's Heathrow) install theirs with mac030_io_install_table().
// Each register block is declared once, with its read, peek and write side
// on the same row, so the read and write decodes cannot drift apart.  (The
// name is historical: the engine was born for the mac030 boards.)
//
// The engine is decoupled from each machine's private state struct: it reads
// everything it needs from the small mac030_io_t the machine fills at init and
// registers (or calls through) as the island's device context.

#ifndef GS_MACHINES_MAC030_IO_H
#define GS_MACHINES_MAC030_IO_H

#include "memory.h"
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

// Devices the engine routes to — the union across all families.  Used to
// index the resolved (handle, interface) pair arrays filled at bind time.  A
// family leaves the slots it lacks NULL (GLUE has no RBV/VDAC; MDU no VIA2;
// the PowerPC islands bind VIA1 and SCC only).
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
    MAC030_DEV_SCSI96, // MCU/AV: the (first) NCR 53C96, registers + pseudo-DMA
    MAC030_DEV_SCSI96_EXT, // Q900/Q950 only: the second (external-bus) 53C96
    MAC030_DEV_COUNT,
} mac030_dev_t;

// How a window maps a masked bus offset to the device's sub-register offset.
typedef enum {
    MAC030_IO_NORMAL = 0, // sub = offset - base
    MAC030_IO_MASK_A0, // sub = (offset - base) & ~1  — the 6522 ignores A0
    MAC030_IO_FIXED, // sub = read ? read_off : write_off — SCSI pseudo-DMA
    MAC030_IO_STRIDE_512, // sub = ((offset - base) >> 9) & 0x0F — the SWIM/IWM
                          // window: the chip's A0-A3 are wired to A9-A12.  A
                          // wiring fact, declared as data, exactly like
                          // MAC030_IO_MASK_A0 above.
    MAC030_IO_STRIDE_16, // sub = ((offset - base) >> 4) & 0x0F — the 53C96
                         // register window: A3-A0 wired to A7-A4.
} mac030_io_xform_t;

// One decoded I/O window.  The engine walks an ordered, sentinel-terminated
// (end == 0) table of these.  `penalty` is the per-byte-access bus penalty in
// cycles; 16/32-bit accesses decompose into bytes, so each constituent byte
// pays it (memory_io_penalty accumulation is split-invariant, so this exactly
// reproduces the former per-window penalty arithmetic).
typedef struct mac030_io_range {
    uint32_t base, end; // [base, end) within the I/O mirror
    mac030_dev_t device; // which device handles this window (when read_fn/write_fn NULL)
    uint16_t penalty; // memory_io_penalty cycles per byte access (ignored when esync set)
    mac030_io_xform_t xform; // offset transform
    uint16_t read_off, write_off; // sub-offsets for MAC030_IO_FIXED windows
    // Optional code hooks for windows a (device, offset) row can't express —
    // bus-error windows, DMA engines, stateful shift registers.  When set,
    // the engine calls them with the machine config + the FULL bus address
    // (so a handler can report the faulting address) instead of routing to a
    // device.  NULL on every GLUE/MDU row (those are pure device routes).
    // Handler rows get the window-relative sub-offset the engine has ALREADY
    // decoded, plus the raw bus address for fault reporting.
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
    // (the IIfx RPU probe and FMC windows).  There is no bus turnaround to
    // charge, so `penalty` is legitimately 0 -- and saying so here is what
    // lets mac030_io_validate insist that every OTHER window declares one.
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
} mac030_io_range_t;

// Handler-row adapters: stamp out a read_fn / write_fn named NAME around a
// chip face that takes (cfg, window offset[, value]) and has no use for the
// raw bus address -- the PowerPC islands' chip faces all look like that.
#define MAC030_IO_ROW_READ(NAME, FN)                                                                                   \
    static uint8_t NAME(struct config *cfg, uint32_t win_off, uint32_t addr) {                                         \
        (void)addr;                                                                                                    \
        return FN(cfg, win_off);                                                                                       \
    }
#define MAC030_IO_ROW_WRITE(NAME, FN)                                                                                  \
    static void NAME(struct config *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {                             \
        (void)addr;                                                                                                    \
        FN(cfg, win_off, value);                                                                                       \
    }

// The island is at most 512 KB (Heathrow's BAR0; the widest 68k
// io_mirror_mask is $0003FFFF), so 128 pages of 4 KB index all of it.
#define MAC030_IO_MAX_PAGES 128
#define MAC030_IO_NO_ROW    0xFFu

// Device handles + cached memory interfaces the engine routes to, indexed by
// mac030_dev_t.  Plus the ordered window table + mirror mask for this family.
// Filled by a family bind (mac030_glue_io_bind / mac030_mdu_io_bind /
// mac030_oss_io_bind / the PowerPC island binds); used as the I/O region's
// device context.
typedef struct mac030_io {
    void *handle[MAC030_DEV_COUNT]; // device-object pointers
    const memory_interface_t *iface[MAC030_DEV_COUNT]; // their memory interfaces
    const mac030_io_range_t *ranges; // ordered, sentinel-terminated
    uint32_t mirror_mask; // addr & mask before decode
    struct config *cfg; // for handler-row (read_fn/write_fn) dispatch
    uint8_t unmapped_read; // value returned on a no-match read (0 GLUE/MDU; 0xFF OSS)
    // The island models no bus turnaround: every window's penalty is 0 by
    // design, so mac030_io_validate does not insist on one.  The PowerPC
    // islands: their timing lives in the CPU core, not in the I/O decode.
    bool untimed;
    uint32_t window_base; // physical base of the I/O window (bus-error addresses)
    uint32_t berr_lo, berr_hi; // window-relative bus-error span (desc->io_berr_*); lo > hi = none
    // Decode-miss log-once bitmaps, one bit per 4 KB of island (128 bits, the
    // MAC030_IO_MAX_PAGES island).  Diagnostic state only, and full of host
    // pointers besides -- this whole struct is rebuilt at init and never
    // checkpointed.
    uint64_t miss_logged_read[2], miss_logged_write[2];
    // Decode index: for each 4 KB page of the island, the span of rows that
    // touch it -- first..last inclusive, MAC030_IO_NO_ROW when no row does.
    // Built by mac030_io_install_table from the table itself.  A span, not
    // a single row, because the table needs no particular order: the IIfx
    // nests a 32-byte bus-error window inside the 16 KB oss_ext window and
    // declares the narrow one FIRST so the linear walk's first-match gives
    // it priority.  Scanning the span in table order reproduces that
    // exactly, whatever the order and whatever overlaps.
    uint8_t page_first_row[MAC030_IO_MAX_PAGES];
    uint8_t page_last_row[MAC030_IO_MAX_PAGES];
    uint8_t page_count;
    bool indexed;
} mac030_io_t;

// --- The engine ------------------------------------------------------------

// Pure decode: the window in `ranges` containing `offset & mirror`, or NULL if
// unmapped.  Exposed for the address-map unit tests.  Board guards are not
// consulted (there is no machine to ask); see mac030_io_decode_indexed.
const mac030_io_range_t *mac030_io_decode(const mac030_io_range_t *ranges, uint32_t mirror, uint32_t offset);

// The same decode the dispatch path actually takes, through this board's page
// index and its board guards.  Must agree with mac030_io_decode() on every
// offset of the island for every board without guarded rows -- which is what
// the unit test sweeps.
const mac030_io_range_t *mac030_io_decode_indexed(const mac030_io_t *io, uint32_t addr);

// The dispatch entry-points (the shared engine).  `ctx` is a mac030_io_t*.
uint8_t mac030_io_read_uint8(void *ctx, uint32_t addr);
uint8_t mac030_io_peek_uint8(void *ctx, uint32_t addr);
uint16_t mac030_io_peek_uint16(void *ctx, uint32_t addr);
uint32_t mac030_io_peek_uint32(void *ctx, uint32_t addr);
uint16_t mac030_io_read_uint16(void *ctx, uint32_t addr);
uint32_t mac030_io_read_uint32(void *ctx, uint32_t addr);
void mac030_io_write_uint8(void *ctx, uint32_t addr, uint8_t value);
void mac030_io_write_uint16(void *ctx, uint32_t addr, uint16_t value);
void mac030_io_write_uint32(void *ctx, uint32_t addr, uint32_t value);

// Fill `iface` with the engine entry-points (shared by all families).
void mac030_io_fill_interface(memory_interface_t *iface);

// Install a window table, mirror mask and unmapped-read value, clear every
// device slot, and build the page index.  No bus-error span, window base 0,
// timed.  The board-descriptor form (mac030_io_install, mac030_glue_io.h)
// opens with this; the PowerPC islands call it directly.
void mac030_io_install_table(mac030_io_t *io, struct config *cfg, const mac030_io_range_t *ranges, uint32_t mirror_mask,
                             uint8_t unmapped_read);

// Bind one device slot: its handle and the memory interface that decodes it,
// together.  The two arrays are indexed by the same mac030_dev_t and are
// meaningless apart -- a handle without its interface decodes to NULL, and an
// interface without its handle dispatches on NULL -- so setting them in one
// call is what stops them drifting.  Four families were writing the pair by
// hand at twenty sites.
static inline void mac030_io_bind_dev(mac030_io_t *io, mac030_dev_t dev, void *handle,
                                      const memory_interface_t *iface) {
    io->handle[dev] = handle;
    io->iface[dev] = iface;
}

// Check that every device row in the installed table has a bound interface.
// A row naming a chip nobody built is not a crash any more (the dispatcher
// falls back to unmapped_read), which makes it silent: the machine boots and
// the window just reads $FF forever.  This is the thing that says so.  Called
// from mac030_glue_finish once a 68k machine is fully built, and from each
// PowerPC island's bind -- and it takes the engine as a parameter precisely
// so a new family cannot forget to run it.  Returns the number of unbound
// rows found, logging each.
int mac030_io_validate(const mac030_io_t *io, const char *machine_id);

#endif // GS_MACHINES_MAC030_IO_H
