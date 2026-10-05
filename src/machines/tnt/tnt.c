// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// tnt.c
// The TNT family substrate (Power Macintosh 7500/8500/9500) — the second
// PowerPC family and the first PCI machine.  See tnt.h.
//
// Memory model (Apple, "Power Macintosh 7500 and 8500 Computers"
// Developer Note, 1995; the shipping ROM's Open Firmware device tree):
//   $00000000-RAM top   main DRAM, contiguous from 0 (Hammerhead-banked
//                       on hardware; Open Firmware sizes it and the tree
//                       is authoritative afterwards)
//   $80000000-$8FFFFFFF Bandit 1 PCI memory space — empty: recoverable
//                       transfer error (bandit.c)
//   $90000000-$9FFFFFFF Chaos/VCI memory space — likewise
//   $F0000000-$F1FFFFFF Chaos bridge + display-bus device space
//   $F2000000-$F2FFFFFF Bandit 1 bridge (config ports at +$800000/+$C00000)
//   $F3000000-$F3FFFFFF Bandit 1 PCI I/O window — Grand Central decodes
//                       the 128 KB at its base (grand_central.c)
//   $F4000000-$F5FFFFFF Bandit 2 (8500/9500 only)
//   $F8000000           Hammerhead register window (hammerhead.c)
//   $FFC00000-$FFFFFFFF the 4 MB ROM (601/604 reset vector $FFF00100 =
//                       image + $300100, the NanoKernel reset entry)
//
// Everything else is decoded by nobody and reads zero; the boot path is
// not expected to touch it (Open Firmware probes only what its drivers
// know, under fault catchers that the claimed windows provide).

#include "tnt.h"
#include "config_seed.h"

#include "cuda.h" // the shared behavioral Cuda model (machines/av/)
#include "dbdma.h"

#include "adb.h"
#include "appletalk.h"
#include "debug.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "mac_host_io.h"
#include "machine_checkpoint.h"
#include "machine_teardown.h" // the shared config_t-owned delete chain
#include "of_nvram.h"
#include "pci.h"
#include "ppc.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "slot_tables.h"
#include "swim3.h"
#include "sym53c8xx.h" // the fast/wide controllers the ANS slot table seats
#include "via.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

// ============================================================
// Page-table helpers (the pdm_fill_page shape, kept local so the TNT
// family does not pull 68K-family headers)
// ============================================================

void tnt_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    if (page_index >= (uint32_t)g_page_count)
        return;
    g_page_table[page_index].host_base = host_ptr;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = writable;
    uint32_t guest_base = page_index << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;
    // Supervisor arrays hold the eager physical identity view; the USER
    // arrays belong to the PPC MMU front end (logical fills, ppc_mmu.c)
    // and are only ever cleared here.
    if (g_supervisor_read)
        g_supervisor_read[page_index] = adjusted;
    if (g_supervisor_write) // write entry refused on a predecoded code page (memory.h)
        g_supervisor_write[page_index] = writable ? memory_write_fill(page_index, host_ptr, adjusted) : 0;
    if (g_user_read)
        g_user_read[page_index] = 0;
    if (g_user_write)
        g_user_write[page_index] = 0;
    memory_logpoint_guard_page(page_index);
}

void tnt_clear_page(uint32_t page_index) {
    if (page_index >= (uint32_t)g_page_count)
        return;
    g_page_table[page_index].host_base = NULL;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = false;
    if (g_supervisor_read)
        g_supervisor_read[page_index] = 0;
    if (g_supervisor_write)
        g_supervisor_write[page_index] = 0;
    if (g_user_read)
        g_user_read[page_index] = 0;
    if (g_user_write)
        g_user_write[page_index] = 0;
}

// ============================================================
// Grand Central island interface ($F3000000, 128 KB)
// ============================================================
// The island mixes byte-wide cells ($10/$200 centres) with 32-bit
// little-endian registers; grand_central.c dispatches by block.  16-bit
// access is not a natural size for anything in the chip — decompose into
// bytes, big-endian, matching what the bus would deliver.

//
// The island is PCI pass-through memory behind Bandit 1, mapped directly
// here for speed — so Bandit 1's byte-lane mode (bandit.c, pci.h) has to
// be applied at this edge exactly as the bus windows apply it: with the
// lanes reversed an N-byte access at offset o is the straight access at
// o ^ (8-N) with its bytes reversed.

// Is Bandit 1 reversing its lanes right now?
static inline bool gc_lanes_reversed(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    return st && st->gc_bus && pci_bus_lane_reverse(st->gc_bus);
}

static uint8_t gc_read8(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    if (gc_lanes_reversed(cfg))
        offset ^= 7u;
    return tnt_gc_read8(cfg, offset);
}

static uint8_t gc_peek8(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    if (gc_lanes_reversed(cfg))
        offset ^= 7u;
    return tnt_gc_peek8(cfg, offset);
}

static uint16_t gc_peek16(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    bool rev = gc_lanes_reversed(cfg);
    if (rev)
        offset ^= 6u;
    uint16_t v = (uint16_t)((tnt_gc_peek8(cfg, offset) << 8) | tnt_gc_peek8(cfg, offset + 1));
    return rev ? __builtin_bswap16(v) : v;
}

static uint32_t gc_peek32(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    if (!gc_lanes_reversed(cfg))
        return tnt_gc_peek32(cfg, offset);
    return __builtin_bswap32(tnt_gc_peek32(cfg, offset ^ 4u));
}

static void gc_write8(void *ctx, uint32_t offset, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    if (gc_lanes_reversed(cfg))
        offset ^= 7u;
    tnt_gc_write8(cfg, offset, value);
}

static uint16_t gc_read16(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    bool rev = gc_lanes_reversed(cfg);
    if (rev)
        offset ^= 6u;
    uint16_t v = (uint16_t)((tnt_gc_read8(cfg, offset) << 8) | tnt_gc_read8(cfg, offset + 1));
    return rev ? __builtin_bswap16(v) : v;
}

static void gc_write16(void *ctx, uint32_t offset, uint16_t value) {
    config_t *cfg = (config_t *)ctx;
    if (gc_lanes_reversed(cfg)) {
        offset ^= 6u;
        value = __builtin_bswap16(value);
    }
    tnt_gc_write8(cfg, offset, (uint8_t)(value >> 8));
    tnt_gc_write8(cfg, offset + 1, (uint8_t)value);
}

static uint32_t gc_read32(void *ctx, uint32_t offset) {
    config_t *cfg = (config_t *)ctx;
    if (!gc_lanes_reversed(cfg))
        return tnt_gc_read32(cfg, offset);
    return __builtin_bswap32(tnt_gc_read32(cfg, offset ^ 4u));
}

static void gc_write32(void *ctx, uint32_t offset, uint32_t value) {
    config_t *cfg = (config_t *)ctx;
    if (!gc_lanes_reversed(cfg))
        tnt_gc_write32(cfg, offset, value);
    else
        tnt_gc_write32(cfg, offset ^ 4u, __builtin_bswap32(value));
}

// ============================================================
// Hammerhead window interface ($F8000000)
// ============================================================
// Byte-wide model with big-endian decomposition for wider access — the
// one block on the machine that is NOT little-endian (processor bus).

static uint8_t hh_read8(void *ctx, uint32_t offset) {
    return tnt_hh_read((config_t *)ctx, offset);
}

static void hh_write8(void *ctx, uint32_t offset, uint8_t value) {
    tnt_hh_write((config_t *)ctx, offset, value);
}

static uint16_t hh_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((hh_read8(ctx, offset) << 8) | hh_read8(ctx, offset + 1));
}

static uint32_t hh_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)hh_read16(ctx, offset) << 16) | hh_read16(ctx, offset + 2);
}

static void hh_write16(void *ctx, uint32_t offset, uint16_t value) {
    hh_write8(ctx, offset, (uint8_t)(value >> 8));
    hh_write8(ctx, offset + 1, (uint8_t)value);
}

static void hh_write32(void *ctx, uint32_t offset, uint32_t value) {
    hh_write16(ctx, offset, (uint16_t)(value >> 16));
    hh_write16(ctx, offset + 2, (uint16_t)value);
}

// ============================================================
// Chaos-window probe logging (see tnt_memory_layout)
// ============================================================

static uint32_t chaos_probe(void *ctx, uint32_t offset, bool write, unsigned width, uint32_t value) {
    (void)ctx;
    LOG(1, "Chaos window %s%u $%08X%s%s$%08X", write ? "write" : "read", width * 8, TNT_CHAOS_BASE + offset,
        write ? " = " : "", write ? "" : " -> ", write ? value : 0);
    return 0;
}

static uint8_t chaos_probe_read8(void *ctx, uint32_t offset) {
    return (uint8_t)chaos_probe(ctx, offset, false, 1, 0);
}
static uint16_t chaos_probe_read16(void *ctx, uint32_t offset) {
    return (uint16_t)chaos_probe(ctx, offset, false, 2, 0);
}
static uint32_t chaos_probe_read32(void *ctx, uint32_t offset) {
    return chaos_probe(ctx, offset, false, 4, 0);
}
static void chaos_probe_write8(void *ctx, uint32_t offset, uint8_t value) {
    chaos_probe(ctx, offset, true, 1, value);
}
static void chaos_probe_write16(void *ctx, uint32_t offset, uint16_t value) {
    chaos_probe(ctx, offset, true, 2, value);
}
static void chaos_probe_write32(void *ctx, uint32_t offset, uint32_t value) {
    chaos_probe(ctx, offset, true, 4, value);
}

// ============================================================
// Memory layout
// ============================================================

static void tnt_memory_layout(config_t *cfg, checkpoint_t *cp) {
    tnt_state_t *st = tnt_st(cfg);

    // RAM: wherever the Hammerhead's bank base registers put the DIMMs
    // (hammerhead.c).  POST sizes them and re-bases them contiguously
    // from 0; Open Firmware then publishes /memory's reg -- the tree is
    // the contract.
    tnt_hh_remap(cfg);

    // ROM: 4 MB at $FFC00000 (direct read-only pages).
    uint8_t *rom = ram_native_pointer(cfg->mem_map, cfg->ram_size);
    for (uint32_t p = 0; p < (cfg->machine->rom_size >> PAGE_SHIFT); p++)
        tnt_fill_page((TNT_ROM_BASE >> PAGE_SHIFT) + p, rom + (p << PAGE_SHIFT), false);

    // Grand Central: the 128 KB island.
    st->gc_interface.read_uint8 = gc_read8;
    st->gc_interface.read_uint16 = gc_read16;
    st->gc_interface.read_uint32 = gc_read32;
    st->gc_interface.write_uint8 = gc_write8;
    st->gc_interface.write_uint16 = gc_write16;
    st->gc_interface.write_uint32 = gc_write32;
    st->gc_interface.peek_uint8 = gc_peek8;
    st->gc_interface.peek_uint16 = gc_peek16;
    st->gc_interface.peek_uint32 = gc_peek32;
    memory_map_add(cfg->mem_map, TNT_GC_BASE, TNT_GC_ISLAND_SIZE, "Grand Central", &st->gc_interface, cfg);

    // Hammerhead: the register window (page granularity is ours; the file
    // answers $000..$7FF and logs above it).
    st->hh_interface.read_uint8 = hh_read8;
    st->hh_interface.read_uint16 = hh_read16;
    st->hh_interface.read_uint32 = hh_read32;
    st->hh_interface.write_uint8 = hh_write8;
    st->hh_interface.write_uint16 = hh_write16;
    st->hh_interface.write_uint32 = hh_write32;
    memory_map_add(cfg->mem_map, TNT_HH_BASE, MEM_PAGE_SIZE, "Hammerhead", &st->hh_interface, cfg);

    // PCI: the root first (the bridges create their buses on it), then the
    // bridges themselves — config ports, per-bridge bus, each bridge's own
    // device-11 header and the PCI memory windows.
    cfg->pci = pci_root_create(cfg);
    pci_init(cfg->pci, cfg->machine->pci_slots);
    tnt_bandit_init(cfg, cp);

    // The rest of the Chaos display-bus window, logged open bus until the
    // Control model claims its apertures: reads 0, and every access is
    // recorded so the firmware's probe sequence can be fitted (the R1
    // Hammerhead method).
    st->chaos_probe_interface.read_uint8 = chaos_probe_read8;
    st->chaos_probe_interface.read_uint16 = chaos_probe_read16;
    st->chaos_probe_interface.read_uint32 = chaos_probe_read32;
    st->chaos_probe_interface.write_uint8 = chaos_probe_write8;
    st->chaos_probe_interface.write_uint16 = chaos_probe_write16;
    st->chaos_probe_interface.write_uint32 = chaos_probe_write32;
    memory_map_add(cfg->mem_map, TNT_CHAOS_BASE, TNT_PCI_CFG_ADDR, "Chaos window", &st->chaos_probe_interface, cfg);
    memory_map_add(cfg->mem_map, TNT_CHAOS_BASE + 0x01000000u, 0x01000000u, "Chaos window hi",
                   &st->chaos_probe_interface, cfg);
}

// ============================================================
// DBDMA hooks — guest-physical movers + the channel interrupt line
// ============================================================
// Bus-master DMA: RAM is moved through the host backing store directly
// (descriptors and data buffers live there); anything outside RAM (a
// STORE_QUAD/LOAD_QUAD aimed at a device register) goes through the
// bus's slow path byte by byte.  The CPU MMU is deliberately not in the
// path (the sonic/psc memory-hook precedent).

// The DBDMA port: RAM by memcpy, everything else through the bus.  NOT
// dma_mem_port_physical, which resolves host memory only and reads device
// space as zero (dma_mem.h).
//
// The engine is a bus master behind Bandit 1, so its traffic crosses the
// same reversed lanes as the CPU's when the bridge is in little-endian
// mode: PCI byte n of the transfer is host byte n^7.  The RAM fast path
// stays a memcpy in the straight case; the reversed case walks bytes (the
// XOR never leaves the aligned 8-byte group, so a block inside RAM stays
// inside RAM).
static void tnt_dbdma_mem_read(void *ctx, uint32_t phys, uint8_t *buf, uint32_t len) {
    config_t *cfg = (config_t *)ctx;
    bool rev = gc_lanes_reversed(cfg);
    if (phys < cfg->ram_size && len <= cfg->ram_size - phys) {
        const uint8_t *ram = ram_native_pointer(cfg->mem_map, 0);
        if (!rev)
            memcpy(buf, ram + phys, len);
        else
            for (uint32_t i = 0; i < len; i++)
                buf[i] = ram[(phys + i) ^ 7u];
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        buf[i] = memory_read_uint8_slow(rev ? ((phys + i) ^ 7u) : (phys + i));
}

static void tnt_dbdma_mem_write(void *ctx, uint32_t phys, const uint8_t *buf, uint32_t len) {
    config_t *cfg = (config_t *)ctx;
    bool rev = gc_lanes_reversed(cfg);
    if (phys < cfg->ram_size && len <= cfg->ram_size - phys) {
        uint8_t *ram = ram_native_pointer(cfg->mem_map, 0);
        if (!rev) {
            memory_host_written(ram + phys, len); // bus-master DMA over cached code
            memcpy(ram + phys, buf, len);
        } else {
            // Reversed lanes keep each byte within its aligned doubleword.
            uint32_t lo = phys & ~7u;
            memory_host_written(ram + lo, ((phys + len + 7u) & ~7u) - lo);
            for (uint32_t i = 0; i < len; i++)
                ram[(phys + i) ^ 7u] = buf[i];
        }
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        memory_write_uint8_slow(rev ? ((phys + i) ^ 7u) : (phys + i), buf[i]);
}

// Channel completion -> Grand Central interrupt n (== channel n), an
// edge event into the fabric.
// A DBDMA channel interrupt is a LEVEL, not a pulse: the channel holds
// its request asserted — visible in Grand Central's Levels register —
// until the host acknowledges it through the interrupt-clear register.
// That is the only way Mac OS can see one at all: the NanoKernel's
// ExtIntHandlerTNT classifies from Levels & Mask and never reads Events,
// so a completion raised as a one-shot event reached the 68k side never.
// The shipping sound driver shows the contract from the other end: it
// acknowledges channel 8 (clear $100) before it starts a program, and
// its completion handler acknowledges again — with a pulse model those
// acknowledges cleared nothing, the handler never ran after the first
// program parked, and every sound after the firmware beep was silence.
static void tnt_dbdma_irq(void *ctx, int chan) {
    tnt_gc_set_source((config_t *)ctx, chan, true);
}

// ============================================================
// VIA1 callbacks — Cuda transport (the PDM/AV pattern, third instance)
// ============================================================

// Port B carries the Cuda handshake (PB3 TREQ in, PB4 BYTEACK out, PB5 TIP
// out — the classic Cuda bit positions); the SR shift-out is a command byte.
static void tnt_via1_output(void *context, uint8_t port, uint8_t value) {
    tnt_state_t *st = tnt_st((config_t *)context);
    av_cuda_via1_port_output(st ? st->cuda : NULL, port, value);
}

static void tnt_via1_shift_out(void *context, uint8_t byte) {
    tnt_state_t *st = tnt_st((config_t *)context);
    av_cuda_via1_shift_input(st ? st->cuda : NULL, byte);
}

// VIA1 aggregate IRQ -> Grand Central interrupt 18 (level-sensitive; the
// NanoKernel classifies it to 68k IPL 1).
static void tnt_via1_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (tnt_st(cfg))
        tnt_gc_set_source(cfg, TNT_INT_VIA1, active);
}

// SCC chip INT (one line for both channels) -> Grand Central interrupts 15
// (ch A) and 16 (ch B) together; the guest discriminates channels via
// RR2B/RR3 exactly as on every other Mac.  Both classify to 68k IPL 4.
static void tnt_scc_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (!tnt_st(cfg))
        return;
    tnt_gc_set_source(cfg, TNT_INT_SCCA, active);
    tnt_gc_set_source(cfg, TNT_INT_SCCB, active);
}

// 53C94 /IRQ -> Grand Central interrupt 12 (level; 68k IPL 2).
// MESH's interrupt line goes to Grand Central's MESH source; its DBDMA data
// phases go to channel 10.  Both used to be reached from inside the model.
static void tnt_mesh_irq(void *ctx, bool level) {
    tnt_gc_set_source((config_t *)ctx, TNT_INT_MESH, level);
}

static void tnt_mesh_dbdma_kick(void *ctx) {
    config_t *cfg = (config_t *)ctx;
    dbdma_kick(tnt_st(cfg)->dbdma, 10);
}

static void tnt_scsi96_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    if (tnt_st(cfg))
        tnt_gc_set_source(cfg, TNT_INT_SCSI0, active);
}

// DBDMA channel-0 device port for the 53C94: the classic DREQ-gated
// pseudo-DMA byte stream.  With no bus attached the chip never raises
// DREQ, so the port is exercised only when the external chain gains
// devices (CD-ROM phase) — at which point a DREQ-edge kick will be
// wired alongside.
static int tnt_scsi0_port_in(void *ctx, uint8_t *buf, int len) {
    tnt_state_t *st = tnt_st((config_t *)ctx);
    int n = 0;
    while (n < len && scsi_53c96_dreq(st->scsi96))
        buf[n++] = scsi_53c96_pdma_read8(st->scsi96);
    return n;
}

static int tnt_scsi0_port_out(void *ctx, const uint8_t *buf, int len) {
    tnt_state_t *st = tnt_st((config_t *)ctx);
    int n = 0;
    while (n < len && scsi_53c96_dreq(st->scsi96))
        scsi_53c96_pdma_write8(st->scsi96, buf[n++]);
    return n;
}

static void tnt_scsi0_port_init(config_t *cfg) {
    dbdma_port_t port = {
        .out = tnt_scsi0_port_out,
        .in = tnt_scsi0_port_in,
        .s_bits = NULL,
        .ctx = cfg,
    };
    dbdma_set_port(tnt_st(cfg)->dbdma, 0, &port);
}

// Hand each fast/wide controller the bus it drives.  The two 53C825As are
// PCI cards the slot table seated (slots 8 and 9 in the Network Server
// profiles), so this runs after pci_seat_slots and after both bus objects
// exist.  Channel 0 gets cfg->scsi — the bus every existing consumer knows
// — and channel 1 gets the second one.  A Macintosh board seats neither
// card and this does nothing.
static void tnt_fwscsi_attach(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    struct scsi *bus[2] = {cfg->scsi, st->scsi2};
    for (const pci_slot_decl_t *d = cfg->machine->pci_slots; d && d->slot; d++) {
        sym53c8xx_t *chip = sym53c8xx_from_device(pci_slot_device(cfg->pci, d->slot));
        if (!chip)
            continue;
        if (chip->channel < 0 || chip->channel > 1) {
            LOG(0, "53C825A in slot %d declares channel %d, which no bus serves", d->slot, chip->channel);
            continue;
        }
        sym53c8xx_attach_bus(chip, bus[chip->channel]);
        LOG(1, "fast/wide channel %d bound to %s", chip->channel, chip->channel ? "machine.scsi2" : "machine.scsi");
    }
}

// ============================================================
// Substrate lifecycle
// ============================================================

// The NVRAM part is NON-VOLATILE: an 8 KB store whose content survives
// power cycles, and it does so here for the hardware's own reason --
// machine.restart (the power switch) and machine.reset never destroy the
// machine, so the part is simply never touched.  A new machine
// (machine.boot) gets a new part; nothing carries a store
// across a teardown.  That rule is the fix for #112: the store used to ride
// a process-lifetime holder across every teardown, and a run stopped part-
// way through Open Firmware's format of a blank store left it torn.
//
// A new Macintosh board's part is not blank: it holds the store its own
// firmware formats (of_nvram.h) -- a valid Open Firmware partition and the
// ROM's parameter RAM defaults -- so the firmware skips its format pass and
// the FIRST boot can match a boot driver (XPRAM $77 "Default OS" = 1) and
// reach the startup volume.  On a blank store it could not: the format
// clears the PRAM partition, and only the second boot found a disk.
//
// The Network Server still starts blank.  Its firmware versions and their
// defaults are not characterised, and its POST caches the DIMM sizing in
// the store; rows that need the second boot (suite-ans's ans500-diag-floppy)
// take it with machine.restart.  A checkpoint restore loads the store from
// the gc blob.
static void tnt_nvram_factory(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (!st)
        return;
    if (tnt_board(cfg)->kind == TNT_BOARD_MAC)
        of_nvram_factory(st->gc.nvram, &of_nvram_defaults_tnt);
    else
        memset(st->gc.nvram, 0, TNT_NVRAM_SIZE);
}

// Clear the non-volatile store -- what pulling the battery does: the store
// goes back to what a new board carries (above).
//
// Apple, Network Server Hardware Developer Notes, §2.7: "Removal of a
// battery from the Main Logic Board will reset all parameter and NVRAM to
// default values."  It is the machine's own documented way back to a virgin
// configuration, and it is a real need rather than a test convenience: the
// ROM caches its DIMM sizing and its Open Firmware environment in there, so
// a store written by one model is not necessarily meaningful to another.
void tnt_nvram_clear(config_t *cfg) {
    tnt_nvram_factory(cfg);
    LOG(1, "NVRAM cleared (battery removed)");
}

// Checkpoint parts of the TNT board's own (machine_parts.h).
static void part_save_dbdma(void *obj, checkpoint_t *cp) {
    dbdma_checkpoint(obj, cp);
}

static void part_save_mesh(void *obj, checkpoint_t *cp) {
    mesh_checkpoint(obj, cp);
}

// Hammerhead, Grand Central and the bridges' mode registers.
static void part_save_tnt_board(void *obj, checkpoint_t *cp) {
    tnt_state_t *st = obj;
    system_write_checkpoint_data(cp, &st->hh, sizeof(st->hh));
    system_write_checkpoint_data(cp, &st->gc, sizeof(st->gc));
    for (int i = 0; i < st->bridge_count; i++) {
        system_write_checkpoint_data(cp, &st->bridge[i].cfg_addr, sizeof(st->bridge[i].cfg_addr));
        system_write_checkpoint_data(cp, &st->bridge[i].mode_select, sizeof(st->bridge[i].mode_select));
    }
}

// AWACS and Control, with Control's VRAM where the board has Control.
static void part_save_tnt_av(void *obj, checkpoint_t *cp) {
    tnt_state_t *st = obj;
    system_write_checkpoint_data(cp, &st->awacs, sizeof(st->awacs));
    system_write_checkpoint_data(cp, &st->control, sizeof(st->control));
    if (st->vram)
        system_write_checkpoint_data(cp, st->vram, TNT_VRAM_SIZE);
}

// The GBUS island (Network Servers only; zeroed and unread elsewhere), and
// the floppy controller with its DBDMA byte ring.
static void part_save_tnt_io(void *obj, checkpoint_t *cp) {
    tnt_state_t *st = obj;
    system_write_checkpoint_data(cp, &st->gbus, sizeof(st->gbus));
    system_write_checkpoint_data(cp, &st->lcd, sizeof(st->lcd));
    // offsetof, not sizeof: swim3_t's tail is `struct floppy *fd; struct
    // scheduler *sched; swim3_backend_t be;` and swim3.h labels it "not
    // checkpointed; swim3_bind".  Writing the whole struct put host pointers
    // in a user-shareable save file, and made two saves of the same guest
    // state differ -- which defeats any diff-based checkpoint testing.  The
    // restore re-binds through *_swim3_bind either way, so the values were
    // harmless; the leak and the non-reproducibility were not.
    system_write_checkpoint_data(cp, &st->swim3, offsetof(swim3_t, fd));
    system_write_checkpoint_data(cp, &st->fdring, sizeof(st->fdring));
}

static int tnt_init(config_t *cfg, checkpoint_t *cp) {
    tnt_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;
    tnt_nvram_factory(cfg); // a checkpoint below restores over it

    // Core: memory map, the 601/604 per profile, the scheduler on the PPC
    // seam.  CPI 1.0 — the same determinism-and-measurement rationale as
    // PDM; whether any TNT guest code times itself against the TB and
    // cares is a ladder observable.
    machine_part_begin(cfg, cp, "memory");
    cfg->mem_map =
        memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, MEMORY_BUS_ERR_NONE,
                        &cfg->build_opts.rom, cp); // no bus-error watchdog: unanswered floats to $FF
    machine_part(cfg, cp, "memory", part_save_memory, cfg->mem_map);
    // No 68k MMU owns this machine's page table; host-backed regions that
    // core code registers on the bus map are filled through our filler.
    memory_map_set_host_fill(cfg->mem_map, tnt_fill_page);
    const int cpu_model = cfg->machine->cpu_model;
    machine_part_begin(cfg, cp, "cpu");
    cfg->ppc = ppc_init(cp, cpu_model);
    if (cfg->ppc) {
        memory_cpu_hooks_t hooks = ppc_memory_hooks(cfg->ppc);
        memory_map_set_cpu_hooks(cfg->mem_map, &hooks);
    }
    if (!cfg->ppc) {
        LOG(0, "Error: out of memory constructing the PowerPC core");
        return -1;
    }
    machine_part(cfg, cp, "cpu", part_save_ppc, cfg->ppc);
    sched_cpu_if_t cpu_if = ppc_sched_if(cfg->ppc);
    machine_part_begin(cfg, cp, "scheduler");
    cfg->scheduler = scheduler_init(&cpu_if, cp);
    machine_part(cfg, cp, "scheduler", part_save_scheduler, cfg->scheduler);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    // CPI 2: a real 601/604 under Mac OS sustains well under one
    // instruction per clock (cache misses, the 68k emulator's dispatch);
    // CPI 1 over-modeled the chip and demanded 100+ host MIPS to pace
    // real time — beyond what the wasm build delivers, which surfaced as
    // stretched guest time and a jumpy, accelerated-step mouse.
    scheduler_set_cpi(cfg->scheduler, 2);
    // Time: the 601's RTC input keeps the PDM 7.8336 MHz assumption until
    // ladder rung T2 proves otherwise; the 604's timebase/DEC tick at a
    // quarter of the bus clock (Motorola, MPC604UM/AD, §1.3.2.2).
    uint32_t tick_hz = (cpu_model == CPU_MODEL_PPC601) ? 7833600u : tnt_board(cfg)->bus_hz / 4u;
    ppc_bind_time(cfg->ppc, cfg->scheduler, cfg->machine->freq, tick_hz);

    machine_part_begin(cfg, cp, "rtc");
    cfg->rtc = rtc_init(cfg->scheduler, cp, true, cfg->machine->pram);
    machine_part(cfg, cp, "rtc", part_save_rtc, cfg->rtc);

    // The ESCC cell behind the Grand Central decode, reachable through two
    // apertures (legacy +$12000 for the 68k Serial Driver, ESCC +$13000
    // for Open Firmware/native drivers — grand_central.c).  Clocks follow
    // the PDM values pending a TNT-specific measurement.  The chip's one
    // INT line fans to Grand Central interrupts 15/16 (ch A/B) — per-
    // channel splitting belongs to the serial datapath.
    machine_part_begin(cfg, cp, "scc");
    cfg->scc = scc_init(NULL, cfg->scheduler, tnt_scc_irq, cfg, cp);
    machine_part(cfg, cp, "scc", part_save_scc, cfg->scc);
    scc_set_clocks(cfg->scc, 15667200, 3672000);

    // AppleTalk rides the SCC's LocalTalk channel, so it is built as soon as
    // the SCC exists.  LocalTalk is the only AppleTalk path these machines have here: the
    // Grand Central MACE window is a #define and nothing else, so there is no
    // EtherTalk to prefer.  NOTE: the stack has only ever been exercised
    // against a Mac Plus guest (tests/integration/appletalk-*), so this wires
    // the family up rather than proving it.
    machine_part_begin(cfg, cp, "appletalk");
    cfg->atalk = atalk_conn_new(appletalk_network(), cfg->scheduler, cfg->scc, cp);
    machine_part(cfg, cp, "appletalk", part_save_atalk, cfg->atalk);
    machine_part_imagewriter(cfg, cp, false);

    // VIA1: one real 6522 behind the Grand Central decode, byte-wide on
    // $200 centres.  Timer clock: 783.36 kHz is the classic rate and the
    // starting assumption — the actual TNT VIA input clock is pinned at
    // the ladder's tick-rate rung (T8).
    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    machine_part_begin(cfg, cp, "via1");
    cfg->via1 =
        via_init(NULL, cfg->scheduler, via_ff, "via1", tnt_via1_output, tnt_via1_shift_out, tnt_via1_irq, cfg, cp);
    machine_part(cfg, cp, "via1", part_save_via, cfg->via1);
    via_set_exact_clock(cfg->via1, cfg->machine->freq);

    // VIA1 idle input levels: PB3 is Cuda TREQ (active-low, idle high);
    // CA1 and the Cuda CB1/CB2 lines idle high.
    via_input(cfg->via1, 1, 3, 1);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);
    via_input_c(cfg->via1, 1, 1, 1);

    // ADB device state, serviced through Cuda packets (the AV pattern).
    machine_part_begin(cfg, cp, "adb");
    cfg->adb = adb_init(NULL, cfg->scheduler, cp);
    machine_part(cfg, cp, "adb", part_save_adb, cfg->adb);

    // The behavioral Cuda (firmware 2.37 — the same 341S0788 part as the
    // AV and PDM machines) on the VIA1 shift register + PB3/4/5.  The
    // Mode3Clock tick is on, as on PDM: the guest clock lives behind
    // Cuda RdTime/PRAM here too and needs the real seed.
    machine_part_begin(cfg, cp, "cuda");
    st->cuda = av_cuda_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp, /*mode3_clock=*/true);
    if (!st->cuda) {
        LOG(0, "Error: out of memory constructing the Cuda");
        return -1;
    }
    machine_part(cfg, cp, "cuda", part_save_cuda, st->cuda);
    // Control's pixel-clock synthesiser hangs off Cuda's I2C bus (device
    // $50): the video driver programs it with three RdWrIIC packets per
    // mode-set, and the retrace period derives from what it wrote.
    av_cuda_attach_i2c_write(st->cuda, tnt_control_i2c_write, cfg);

    // The DBDMA engine behind the island's +$8000 channel windows.  No
    // device ports are attached yet — each datapath phase (AWACS ch 8,
    // SCSI ch 0/10, ...) registers its port as it lands; until then a
    // channel's data commands stall honestly.
    machine_part_begin(cfg, cp, "dbdma");
    st->dbdma = dbdma_init(cp, DBDMA_CHANNELS_GRAND_CENTRAL);
    if (!st->dbdma)
        return -1;
    machine_part(cfg, cp, "dbdma", part_save_dbdma, st->dbdma);
    // The internal SuperDrive behind SWIM3: the shared floppy module owns
    // the drive and media, the shared SWIM3 model (core/peripherals) the
    // chip, and swim3.c here binds the two to Grand Central and DBDMA
    // channel 1.  No memory map of its own: the island decodes it.
    machine_part_begin(cfg, cp, "floppy");
    cfg->floppy =
        floppy_init(FLOPPY_TYPE_SWIM3, NULL, cfg->scheduler, machine_floppy_count(cfg), cp, CONFIG_IMAGES(cfg));
    machine_part(cfg, cp, "floppy", part_save_floppy, cfg->floppy);
    tnt_swim3_bind(cfg);
    tnt_swim3_init(cfg);
    tnt_scc_dma_init(cfg);
    static const dma_mem_port_t dbdma_port = {
        .read_block = tnt_dbdma_mem_read,
        .write_block = tnt_dbdma_mem_write,
    };
    dma_mem_port_t port = dbdma_port;
    port.ctx = cfg;
    dbdma_set_memory_port(st->dbdma, &port);
    dbdma_set_irq_hook(st->dbdma, tnt_dbdma_irq, cfg);

    // The AWACS sound face on channel 8 (Open Firmware's beep is the
    // first exerciser, long before the 68k chime).
    tnt_awacs_register_events(cfg);
    tnt_swim3_register_events(cfg);
    tnt_awacs_init(cfg);
    tnt_awacs_reset(cfg);

    // Board state + memory map.
    tnt_hh_init(cfg);
    tnt_gc_init(cfg);
    tnt_gc_attach_object(cfg); // machine.gc; construction only, not on reset
    // The Network Server's GBUS island — built before the memory layout so
    // the LCD is answering from the very first POST write.  That ordering is
    // the whole point of it: POST establishes its LCD path before it sizes
    // DRAM, so the panel is the only narrator during the phase most likely
    // to break.
    if (tnt_board(cfg)->has_gbus) {
        tnt_gbus_init(cfg);
        tnt_lcd_init(cfg);
    }
    tnt_memory_layout(cfg, cp);

    // The PCI slot walk: seats every device the machine's slot table names
    // — Control (the BUILTIN video entry, whose factory allocates its VRAM
    // and display) and any card the boot document names for a socket — then
    // projects the whole topology into the object model.
    pci_seat_slots(cfg->pci, cp);

    // The PCI memory windows come after the walk: $90000000 goes to Chaos
    // or to Bandit 2 depending on whether the VCI bus seated anything.
    tnt_bandit_claim_memory(cfg);

    // The board's own state: register files + NVRAM are plain data; the CPU
    // line is recomputed below.
    machine_part_begin(cfg, cp, "tnt");
    if (cp) {
        system_read_checkpoint_data(cp, &st->hh, sizeof(st->hh));
        tnt_hh_remap(cfg);
        system_read_checkpoint_data(cp, &st->gc, sizeof(st->gc));
        for (int i = 0; i < st->bridge_count; i++) {
            system_read_checkpoint_data(cp, &st->bridge[i].cfg_addr, sizeof(st->bridge[i].cfg_addr));
            system_read_checkpoint_data(cp, &st->bridge[i].mode_select, sizeof(st->bridge[i].mode_select));
        }
        tnt_bandit_modes_restored(cfg); // the buses are rebuilt, not restored
    }
    machine_part(cfg, cp, "tnt", part_save_tnt_board, st);
    // Every PCI device read its config header with its own part; its decode
    // waits for the bus windows, which exist now.
    if (cp)
        pci_replay_decode(cfg->pci);
    machine_part_begin(cfg, cp, "tnt.av");
    if (cp) {
        system_read_checkpoint_data(cp, &st->awacs, sizeof(st->awacs));
        // Control's registers.  The monitor strap is not chip state: it was
        // set at construction from the slot entry (tnt_control_init), and
        // the saved copy does not replace it.
        uint8_t mon_grounded = st->control.mon_grounded;
        system_read_checkpoint_data(cp, &st->control, sizeof(st->control));
        st->control.mon_grounded = mon_grounded;
        // Control's VRAM is only there on a board that has Control.  A
        // Network Server's video is a PCI card in a socket, so `st->vram`
        // is NULL and the block is absent on both sides.
        if (st->vram)
            system_read_checkpoint_data(cp, st->vram, TNT_VRAM_SIZE);
    }
    machine_part(cfg, cp, "tnt.av", part_save_tnt_av, st);
    if (cp) {
        via_redrive_outputs(cfg->via1);
        tnt_gc_recompute(cfg);
        if (st->vram)
            tnt_control_update(cfg); // rebuild the descriptor from restored regs
    }

    // SCSI.  The image list restores before the devices that resolve media
    // out of it, then the shared bus, then the chips.  hd= media land on
    // cfg->scsi = the MESH internal bus (boot disks are internal on the
    // real machines); the external 53C94 is instantiated with NO bus
    // attached — every select times out, the empty-chain presentation
    // (the PDM 8100 fast-chip precedent).  No CD-ROM sits on the 53C94
    // chain yet (see pm7500.c's has_cdrom).
    machine_part_images(cfg, cp);
    machine_part_begin(cfg, cp, "scsi");
    cfg->scsi = machine_scsi_bus_init(cfg, cp, "scsi");
    machine_part(cfg, cp, "scsi", part_save_scsi, cfg->scsi);
    // The Network Servers carry TWO fast/wide buses.  `cfg->scsi` is
    // channel 0 (Open Firmware's `scsi-int`, bays 0-3, the `disk0`..`disk3`
    // aliases), so `hd=` / `cd=` and every existing consumer of
    // `machine.scsi` keep landing where the boot disk goes.  Channel 1
    // (`scsi-int2`, bays 4-6 plus the 700's two rear drives) mounts beside
    // it as `machine.scsi2`.
    if (tnt_board(cfg)->kind == TNT_BOARD_SHINER) {
        machine_part_begin(cfg, cp, "scsi2");
        st->scsi2 = machine_scsi_bus_init(cfg, cp, "scsi2");
        machine_part(cfg, cp, "scsi2", part_save_scsi, st->scsi2);
    }
    machine_part_begin(cfg, cp, "scsi96");
    st->scsi96 = scsi_53c96_init(cfg->scheduler, 25000000, cp); // 25 MHz (OF clock-frequency)
    machine_part(cfg, cp, "scsi96", part_save_scsi96, st->scsi96);
    scsi_53c96_set_irq_callback(st->scsi96, tnt_scsi96_irq, cfg);
    if (tnt_board(cfg)->has_mesh) {
        // MESH is a controller like any other: the machine builds it, tells it
        // where its interrupt goes and which bus it drives, and registers its
        // DBDMA channel-10 port.  It used to reach all three back through
        // config_t from inside its own model.
        machine_part_begin(cfg, cp, "mesh");
        st->mesh = mesh_init(cfg->scheduler, cp);
        machine_part(cfg, cp, "mesh", part_save_mesh, st->mesh);
        mesh_attach_bus(st->mesh, cfg->scsi);
        mesh_set_irq_callback(st->mesh, tnt_mesh_irq, cfg);
        mesh_set_dbdma_kick(st->mesh, tnt_mesh_dbdma_kick, cfg);
        dbdma_port_t mesh_port = {
            .out = mesh_port_out,
            .in = mesh_port_in,
            .s_bits = NULL,
            // MESH pops straight off the SCSI bus, so it never returns
            // short and the channel would run a whole transfer inside one
            // register store; the burst makes it yield and MESH's own pump
            // kicks it back on the bus's cadence.
            .burst = MESH_DMA_BURST,
            .ctx = st->mesh,
        };
        dbdma_set_port(st->dbdma, 10, &mesh_port);
    }

    machine_part_begin(cfg, cp, "tnt.io");
    if (cp) {
        system_read_checkpoint_data(cp, &st->gbus, sizeof(st->gbus));
        system_read_checkpoint_data(cp, &st->lcd, sizeof(st->lcd));
        // The prefix only; *_swim3_bind re-attaches the pointer tail.
        system_read_checkpoint_data(cp, &st->swim3, offsetof(swim3_t, fd));
        system_read_checkpoint_data(cp, &st->fdring, sizeof(st->fdring));
        tnt_swim3_bind(cfg); // the restore overwrote the chip's pointer tail
        tnt_gc_recompute(cfg); // mesh/53C94 lines fold into the fabric
    }
    machine_part(cfg, cp, "tnt.io", part_save_tnt_io, st);
    // MESH is a Macintosh-only cell.  The Network Servers deleted it — two
    // 53C825A PCI controllers carry the internal fast/wide buses instead —
    // so the board flag gates construction, the island decode
    // (grand_central.c) and DBDMA channel 10, which simply goes unused
    // there along with TNT_INT_MESH.
    tnt_scsi0_port_init(cfg); // DBDMA ch-0 port (53C94 pdma)
    // Hand each 53C825A its bus.  The controllers are PCI cards seated by
    // the slot walk, so this runs after it — and after the buses exist,
    // which is why it is here rather than in the card factory.
    tnt_fwscsi_attach(cfg);

    // Finish: the debugger.
    cfg->debugger = debug_init();
    return 0;
}

// A power cycle's power-on-only half (machine_profile.h): Cuda stays
// powered, but the host side of its VIA1 handshake went down under it.
static void tnt_power_on(config_t *cfg) {
    av_cuda_host_power_cycle(tnt_st(cfg)->cuda);
}

static void tnt_bus_reset(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    // Chipset registers to their power-on state.  The CPU going back to
    // $FFF00100 is the CPU half and belongs to level 2
    // (system_machine_reset); see the PDM twin.  NVRAM survives — it is
    // non-volatile, and POST's log plus the Open Firmware environment must
    // persist across restarts (warm-restart semantics proper are observed at
    // the ladder).
    tnt_hh_init(cfg);
    tnt_gc_init(cfg);
    dbdma_reset(st->dbdma);
    // The floppy CONTROLLER behind Grand Central +$15000.  cfg->floppy (the
    // drive and its media) is reset by the shared chain; the SWIM3 was the
    // one controller in the tree that survived a reset.
    swim3_reset(&st->swim3);
    tnt_awacs_reset(cfg);
    tnt_control_reset(cfg);
    if (tnt_board(cfg)->has_mesh)
        mesh_reset(st->mesh);
    if (tnt_board(cfg)->has_gbus) {
        tnt_gbus_reset(cfg);
        tnt_lcd_reset(cfg);
    }
    if (st->scsi96)
        scsi_53c96_reset(st->scsi96);
    scc_reset(cfg->scc);
    // The bridges first, and not by zeroing mode_select: BANDIT_BIG_ENDIAN
    // SET is big-endian, so a zeroed register is one with its byte lanes
    // REVERSED (little-endian, for Windows NT), and every reset would leave
    // the Mac OS half of the machine talking to its PCI devices backwards.
    // tnt_bandit_reset puts each bridge back to big-endian and re-applies the
    // lane mapping to its bus.
    tnt_bandit_reset(cfg);
    // PCI RST# (every seated device's header back to power-on, dropping the
    // assigned BARs and with them the decode), NuBus and SCSI -- the shared
    // fan-out, since they are all on the one net.
    system_reset_common_devices(cfg);
    tnt_gc_recompute(cfg);
}

static void tnt_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    tnt_state_t *st = tnt_st(cfg);
    if (st) {
        tnt_gc_detach_object(cfg);
        tnt_awacs_teardown(cfg);
        tnt_gbus_teardown(cfg);
        tnt_lcd_teardown(cfg);
    }
    // The PCI root is NOT deleted here: system_destroy owns both expansion
    // buses and tears them down before calling this, which is what frees
    // Control's VRAM and display buffers (its ops->teardown) and what puts
    // the 53C825As in their graves before the buses they borrow below.
    if (st && st->scsi96) {
        scsi_53c96_delete(st->scsi96);
        st->scsi96 = NULL;
    }
    // MESH is a controller the machine owns now, so the machine frees it --
    // before the bus it borrows, like the 53C96 above.
    if (st && st->mesh) {
        mesh_delete(st->mesh);
        st->mesh = NULL;
    }
    if (cfg->floppy) {
        floppy_delete(cfg->floppy);
        cfg->floppy = NULL;
    }
    // The four devices that used to sit between cfg->scsi and cfg->via1 in
    // this family's own copy of the chain, kept in the same relative order and
    // simply hoisted above the shared one (machine_teardown.h).  Only PDM and
    // TNT have them, so they stay here rather than joining the shared chain.
    //
    // The single ordering change is that cfg->scsi is now freed after these
    // four instead of before the first of them.  Safe on both counts that
    // matter: none of scsi_delete(scsi2), dbdma_delete, av_cuda_delete or
    // adb_delete reads cfg->scsi, and the controllers that DO hold the two
    // buses are already gone -- MESH/53C96 just above, and the 53C825As with
    // the PCI root, which system_destroy frees before this runs.  DBDMA still
    // goes after the floppy and before the SCC whose channels it serves, and
    // Cuda still goes before the via1, rtc and adb it was handed at init.
    if (st && st->scsi2) {
        scsi_delete(st->scsi2);
        st->scsi2 = NULL;
    }
    if (st && st->dbdma) {
        dbdma_delete(st->dbdma);
        st->dbdma = NULL;
    }
    if (st && st->cuda) {
        av_cuda_delete(st->cuda);
        st->cuda = NULL;
    }
    if (cfg->adb) {
        adb_delete(cfg->adb);
        cfg->adb = NULL;
    }
    machine_teardown_config_devices(cfg);
    if (st) {
        free(st);
        cfg->machine_context = NULL;
    }
}

// Frame tick (scheduler-paced, one per VBL frame-unit): the 60.15 Hz
// reference into VIA1 CA1 — the same line AMIC (PDM) and the AV feed
// their VIA1, third instance.  This is load-bearing: the Cuda driver's
// init waits for a CA1 edge right after its SecMode exchange (it polls
// IFR bit 1, then enables the CA1 interrupt) and the whole boot — ADB
// enumeration, DrawBeepScreen, the video driver — hangs forever without
// it (a boot that seemed to stall at video init was parked HERE, inside
// InitADB).
// Also: media insertion polling and the display's re-upload mark (guest
// CPU writes into VRAM bypass the renderer).
static void tnt_trigger_vbl(config_t *cfg) {
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    image_tick_all(cfg);
    tnt_control_host_vbl(cfg);
    pci_tick_vbl(cfg->pci);
}

// Media attach, with the Network Servers' SECOND SCSI bus: on a Shiner a
// medium in a rear bay is on `machine.scsi2`.
static int tnt_media_attach(config_t *cfg, const media_slot_t *slot) {
    if (slot->bus == MEDIA_BUS_SCSI2) {
        tnt_state_t *st = tnt_st(cfg);
        return system_media_attach_scsi_bus(cfg, st ? st->scsi2 : NULL, slot);
    }
    return system_media_attach_std(cfg, slot);
}

// The runtime attach/eject verbs' view of the same second bus.
static bool tnt_media_present(config_t *cfg, media_bus_t bus, int unit) {
    if (bus == MEDIA_BUS_SCSI2) {
        tnt_state_t *st = tnt_st(cfg);
        return system_media_present_scsi_bus(st ? st->scsi2 : NULL, unit);
    }
    return system_media_present_std(cfg, bus, unit);
}

static int tnt_media_eject(config_t *cfg, media_bus_t bus, int unit) {
    if (bus == MEDIA_BUS_SCSI2) {
        tnt_state_t *st = tnt_st(cfg);
        return system_media_eject_scsi_bus(st ? st->scsi2 : NULL, unit);
    }
    return system_media_eject_std(cfg, bus, unit);
}

// A PCI slot's strapped INTA-D line.  The slot table names the Grand
// Central external it reaches (23-25 on Bandit 1, 27-29 on Bandit 2 — the
// 9500's own published external-interrupt assignment); the lines are
// level-sensitive, which the interrupt block's mode-1 "interrupt on
// change" semantics already handle, deassert edge included.
static void tnt_pci_slot_irq(config_t *cfg, int slot, bool active) {
    const pci_slot_decl_t *d = pci_slot_decl_get(cfg->pci, slot);
    if (!d || d->int_line <= 0) {
        LOG(1, "PCI slot %d asserted an interrupt but declares no line", slot);
        return;
    }
    tnt_gc_set_source(cfg, d->int_line, active);
}

// Floppy: the one internal SuperDrive behind SWIM3 (swim3.c).  Drive 1 is
// the only bay the family has — no external port — so slot 1 refuses
// whatever the caller asks.
//
// The board's one internal SuperDrive is the same shape the Quadras and the
// PDM Power Macs present, so the five profiles reference the shared
// mac_floppy_slots_1hd (slot_tables.h) rather than the TNT keeping a seventh
// identical copy.  tnt_fd_insert below is what makes one slot the right count.

// The Power Macintosh boards' internal bus, MESH: the hard disk bay at ID 0
// and the 3.5" bay at ID 1 (the 8500 and 9500 add a second 3.5" bay at ID 2).
// The external connector and the CD-ROM bay are on the board's other bus, the
// 53C94 in Curio, which has no bus attached yet -- so these machines have no
// CD-ROM drive until it has.
static const storage_bay_decl_t tnt_bays_7500[] = {
    {.unit = 0, .label = "Internal hard disk bay"},
    {.unit = 1, .label = "3.5\xe2\x80\xb3 bay"},
    {0},
};

static const storage_bay_decl_t tnt_bays_8500[] = {
    {.unit = 0, .label = "Internal hard disk bay"},
    {.unit = 1, .label = "3.5\xe2\x80\xb3 bay"},
    {.unit = 2, .label = "Second 3.5\xe2\x80\xb3 bay"},
    {0},
};

const storage_bus_decl_t tnt_storage_7500[] = {
    MAC_SCSI_BUS("scsi", "Internal fast SCSI", MEDIA_BUS_SCSI, tnt_bays_7500, false),
    {0},
};

const storage_bus_decl_t tnt_storage_8500[] = {
    MAC_SCSI_BUS("scsi", "Internal fast SCSI", MEDIA_BUS_SCSI, tnt_bays_8500, false),
    {0},
};

// A hard disk in the internal bay; no CD-ROM drive (see above).
const storage_device_decl_t tnt_default_storage[] = {
    {.bus = "scsi", .unit = 0, .type = STORAGE_DEV_HD},
    {0},
};

// The Network Servers' front backplane: "seven slots with hot swap.  It is
// expected (but not required) that slot 0 will be a CD ROM."  Bay numbering
// runs top to bottom with 0 uppermost, and the production ROM's own device
// aliases settle which controller owns which bay -- `disk0`..`disk3` resolve
// through `/bandit/53c825@11` (bus 0), `disk4` onward through `@12` (bus 1).
// Open Firmware's default boot device is disk2:aix, bay 2.
//
// The second controller's table is deliberately NOT shared: the 700 hangs two
// rear bays off it and the 500 does not, and that is the "More drive Bays"
// half of Apple's own four-way split between the models (Network Server
// Hardware Developer Notes, 1996, S1.1.2).
const storage_bay_decl_t ans_bays_bus0[] = {
    {.unit = 0, .label = "Front bay 0"},
    {.unit = 1, .label = "Front bay 1"},
    {.unit = 2, .label = "Front bay 2"},
    {.unit = 3, .label = "Front bay 3"},
    {0},
};

// The CD-ROM drive in front bay 0, the hard disk in front bay 2.
const storage_device_decl_t ans_default_storage[] = {
    {.bus = "scsi", .unit = 0, .type = STORAGE_DEV_CD},
    {.bus = "scsi", .unit = 2, .type = STORAGE_DEV_HD},
    {0},
};

// The front keyswitch, a construction input (gbus.c): Apple's position
// names, Unlocked by default -- the machine's normal running position.
static const config_value_decl_t ans_keyswitch_values[] = {
    {.id = "unlocked", .label = "Unlocked"},
    {.id = "service", .label = "Service"},
    {.id = "locked", .label = "Locked"},
    {.id = NULL},
};

#define ANS_KEYSWITCH_OPTION                                                                                           \
    {.id = "keyswitch", .label = "Keyswitch", .values = ans_keyswitch_values, .default_value = "unlocked"}

const config_option_decl_t ans500_options[] = {
    ANS_KEYSWITCH_OPTION,
    {.id = NULL},
};

// The 700 takes a second, redundant power supply; it shipped with one.
static const config_value_decl_t ans_psu_values[] = {
    {.id = "one", .label = "One"},
    {.id = "two", .label = "Two"},
    {.id = NULL},
};

const config_option_decl_t ans700_options[] = {
    {.id = "power_supplies", .label = "Power supplies", .values = ans_psu_values, .default_value = "one"},
    ANS_KEYSWITCH_OPTION,
    {.id = NULL},
};

// The Cirrus Logic 54M30's VGA port: a multisync monitor the guest drives at
// its own choice of raster, so there is no monitor to pick.
static bool ans_monitor_at(size_t i, const char **id, const char **monitor) {
    if (i != 0)
        return false;
    *id = *monitor = "vga";
    return true;
}

const builtin_video_desc_t ans_builtin_video = {
    .detail = "Cirrus Logic 54M30",
    .monitor_at = ans_monitor_at,
    .default_monitor = "vga",
};

// The Shiner backplane -- the PCI topology of BOTH Network Servers, which
// share one board ("Shiner LE" = 500/132, "Shiner HE" = 700/150).  The two
// models differ only in clock, L2 size, supply count and drive bays; not one
// of those is visible here, which is why this is one table and not two.
//
// It is NOT the 9500's, despite the ANS being a 9500 derivative everywhere
// else (same Hammerhead, same two Bandits, same Grand Central).  Before
// merging this with pm9500_pci_slots, note that all five of these differ:
//
//   * the split is 2/4 across the Bandits, not 3/3
//   * bus 2 carries a FOURTH IDSEL (16) that no Power Macintosh uses
//   * bus 1 IDSEL 15 is a soldered VIDEO device here, a socket ("C1") there
//   * three builtins (VIDEO + two 53C825As) against the Power Macs' one VCI,
//     because MESH is gone and video moved onto the bus
//   * the slots are numbered 1-6 (the firmware's slot-names are the detail),
//     not lettered A1..F2
//
// PCI topology (Apple, ibid., §4.6.2 and §7.1.1; independently confirmed by
// the six per-slot Open Firmware boot commands printed in "Using the PCI
// RAID Card").  Two facts here are boot-critical and are pure data:
//
//   * The split is 2/4, not the 9500's 3/3: "The Network Server uses two
//     separate PCI buses for on-board I/O (and two slots) and card
//     expansion (four slots)" — "For PCI Bus 2, PCI Slot 3 is moved to the
//     second Bandit."  Bandit 1 therefore carries SIX devices with no
//     PCI-to-PCI bridge: two sockets plus the 54M30, Grand Central and both
//     53C825As.
//   * A slot's interrupt does NOT follow its bridge.  Slot 3 sits on Bandit
//     2 but keeps EXT5 (ANS_INT_SLOT3) — the line a 9500 gives Bandit 1's
//     third slot.  Deriving the line from the bus is wrong for exactly one
//     slot, which is the worst possible failure shape, so the map is data.
//
// Apple gives IDSELs in DECIMAL in §4.6.2/§7.1.1 and the matching unit
// addresses in HEX in Listing 6-1 and the RAID boot commands; `device`
// below is the decimal IDSEL AD line, which is what the config-cycle
// encoding wants.
//
// The DETAILS are the ROM's own slot names, read out of each bridge node's
// `slot-names` property under Open Firmware: Bandit 1 publishes
// `00006000 "SLOT1_PCI0" "SLOT2_PCI0"` and Bandit 2 publishes
// `0001E000 "SLOT3_PCI1" "SLOT4_PCI1" "SLOT5_PCI1" "SLOT6_PCI1"`.  Note
// the bus number in the string is ZERO-based while Apple's own prose and
// its `pci1`/`pci2` device aliases are one-based — which is why the
// worked example in the Software Developer Notes shows a slot-SIX card as
// `SLOT6_PCI1` and not `SLOT6_PCI2`.  The bitmask halves also confirm the
// 2/4 split and the IDSELs: bits 13-14 on the first bridge, 13-16 on the
// second.  Apple's recommended order for adding cards is slot 6 first (slot 1
// is kept for the RAID card), which `fill_order` records.
const pci_slot_decl_t ans_pci_slots[] = {
    {.slot = 1,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot 1",
     .detail = "SLOT1_PCI0",
     .fill_order = 6,
     .bus = TNT_PCI_BUS_1,
     .device = 13,
     .int_line = ANS_INT_SLOT1},
    {.slot = 2,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot 2",
     .detail = "SLOT2_PCI0",
     .fill_order = 5,
     .bus = TNT_PCI_BUS_1,
     .device = 14,
     .int_line = ANS_INT_SLOT2},
    {.slot = 3,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot 3",
     .detail = "SLOT3_PCI1",
     .fill_order = 4,
     .bus = TNT_PCI_BUS_2,
     .device = 13,
     .int_line = ANS_INT_SLOT3},
    {.slot = 4,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot 4",
     .detail = "SLOT4_PCI1",
     .fill_order = 3,
     .bus = TNT_PCI_BUS_2,
     .device = 14,
     .int_line = ANS_INT_SLOT4},
    {.slot = 5,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot 5",
     .detail = "SLOT5_PCI1",
     .fill_order = 2,
     .bus = TNT_PCI_BUS_2,
     .device = 15,
     .int_line = ANS_INT_SLOT5},
    {.slot = 6,
     .kind = PCI_SLOT_SOCKET,
     .label = "PCI slot 6",
     .detail = "SLOT6_PCI1",
     .fill_order = 1,
     .bus = TNT_PCI_BUS_2,
     .device = 16,
     .int_line = ANS_INT_SLOT6},
    // The three soldered-down PCI devices, all on Bandit 1 (Apple, ibid.,
    // §4.6.2 — the six-device bus).  Grand Central's own config presence at
    // IDSEL 16 is attached by grand_central.c, not from this table, exactly
    // as on the Macintosh boards.
    //
    // The 54M30 takes NO interrupt line: "the 54M30 does not have an
    // interrupt" (ibid., §4.2), and allocating it a Grand Central external
    // would corrupt the map.  The two 53C825As take EXT2 and EXT6, the two
    // positions the Network Server freed by ganging both Bandits' error
    // interrupts onto EXT1.
    {.slot = 7,
     .kind = PCI_SLOT_BUILTIN,
     .label = "Built-in video",
     .detail = "VIDEO",
     .bus = TNT_PCI_BUS_1,
     .device = 15,
     .int_line = 0,
     .builtin_card_id = "cirrus_54m30"},
    {.slot = 8,
     .kind = PCI_SLOT_BUILTIN,
     .label = "Fast and wide SCSI-2 controller (channel 0)",
     .detail = "FWSCSI0",
     .bus = TNT_PCI_BUS_1,
     .device = 17,
     .int_line = ANS_INT_FW0,
     .builtin_card_id = "sym53c825_0"},
    {.slot = 9,
     .kind = PCI_SLOT_BUILTIN,
     .label = "Fast and wide SCSI-2 controller (channel 1)",
     .detail = "FWSCSI1",
     .bus = TNT_PCI_BUS_1,
     .device = 18,
     .int_line = ANS_INT_FW1,
     .builtin_card_id = "sym53c825_1"},
    {0},
};

static int tnt_fd_insert(config_t *cfg, int drive, struct image *disk) {
    if (!cfg->floppy || drive != 0)
        return -1;
    return floppy_insert(cfg->floppy, drive, disk);
}

// A drive the board does not have holds no disk (see pdm_fd_present: the
// auto-select no longer needs a phantom "occupied" to stay off drive 1).
static bool tnt_fd_present(config_t *cfg, int drive) {
    if (!cfg->floppy || drive != 0)
        return false;
    return floppy_is_inserted(cfg->floppy, drive);
}

// The seeding step: Mac OS keeps its PRAM in the NVRAM's XPRAM partition
// here, not in Cuda, so the AppleTalk and startup-device records go there.
static void tnt_seed(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (!st)
        return;
    mac_seed_xpram_appletalk(st->gc.nvram + OF_NVRAM_XPRAM, cfg, of_nvram_defaults_tnt.pram);
    int id = mac_seed_startup_scsi_id(cfg, "scsi");
    if (id != -2)
        of_nvram_set_startup_scsi(st->gc.nvram, id, &of_nvram_defaults_tnt);
}

const machine_substrate_t tnt_substrate = {
    .init = tnt_init,
    .bus_reset = tnt_bus_reset,
    .power_on = tnt_power_on,
    .teardown = tnt_teardown,
    .seed = tnt_seed,
    .pci_slot_irq = tnt_pci_slot_irq,
    .trigger_vbl = tnt_trigger_vbl,
    .fd_insert = tnt_fd_insert,
    .fd_present = tnt_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
    .media_attach = tnt_media_attach,
    .media_present = tnt_media_present,
    .media_eject = tnt_media_eject,
};
