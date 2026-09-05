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

#include "cuda.h" // the shared behavioral Cuda model (machines/av/)
#include "dbdma.h"

#include "adb.h"
#include "checkpoint_images.h"
#include "debug.h"
#include "debug_mac.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "mac_host_io.h"
#include "machine_config.h" // machine_boot_is_restart (the NVRAM carry rule)
#include "pci.h"
#include "ppc.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "sym53c8xx.h" // the fast/wide controllers the ANS slot table seats
#include "via.h"

#include <assert.h>
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
    if (g_supervisor_write)
        g_supervisor_write[page_index] = writable ? adjusted : 0;
    if (g_user_read)
        g_user_read[page_index] = 0;
    if (g_user_write)
        g_user_write[page_index] = 0;
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

static void tnt_memory_layout(config_t *cfg) {
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
    tnt_bandit_init(cfg);

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
        if (!rev)
            memcpy(ram + phys, buf, len);
        else
            for (uint32_t i = 0; i < len; i++)
                ram[(phys + i) ^ 7u] = buf[i];
        return;
    }
    for (uint32_t i = 0; i < len; i++)
        memory_write_uint8_slow(rev ? ((phys + i) ^ 7u) : (phys + i), buf[i]);
}

// Channel completion -> Grand Central interrupt n (== channel n), an
// edge event into the fabric (interrupt-map §2.1).
static void tnt_dbdma_irq(void *ctx, int chan) {
    tnt_gc_pulse_event((config_t *)ctx, chan);
}

// ============================================================
// VIA1 callbacks — Cuda transport (the PDM/AV pattern, third instance)
// ============================================================

static void tnt_via1_output(void *context, uint8_t port, uint8_t value) {
    config_t *cfg = (config_t *)context;
    tnt_state_t *st = tnt_st(cfg);
    // Port B carries the Cuda handshake (PB3 TREQ in, PB4 BYTEACK out,
    // PB5 TIP out — the classic Cuda bit positions).
    if (port == 1 && st && st->cuda)
        av_cuda_via1_pb_input(st->cuda, value);
}

static void tnt_via1_shift_out(void *context, uint8_t byte) {
    config_t *cfg = (config_t *)context;
    tnt_state_t *st = tnt_st(cfg);
    if (st && st->cuda)
        av_cuda_via1_shift_input(st->cuda, byte);
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
    tnt_dbdma_port_t port = {
        .out = tnt_scsi0_port_out,
        .in = tnt_scsi0_port_in,
        .s_bits = NULL,
        .ctx = cfg,
    };
    tnt_dbdma_set_port(tnt_st(cfg)->dbdma, 0, &port);
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
// power cycles.  machine.restart tears the whole substrate down and
// rebuilds it, so the content is carried across teardown/init in this
// process-lifetime holder — the soldered chip surviving the power
// switch.  Load-bearing for booting from disk: Open Firmware reformats
// a blank store (clearing the Mac OS PRAM partition AFTER the OS's
// XPRAM shadow would need it), so the first-ever cold boot of a virgin
// machine cannot match a boot driver (XPRAM $77 "Default OS" reads 0)
// and only the SECOND boot — against the now-valid store — reaches the
// startup volume.  Real hardware behaves the same way; its NVRAM just
// never starts blank twice.  A checkpoint restore overrides the carry
// (the gc blob holds the store).
//
// The carry follows the power switch, i.e. machine.restart, and stops
// at machine.boot: that builds a NEW machine, which inherits nothing it
// was not given (proposal-boot-vs-reset §2).  The distinction is not
// bookkeeping.  A run stopped part-way through Open Firmware's format of
// a virgin store — a bounded `scheduler.run`, a client that walked away
// mid-run — leaves the store torn, and a machine built on a torn store
// stops in the ROM's serial-console read loop with a black screen and
// no way back short of restarting the process.  Rows that DO want the
// same chip across two cold boots (ans-diag-floppy's DIMM table) say so
// with machine.restart.
static uint8_t tnt_nvram_carry[TNT_NVRAM_SIZE];
static bool tnt_nvram_carry_valid;

// Clear the non-volatile store — what pulling the battery does.
//
// Apple, Network Server Hardware Developer Notes, §2.7: "Removal of a
// battery from the Main Logic Board will reset all parameter and NVRAM to
// default values."  It is the machine's own documented way back to a virgin
// configuration, and it is a real need rather than a test convenience: the
// ROM caches its DIMM sizing and its Open Firmware environment in there, so
// a store written by one model is not necessarily meaningful to another.
// The process-lifetime carry goes with it, or the next machine built in
// this process would inherit what was just erased.
void tnt_nvram_clear(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (st)
        memset(st->gc.nvram, 0, TNT_NVRAM_SIZE);
    memset(tnt_nvram_carry, 0, TNT_NVRAM_SIZE);
    tnt_nvram_carry_valid = false;
    LOG(1, "NVRAM cleared (battery removed)");
}

static int tnt_init(config_t *cfg, checkpoint_t *cp) {
    tnt_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;
    if (!cp && tnt_nvram_carry_valid)
        memcpy(st->gc.nvram, tnt_nvram_carry, TNT_NVRAM_SIZE);

    // Core: memory map, the 601/604 per profile, the scheduler on the PPC
    // seam.  CPI 1.0 — the same determinism-and-measurement rationale as
    // PDM; whether any TNT guest code times itself against the TB and
    // cares is a ladder observable.
    cfg->mem_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, cp);
    // No 68k MMU owns this machine's page table; host-backed regions that
    // core code registers on the bus map are filled through our filler.
    g_mem_host_fill = tnt_fill_page;
    const int cpu_model = cfg->machine->cpu_model;
    cfg->ppc = ppc_init(cp, cpu_model);
    if (!cfg->ppc) {
        LOG(0, "Error: out of memory constructing the PowerPC core");
        return -1;
    }
    sched_cpu_if_t cpu_if = ppc_sched_if(cfg->ppc);
    cfg->scheduler = scheduler_init(&cpu_if, cp);
    debug_mac_register_scheduler_events(cfg->scheduler); // before scheduler_start replays a restore
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

    cfg->rtc = rtc_init(cfg->scheduler, cp, true);

    // The ESCC cell behind the Grand Central decode, reachable through two
    // apertures (legacy +$12000 for the 68k Serial Driver, ESCC +$13000
    // for Open Firmware/native drivers — grand_central.c).  Clocks follow
    // the PDM values pending a TNT-specific measurement.  The chip's one
    // INT line fans to Grand Central interrupts 15/16 (ch A/B) — per-
    // channel splitting arrives with the Phase F serial datapath.
    cfg->scc = scc_init(NULL, cfg->scheduler, tnt_scc_irq, cfg, cp);
    scc_set_clocks(cfg->scc, 15667200, 3672000);

    // VIA1: one real 6522 behind the Grand Central decode, byte-wide on
    // $200 centres.  Timer clock: 783.36 kHz is the classic rate and the
    // starting assumption — the actual TNT VIA input clock is pinned at
    // the ladder's tick-rate rung (T8).
    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    cfg->via1 =
        via_init(NULL, cfg->scheduler, via_ff, "via1", tnt_via1_output, tnt_via1_shift_out, tnt_via1_irq, cfg, cp);
    via_set_exact_clock(cfg->via1, cfg->machine->freq);

    // VIA1 idle input levels: PB3 is Cuda TREQ (active-low, idle high);
    // CA1 and the Cuda CB1/CB2 lines idle high.
    via_input(cfg->via1, 1, 3, 1);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);
    via_input_c(cfg->via1, 1, 1, 1);

    // ADB device state, serviced through Cuda packets (the AV pattern).
    cfg->adb = adb_init(NULL, cfg->scheduler, cp);

    // The behavioral Cuda (firmware 2.37 — the same 341S0788 part as the
    // AV and PDM machines) on the VIA1 shift register + PB3/4/5.  The
    // Mode3Clock tick is on, as on PDM: the guest clock lives behind
    // Cuda RdTime/PRAM here too and needs the real seed.
    st->cuda = av_cuda_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp, /*mode3_clock=*/true);
    if (!st->cuda) {
        LOG(0, "Error: out of memory constructing the Cuda");
        return -1;
    }

    // The DBDMA engine behind the island's +$8000 channel windows.  No
    // device ports are attached yet — each datapath phase (AWACS ch 8,
    // SCSI ch 0/10, ...) registers its port as it lands; until then a
    // channel's data commands stall honestly.
    st->dbdma = tnt_dbdma_init(cp);
    if (!st->dbdma)
        return -1;
    // The internal SuperDrive behind SWIM3: the shared floppy module owns
    // the drive and media, the shared SWIM3 model (core/peripherals) the
    // chip, and swim3.c here binds the two to Grand Central and DBDMA
    // channel 1.  No memory map of its own: the island decodes it.
    cfg->floppy = floppy_init(FLOPPY_TYPE_SWIM3, NULL, cfg->scheduler, cp);
    tnt_swim3_bind(cfg);
    tnt_swim3_init(cfg);
    tnt_scc_dma_init(cfg);
    tnt_dbdma_set_memory_hooks(st->dbdma, tnt_dbdma_mem_read, tnt_dbdma_mem_write, cfg);
    tnt_dbdma_set_irq_hook(st->dbdma, tnt_dbdma_irq, cfg);

    // The AWACS sound face on channel 8 (Open Firmware's beep is the
    // first exerciser, long before the 68k chime).
    tnt_awacs_register_events(cfg);
    tnt_swim3_register_events(cfg);
    tnt_awacs_init(cfg);
    tnt_awacs_reset(cfg);

    // Board state + memory map.
    tnt_hh_init(cfg);
    tnt_gc_init(cfg);
    // The Network Server's GBUS island — built before the memory layout so
    // the LCD is answering from the very first POST write.  That ordering is
    // the whole point of it: POST establishes its LCD path before it sizes
    // DRAM, so the panel is the only narrator during the phase most likely
    // to break.
    if (tnt_board(cfg)->has_gbus) {
        tnt_gbus_init(cfg);
        tnt_lcd_init(cfg);
    }
    tnt_memory_layout(cfg);

    // The PCI slot walk: seats every device the machine's slot table names
    // — Control (the BUILTIN video entry, whose factory allocates its VRAM
    // and display) and any card the user staged into a socket — then
    // projects the whole topology into the object model.
    pci_seat_slots(cfg->pci, cp);

    // The PCI memory windows come after the walk: $90000000 goes to Chaos
    // or to Bandit 2 depending on whether the VCI bus seated anything.
    tnt_bandit_claim_memory(cfg);

    // Substrate-private checkpoint tail: register files + NVRAM are plain
    // data; the CPU line is recomputed below.
    if (cp) {
        system_read_checkpoint_data(cp, &st->hh, sizeof(st->hh));
        tnt_hh_remap(cfg);
        system_read_checkpoint_data(cp, &st->gc, sizeof(st->gc));
        for (int i = 0; i < st->bridge_count; i++) {
            system_read_checkpoint_data(cp, &st->bridge[i].cfg_addr, sizeof(st->bridge[i].cfg_addr));
            system_read_checkpoint_data(cp, &st->bridge[i].mode_select, sizeof(st->bridge[i].mode_select));
        }
        tnt_bandit_modes_restored(cfg); // the buses are rebuilt, not restored
        // Every seated device's config header, in canonical (bus, device)
        // order; the restore replays each BAR transition so the decode is
        // rebuilt without any device code.
        pci_checkpoint_restore(cfg->pci, cp);
        system_read_checkpoint_data(cp, &st->awacs, sizeof(st->awacs));
        system_read_checkpoint_data(cp, &st->control, sizeof(st->control));
        // Control's VRAM is only there on a board that has Control.  A
        // Network Server's video is a PCI card in a socket, so `st->vram`
        // is NULL and the block is absent from the stream on both sides.
        if (st->vram)
            system_read_checkpoint_data(cp, st->vram, TNT_VRAM_SIZE);
        via_redrive_outputs(cfg->via1);
        tnt_gc_recompute(cfg);
        if (st->vram)
            tnt_control_update(cfg); // rebuild the descriptor from restored regs
    }

    // SCSI (Phase E; appended at the end of the positional stream).  The
    // image list restores before the devices that resolve media out of
    // it, then the shared bus, then the chips.  hd= media land on
    // cfg->scsi = the MESH internal bus (boot disks are internal on the
    // real machines); the external 53C94 is instantiated with NO bus
    // attached — every select times out, the empty-chain presentation
    // (the PDM 8100 fast-chip precedent).  CD-ROM joins the 53C94 chain
    // in a later phase.
    if (cp)
        mac_checkpoint_restore_images(cfg, cp);
    cfg->scsi = scsi_init(NULL, cp);
    // The Network Servers carry TWO fast/wide buses.  `cfg->scsi` is
    // channel 0 (Open Firmware's `scsi-int`, bays 0-3, the `disk0`..`disk3`
    // aliases), so `hd=` / `cd=` and every existing consumer of
    // `machine.scsi` keep landing where the boot disk goes.  Channel 1
    // (`scsi-int2`, bays 4-6 plus the 700's two rear drives) mounts beside
    // it as `machine.scsi2`.
    if (tnt_board(cfg)->kind == TNT_BOARD_SHINER)
        st->scsi2 = scsi_init_named(NULL, cp, "scsi2");
    st->scsi96 = scsi_53c96_init(cfg->scheduler, 25000000, cp); // 25 MHz (OF clock-frequency)
    scsi_53c96_set_irq_callback(st->scsi96, tnt_scsi96_irq, cfg);
    if (cp) {
        system_read_checkpoint_data(cp, &st->mesh, sizeof(st->mesh));
        system_read_checkpoint_data(cp, &st->gbus, sizeof(st->gbus));
        system_read_checkpoint_data(cp, &st->lcd, sizeof(st->lcd));
        system_read_checkpoint_data(cp, &st->swim3, sizeof(st->swim3));
        system_read_checkpoint_data(cp, &st->fdring, sizeof(st->fdring));
        tnt_swim3_bind(cfg); // the restore overwrote the chip's pointer tail
        tnt_gc_recompute(cfg); // mesh/53C94 lines fold into the fabric
    }
    // MESH is a Macintosh-only cell.  The Network Servers deleted it — two
    // 53C825A PCI controllers carry the internal fast/wide buses instead —
    // so the board flag gates construction, the island decode
    // (grand_central.c) and DBDMA channel 10, which simply goes unused
    // there along with TNT_INT_MESH.  The checkpoint stream still carries
    // the (untouched) struct so it stays positional across both boards.
    if (tnt_board(cfg)->has_mesh)
        tnt_mesh_init(cfg); // DBDMA ch-10 port
    tnt_scsi0_port_init(cfg); // DBDMA ch-0 port (53C94 pdma)
    // Hand each 53C825A its bus.  The controllers are PCI cards seated by
    // the slot walk, so this runs after it — and after the buses exist,
    // which is why it is here rather than in the card factory.
    tnt_fwscsi_attach(cfg);

    // Finish: debugger + scheduler start.
    cfg->debugger = debug_init();
    scheduler_start(cfg->scheduler);
    return 0;
}

static void tnt_reset(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    // Power-on reset: the CPU back to $FFF00100, chipset registers to
    // their power-on state.  NVRAM survives — it is non-volatile, and
    // POST's log plus the Open Firmware environment must persist across
    // restarts (warm-restart semantics proper are observed at the ladder).
    ppc_reset(cfg->ppc);
    tnt_hh_init(cfg);
    tnt_gc_init(cfg);
    tnt_dbdma_reset(st->dbdma);
    tnt_awacs_reset(cfg);
    tnt_control_reset(cfg);
    if (tnt_board(cfg)->has_mesh)
        tnt_mesh_reset(cfg);
    if (tnt_board(cfg)->has_gbus) {
        tnt_gbus_reset(cfg);
        tnt_lcd_reset(cfg);
    }
    if (st->scsi96)
        scsi_53c96_reset(st->scsi96);
    scc_reset(cfg->scc);
    tnt_bandit_reset(cfg);
    // PCI RST#: every seated device's header back to power-on, which drops
    // the assigned BARs and with them the decode.
    pci_reset(cfg->pci);
    tnt_gc_recompute(cfg);
}

static void tnt_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    tnt_state_t *st = tnt_st(cfg);
    if (st) {
        // Power-cycle (machine.restart): the soldered part comes back with
        // the machine.  New machine (machine.boot): it gets a virgin store,
        // and the previous machine's goes with the previous machine.
        if (machine_boot_is_restart()) {
            memcpy(tnt_nvram_carry, st->gc.nvram, TNT_NVRAM_SIZE);
            tnt_nvram_carry_valid = true;
        } else {
            tnt_nvram_carry_valid = false;
        }
    }
    if (st) {
        tnt_awacs_teardown(cfg);
        tnt_gbus_teardown(cfg);
        tnt_lcd_teardown(cfg);
    }
    // Deleting the PCI root tears down every seated device, which is what
    // frees Control's VRAM and display buffers (its ops->teardown).
    if (cfg->pci) {
        pci_root_delete(cfg->pci);
        cfg->pci = NULL;
    }
    if (st && st->scsi96) {
        scsi_53c96_delete(st->scsi96);
        st->scsi96 = NULL;
    }
    if (cfg->floppy) {
        floppy_delete(cfg->floppy);
        cfg->floppy = NULL;
    }
    if (cfg->scsi) {
        scsi_delete(cfg->scsi);
        cfg->scsi = NULL;
    }
    if (st && st->scsi2) {
        scsi_delete(st->scsi2);
        st->scsi2 = NULL;
    }
    if (st && st->dbdma) {
        tnt_dbdma_delete(st->dbdma);
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
    if (cfg->via1) {
        via_delete(cfg->via1);
        cfg->via1 = NULL;
    }
    if (cfg->scc) {
        scc_delete(cfg->scc);
        cfg->scc = NULL;
    }
    if (cfg->rtc) {
        rtc_delete(cfg->rtc);
        cfg->rtc = NULL;
    }
    if (cfg->scheduler) {
        scheduler_delete(cfg->scheduler);
        cfg->scheduler = NULL;
    }
    if (cfg->ppc) {
        ppc_delete(cfg->ppc);
        cfg->ppc = NULL;
    }
    if (cfg->mem_map) {
        memory_map_delete(cfg->mem_map);
        cfg->mem_map = NULL;
    }
    if (cfg->debugger) {
        debug_cleanup(cfg->debugger);
        cfg->debugger = NULL;
    }
    if (st) {
        free(st);
        cfg->machine_context = NULL;
    }
}

static void tnt_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    tnt_state_t *st = tnt_st(cfg);
    // Same relative order as the tnt_init construction sequence (the
    // checkpoint stream is positional).
    memory_map_checkpoint(cfg->mem_map, cp);
    ppc_checkpoint(cfg->ppc, cp);
    scheduler_checkpoint(cfg->scheduler, cp);
    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    via_checkpoint(cfg->via1, cp);
    adb_checkpoint(cfg->adb, cp);
    av_cuda_checkpoint(st->cuda, cp);
    tnt_dbdma_checkpoint(st->dbdma, cp);
    // The floppy drive and media, where floppy_init reads them back on a
    // restore (right after the DBDMA engine, before the board state).
    floppy_checkpoint(cfg->floppy, cp);
    // Substrate-private tail (mirrored by the restore block in tnt_init).
    system_write_checkpoint_data(cp, &st->hh, sizeof(st->hh));
    system_write_checkpoint_data(cp, &st->gc, sizeof(st->gc));
    for (int i = 0; i < st->bridge_count; i++) {
        system_write_checkpoint_data(cp, &st->bridge[i].cfg_addr, sizeof(st->bridge[i].cfg_addr));
        system_write_checkpoint_data(cp, &st->bridge[i].mode_select, sizeof(st->bridge[i].mode_select));
    }
    pci_checkpoint_save(cfg->pci, cp);
    system_write_checkpoint_data(cp, &st->awacs, sizeof(st->awacs));
    system_write_checkpoint_data(cp, &st->control, sizeof(st->control));
    if (st->vram)
        system_write_checkpoint_data(cp, st->vram, TNT_VRAM_SIZE);
    // Phase-E SCSI block (mirrors the tnt_init append order exactly).
    mac_checkpoint_save_images(cfg, cp);
    scsi_checkpoint(cfg->scsi, cp);
    if (st->scsi2)
        scsi_checkpoint(st->scsi2, cp);
    scsi_53c96_checkpoint(st->scsi96, cp);
    system_write_checkpoint_data(cp, &st->mesh, sizeof(st->mesh));
    // The GBUS island (Network Servers only; zeroed and unread elsewhere).
    // Both blobs are plain data: the LCD's DDRAM and the board's keyswitch
    // positions and injected environmental faults.
    system_write_checkpoint_data(cp, &st->gbus, sizeof(st->gbus));
    system_write_checkpoint_data(cp, &st->lcd, sizeof(st->lcd));
    // The floppy controller and its DBDMA byte ring (swim3.c); the drive
    // itself is in the images block above.
    system_write_checkpoint_data(cp, &st->swim3, sizeof(st->swim3));
    system_write_checkpoint_data(cp, &st->fdring, sizeof(st->fdring));
}

// Frame tick (scheduler-paced, one per VBL frame-unit): the 60.15 Hz
// reference into VIA1 CA1 — the same line AMIC (PDM) and the AV feed
// their VIA1, third instance.  This is load-bearing: the Cuda driver's
// init waits for a CA1 edge right after its SecMode exchange (it polls
// IFR bit 1, then enables the CA1 interrupt) and the whole boot — ADB
// enumeration, DrawBeepScreen, the video driver — hangs forever without
// it (the Phase-D "video wall" turned out to park HERE, inside InitADB).
// Also: media insertion polling and the display's re-upload mark (guest
// CPU writes into VRAM bypass the renderer).
static void tnt_trigger_vbl(config_t *cfg) {
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    image_tick_all(cfg);
    tnt_control_host_vbl(cfg);
    pci_tick_vbl(cfg->pci);
}

// Primary display: the first display-capable PCI device in declared slot
// order.  Control is itself a pci_device_t with a display op, seated in the
// LAST declared slot (7 on the 9500, 4 on the 7500/8500), so this reads
// "a seated video card when one exists, Control otherwise" with no
// special-casing — the slot ordering was chosen for exactly this.
//
// The direct call survives as the fallback for the window between
// tnt_control_init and slot seating, when the PCI object graph is not yet
// answering.
static struct display *tnt_display(config_t *cfg) {
    struct display *d = pci_primary_display(cfg->pci);
    return d ? d : tnt_control_display(cfg);
}

// machine.restart handle transfer, with the Network Servers' SECOND SCSI
// bus.  The standard transfer walks the floppies and `cfg->scsi`; on a
// Shiner a medium in a rear bay is on `machine.scsi2`, and a transfer that
// does not know that drops it — the handle stays on the tracked-image list
// and system_destroy closes it, so the drive is simply gone after a
// power-cycle, with every delta write in it.
static int tnt_media_detach(config_t *cfg, media_slot_t *out, int max) {
    int n = system_media_detach_std(cfg, out, max);
    tnt_state_t *st = tnt_st(cfg);
    if (st && st->scsi2)
        n += system_media_detach_scsi_bus(cfg, st->scsi2, MEDIA_BUS_SCSI2, out + n, max - n);
    return n;
}

static int tnt_media_attach(config_t *cfg, const media_slot_t *slot) {
    if (slot->bus == MEDIA_BUS_SCSI2) {
        tnt_state_t *st = tnt_st(cfg);
        return system_media_attach_scsi_bus(cfg, st ? st->scsi2 : NULL, slot);
    }
    return system_media_attach_std(cfg, slot);
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

// Chipset IRQ spine.  Nothing routes through it: every on-board source is
// a Grand Central interrupt number (tnt_gc_set_source).
static void tnt_update_ipl(config_t *cfg, int source, bool active) {
    (void)cfg;
    LOG(1, "update_ipl source=%d active=%d (TNT sources drive Grand Central directly)", source, active);
}

// Floppy: the one internal SuperDrive behind SWIM3 (swim3.c).  Drive 1 is
// the only bay the family has — no external port — so slot 1 refuses
// whatever the caller asks.
//
// The slot table is declared once here rather than copied into each of the
// five profiles: it is a fact about the TNT board, and tnt_fd_insert below
// is what makes one slot the right count.
const struct floppy_slot tnt_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {0},
};

static int tnt_fd_insert(config_t *cfg, int drive, struct image *disk) {
    if (!cfg->floppy || drive != 0)
        return -1;
    return floppy_insert(cfg->floppy, drive, disk);
}

static bool tnt_fd_present(config_t *cfg, int drive) {
    if (!cfg->floppy || drive != 0)
        return true; // no such bay: report it occupied so nothing targets it
    return floppy_is_inserted(cfg->floppy, drive);
}

const machine_substrate_t tnt_substrate = {
    .init = tnt_init,
    .reset = tnt_reset,
    .teardown = tnt_teardown,
    .checkpoint_save = tnt_checkpoint_save,
    .update_ipl = tnt_update_ipl,
    .pci_slot_irq = tnt_pci_slot_irq,
    .trigger_vbl = tnt_trigger_vbl,
    .fd_insert = tnt_fd_insert,
    .fd_present = tnt_fd_present,
    .input_key = mac_input_key,
    .input_mouse_move = mac_input_mouse_move,
    .input_mouse_button = mac_input_mouse_button,
    .display = tnt_display,
    .media_detach = tnt_media_detach,
    .media_attach = tnt_media_attach,
};
