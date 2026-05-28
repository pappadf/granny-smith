// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iifx.c
// Macintosh IIfx machine implementation.

#include "machine.h"
#include "system_config.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "debug.h"
#include "floppy.h"
#include "glue030.h"
#include "image.h"
#include "iop.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "oss.h"
#include "rom.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "shell.h"
#include "via.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("iifx");

// Top-level IIfx address-space constants.
#define IIFX_ROM_START 0x40000000UL
#define IIFX_ROM_END   0x50000000UL
#define IIFX_IO_BASE   0x50000000UL
#define IIFX_IO_SIZE   0x10000000UL

// IIfx I/O devices mirror through the canonical 0x50Fxxxxx island.
#define IIFX_IO_MIRROR 0x0003ffffUL

// Compact I/O offsets after IIfx mirroring.
#define IO_VIA1           0x00000
#define IO_VIA1_END       0x02000
#define IO_SCC_IOP        0x04000
#define IO_SCC_IOP_END    0x06000
#define IO_SCSI_DMA       0x08000
#define IO_SCSI_DMA_END   0x0a000
#define IO_SCSI_REG       0x0a000
#define IO_SCSI_REG_END   0x0c000
#define IO_SCSI_DRQ_R     0x0c000
#define IO_SCSI_DRQ_R_END 0x0d000
#define IO_SCSI_DRQ_W     0x0d000
#define IO_SCSI_DRQ_W_END 0x0e000
#define IO_ASC            0x10000
#define IO_ASC_END        0x12000
#define IO_SWIM_IOP       0x12000
#define IO_SWIM_IOP_END   0x14000
#define IO_BIU            0x18000
#define IO_BIU_END        0x1a000
#define IO_OSS            0x1a000
#define IO_OSS_END        0x1c000
// $50F1C000-$50F1FFFF: OSS extension / BIU30 register surface (per the
// IIfx descriptor at $40803530, offsets $4C and $50).  Apple docs name
// these but the register layout is undocumented publicly.  Phase $8F
// at $40842E34 tests $50F1C000 (= $4C(A0))
// as a 16-bit serial shift register: 16 byte writes shift in bit-0 of each
// byte, 16 byte reads shift out the low bit (LSB first).  Phase $93 at
// $408430DC accesses $50F1E000 (= $50(A0)) as a long word inside a
// bus-error-driven probe.  For the rest of the OSS_EXT range we keep a
// simple R/W backing store.
//
// $50F1E000 is the address of the optional RPU (RAM Parity Unit) chip —
// see local/gs-docs/code/sys71src-main/OS/UniversalTables.a:7701 where
// the IIfx DecoderInfo's RPUAddr field is listed as "RPU - (optional)".
// On real hardware the chip is only present if parity SIMMs are
// installed; without it, the IIfx's BIU30 leaves the address undecoded
// and every access bus-errors.  POST phase $93 ($408430DC) probes the
// address by trying 32 BSET / BCLR / MOVE.L cycles and PASSES only if
// every access traps (= no RPU).  Later, the OS's gestaltParityAttr
// handler reads RPUAddr from DecoderInfo and writes to the chip — but
// only after first checking AddrMapFlags bit 20 (RPUExists), which the
// IIfx ROM only sets when POST phase $93 detected an installed chip.
// So when we bus-error at this window:
//   1. POST phase $93 sees the expected bus errors → RPUExists stays 0
//   2. _Gestalt(gestaltParityAttr) takes the @parityExit branch and
//      returns 0 → System 7's ParityINIT skips the dialog.
#define IO_OSS_EXT_START 0x1c000
#define IO_OSS_EXT_END   0x20000
#define IO_OSS_EXT_SHIFT 0x1c000 // 16-bit serial shift register
#define IO_RPU_PROBE     0x1e000 // optional RPU base; bus-errors when absent
#define IO_RPU_PROBE_END 0x1e020 // 32 bytes of register window (mode + reset)
#define IO_FMC_BERR      0x24000
#define IO_FMC_BERR_END  0x28000

// IIfx OSS source numbers (canonical IIfx hardware layout).
#define IIFX_OSS_SLOT9 0
#define IIFX_OSS_PSWM  6
#define IIFX_OSS_PSCC  7
#define IIFX_OSS_SCSI  9
#define IIFX_OSS_60HZ  10
#define IIFX_OSS_VIA1  11

// I/O bus wait-state penalties.
#define IIFX_VIA_IO_PENALTY  16
#define IIFX_IOP_IO_PENALTY  2
#define IIFX_SCSI_IO_PENALTY 2
#define IIFX_ASC_IO_PENALTY  2
#define IIFX_OSS_IO_PENALTY  2

// IIfx SCSI DMA controller — Apple 343S0064-A "IIfx Custom SCSI DMA
// Controller, QFP-100". Full register/state spec lives in
// local/gs-docs/iifx-scsi-dma/iifx-scsi-dma.md.
//
// Register offsets are spaced by $10 and decoded from A[8:4]; the
// embedded NCR 53C80 cell occupies $000..$070 (one byte each on
// D[31:24]), and Apple's wrapper registers occupy $080..$1FF (32-bit).
#define SCSIDMA_DCTRL 0x080 // DMA control / status
#define SCSIDMA_DCNT  0x0c0 // DMA byte count
#define SCSIDMA_DADDR 0x100 // DMA address
#define SCSIDMA_DTIME 0x140 // Watchdog timer reload
#define SCSIDMA_FIFO  0x180 // FIFO register
// $080 bits (spec §4). Bit 4 is asymmetric: write=RESET_53C80 strobe,
// read=FIFO_BYTES_LEFT status. Bits 6/7/8/13 are read-only latches.
#define SCSIDMA_DMAEN   0x0001 // bit 0 (R/W): Apple bus-master DMA enable
#define SCSIDMA_INTREN  0x0002 // bit 1 (R/W): SCSI/DMA interrupt enable
#define SCSIDMA_TIMEEN  0x0004 // bit 2 (R/W): watchdog interrupt enable
#define SCSIDMA_HSKEN   0x0008 // bit 3 (R/W): HW-handshake / pseudo-DMA mode
#define SCSIDMA_RESET   0x0010 // bit 4 (W):  reset embedded 53C80 (strobe)
#define SCSIDMA_FIFONE  0x0010 // bit 4 (R):  FIFO not empty
#define SCSIDMA_TEST    0x0020 // bit 5 (R/W): FIFO loopback test mode
#define SCSIDMA_SCSIP   0x0040 // bit 6 (R):  53C80 IRQ or Apple DMA done
#define SCSIDMA_TIMEP   0x0080 // bit 7 (R):  watchdog timeout pending
#define SCSIDMA_DMABERR 0x0100 // bit 8 (R):  DMA halted by /BERR
#define SCSIDMA_ID0     0x0200 // bit 9 (R/W): auto-arb SCSI ID bit 0
#define SCSIDMA_ID1     0x0400 // bit 10 (R/W): auto-arb SCSI ID bit 1
#define SCSIDMA_ID2     0x0800 // bit 11 (R/W): auto-arb SCSI ID bit 2
#define SCSIDMA_ID_MASK 0x0E00 // bits 9..11
#define SCSIDMA_ARBEN   0x1000 // bit 12 (R/W): auto-arbitration enable
#define SCSIDMA_WONARB  0x2000 // bit 13 (R):  auto-arbitration won

// Mask of bits the kernel can store via writes to $080.
#define SCSIDMA_CTRL_WRITABLE                                                                                          \
    (SCSIDMA_DMAEN | SCSIDMA_INTREN | SCSIDMA_TIMEEN | SCSIDMA_HSKEN | SCSIDMA_TEST | SCSIDMA_ID_MASK | SCSIDMA_ARBEN)

// Per-machine IIfx runtime state.
typedef struct iifx_state {
    struct asc *asc;
    struct floppy *floppy;
    oss_t *oss;
    adb_t *adb; // ADB device state — driven via SWIM IOP slot 3
    iop_t *scc_iop;
    iop_t *swim_iop;

    bool rom_overlay;
    mmu_state_t *mmu;

    const memory_interface_t *via1_iface;
    const memory_interface_t *scc_iface;
    const memory_interface_t *scsi_iface;
    const memory_interface_t *asc_iface;
    const memory_interface_t *floppy_iface;
    const memory_interface_t *oss_iface;
    const memory_interface_t *scc_iop_iface;
    const memory_interface_t *swim_iop_iface;

    // OSS-extension R/W backing store covering $50F1C000-$50F1FFFF
    // (16 KB).  See `IIfx-ROM.asm` §16e-g.
    uint8_t oss_ext[0x4000];

    // 16-bit serial shift register at $50F1C000 (byte offset 0 of
    // OSS_EXT).  Phase $8F at $40842E34 loads test patterns by writing
    // 16 bytes and reads them back by reading 16 bytes; bit-0 of each
    // byte is the serial-in / serial-out.  Right-shift model: writes
    // insert at bit 15, reads take bit 0 and shift right.
    uint16_t oss_ext_shift;

    // FMC "ROM-mirror invert" state for phase $90 of POST.  See the
    // full model + algebraic walk-through above iifx_apply_fmc_rom_invert().
    bool fmc_rom_invert; // current state of the FMC's invert flip-flop
    uint8_t *fmc_inverted_rom; // pre-computed ~ROM image; lazy-alloc'd in iifx_apply_fmc_rom_invert
    bool fmc_prev_bit3; // last observed OSS_ROM_CTRL bit 3 (for edge detection)

    // ── IIfx SCSI DMA controller state (Apple 343S0064-A spec) ──────
    //
    // Register-visible state ($080 / $0C0 / $100 / $140 / $180):
    //   ctrl              — sticky writable bits of $080
    //   count             — $0C0 byte count, auto-decrements per
    //                       SCSI-side byte (spec §5.1, §15.1)
    //   addr              — $100 current byte address, auto-increments
    //                       per SCSI-side byte (spec §5.2)
    //   watchdog_reload   — $140 stored reload value (spec §5.3)
    //   fifo_word         — $180 FIFO byte-router word (spec §13)
    //
    // FIFO byte-router: one 32-bit word + per-lane validity. Bytes
    // cross between the 53C80 data register and main memory through
    // these four lanes. dma_addr's two low bits select the active
    // lane on each side; refills/flushes align to longword
    // boundaries (spec §15, §16).
    //
    // Read-side latches (composed into $080 reads):
    //   done_latch         — bit 6: Apple DMA completed successfully
    //                        (OR'd with chip /IRQ for SCSIP)
    //   bus_error_latch    — bit 8: DMA halted by /BERR
    //   watchdog_irq_latch — bit 7: watchdog timeout pending
    //   wonarb_latch       — bit 13: auto-arb won
    //   tri_state_test     — $010 bit 6 stored (manufacturing mode)
    //   fifo_loopback_test — $080 bit 5 cached for fast checks
    //
    // Latch / counter split (ERS §6.1.1.1, §6.2.3.3, §6.5.1):
    //
    //   The chip has two physically distinct objects for the DMA
    //   address: an "initial-value-holding register" exposed at
    //   $100 (CPU latch, scsi_dma_addr_latch) AND an internal DMA
    //   Address Counter (scsi_dma_addr) that advances per byte
    //   transferred.  CPU reads of $100 return the live counter
    //   through the Address Readback Buffer.
    //
    //   The counter is reloaded from the latch on the rising edge
    //   (0 → 1) of MR_DMA — bit 1 of the 53C80 cell's Mode Register
    //   at offset $020 — **and only when the wrapper is actually
    //   in bus-master mode** ($080.DMAEN = 1).  Mac OS uses pseudo-
    //   DMA / iHSKEN: it also sets MR_DMA on the 53C80 (it's the
    //   same physical bit) but never touches the bus-master DMA
    //   registers ($100 / $0C0 / $080.DMAEN stays 0), so the
    //   counter-load semantics don't apply to its transfers.
    //   The DMAEN gate naturally restricts the chip-internal cur_addr
    //   behaviour to A/UX-driven bus-master transfers.
    //
    //   A 2026-05-26 byte-level trace audit (see doc-111) ESTABLISHED
    //   that this chip-faithful model alone CANNOT load libc1_s
    //   correctly: A/UX writes $100 = $2EBA00 byte-identically before
    //   every arm of the 10-arm exec-load, with no other chip-bus
    //   signal (no $080 bit 4 reset, no ARBEN cycling, no different
    //   $020 sequence) distinguishing arm 1 (must overwrite) from
    //   arms 2..10 (must advance).  Arm 1 vs arm 9 diff = exactly the
    //   3 CDB bytes containing the LBA field.  The 343S0064-A DMA
    //   engine cannot decode CDBs.
    //
    //   To get past libc1_s we run an A/UX-driver-specific software
    //   shim on top of this chip-faithful model that inspects the
    //   SCSI READ CDB's LBA field and computes a "credit offset"
    //   added to the latch at MR_DMA-edge load.  See the big warning
    //   header above iifx_scsidma_compute_credit_advance().  The shim
    //   is a known dead-end; it lives here as scaffolding until we
    //   find the real mechanism (most likely something invisible to
    //   our current trace — e.g., FMC/OSS state, MMU activity, or a
    //   second wrapper aperture we haven't mapped).
    uint32_t scsi_dma_ctrl;
    uint32_t scsi_dma_count;
    uint32_t scsi_dma_addr; // internal DMA Address Counter
    uint32_t scsi_dma_addr_latch; // $100 CPU latch
    bool scsi_dma_prev_mr_dma; // last-observed MR_DMA, for edge detect
    uint32_t scsi_dma_watchdog_reload;
    uint32_t scsi_dma_fifo_word;
    bool scsi_dma_fifo_valid[4];
    unsigned scsi_dma_fifo_count;
    bool scsi_dma_active;
    bool scsi_dma_done_latch;
    bool scsi_dma_bus_error_latch;
    bool scsi_dma_watchdog_irq_latch;
    bool scsi_dma_wonarb_latch;
    bool scsi_dma_tri_state_test;
    bool scsi_dma_fifo_loopback_test;

    // ─── A/UX kernel-credit shim state (see warning at the helper) ──
    // Snapshot of previous SCSI READ-or-WRITE command for overlap
    // detection, plus the accumulated "credit offset" added to the
    // chip's $100 latch at the next MR_DMA-edge load.  -1 LBA /
    // target = no previous data-transfer command.  The "_read_"
    // suffix is historical from when the shim only handled READs.
    int scsi_dma_prev_read_lba;
    uint16_t scsi_dma_prev_read_tl;
    int scsi_dma_prev_read_target;
    uint16_t scsi_dma_prev_read_blk_sz;
    uint32_t scsi_dma_credit_offset;

    memory_interface_t rom_interface;
    memory_interface_t io_interface;
} iifx_state_t;

// Returns the IIfx state for a config.
static inline iifx_state_t *iifx_state(config_t *cfg) {
    return (iifx_state_t *)cfg->machine_context;
}

// Forward declarations for profile callbacks.
static void iifx_init(config_t *cfg, checkpoint_t *checkpoint);
static void iifx_teardown(config_t *cfg);
static void iifx_reset(config_t *cfg);
static void iifx_checkpoint_save(config_t *cfg, checkpoint_t *cp);
static void iifx_memory_layout_init(config_t *cfg);
static void iifx_update_ipl(config_t *cfg, int source, bool active);
static void iifx_trigger_vbl(config_t *cfg);

// Fills one page-table entry with a direct host mapping.
static void iifx_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable) {
    if ((int)page_index >= g_page_count)
        return;
    g_page_table[page_index].host_base = host_ptr;
    g_page_table[page_index].dev = NULL;
    g_page_table[page_index].dev_context = NULL;
    g_page_table[page_index].writable = writable;
    uint32_t guest_base = page_index << PAGE_SHIFT;
    uintptr_t adjusted = (uintptr_t)host_ptr - guest_base;
    if (g_supervisor_read)
        g_supervisor_read[page_index] = adjusted;
    if (g_user_read)
        g_user_read[page_index] = adjusted;
    if (writable) {
        if (g_supervisor_write)
            g_supervisor_write[page_index] = adjusted;
        if (g_user_write)
            g_user_write[page_index] = adjusted;
    } else {
        if (g_supervisor_write)
            g_supervisor_write[page_index] = 0;
        if (g_user_write)
            g_user_write[page_index] = 0;
    }
}

// Repoints the page-table entries for $40008000-$4000FFFF to either
// the ROM image or a pre-computed bitwise-inverted ROM image,
// depending on `enable`.
//
// ── BACKGROUND ─────────────────────────────────────────────────────
//
// Phase $90 of the IIfx ROM POST (at $40842EB2) is the FMC's "ROM-
// mirror coherency" self-test.  It runs four loops with two
// comparisons:
//
//   Loop 2 — write NOT(read(A3+D5)) to A2+D5   [A3=$40008000, A2=$8000]
//   Loop 4 — compare read(A3+D5) == read(A2+D5)   ← must hold
//   Loop 5 — write NOT(read(A3+D5)) to A2+D5
//   Loop 6 — compare NOT(read(A3+D5)) == read(A2+D5)   ← must hold
//
// Between these, the test toggles bit 3 of OSS_ROM_CTRL ($50F1A204)
// twice: BCLR ($40842F04, $40842F62) and BSET ($40842F2E, $40842F96).
//
// Algebraically, both comparisons can hold simultaneously ONLY if:
//
//   1. A3 reads at Loop 2 and Loop 4 differ (Loop 4 expects A3 read
//      to equal NOT(A3 read in Loop 2), since A2 holds NOT(Loop 2's
//      A3 read) from Loop 2's writes).
//   2. A3 reads at Loop 5 and Loop 6 are equal (Loop 6 expects
//      NOT(A3) to equal A2 = NOT(A3 read in Loop 5)).
//
// Between Loop 2 (bit 3 = 1) and Loop 4 (bit 3 = 1 again after
// BCLR/BSET), the bit-3 value is the SAME — so something else must
// change to flip A3 reads.  That "something else" is an internal
// FMC state flag that TOGGLES on each 0→1 transition of bit 3
// (BSETs that change the bit; BCLRs do not toggle).
//
// ── MODEL ──────────────────────────────────────────────────────────
//
// State:  fmc_rom_invert  (bool, initially false at reset)
// Toggle: each time OSS_ROM_CTRL bit 3 transitions 0→1, flip
//         fmc_rom_invert.
// Effect: when fmc_rom_invert is true, reads from
//         $40008000-$4000FFFF (32 KB ROM-mirror page 1) return the
//         BITWISE COMPLEMENT of the corresponding ROM bytes.
//         Writes are ignored (this region is ROM-mirror, read-only).
//
// ── WALK-THROUGH FOR PHASE $90 ─────────────────────────────────────
//
//   $40802E50 (init): write $0D, bit 3 = 1.  Edge 0→1 → toggle.
//                     invert = true.  A3 reads = ~ROM.
//   Loop 2:           A3 = ~ROM, NOT = ROM, write to A2.  RAM = ROM.
//   $40842F04 BCLR:   bit 3 = 0.  NO toggle.  invert still true.
//   Loop 3:           reads A2 = RAM = ROM.  No state change.
//   $40842F2E BSET:   bit 3 = 1.  Edge 0→1 → toggle.  invert = false.
//                     A3 reads = ROM (normal).
//   Loop 4:           A3 = ROM, A2 = RAM = ROM.  EQUAL.  PASS ✓
//   Loop 5:           A3 = ROM, NOT = NOT(ROM), write to A2.
//                     RAM = NOT(ROM).
//   $40842F62 BCLR:   bit 3 = 0.  NO toggle.  invert still false.
//   Loop 6:           A3 = ROM, NOT(A3) = NOT(ROM), A2 = RAM
//                     = NOT(ROM).  EQUAL.  PASS ✓
//   $40842F96 BSET:   cleanup; toggle → invert = true again.
//
// Both comparisons hold.  D6 = 0.  Phase $90 succeeds.
//
// ── CONSTRAINT (why this can't be observed except in the test) ─────
//
// The boot writes $0D to OSS_ROM_CTRL at $40802E50 very early in
// §3 POST init (bit 3 = 1 → first toggle, invert becomes true).
// From that point, reads at $40008000-$4000FFFF return ~ROM.
//
// This region is ROM-mirror page 1 (= ROM bytes at file offset
// $8000-$FFFF).  The boot's instruction fetches happen at
// $40800000+ (= ROM body, a different mirror) and never reach
// $40008000-$4000FFFF before phase $90.  So having invert=true
// pre-set has no observable effect on normal boot — confirmed
// by live trace.
//
// ── PHYSICAL HYPOTHESIS (why Apple built it this way) ──────────────
//
// Likely an FMC manufacturing/diagnostic feature: a flip-flop on
// the bit-3 line, toggled on the rising edge, drives a bank of
// XOR gates between the ROM data path and the CPU data bus for
// the $40008000-$4000FFFF window.  Apple's ROM POST then runs
// phase $90 once per boot as a self-check that the FMC's flip-
// flop and inverter logic are functional.
//
// Used by the phase-$90 invert mechanism (toggled on each 0→1
// transition of OSS_ROM_CTRL bit 3 in `iifx_oss_control()`).
static void iifx_apply_fmc_rom_invert(config_t *cfg, bool enable) {
    iifx_state_t *st = iifx_state(cfg);
    uint32_t rom_size = cfg->machine->rom_size;
    if (rom_size == 0)
        return;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, cfg->ram_size);
    if (!st->fmc_inverted_rom) {
        st->fmc_inverted_rom = malloc(rom_size);
        if (!st->fmc_inverted_rom)
            return;
        for (uint32_t i = 0; i < rom_size; i++)
            st->fmc_inverted_rom[i] = (uint8_t)~rom_data[i];
    }
    uint8_t *src = enable ? st->fmc_inverted_rom : rom_data;
    // Pages $40008000-$4000FFFF (8 pages × 4 KB = 32 KB) get repointed.
    uint32_t start_page = 0x40008000u >> PAGE_SHIFT;
    uint32_t end_page = 0x40010000u >> PAGE_SHIFT;
    for (uint32_t p = start_page; p < end_page; p++) {
        uint32_t guest = p << PAGE_SHIFT;
        uint8_t *host_ptr = src + ((guest - 0x40000000u) % rom_size);
        iifx_fill_page(p, host_ptr, false);
    }
}

// Sets or clears the reset-time ROM overlay at physical zero.
static void iifx_set_rom_overlay(config_t *cfg, bool overlay) {
    iifx_state_t *st = iifx_state(cfg);
    if (st->rom_overlay == overlay)
        return;
    st->rom_overlay = overlay;

    uint32_t rom_size = cfg->machine->rom_size;
    uint32_t rom_pages = rom_size >> PAGE_SHIFT;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, cfg->ram_size);
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);

    for (uint32_t p = 0; p < rom_pages && (int)p < g_page_count; p++) {
        uint8_t *host_ptr = overlay ? rom_data + (p << PAGE_SHIFT) : ram_base + (p << PAGE_SHIFT);
        iifx_fill_page(p, host_ptr, !overlay);
    }
}

// Raises a plain bus-timeout exception for the FMC probe window.
static void iifx_bus_error(uint32_t addr, bool read) {
    if (g_bus_error_pending)
        return;
    g_bus_error_pending = 1;
    g_bus_error_address = addr;
    g_bus_error_rw = read ? 1 : 0;
    g_bus_error_fc =
        read ? ((g_active_read == g_supervisor_read) ? 5 : 1) : ((g_active_write == g_supervisor_write) ? 5 : 1);
    g_bus_error_is_pmmu = 0;
    if (g_bus_error_instr_ptr)
        *g_bus_error_instr_ptr = 0;
}

// Reads one byte from the ROM device and drops the overlay.
static uint8_t iifx_rom_read_uint8(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    iifx_set_rom_overlay(cfg, false);
    const uint8_t *rom = memory_rom_bytes(cfg->mem_map);
    uint32_t rom_size = memory_rom_size(cfg->mem_map);
    if (!rom || rom_size == 0)
        return 0xff;
    return rom[addr % rom_size];
}

// Reads one word from the ROM device.
static uint16_t iifx_rom_read_uint16(void *ctx, uint32_t addr) {
    uint16_t hi = iifx_rom_read_uint8(ctx, addr);
    uint16_t lo = iifx_rom_read_uint8(ctx, addr + 1);
    return (uint16_t)((hi << 8) | lo);
}

// Reads one long from the ROM device.
static uint32_t iifx_rom_read_uint32(void *ctx, uint32_t addr) {
    uint32_t hi = iifx_rom_read_uint16(ctx, addr);
    uint32_t lo = iifx_rom_read_uint16(ctx, addr + 2);
    return (hi << 16) | lo;
}

// Ignores writes to the ROM device.
static void iifx_rom_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    (void)ctx;
    (void)addr;
    (void)value;
}

// Ignores word writes to the ROM device.
static void iifx_rom_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    (void)ctx;
    (void)addr;
    (void)value;
}

// Ignores long writes to the ROM device.
static void iifx_rom_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    (void)ctx;
    (void)addr;
    (void)value;
}

// Reads one byte from a big-endian 32-bit SCSI DMA register.
static uint8_t iifx_read_reg32_byte(uint32_t value, uint32_t addr) {
    return (uint8_t)(value >> ((3u - (addr & 3u)) * 8u));
}

// Writes one byte into a big-endian 32-bit SCSI DMA register.
static void iifx_write_reg32_byte(uint32_t *reg, uint32_t addr, uint8_t value) {
    uint32_t shift = (3u - (addr & 3u)) * 8u;
    *reg = (*reg & ~(0xffu << shift)) | ((uint32_t)value << shift);
}

// ============================================================================
// IIfx SCSI DMA controller — Apple 343S0064-A spec-compliant model
// ============================================================================
//
// The IIfx ships a custom QFP-100 SCSI controller built around an
// enhanced NCR 53C80 cell. Apple's wrapper adds bus-master DMA, a
// 32-bit FIFO byte-router, a watchdog, and auto-arbitration. Full
// register/state spec: local/gs-docs/iifx-scsi-dma/iifx-scsi-dma.md.
//
// Register layout (spec §2):
//   $000..$070  embedded 53C80 (8-bit on D[31:24]), passed through
//   $080        DMA control / status (32-bit)
//   $0C0        DMA byte count    (32-bit, auto-decrements per byte)
//   $100        DMA address       (32-bit, auto-increments per byte)
//   $140        Watchdog reload   (32-bit)
//   $180        FIFO              (32-bit, R/W only in loopback test)
//
// Three byte-transport modes coexist (spec §8):
//
//   (1) Slave / PIO. CPU pokes 5380 registers via $000..$070; chip
//       drives REQ/ACK and runs SCSI protocol directly.
//
//   (2) Hardware-handshake / pseudo-DMA. $080.HW_HANDSHAKE=1 + chip
//       MR_DMA=1 + Start-DMA-Send/Recv. CPU writes/reads $000 or
//       $060; wrapper blocks the bus cycle until the SCSI byte
//       handshake completes. No bus mastering.
//
//   (3) Apple direct DMA (bus-master). $080.DMAEN=1 + chip MR_DMA=1
//       + Start-DMA-Send/Recv. Wrapper transfers bytes between main
//       memory and the 53C80's data register through the FIFO
//       byte-router. dma_addr and dma_count advance per SCSI-side
//       byte, not per memory bus cycle (spec §15.1 "important").
//
// Bus-master byte-router (spec §13, §15, §16):
//
//   mem -> SCSI ("DMA Read"):
//     refill: read 1..4 bytes from RAM[dma_addr..] into FIFO lanes,
//             aligned to longword boundary (spec §15.1)
//     drain : pop FIFO[lane = dma_addr & 3], hand to chip via auto-
//             handshake; dma_addr++, dma_count--
//
//   SCSI -> mem ("DMA Write"):
//     accept: byte from chip lands in FIFO[lane = dma_addr & 3];
//             dma_addr++, dma_count--
//     flush : write back when FIFO full or count exhausted, at
//             address (dma_addr - fifo_count) (spec §16.2 / Apple
//             write example)
//
// On completion (dma_count == 0 and FIFO drained):
//   - done_latch <- true (composes into $080 bit 6)
//   - signal EOP to chip (chip latches end_of_dma -> /IRQ)
//   - /INT asserts if $080.SCSI_INT_EN
//
// Direction is inferred from the chip's bus phase: data_in implies
// the kernel armed Start-DMA-Initiator-Receive ($070), data_out
// implies Start-DMA-Send ($050). The kernel sets the chip's phase
// expectation via $030/$010 before writing the Start-DMA register
// (spec §11.2, §11.3).
//
// Pump trigger points:
//   - DRQ assertion from chip (chip has/wants a byte)
//   - Writes to ctrl/count/addr (post-arm state change)
//
// Memory access bypasses the CPU MMU: the chip is a bus master that
// drives the physical address bus directly. A/UX converts virtual
// buffer pointers via realvtop before programming sDADDR, so the
// value stored there is a physical address.

// Forward declarations.
static void iifx_scsidma_pump(config_t *cfg);
static void iifx_scsidma_update_int(config_t *cfg);

// ════════════════════════════════════════════════════════════════════
//                  ╔══════════════════════════════════╗
//                  ║   A/UX KERNEL-CREDIT SHIM        ║
//                  ║   THIS IS A HEURISTIC HACK.      ║
//                  ║   PROBABLY WRONG IN MANY CASES.  ║
//                  ║   EXPECT TO REWRITE REPEATEDLY.  ║
//                  ╚══════════════════════════════════╝
//
// What this is.  An A/UX-driver-specific software shim that inspects
// the *SCSI READ CDB's LBA field* — a thing the real 343S0064-A DMA
// engine demonstrably cannot see — to compute a "credit offset"
// that gets added to the $100 latch at the next MR_DMA-edge load.
// The shim is the only known way (as of 2026-05-26) to get past
// the libc1_s exec-load on the IIfx A/UX boot.
//
// Why this is wrong.  The 2026-05-26 byte-level trace audit
// showed that A/UX issues 10 SCSI READs at advancing LBAs but
// writes $100 = $2EBA00 byte-identically every arm.  At the chip-bus level,
// arm 1 (which must overwrite the buffer) is *byte-identical* to
// arms 2..10 (which must auto-advance), except for the CDB LBA
// bytes the kernel sends through the 53C80 ODR ($000).  No
// chip-faithful rule based on register writes can distinguish the
// two patterns.  Therefore we cheat by decoding the CDB ourselves.
//
// What we expect to keep breaking.
//   • Any SCSI command sequence that the kernel issues with a
//     "stable base pointer + advancing LBA" pattern but where the
//     advance is *not* lba_delta * blk_sz (e.g., scatter-gather,
//     short transfers, retry loops).
//   • Bus-master WRITE paths.  This shim only credits READs.
//   • Any kernel that issues a READ at a non-zero LBA offset for
//     reasons unrelated to chunking (the shim will mis-credit).
//   • Interleaved transfers from two different SCSI targets — the
//     prev_target check helps but the logic still assumes one
//     bus-master session at a time.
//   • The transition between exec-load and runtime fs reads — we
//     reset prev_lba aggressively but the heuristic still may not
//     cover every disconnect/reselect pattern A/UX uses.
//
// What replacing this looks like.  Either (a) find the real chip
// mechanism we're missing (something not visible in our current
// trace — FMC/OSS state, MMU activity, an OSS-side aperture we
// haven't mapped, or a chip behaviour the published ERS doesn't
// document), or (b) accept it's a software shim, narrow its blast
// radius (e.g., gate it on a "we are in the A/UX kernel" PC range
// so Mac OS bus-master DMA — should it ever be added — bypasses it).
// ════════════════════════════════════════════════════════════════════

// On each per-arm count write ($0C0), snapshot the current SCSI
// READ command and update scsi_dma_credit_offset.  The offset is
// added to scsi_dma_addr_latch at the next MR_DMA-edge load (see
// iifx_scsidma_observe_mr_dma).  Non-READ / non-overlapping arms
// reset the offset and the prev tracker; overlapping arms
// accumulate (delta_lba * blk_sz).
static void iifx_scsidma_recompute_credit(iifx_state_t *st, scsi_t *scsi) {
    uint8_t opcode = scsi_get_cmd_opcode(scsi);
    bool is_read = (opcode == SCSI_OPCODE_READ_6 || opcode == SCSI_OPCODE_READ_10);
    bool is_write = (opcode == SCSI_OPCODE_WRITE_6 || opcode == SCSI_OPCODE_WRITE_10);
    if (!is_read && !is_write) {
        if (getenv("GS_IIFX_SHIM_TRACE"))
            fprintf(stdout, "SHIM reset (non-data opcode=$%02x)\n", opcode);
        st->scsi_dma_prev_read_lba = -1;
        st->scsi_dma_prev_read_target = -1;
        st->scsi_dma_prev_read_tl = 0;
        st->scsi_dma_prev_read_blk_sz = 0;
        st->scsi_dma_credit_offset = 0;
        return;
    }
    int new_target = scsi_get_cmd_target(scsi);
    uint32_t new_lba = scsi_get_cmd_lba(scsi);
    uint16_t new_tl = scsi_get_cmd_tl(scsi);
    uint16_t blk_sz = scsi_get_cmd_blk_sz(scsi);

    // Overlap: same target + same block size + new lba in (prev_lba,
    // prev_lba + prev_tl) — i.e. the new READ/WRITE picks up partway
    // through the previous command's still-pending range.  We don't
    // distinguish read vs. write for the test: the kernel uses the
    // same chunked-arm pattern for both, and an interleaved
    // read-then-write of the same range would normally reset because
    // the kernel re-targets the buffer between them.  (When it
    // doesn't, the wrong-direction overlap is the kind of false
    // positive this shim is known to produce — see header.)
    bool overlap = st->scsi_dma_prev_read_target == new_target && st->scsi_dma_prev_read_lba >= 0 &&
                   (int)new_lba > st->scsi_dma_prev_read_lba &&
                   (uint32_t)((int)new_lba - st->scsi_dma_prev_read_lba) < st->scsi_dma_prev_read_tl &&
                   blk_sz == st->scsi_dma_prev_read_blk_sz;

    if (overlap) {
        uint32_t credit = (uint32_t)((int)new_lba - st->scsi_dma_prev_read_lba) * (uint32_t)blk_sz;
        st->scsi_dma_credit_offset += credit;
        if (getenv("GS_IIFX_SHIM_TRACE"))
            fprintf(stderr,
                    "SHIM credit  %s tgt=%d lba=%u tl=%u (prev lba=%d tl=%u) +%u  total_off=%u  latch=$%08x => "
                    "addr=$%08x\n",
                    is_read ? "RD" : "WR", new_target, new_lba, new_tl, st->scsi_dma_prev_read_lba,
                    st->scsi_dma_prev_read_tl, credit, st->scsi_dma_credit_offset, st->scsi_dma_addr_latch,
                    st->scsi_dma_addr_latch + st->scsi_dma_credit_offset);
    } else {
        if (getenv("GS_IIFX_SHIM_TRACE"))
            fprintf(stdout, "SHIM fresh   %s tgt=%d lba=%u tl=%u (prev lba=%d tl=%u)  offset=0  latch=$%08x\n",
                    is_read ? "RD" : "WR", new_target, new_lba, new_tl, st->scsi_dma_prev_read_lba,
                    st->scsi_dma_prev_read_tl, st->scsi_dma_addr_latch);
        st->scsi_dma_credit_offset = 0;
    }
    st->scsi_dma_prev_read_lba = (int)new_lba;
    st->scsi_dma_prev_read_target = new_target;
    st->scsi_dma_prev_read_tl = new_tl;
    st->scsi_dma_prev_read_blk_sz = blk_sz;
}

// MR_DMA edge-load (ERS §6.5.1, §6.7.2.3): the chip's internal DMA
// Address Counter is reloaded from the $100 latch on the rising
// edge of 53C80 MR_DMA (bit 1 of $020 going 0 → 1).  We sample
// MR_DMA after every 53C80 register access.  Gated on DMAEN: Mac
// OS pseudo-DMA / iHSKEN also sets MR_DMA on the chip but doesn't
// arm the wrapper's bus-master engine, so the counter-load
// semantics don't apply to its transfers.
//
// Shim hook: the credit-offset (set by iifx_scsidma_recompute_credit
// on each $0C0 write) is added to the latch here.  For chip-faithful
// behaviour the offset is 0 and the load is just `latch`.
static inline void iifx_scsidma_observe_mr_dma(iifx_state_t *st, scsi_t *scsi) {
    bool now = scsi_get_mr_dma(scsi);
    if (now && !st->scsi_dma_prev_mr_dma && (st->scsi_dma_ctrl & SCSIDMA_DMAEN))
        st->scsi_dma_addr = st->scsi_dma_addr_latch + st->scsi_dma_credit_offset;
    st->scsi_dma_prev_mr_dma = now;
}

// ── FIFO byte-router primitives (spec §13) ─────────────────────────
// Big-endian byte lanes: addr bit pattern A1:A0 = 00 -> D[31:24]
// (lane 0), 01 -> D[23:16] (lane 1), 10 -> D[15:8] (lane 2),
// 11 -> D[7:0] (lane 3). "MSBs go in LS addresses."
static inline unsigned scsidma_lane_shift(int lane) {
    return (3u - (unsigned)lane) * 8u;
}

static void scsidma_fifo_clear(iifx_state_t *st) {
    st->scsi_dma_fifo_count = 0;
    for (int i = 0; i < 4; i++)
        st->scsi_dma_fifo_valid[i] = false;
}

static void scsidma_fifo_put_lane(iifx_state_t *st, int lane, uint8_t byte) {
    unsigned sh = scsidma_lane_shift(lane);
    st->scsi_dma_fifo_word = (st->scsi_dma_fifo_word & ~(0xffu << sh)) | ((uint32_t)byte << sh);
    if (!st->scsi_dma_fifo_valid[lane]) {
        st->scsi_dma_fifo_valid[lane] = true;
        st->scsi_dma_fifo_count++;
    }
}

static uint8_t scsidma_fifo_get_lane(const iifx_state_t *st, int lane) {
    return (uint8_t)(st->scsi_dma_fifo_word >> scsidma_lane_shift(lane));
}

// ── Reset & completion ────────────────────────────────────────────
// Spec §6.2: "$080 bit 4 = 1" resets the embedded 53C80 cell only.
// Apple DMA registers (addr, count, watchdog, FIFO) are NOT touched
// by this strobe per spec, but in practice we also drop any pending
// engine state — the kernel's reset path issues this before re-arming
// from scratch, and not clearing the engine state would leave a
// stale active flag and FIFO content.
static void iifx_scsidma_reset_chip_only(config_t *cfg) {
    iifx_state_t *st = iifx_state(cfg);
    st->scsi_dma_active = false;
    st->scsi_dma_done_latch = false;
    st->scsi_dma_bus_error_latch = false;
    scsidma_fifo_clear(st);
}

// Successful Apple DMA completion (spec §17).
static void iifx_scsidma_finish_success(config_t *cfg) {
    iifx_state_t *st = iifx_state(cfg);
    st->scsi_dma_active = false;
    st->scsi_dma_done_latch = true;
    scsi_signal_eop(cfg->scsi);
    iifx_scsidma_update_int(cfg);
}

// ── $080 read composition (spec §4.1) ─────────────────────────────
// Compose the read-side value of $080 from the sticky writable bits
// plus the read-only status latches and live status bits.
static uint32_t iifx_scsidma_read_ctrl_status(const iifx_state_t *st, scsi_t *scsi) {
    uint32_t v = st->scsi_dma_ctrl & SCSIDMA_CTRL_WRITABLE;
    if (st->scsi_dma_fifo_count != 0)
        v |= SCSIDMA_FIFONE;
    if (st->scsi_dma_done_latch || scsi_get_irq_active(scsi))
        v |= SCSIDMA_SCSIP;
    if (st->scsi_dma_watchdog_irq_latch)
        v |= SCSIDMA_TIMEP;
    if (st->scsi_dma_bus_error_latch)
        v |= SCSIDMA_DMABERR;
    if (st->scsi_dma_wonarb_latch)
        v |= SCSIDMA_WONARB;
    return v;
}

// ── $080 write (spec §4.2) ────────────────────────────────────────
// Store writable bits; if bit 4 strobed, reset embedded 53C80 only;
// if DMAEN cleared, abort active DMA; if ARBEN cleared, clear
// WONARB latch.
static void iifx_scsidma_write_ctrl(config_t *cfg, uint32_t data) {
    iifx_state_t *st = iifx_state(cfg);
    uint32_t prev = st->scsi_dma_ctrl;

    if (data & SCSIDMA_RESET)
        iifx_scsidma_reset_chip_only(cfg);

    st->scsi_dma_ctrl = data & SCSIDMA_CTRL_WRITABLE;
    st->scsi_dma_fifo_loopback_test = (st->scsi_dma_ctrl & SCSIDMA_TEST) != 0;

    if (!(st->scsi_dma_ctrl & SCSIDMA_DMAEN)) {
        st->scsi_dma_active = false;
        st->scsi_dma_done_latch = false;
        st->scsi_dma_bus_error_latch = false;
    }
    if (!(st->scsi_dma_ctrl & SCSIDMA_ARBEN))
        st->scsi_dma_wonarb_latch = false;

    // Auto-arbitration model (spec §21): the embedded 53C80 already
    // handles bus-free / BSY / ID-assertion mechanics under software
    // control. When ARBEN is set, treat the wrapper as having won
    // immediately — this matches the kernel's expectation of
    // "ARBEN=1 -> WONARB will be true on next read" without requiring
    // a separate state machine.
    if (st->scsi_dma_ctrl & SCSIDMA_ARBEN)
        st->scsi_dma_wonarb_latch = true;

    // INTREN rising edge with chip /IRQ already asserted: the wrapper's
    // IRQ output is level-sensitive through the enable gate. If the
    // chip /IRQ is high when the gate transitions 0->1, propagate.
    // (A/UX arms SDMA with INTREN=0, waits for chip EOP, then enables
    //  INTREN once the request is fully recorded.)
    bool intren_rising = !(prev & SCSIDMA_INTREN) && (st->scsi_dma_ctrl & SCSIDMA_INTREN);
    bool timeen_rising = !(prev & SCSIDMA_TIMEEN) && (st->scsi_dma_ctrl & SCSIDMA_TIMEEN);
    (void)intren_rising;
    (void)timeen_rising;

    iifx_scsidma_update_int(cfg);
    iifx_scsidma_pump(cfg);
}

// ── IRQ aggregation (spec §20, A/UX-compatible) ───────────────────
//
// Spec §20 composes /INT from:
//   (SCSI_INT_EN && (53C80 /IRQ || dma_done || dma_bus_err || wonarb))
//     || (WD_INT_EN && watchdog_timeout)
//
// But A/UX's scsiirq cleanup only reads $070 to ack the chip's /IRQ;
// it does NOT clear DMAEN between consecutive transfers (scsi_in just
// reasserts DMAEN+TIMEEN on the next arm). Per the spec's recommended
// `write_dma_control`, the wrapper's done/bus_err latches only clear
// when DMAEN is dropped — so including them in the live /INT gate
// would keep /INT asserted forever after the first completion,
// causing infinite scsiirq re-entry.
//
// Resolution: the latches still compose into $080 reads (so polling
// software can observe them), but the gated /INT output is driven by
// the chip's level-sensitive /IRQ line only. A/UX clears /IRQ via the
// $070 read in scsiirq — that's what releases the wrapper's gate.
// The result is functionally what the kernel expects and what the
// pre-rewrite heuristic model already did; the latches give the spec
// model its register-visible behaviour without driving a redundant
// IRQ source.
static void iifx_scsidma_update_int(config_t *cfg) {
    iifx_state_t *st = iifx_state(cfg);
    bool scsi_side = scsi_get_irq_active(cfg->scsi);
    bool wd_side = st->scsi_dma_watchdog_irq_latch;
    bool asserted =
        ((st->scsi_dma_ctrl & SCSIDMA_INTREN) && scsi_side) || ((st->scsi_dma_ctrl & SCSIDMA_TIMEEN) && wd_side);
    if (st->oss)
        oss_set_source(st->oss, IIFX_OSS_SCSI, asserted);
}

// ── Register access ───────────────────────────────────────────────
//
// Reads one SCSI DMA register byte. Offsets $000..$070 forward to
// the embedded 53C80; $080..$1FF are wrapper-local.
static uint8_t iifx_scsidma_read_uint8(config_t *cfg, uint32_t offset) {
    iifx_state_t *st = iifx_state(cfg);
    uint32_t off = offset & 0x1fff;

    if (off < 0x80) {
        uint8_t v = st->scsi_iface->read_uint8(cfg->scsi, off);
        // Reading $070 clears the 53C80 IRQ latch (spec §6.4 / §20).
        // Re-evaluate the wrapper's gated /INT output.
        if ((off & 0xf0) == 0x70)
            iifx_scsidma_update_int(cfg);
        return v;
    }
    if (off >= SCSIDMA_DCTRL && off < SCSIDMA_DCTRL + 4) {
        uint32_t v = iifx_scsidma_read_ctrl_status(st, cfg->scsi);
        return iifx_read_reg32_byte(v, off);
    }
    if (off >= SCSIDMA_DCNT && off < SCSIDMA_DCNT + 4)
        return iifx_read_reg32_byte(st->scsi_dma_count, off);
    if (off >= SCSIDMA_DADDR && off < SCSIDMA_DADDR + 4)
        return iifx_read_reg32_byte(st->scsi_dma_addr, off);
    if (off >= SCSIDMA_DTIME && off < SCSIDMA_DTIME + 4) {
        // Reading $140 clears the watchdog IRQ latch (spec §19.3).
        uint8_t v = iifx_read_reg32_byte(st->scsi_dma_watchdog_reload, off);
        if ((off & 3) == 3 && st->scsi_dma_watchdog_irq_latch) {
            st->scsi_dma_watchdog_irq_latch = false;
            iifx_scsidma_update_int(cfg);
        }
        return v;
    }
    if (off >= SCSIDMA_FIFO && off < SCSIDMA_FIFO + 4)
        return iifx_read_reg32_byte(st->scsi_dma_fifo_word, off);
    return 0;
}

// Writes one SCSI DMA register byte.
static void iifx_scsidma_write_uint8(config_t *cfg, uint32_t offset, uint8_t value) {
    iifx_state_t *st = iifx_state(cfg);
    uint32_t off = offset & 0x1fff;

    // Per-register PC trace for the discriminator investigation
    // (env-gated, same toggle as the shim trace).  Emits one line per
    // interesting wrapper-register write with the CPU PC at issue
    // time.  $0C0/$100 emit only on the final byte so the assembled
    // 32-bit value appears.  $020/$050/$070 are byte-wide registers.
    if (getenv("GS_IIFX_SHIM_TRACE")) {
        if (off == 0x020 || off == 0x050 || off == 0x070) {
            fprintf(stdout, "REG W $%03x = $%02x  pc=$%08x  ctrl=$%08x\n", off, value, cpu_get_pc(cfg->cpu),
                    st->scsi_dma_ctrl);
        } else if ((off & 0xff0) == SCSIDMA_DCTRL && (off & 3) == 3) {
            uint32_t composed = st->scsi_dma_ctrl;
            iifx_write_reg32_byte(&composed, off, value);
            fprintf(stdout, "REG W $080 = $%08x  pc=$%08x\n", composed, cpu_get_pc(cfg->cpu));
        } else if ((off & 0xff0) == SCSIDMA_DCNT && (off & 3) == 3) {
            uint32_t composed = st->scsi_dma_count;
            iifx_write_reg32_byte(&composed, off, value);
            fprintf(stdout, "REG W $0c0 = $%08x  pc=$%08x\n", composed, cpu_get_pc(cfg->cpu));
        } else if ((off & 0xff0) == SCSIDMA_DADDR && (off & 3) == 3) {
            uint32_t composed = st->scsi_dma_addr_latch;
            iifx_write_reg32_byte(&composed, off, value);
            fprintf(stdout, "REG W $100 = $%08x  pc=$%08x\n", composed, cpu_get_pc(cfg->cpu));
        }
    }

    if (off < 0x80) {
        // Hardware-handshake write path: when HW_HANDSHAKE=1, byte
        // writes to the chip's ODR ($000..$00F) auto-handshake onto
        // the SCSI bus. Bypass the chip's pseudo-DMA primer-slot
        // gate (which is an A/UX-driver heuristic that would drop a
        // leading $00 byte). 5380 decodes (addr >> 4) & 7, so $00..$0F
        // all map to ODR; MOVE.L (A2)+,(scsi_data) decomposes into
        // four byte writes at offsets 0..3.
        if (off < 0x10 && (st->scsi_dma_ctrl & SCSIDMA_HSKEN)) {
            scsi_hsken_data_out_byte(cfg->scsi, value);
            return;
        }
        // Track $010 bit 6 (TRI_STATE_TEST, spec §3.2 / §7.2).
        if ((off & 0xf0) == 0x10)
            st->scsi_dma_tri_state_test = (value & 0x40) != 0;
        // MR_DMA edge-load (ERS §6.5.1 / §6.7.2.3) MUST happen BEFORE
        // the write reaches scsi.c.  Reason: scsi.c's write_mr fires
        // a DRQ callback (via scsi_update_drq) when MR_DMA goes 0→1,
        // and that callback re-enters our pump (iifx_scsi_irq).  If
        // the counter hasn't yet been reloaded from $100, the pump
        // drains bytes to whatever stale cur_addr is left over from
        // the previous arm.  Predict the new MR_DMA value from the
        // write target + value before forwarding.
        if ((off & 0xf0) == 0x20) {
            bool prev = scsi_get_mr_dma(cfg->scsi);
            bool now = (value & 0x02) != 0;
            if (now && !prev && (st->scsi_dma_ctrl & SCSIDMA_DMAEN))
                st->scsi_dma_addr = st->scsi_dma_addr_latch + st->scsi_dma_credit_offset;
            st->scsi_dma_prev_mr_dma = now;
        }
        st->scsi_iface->write_uint8(cfg->scsi, off, value);
        // Writing $050 / $070 (Start DMA Send / Initiator Receive)
        // arms the chip's DMA mode; if Apple bus-master DMA is also
        // enabled, mark the engine active so the pump can run.
        if (((off & 0xf0) == 0x50 || (off & 0xf0) == 0x70) && (st->scsi_dma_ctrl & SCSIDMA_DMAEN))
            st->scsi_dma_active = true;
        // Re-sample MR_DMA for non-MR writes (e.g. RESET via $010
        // can clear MR_DMA chip-side).
        if ((off & 0xf0) != 0x20)
            iifx_scsidma_observe_mr_dma(st, cfg->scsi);
        // Re-evaluate /INT (reset bit may have raised chip IRQ;
        // mode/start writes can also change chip state) and pump
        // any pending bytes.
        iifx_scsidma_update_int(cfg);
        iifx_scsidma_pump(cfg);
        return;
    }
    if (off >= SCSIDMA_DCTRL && off < SCSIDMA_DCTRL + 4) {
        // Compose the new 32-bit value over the STORED writable bits
        // (not the read-side status composition: FIFONE at bit 4
        // would collide with the RESET strobe semantics on write).
        // The kernel typically does a full MOVE.L; byte decomposition
        // is an artifact of our I/O dispatch, so we treat the cumulative
        // byte writes as overlays on the stored value.
        uint32_t composed = st->scsi_dma_ctrl;
        iifx_write_reg32_byte(&composed, off, value);
        iifx_scsidma_write_ctrl(cfg, composed);
        return;
    }
    if (off >= SCSIDMA_DCNT && off < SCSIDMA_DCNT + 4) {
        // sDCNT — direct-access counter (ERS §6.6.3): CPU writes
        // load the counter, per-byte transfer decrements it.
        iifx_write_reg32_byte(&st->scsi_dma_count, off, value);
        if ((off & 3) == 3) {
            scsidma_fifo_clear(st);
            if (st->scsi_dma_count != 0 && (st->scsi_dma_ctrl & SCSIDMA_DMAEN))
                st->scsi_dma_active = true;
            // Per-arm hook for the kernel-credit shim — see the big
            // warning at iifx_scsidma_recompute_credit().  Updates the
            // credit_offset that the next MR_DMA-edge load will add
            // to the $100 latch.
            iifx_scsidma_recompute_credit(st, cfg->scsi);
        }
        iifx_scsidma_pump(cfg);
        return;
    }
    if (off >= SCSIDMA_DADDR && off < SCSIDMA_DADDR + 4) {
        // sDADDR — "initial-value-holding register" (ERS §6.1.1.1,
        // §6.2.3.3).  Writes update only the latch; the internal
        // counter is reloaded from the latch on the MR_DMA 0→1
        // rising edge (iifx_scsidma_observe_mr_dma).  Reads of $100
        // return the live counter through the Address Readback
        // Buffer.
        iifx_write_reg32_byte(&st->scsi_dma_addr_latch, off, value);
        if ((off & 3) == 3)
            scsidma_fifo_clear(st);
        iifx_scsidma_pump(cfg);
        return;
    }
    if (off >= SCSIDMA_DTIME && off < SCSIDMA_DTIME + 4) {
        iifx_write_reg32_byte(&st->scsi_dma_watchdog_reload, off, value);
        // Writing $140 loads the watchdog counter and clears its IRQ
        // latch (spec §19.3). We don't actively tick the counter
        // (DMA bytes complete synchronously here, so the watchdog
        // would only matter across a true stall), but the latch
        // semantics matter for the kernel.
        if ((off & 3) == 3 && st->scsi_dma_watchdog_irq_latch) {
            st->scsi_dma_watchdog_irq_latch = false;
            iifx_scsidma_update_int(cfg);
        }
        return;
    }
    if (off >= SCSIDMA_FIFO && off < SCSIDMA_FIFO + 4) {
        // $180 writes are ignored unless FIFO loopback test mode is
        // active (spec §5.4 / §7.3).
        if (st->scsi_dma_fifo_loopback_test)
            iifx_write_reg32_byte(&st->scsi_dma_fifo_word, off, value);
        return;
    }
}

// ── Bus-master pump (spec §15, §16) ───────────────────────────────
//
// Walks the FIFO byte-router in whichever direction the chip phase
// implies. Refill/drain (mem -> SCSI) or accept/flush (SCSI -> mem).
// Bytes are moved one at a time through the FIFO lane addressed by
// (dma_addr & 3); dma_addr and dma_count advance per byte.
//
// Idempotent: if not armed, MR_DMA not set, count=0, or phase has no
// work, returns immediately. The chip-side helpers (push/pop) drive
// REQ/ACK / EOP / phase transitions internally; we re-check phase
// on each iteration so a phase change mid-burst cleanly exits.
static void iifx_scsidma_pump(config_t *cfg) {
    iifx_state_t *st = iifx_state(cfg);
    scsi_t *scsi = cfg->scsi;

    // Engine runs whenever:
    //   - Apple DMA is enabled in $080
    //   - There are bytes left to move (count > 0 or FIFO not empty)
    //   - The 53C80 is in pseudo-DMA mode (MR_DMA set)
    // The kernel may write the register set in different orders;
    // the spec doesn't require a strict "Start-DMA" trigger to arm
    // the byte-router beyond these preconditions.
    if (!(st->scsi_dma_ctrl & SCSIDMA_DMAEN))
        return;
    if (st->scsi_dma_count == 0 && st->scsi_dma_fifo_count == 0)
        return;
    if (!scsi_get_mr_dma(scsi))
        return;
    // ERS §6.7.2.3: the chip starts DMA only after one of the
    // Start-DMA registers is written ($050 = Start DMA Send, $070
    // = Start DMA Initiator Receive).  Without this gate the pump
    // would drain bytes the moment MR_DMA is written — before the
    // kernel has finished the rest of its setup sequence — and the
    // resulting early EOP IRQ would race the kernel's sleep,
    // dropping the wakeup.  scsi_dma_active is set when $050/$070
    // is written with DMAEN already enabled (see SCSI register
    // write path) and cleared on RESET, DMAEN-clear, or EOP
    // completion.
    if (!st->scsi_dma_active)
        return;

    int phase = scsi_get_bus_phase(scsi);

    if (phase == scsi_data_in) {
        // SCSI -> mem. Direct byte-at-a-time transfer between chip
        // and RAM. The spec describes a 4-byte FIFO byte-router that
        // assembles longword bus cycles; its only externally visible
        // side effects are $080 bit 4 (FIFONE) and $180 (FIFO word),
        // both of which are zero / unread under A/UX's driver
        // pattern (transfers complete before the kernel inspects
        // them). The byte-router is therefore behaviourally
        // equivalent to a direct byte path for this caller.
        uint8_t byte;
        while (st->scsi_dma_count > 0 && scsi_pop_data_in_byte(scsi, &byte)) {
            mmu_write_physical_uint8(st->mmu, st->scsi_dma_addr, byte);
            st->scsi_dma_addr++;
            st->scsi_dma_count--;
        }
    } else if (phase == scsi_data_out) {
        // mem -> SCSI. Direct byte path, same reasoning as data-in.
        while (st->scsi_dma_count > 0) {
            uint8_t byte = mmu_read_physical_uint8(st->mmu, st->scsi_dma_addr);
            scsi_push_data_out_byte(scsi, byte);
            st->scsi_dma_addr++;
            st->scsi_dma_count--;
            if (scsi_get_bus_phase(scsi) != scsi_data_out)
                break;
        }
    } else {
        // Any other phase: the chip wouldn't be driving DRQ on real
        // hardware. Nothing to do.
        return;
    }

    // Completion: count exhausted and FIFO drained. Signal EOP and
    // latch the done bit.
    if (st->scsi_dma_count == 0 && st->scsi_dma_fifo_count == 0)
        iifx_scsidma_finish_success(cfg);
}

// Reads one byte from the IIfx I/O space.
static uint8_t iifx_io_read_uint8(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    iifx_state_t *st = iifx_state(cfg);
    uint32_t raw = addr;
    uint8_t v;

    if (raw >= 0x0ffffffc) {
        uint32_t value = 0xa55a000d;
        v = iifx_read_reg32_byte(value, raw);
        LOG(5, "io_r8 @%08x ->%02x [machID]", IIFX_IO_BASE + raw, v);
        return v;
    }

    uint32_t offset = raw & IIFX_IO_MIRROR;
    if (offset >= IO_FMC_BERR && offset < IO_FMC_BERR_END) {
        iifx_bus_error(IIFX_IO_BASE + raw, true);
        return 0xff;
    }
    if (offset < IO_VIA1_END) {
        memory_io_penalty(IIFX_VIA_IO_PENALTY);
        v = st->via1_iface->read_uint8(cfg->via1, (offset - IO_VIA1) & ~1u);
        uint32_t reg = (offset >> 9) & 0x0F;
        LOG(5, "io_r8 @%08x ->%02x [VIA1 r%x]", IIFX_IO_BASE + raw, v, reg);
        return v;
    }
    if (offset >= IO_SCC_IOP && offset < IO_SCC_IOP_END) {
        memory_io_penalty(IIFX_IOP_IO_PENALTY);
        v = st->scc_iop_iface->read_uint8(st->scc_iop, offset - IO_SCC_IOP);
        LOG(5, "io_r8 @%08x ->%02x [SCC_IOP+%x]", IIFX_IO_BASE + raw, v, offset - IO_SCC_IOP);
        return v;
    }
    if (offset >= IO_SCSI_DMA && offset < IO_SCSI_DMA_END) {
        memory_io_penalty(IIFX_SCSI_IO_PENALTY);
        v = iifx_scsidma_read_uint8(cfg, offset - IO_SCSI_DMA);
        LOG(5, "io_r8 @%08x ->%02x [SCSIDMA+%x]", IIFX_IO_BASE + raw, v, offset - IO_SCSI_DMA);
        return v;
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(IIFX_SCSI_IO_PENALTY);
        v = st->scsi_iface->read_uint8(cfg->scsi, offset - IO_SCSI_REG);
        LOG(5, "io_r8 @%08x ->%02x [SCSI r%x]", IIFX_IO_BASE + raw, v, (offset - IO_SCSI_REG) >> 4);
        return v;
    }
    if (offset >= IO_SCSI_DRQ_R && offset < IO_SCSI_DRQ_R_END) {
        memory_io_penalty(IIFX_SCSI_IO_PENALTY);
        v = st->scsi_iface->read_uint8(cfg->scsi, 0);
        LOG(5, "io_r8 @%08x ->%02x [SCSI_DRQR]", IIFX_IO_BASE + raw, v);
        return v;
    }
    if (offset >= IO_SCSI_DRQ_W && offset < IO_SCSI_DRQ_W_END) {
        memory_io_penalty(IIFX_SCSI_IO_PENALTY);
        v = st->scsi_iface->read_uint8(cfg->scsi, 0);
        LOG(5, "io_r8 @%08x ->%02x [SCSI_DRQW]", IIFX_IO_BASE + raw, v);
        return v;
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(IIFX_ASC_IO_PENALTY);
        v = st->asc_iface->read_uint8(st->asc, offset - IO_ASC);
        LOG(5, "io_r8 @%08x ->%02x [ASC]", IIFX_IO_BASE + raw, v);
        return v;
    }
    if (offset >= IO_SWIM_IOP && offset < IO_SWIM_IOP_END) {
        memory_io_penalty(IIFX_IOP_IO_PENALTY);
        v = st->swim_iop_iface->read_uint8(st->swim_iop, offset - IO_SWIM_IOP);
        LOG(5, "io_r8 @%08x ->%02x [SWIM_IOP+%x]", IIFX_IO_BASE + raw, v, offset - IO_SWIM_IOP);
        return v;
    }
    if (offset >= IO_OSS && offset < IO_OSS_END) {
        memory_io_penalty(IIFX_OSS_IO_PENALTY);
        v = st->oss_iface->read_uint8(st->oss, offset - IO_OSS);
        LOG(5, "io_r8 @%08x ->%02x [OSS+%x]", IIFX_IO_BASE + raw, v, offset - IO_OSS);
        return v;
    }
    if (offset >= IO_RPU_PROBE && offset < IO_RPU_PROBE_END) {
        // Optional RPU chip not installed — bus-error like real BIU30
        // does when the parity SIMMs / parity controller are absent.
        iifx_bus_error(IIFX_IO_BASE + raw, true);
        return 0xff;
    }
    if (offset >= IO_OSS_EXT_START && offset < IO_OSS_EXT_END) {
        memory_io_penalty(IIFX_OSS_IO_PENALTY);
        if (offset == IO_OSS_EXT_SHIFT) {
            v = (uint8_t)(st->oss_ext_shift & 1u);
            st->oss_ext_shift >>= 1;
            LOG(5, "io_r8 @%08x ->%02x [OSSx_SHIFT]", IIFX_IO_BASE + raw, v);
            return v;
        }
        v = st->oss_ext[offset - IO_OSS_EXT_START];
        LOG(5, "io_r8 @%08x ->%02x [OSSx+%x]", IIFX_IO_BASE + raw, v, offset - IO_OSS_EXT_START);
        return v;
    }
    if (offset >= IO_BIU && offset < IO_BIU_END) {
        LOG(5, "io_r8 @%08x ->00 [BIU]", IIFX_IO_BASE + raw);
        return 0;
    }
    LOG(5, "io_r8 @%08x ->FF [UNMAPPED]", IIFX_IO_BASE + raw);
    return 0xff;
}

// Reads one word from the IIfx I/O space.
static uint16_t iifx_io_read_uint16(void *ctx, uint32_t addr) {
    return (uint16_t)(((uint16_t)iifx_io_read_uint8(ctx, addr) << 8) | iifx_io_read_uint8(ctx, addr + 1));
}

// Reads one long from the IIfx I/O space.
static uint32_t iifx_io_read_uint32(void *ctx, uint32_t addr) {
    config_t *cfg = (config_t *)ctx;
    iifx_state_t *st = iifx_state(cfg);
    uint32_t offset = addr & IIFX_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ_R && offset < IO_SCSI_DRQ_R_END) {
        memory_io_penalty(IIFX_SCSI_IO_PENALTY * 4);
        uint8_t b0 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b1 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b2 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        uint8_t b3 = st->scsi_iface->read_uint8(cfg->scsi, 0);
        return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | b3;
    }
    return ((uint32_t)iifx_io_read_uint16(ctx, addr) << 16) | iifx_io_read_uint16(ctx, addr + 2);
}

// Writes one byte to the IIfx I/O space.
static void iifx_io_write_uint8(void *ctx, uint32_t addr, uint8_t value) {
    config_t *cfg = (config_t *)ctx;
    iifx_state_t *st = iifx_state(cfg);
    uint32_t raw = addr;
    uint32_t offset = raw & IIFX_IO_MIRROR;

    if (offset >= IO_FMC_BERR && offset < IO_FMC_BERR_END) {
        LOG(5, "io_w8 @%08x =%02x [FMC-BERR]", IIFX_IO_BASE + raw, value);
        iifx_bus_error(IIFX_IO_BASE + raw, false);
        return;
    }
    if (offset < IO_VIA1_END) {
        memory_io_penalty(IIFX_VIA_IO_PENALTY);
        uint32_t reg = (offset >> 9) & 0x0F;
        LOG(5, "io_w8 @%08x =%02x [VIA1 r%x]", IIFX_IO_BASE + raw, value, reg);
        st->via1_iface->write_uint8(cfg->via1, (offset - IO_VIA1) & ~1u, value);
        return;
    }
    if (offset >= IO_SCC_IOP && offset < IO_SCC_IOP_END) {
        memory_io_penalty(IIFX_IOP_IO_PENALTY);
        LOG(5, "io_w8 @%08x =%02x [SCC_IOP+%x]", IIFX_IO_BASE + raw, value, offset - IO_SCC_IOP);
        st->scc_iop_iface->write_uint8(st->scc_iop, offset - IO_SCC_IOP, value);
        return;
    }
    if (offset >= IO_SCSI_DMA && offset < IO_SCSI_DMA_END) {
        memory_io_penalty(IIFX_SCSI_IO_PENALTY);
        LOG(5, "io_w8 @%08x =%02x [SCSIDMA+%x]", IIFX_IO_BASE + raw, value, offset - IO_SCSI_DMA);
        iifx_scsidma_write_uint8(cfg, offset - IO_SCSI_DMA, value);
        return;
    }
    if (offset >= IO_SCSI_REG && offset < IO_SCSI_REG_END) {
        memory_io_penalty(IIFX_SCSI_IO_PENALTY);
        LOG(5, "io_w8 @%08x =%02x [SCSI r%x]", IIFX_IO_BASE + raw, value, (offset - IO_SCSI_REG) >> 4);
        st->scsi_iface->write_uint8(cfg->scsi, offset - IO_SCSI_REG, value);
        return;
    }
    if (offset >= IO_SCSI_DRQ_R && offset < IO_SCSI_DRQ_R_END) {
        memory_io_penalty(IIFX_SCSI_IO_PENALTY);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_SCSI_DRQ_W && offset < IO_SCSI_DRQ_W_END) {
        memory_io_penalty(IIFX_SCSI_IO_PENALTY);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, value);
        return;
    }
    if (offset >= IO_ASC && offset < IO_ASC_END) {
        memory_io_penalty(IIFX_ASC_IO_PENALTY);
        LOG(5, "io_w8 @%08x =%02x [ASC]", IIFX_IO_BASE + raw, value);
        st->asc_iface->write_uint8(st->asc, offset - IO_ASC, value);
        return;
    }
    if (offset >= IO_SWIM_IOP && offset < IO_SWIM_IOP_END) {
        memory_io_penalty(IIFX_IOP_IO_PENALTY);
        LOG(5, "io_w8 @%08x =%02x [SWIM_IOP+%x]", IIFX_IO_BASE + raw, value, offset - IO_SWIM_IOP);
        st->swim_iop_iface->write_uint8(st->swim_iop, offset - IO_SWIM_IOP, value);
        return;
    }
    if (offset >= IO_OSS && offset < IO_OSS_END) {
        memory_io_penalty(IIFX_OSS_IO_PENALTY);
        LOG(5, "io_w8 @%08x =%02x [OSS+%x]", IIFX_IO_BASE + raw, value, offset - IO_OSS);
        st->oss_iface->write_uint8(st->oss, offset - IO_OSS, value);
        return;
    }
    if (offset >= IO_RPU_PROBE && offset < IO_RPU_PROBE_END) {
        // Same as the read side: optional RPU chip absent → bus-error
        // every access so POST phase $93 sees a clean "no RPU" result
        // and System 7's gestaltParityAttr returns "no capability".
        iifx_bus_error(IIFX_IO_BASE + raw, false);
        return;
    }
    if (offset >= IO_OSS_EXT_START && offset < IO_OSS_EXT_END) {
        memory_io_penalty(IIFX_OSS_IO_PENALTY);
        if (offset == IO_OSS_EXT_SHIFT) {
            st->oss_ext_shift = (uint16_t)((st->oss_ext_shift >> 1) | ((value & 1u) << 15));
            LOG(5, "io_w8 @%08x =%02x [OSSx_SHIFT, reg=%04x]", IIFX_IO_BASE + raw, value, st->oss_ext_shift);
            return;
        }
        LOG(5, "io_w8 @%08x =%02x [OSSx+%x]", IIFX_IO_BASE + raw, value, offset - IO_OSS_EXT_START);
        st->oss_ext[offset - IO_OSS_EXT_START] = value;
        return;
    }
    LOG(5, "io_w8 @%08x =%02x [UNMAPPED]", IIFX_IO_BASE + raw, value);
}

// Writes one word to the IIfx I/O space.
static void iifx_io_write_uint16(void *ctx, uint32_t addr, uint16_t value) {
    iifx_io_write_uint8(ctx, addr, (uint8_t)(value >> 8));
    iifx_io_write_uint8(ctx, addr + 1, (uint8_t)value);
}

// Writes one long to the IIfx I/O space.
static void iifx_io_write_uint32(void *ctx, uint32_t addr, uint32_t value) {
    config_t *cfg = (config_t *)ctx;
    iifx_state_t *st = iifx_state(cfg);
    uint32_t offset = addr & IIFX_IO_MIRROR;
    if (offset >= IO_SCSI_DRQ_R && offset < IO_SCSI_DRQ_R_END) {
        memory_io_penalty(IIFX_SCSI_IO_PENALTY * 4);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)value);
        return;
    }
    if (offset >= IO_SCSI_DRQ_W && offset < IO_SCSI_DRQ_W_END) {
        memory_io_penalty(IIFX_SCSI_IO_PENALTY * 4);
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 24));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 16));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)(value >> 8));
        st->scsi_iface->write_uint8(cfg->scsi, 0x201, (uint8_t)value);
        return;
    }
    iifx_io_write_uint16(ctx, addr, (uint16_t)(value >> 16));
    iifx_io_write_uint16(ctx, addr + 2, (uint16_t)value);
}

// Recomputes the CPU IPL from OSS state.
static void iifx_oss_irq_changed(void *context) {
    config_t *cfg = (config_t *)context;
    iifx_state_t *st = iifx_state(cfg);
    cfg->irq = oss_pending(st->oss);
    cpu_set_ipl(cfg->cpu, oss_highest_ipl(st->oss));
    cpu_reschedule();
}

// Handles OSS ROM-control and power-off writes.  Bit 7 is the power-off
// signal; other bits are general-purpose status the ROM stores here.
// The main ROM overlay at $0-$7FFFF is dropped automatically by the
// first read from the $4xxxxxxx ROM region.
//
// Bit 3 of OSS_ROM_CTRL drives an FMC test mode used by phase $90 of
// POST.  See the model + algebraic walk above iifx_apply_fmc_rom_invert().
// Briefly: each 0 → 1 transition of bit 3 (BSETs that change the bit,
// not BCLRs or no-change writes) toggles an internal FMC "ROM-mirror
// invert" flag that makes reads from $40008000-$4000FFFF return the
// bitwise complement of the corresponding ROM bytes.
static void iifx_oss_control(void *context, uint8_t value) {
    config_t *cfg = (config_t *)context;
    iifx_state_t *st = iifx_state(cfg);
    if (value & 0x80u) {
        LOG(0, "IIfx soft power-off (OSS ROM control bit 7)");
        if (cfg->scheduler)
            scheduler_stop(cfg->scheduler);
    }
    bool new_bit3 = (value & 0x08u) != 0;
    if (new_bit3 && !st->fmc_prev_bit3) {
        // 0 → 1 transition: toggle the FMC ROM-invert state.
        st->fmc_rom_invert = !st->fmc_rom_invert;
        iifx_apply_fmc_rom_invert(cfg, st->fmc_rom_invert);
    }
    st->fmc_prev_bit3 = new_bit3;
}

// Routes an IOP host interrupt line to its OSS source.
static void iifx_scc_iop_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    iifx_state_t *st = iifx_state(cfg);
    oss_set_source(st->oss, IIFX_OSS_PSCC, active);
}

// Routes the ISM/SWIM IOP host interrupt line to OSS.
static void iifx_swim_iop_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    iifx_state_t *st = iifx_state(cfg);
    oss_set_source(st->oss, IIFX_OSS_PSWM, active);
}

// Routes a direct SCC interrupt to the SCC IOP OSS source.
static void iifx_scc_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    iifx_state_t *st = iifx_state(cfg);
    if (st && st->oss)
        oss_set_source(st->oss, IIFX_OSS_PSCC, active);
}

// Handles VIA1 output pins.
static void iifx_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    iifx_state_t *st = iifx_state(cfg);
    if (port == 0) {
        if (st->floppy)
            floppy_set_sel_signal(st->floppy, (output & 0x20) != 0);
        return;
    }
    if (cfg->rtc)
        rtc_input(cfg->rtc, (output >> 2) & 1, (output >> 1) & 1, output & 1);
}

// Ignores VIA1 shift-register output because ADB lives behind the ISM IOP.
static void iifx_via1_shift_out(void *context, uint8_t byte) {
    (void)context;
    (void)byte;
}

// Routes VIA1 aggregate interrupts through OSS source 11.
static void iifx_via1_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    iifx_state_t *st = iifx_state(cfg);
    oss_set_source(st->oss, IIFX_OSS_VIA1, active);
}

// Routes NCR 53C80 /IRQ and /DRQ from the chip to the IIfx-specific
// hardware around it.
//
//   /IRQ → composed into the wrapper's gated /INT (spec §20). Routed
//          to OSS source 9 (IIFX_OSS_SCSI) via iifx_scsidma_update_int,
//          which also folds in the wrapper's own latches
//          (done/bus-err/wonarb/watchdog).
//
//   /DRQ → drives the bus-master pump. On real hardware each DRQ pulse
//          handshakes one byte; we model that by calling the pump on
//          DRQ assertion, which drains as many bytes as it can in a
//          single pass (the chip-side emulator delivers buffer
//          contents in bulk, not one byte per REQ).
static void iifx_scsi_irq(void *context, bool irq, bool drq) {
    (void)irq;
    config_t *cfg = (config_t *)context;
    iifx_state_t *st = iifx_state(cfg);
    if (!st || !st->oss)
        return;
    iifx_scsidma_update_int(cfg);
    if (drq)
        iifx_scsidma_pump(cfg);
}

// Handles external machine IRQ requests such as NuBus slots.
static void iifx_update_ipl(config_t *cfg, int source, bool active) {
    iifx_state_t *st = iifx_state(cfg);
    if (!st || !st->oss)
        return;
    oss_set_source_mask(st->oss, (uint16_t)source, active);
}

// Pulses the IIfx 60 Hz sources.
static void iifx_trigger_vbl(config_t *cfg) {
    iifx_state_t *st = iifx_state(cfg);
    via_input_c(cfg->via1, 0, 0, 0);
    via_input_c(cfg->via1, 0, 0, 1);
    if (st && st->oss)
        oss_set_source(st->oss, IIFX_OSS_60HZ, true);
    // (Per-IOP periodic state machines run via scheduler_new_cpu_event
    // from iop_swim.c / iop_scc.c — they don't need VBL ticks here.)
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

// Initializes IIfx physical memory, ROM switching, I/O, and NuBus aliases.
static void iifx_memory_layout_init(config_t *cfg) {
    iifx_state_t *st = iifx_state(cfg);
    uint32_t ram_size = cfg->ram_size;
    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);

    uint32_t ram_pages = ram_size >> PAGE_SHIFT;
    for (uint32_t p = 0; p < ram_pages && (int)p < g_page_count; p++)
        iifx_fill_page(p, ram_base + (p << PAGE_SHIFT), true);

    st->rom_interface = (memory_interface_t){
        .read_uint8 = iifx_rom_read_uint8,
        .read_uint16 = iifx_rom_read_uint16,
        .read_uint32 = iifx_rom_read_uint32,
        .write_uint8 = iifx_rom_write_uint8,
        .write_uint16 = iifx_rom_write_uint16,
        .write_uint32 = iifx_rom_write_uint32,
    };
    memory_map_add(cfg->mem_map, IIFX_ROM_START, IIFX_ROM_END - IIFX_ROM_START, "IIfx ROM switch", &st->rom_interface,
                   cfg);

    st->io_interface = (memory_interface_t){
        .read_uint8 = iifx_io_read_uint8,
        .read_uint16 = iifx_io_read_uint16,
        .read_uint32 = iifx_io_read_uint32,
        .write_uint8 = iifx_io_write_uint8,
        .write_uint16 = iifx_io_write_uint16,
        .write_uint32 = iifx_io_write_uint32,
    };
    memory_map_add(cfg->mem_map, IIFX_IO_BASE, IIFX_IO_SIZE, "IIfx I/O", &st->io_interface, cfg);

    if (st->mmu) {
        if (st->mmu->physical_vram && st->mmu->physical_vram_size > 0) {
            uint32_t pages = st->mmu->physical_vram_size >> PAGE_SHIFT;
            uint32_t start = st->mmu->vram_phys_base >> PAGE_SHIFT;
            for (uint32_t i = 0; i < pages && (int)(start + i) < g_page_count; i++)
                iifx_fill_page(start + i, st->mmu->physical_vram + (i << PAGE_SHIFT), true);
        }
        if (st->mmu->physical_vrom && st->mmu->physical_vrom_size > 0) {
            uint32_t pages = st->mmu->physical_vrom_size >> PAGE_SHIFT;
            uint32_t start = st->mmu->vrom_phys_base >> PAGE_SHIFT;
            for (uint32_t i = 0; i < pages && (int)(start + i) < g_page_count; i++)
                iifx_fill_page(start + i, st->mmu->physical_vrom + (i << PAGE_SHIFT), false);
        }
        if (st->mmu->physical_vram) {
            uint32_t base32 = st->mmu->vram_phys_base;
            uint32_t high = base32 & 0xff000000u;
            if (high >= 0xf9000000u && high <= 0xfe000000u) {
                int slot = (int)((base32 >> 24) & 0xfu);
                uint32_t mode24_base = (uint32_t)slot << 20;
                uint32_t alias_bytes = 0x100000u;
                if (alias_bytes > st->mmu->physical_vram_size)
                    alias_bytes = st->mmu->physical_vram_size;
                uint32_t alias_pages = alias_bytes >> PAGE_SHIFT;
                uint32_t start = mode24_base >> PAGE_SHIFT;
                for (uint32_t i = 0; i < alias_pages && (int)(start + i) < g_page_count; i++)
                    iifx_fill_page(start + i, st->mmu->physical_vram + (i << PAGE_SHIFT), true);
            }
        }
    }

    st->rom_overlay = false;
    iifx_set_rom_overlay(cfg, true);
}

// Reasserts reset-time IIfx hardware state.
static void iifx_reset(config_t *cfg) {
    iifx_state_t *st = iifx_state(cfg);
    st->rom_overlay = false;
    iifx_set_rom_overlay(cfg, true);
    if (st->mmu) {
        st->mmu->enabled = false;
        st->mmu->tc = 0;
        mmu_invalidate_tlb(st->mmu);
    }
}

// Slot table for the six-slot IIfx NuBus cage.
static const char *const iifx_video_cards[] = {"mdc_8_24", NULL};

static const nubus_slot_decl_t iifx_slots[] = {
    {.slot = 0x9, .kind = NUBUS_SLOT_VIDEO, .available_cards = iifx_video_cards, .default_card = "mdc_8_24"},
    {.slot = 0xA, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xB, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xC, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xD, .kind = NUBUS_SLOT_EMPTY},
    {.slot = 0xE, .kind = NUBUS_SLOT_EMPTY},
    {0},
};

// Initializes a Macintosh IIfx machine.
static void iifx_init(config_t *cfg, checkpoint_t *checkpoint) {
    iifx_state_t *st = calloc(1, sizeof(*st));
    assert(st != NULL);
    cfg->machine_context = st;

    // Kernel-credit shim — calloc zeroed everything, but the prev-read
    // tracker uses -1 as "no previous READ" sentinels for the LBA and
    // target fields (so a legitimate lba=0 / target=0 doesn't spoof a
    // continuation).  See the big warning at the helper.
    st->scsi_dma_prev_read_lba = -1;
    st->scsi_dma_prev_read_target = -1;

    cfg->mem_map = memory_map_init(cfg->machine->address_bits, cfg->ram_size, cfg->machine->rom_size, checkpoint);
    cfg->cpu = cpu_init(CPU_MODEL_68030, checkpoint);
    cfg->scheduler = scheduler_init(cfg->cpu, checkpoint);
    scheduler_set_frequency(cfg->scheduler, cfg->machine->freq);
    scheduler_set_cpi(cfg->scheduler, 4, 4);
    if (checkpoint)
        system_read_checkpoint_data(checkpoint, &cfg->irq, sizeof(cfg->irq));

    cfg->rtc = rtc_init(cfg->scheduler, checkpoint);
    cfg->scc = scc_init(NULL, cfg->scheduler, iifx_scc_irq, cfg, checkpoint);
    scc_set_clocks(cfg->scc, 7833600, 3686400);

    cfg->via1 = via_init(NULL, cfg->scheduler, 51, "via1", iifx_via1_output, iifx_via1_shift_out, iifx_via1_irq, cfg,
                         checkpoint);
    rtc_set_via(cfg->rtc, cfg->via1);

    via_input(cfg->via1, 0, 7, 1);
    via_input(cfg->via1, 0, 6, 1);
    via_input(cfg->via1, 0, 5, 0);
    via_input(cfg->via1, 0, 4, 1);
    via_input(cfg->via1, 0, 2, 0);
    via_input(cfg->via1, 0, 1, 1);
    via_input(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 0, 1, 1);
    via_input_c(cfg->via1, 1, 0, 1);
    via_input_c(cfg->via1, 1, 1, 1);

    if (checkpoint)
        glue030_checkpoint_restore_images(cfg, checkpoint);

    cfg->scsi = scsi_init(NULL, checkpoint);
    setup_images(cfg);

    st->asc = asc_init(NULL, cfg->scheduler, checkpoint);
    st->floppy = floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, checkpoint);
    cfg->floppy = st->floppy;

    // ADB device state: the IIfx's ADB bus is bit-banged by the SWIM IOP
    // firmware ($F032 in iop-swim.bin), not by VIA1's shift register, so
    // pass NULL for the VIA argument — slot-3 traffic on the SWIM IOP
    // calls adb_iop_transact() directly without going through
    // via_input_sr() / via_input(pb3).  cfg->adb exposes this instance
    // to system_mouse_update / adb_keyboard_event so host-side mouse +
    // keyboard input routes here transparently.
    st->adb = adb_init(NULL, cfg->scheduler, checkpoint);
    cfg->adb = st->adb;

    st->via1_iface = via_get_memory_interface(cfg->via1);
    st->scc_iface = scc_get_memory_interface(cfg->scc);
    st->scsi_iface = scsi_get_memory_interface(cfg->scsi);
    st->asc_iface = asc_get_memory_interface(st->asc);
    st->floppy_iface = floppy_get_memory_interface(st->floppy);

    st->oss = oss_init(iifx_oss_irq_changed, iifx_oss_control, cfg, checkpoint);
    st->oss_iface = oss_get_memory_interface(st->oss);
    scsi_set_irq_callback(cfg->scsi, iifx_scsi_irq, cfg);
    st->scc_iop = iop_init(SccIopNum, st->scc_iface, cfg->scc, iifx_scc_iop_irq, cfg, cfg->scheduler, checkpoint);
    st->swim_iop =
        iop_init(SwimIopNum, st->floppy_iface, st->floppy, iifx_swim_iop_irq, cfg, cfg->scheduler, checkpoint);
    st->scc_iop_iface = iop_get_memory_interface(st->scc_iop);
    st->swim_iop_iface = iop_get_memory_interface(st->swim_iop);

    uint8_t *ram_base = ram_native_pointer(cfg->mem_map, 0);
    uint32_t ram_size = cfg->ram_size;
    uint8_t *rom_data = ram_native_pointer(cfg->mem_map, ram_size);
    uint32_t rom_size = cfg->machine->rom_size;
    st->mmu = mmu_init(ram_base, ram_size, cfg->machine->ram_max, rom_data, rom_size, IIFX_ROM_START, IIFX_ROM_END);
    assert(st->mmu != NULL);
    g_mmu = st->mmu;
    cpu_attach_mmu(cfg->cpu, st->mmu);
    st->mmu->tt1 = 0xF00F8043;

    cfg->nubus = nubus_init(cfg, iifx_slots, checkpoint);
    memory_set_bus_error_range(cfg->mem_map, 0xF9000000, 0xFEFFFFFF);

    iifx_memory_layout_init(cfg);

    if (checkpoint) {
        system_read_checkpoint_data(checkpoint, &st->scsi_dma_ctrl, sizeof(st->scsi_dma_ctrl));
        system_read_checkpoint_data(checkpoint, &st->scsi_dma_count, sizeof(st->scsi_dma_count));
        system_read_checkpoint_data(checkpoint, &st->scsi_dma_addr, sizeof(st->scsi_dma_addr));
        system_read_checkpoint_data(checkpoint, &st->scsi_dma_watchdog_reload, sizeof(st->scsi_dma_watchdog_reload));
        system_read_checkpoint_data(checkpoint, &st->scsi_dma_fifo_word, sizeof(st->scsi_dma_fifo_word));
        system_read_checkpoint_data(checkpoint, &st->mmu->tc, sizeof(st->mmu->tc));
        system_read_checkpoint_data(checkpoint, &st->mmu->crp, sizeof(st->mmu->crp));
        system_read_checkpoint_data(checkpoint, &st->mmu->srp, sizeof(st->mmu->srp));
        system_read_checkpoint_data(checkpoint, &st->mmu->tt0, sizeof(st->mmu->tt0));
        system_read_checkpoint_data(checkpoint, &st->mmu->tt1, sizeof(st->mmu->tt1));
        system_read_checkpoint_data(checkpoint, &st->mmu->mmusr, sizeof(st->mmu->mmusr));
        system_read_checkpoint_data(checkpoint, &st->mmu->enabled, sizeof(st->mmu->enabled));
        mmu_invalidate_tlb(st->mmu);
        g_mmu = st->mmu;
        cpu_attach_mmu(cfg->cpu, st->mmu);
        via_redrive_outputs(cfg->via1);
    }

    cfg->debugger = debug_init();
    scheduler_register_vbl_type(cfg->scheduler, cfg);
    scheduler_start(cfg->scheduler);
    if (!checkpoint) {
        cfg->irq = 0;
        cpu_set_ipl(cfg->cpu, 0);
    }
}

// Tears down an IIfx machine.
static void iifx_teardown(config_t *cfg) {
    if (cfg->scheduler)
        scheduler_stop(cfg->scheduler);
    iifx_state_t *st = iifx_state(cfg);
    if (st) {
        if (st->scc_iop) {
            iop_delete(st->scc_iop);
            st->scc_iop = NULL;
        }
        if (st->swim_iop) {
            iop_delete(st->swim_iop);
            st->swim_iop = NULL;
        }
        if (st->oss) {
            oss_delete(st->oss);
            st->oss = NULL;
        }
        if (st->mmu) {
            mmu_delete(st->mmu);
            st->mmu = NULL;
        }
        if (st->fmc_inverted_rom) {
            free(st->fmc_inverted_rom);
            st->fmc_inverted_rom = NULL;
        }
        if (st->floppy) {
            floppy_delete(st->floppy);
            st->floppy = NULL;
            cfg->floppy = NULL;
        }
        if (st->asc) {
            asc_delete(st->asc);
            st->asc = NULL;
        }
        if (st->adb) {
            adb_delete(st->adb);
            st->adb = NULL;
            cfg->adb = NULL;
        }
    }
    if (cfg->scsi) {
        scsi_delete(cfg->scsi);
        cfg->scsi = NULL;
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
    if (cfg->cpu) {
        cpu_delete(cfg->cpu);
        cfg->cpu = NULL;
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

// Saves an IIfx checkpoint.
static void iifx_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    iifx_state_t *st = iifx_state(cfg);
    memory_map_checkpoint(cfg->mem_map, cp);
    cpu_checkpoint(cfg->cpu, cp);
    scheduler_checkpoint(cfg->scheduler, cp);
    system_write_checkpoint_data(cp, &cfg->irq, sizeof(cfg->irq));
    rtc_checkpoint(cfg->rtc, cp);
    scc_checkpoint(cfg->scc, cp);
    via_checkpoint(cfg->via1, cp);
    glue030_checkpoint_save_images(cfg, cp);
    scsi_checkpoint(cfg->scsi, cp);
    asc_checkpoint(st->asc, cp);
    adb_checkpoint(st->adb, cp);
    floppy_checkpoint(st->floppy, cp);
    oss_checkpoint(st->oss, cp);
    iop_checkpoint(st->scc_iop, cp);
    iop_checkpoint(st->swim_iop, cp);
    system_write_checkpoint_data(cp, &st->scsi_dma_ctrl, sizeof(st->scsi_dma_ctrl));
    system_write_checkpoint_data(cp, &st->scsi_dma_count, sizeof(st->scsi_dma_count));
    system_write_checkpoint_data(cp, &st->scsi_dma_addr, sizeof(st->scsi_dma_addr));
    system_write_checkpoint_data(cp, &st->scsi_dma_watchdog_reload, sizeof(st->scsi_dma_watchdog_reload));
    system_write_checkpoint_data(cp, &st->scsi_dma_fifo_word, sizeof(st->scsi_dma_fifo_word));
    system_write_checkpoint_data(cp, &st->mmu->tc, sizeof(st->mmu->tc));
    system_write_checkpoint_data(cp, &st->mmu->crp, sizeof(st->mmu->crp));
    system_write_checkpoint_data(cp, &st->mmu->srp, sizeof(st->mmu->srp));
    system_write_checkpoint_data(cp, &st->mmu->tt0, sizeof(st->mmu->tt0));
    system_write_checkpoint_data(cp, &st->mmu->tt1, sizeof(st->mmu->tt1));
    system_write_checkpoint_data(cp, &st->mmu->mmusr, sizeof(st->mmu->mmusr));
    system_write_checkpoint_data(cp, &st->mmu->enabled, sizeof(st->mmu->enabled));
}

// Machine descriptor data.
static const uint32_t iifx_ram_options_kb[] = {4096, 8192, 16384, 32768, 65536, 131072, 0};

static const struct floppy_slot iifx_floppy_slots[] = {
    {.label = "Internal FD0", .kind = FLOPPY_HD},
    {.label = "External FD1", .kind = FLOPPY_HD},
    {0},
};

static const struct scsi_slot iifx_scsi_slots[] = {
    {.label = "SCSI HD0", .id = 0},
    {.label = "SCSI HD1", .id = 1},
    {0},
};

const hw_profile_t machine_iifx = {
    .name = "Macintosh IIfx",
    .id = "iifx",

    .cpu_model = 68030,
    .freq = 40000000,
    .mmu_present = true,
    .fpu_present = true,

    .address_bits = 32,
    .ram_default = 0x800000,
    .ram_max = 0x8000000,
    .rom_size = 0x080000,

    .ram_options = iifx_ram_options_kb,
    .floppy_slots = iifx_floppy_slots,
    .scsi_slots = iifx_scsi_slots,
    .has_cdrom = true,
    .cdrom_id = 3,
    .needs_vrom = true,

    .via_count = 1,
    .has_adb = true,
    .has_nubus = true,
    .nubus_slot_count = 6,
    .nubus_slots = iifx_slots,

    .init = iifx_init,
    .reset = iifx_reset,
    .teardown = iifx_teardown,
    .checkpoint_save = iifx_checkpoint_save,
    .checkpoint_restore = NULL,
    .memory_layout_init = iifx_memory_layout_init,
    .update_ipl = iifx_update_ipl,
    .trigger_vbl = iifx_trigger_vbl,
    .display = NULL,
};
