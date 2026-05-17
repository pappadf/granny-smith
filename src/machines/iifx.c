// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iifx.c
// Macintosh IIfx machine implementation.

#include "machine.h"
#include "system_config.h"

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
#include "memory_internal.h"
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

// SCSI DMA register offsets.
#define SCSIDMA_DCTRL   0x080
#define SCSIDMA_DCNT    0x0c0
#define SCSIDMA_DADDR   0x100
#define SCSIDMA_DTIME   0x140
#define SCSIDMA_TEST    0x180
#define SCSIDMA_INTREN  0x0002
#define SCSIDMA_RESET   0x0010
#define SCSIDMA_ARBIDEN 0x1000
#define SCSIDMA_WONARB  0x2000

// Per-machine IIfx runtime state.
typedef struct iifx_state {
    struct asc *asc;
    struct floppy *floppy;
    oss_t *oss;
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

    uint32_t scsi_dma_ctrl;
    uint32_t scsi_dma_count;
    uint32_t scsi_dma_addr;
    uint32_t scsi_dma_timer;
    uint32_t scsi_dma_test;

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

// Reads one SCSI DMA wrapper byte.
static uint8_t iifx_scsidma_read_uint8(config_t *cfg, uint32_t offset) {
    iifx_state_t *st = iifx_state(cfg);
    uint32_t off = offset & 0x1fff;
    if (off < 0x80)
        return st->scsi_iface->read_uint8(cfg->scsi, off);
    if (off >= SCSIDMA_DCTRL && off < SCSIDMA_DCTRL + 4) {
        uint32_t value = st->scsi_dma_ctrl;
        if (value & SCSIDMA_ARBIDEN)
            value |= SCSIDMA_WONARB;
        return iifx_read_reg32_byte(value, off);
    }
    if (off >= SCSIDMA_DCNT && off < SCSIDMA_DCNT + 4)
        return iifx_read_reg32_byte(st->scsi_dma_count, off);
    if (off >= SCSIDMA_DADDR && off < SCSIDMA_DADDR + 4)
        return iifx_read_reg32_byte(st->scsi_dma_addr, off);
    if (off >= SCSIDMA_DTIME && off < SCSIDMA_DTIME + 4)
        return iifx_read_reg32_byte(st->scsi_dma_timer, off);
    if (off >= SCSIDMA_TEST && off < SCSIDMA_TEST + 4)
        return iifx_read_reg32_byte(st->scsi_dma_test, off);
    return 0;
}

// Writes one SCSI DMA wrapper byte.
static void iifx_scsidma_write_uint8(config_t *cfg, uint32_t offset, uint8_t value) {
    iifx_state_t *st = iifx_state(cfg);
    uint32_t off = offset & 0x1fff;
    if (off < 0x80) {
        st->scsi_iface->write_uint8(cfg->scsi, off, value);
        return;
    }
    if (off >= SCSIDMA_DCTRL && off < SCSIDMA_DCTRL + 4) {
        iifx_write_reg32_byte(&st->scsi_dma_ctrl, off, value);
        if (st->scsi_dma_ctrl & SCSIDMA_RESET) {
            st->scsi_dma_ctrl = SCSIDMA_INTREN;
            st->scsi_dma_count = 0;
        }
        return;
    }
    if (off >= SCSIDMA_DCNT && off < SCSIDMA_DCNT + 4) {
        iifx_write_reg32_byte(&st->scsi_dma_count, off, value);
        return;
    }
    if (off >= SCSIDMA_DADDR && off < SCSIDMA_DADDR + 4) {
        iifx_write_reg32_byte(&st->scsi_dma_addr, off, value);
        return;
    }
    if (off >= SCSIDMA_DTIME && off < SCSIDMA_DTIME + 4) {
        iifx_write_reg32_byte(&st->scsi_dma_timer, off, value);
        return;
    }
    if (off >= SCSIDMA_TEST && off < SCSIDMA_TEST + 4)
        iifx_write_reg32_byte(&st->scsi_dma_test, off, value);
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

    st->via1_iface = via_get_memory_interface(cfg->via1);
    st->scc_iface = scc_get_memory_interface(cfg->scc);
    st->scsi_iface = scsi_get_memory_interface(cfg->scsi);
    st->asc_iface = asc_get_memory_interface(st->asc);
    st->floppy_iface = floppy_get_memory_interface(st->floppy);

    st->oss = oss_init(iifx_oss_irq_changed, iifx_oss_control, cfg, checkpoint);
    st->oss_iface = oss_get_memory_interface(st->oss);
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
    cfg->cpu->mmu = st->mmu;
    st->mmu->tt1 = 0xF00F8043;

    cfg->nubus = nubus_init(cfg, iifx_slots, checkpoint);
    memory_set_bus_error_range(cfg->mem_map, 0xF9000000, 0xFEFFFFFF);

    iifx_memory_layout_init(cfg);

    if (checkpoint) {
        system_read_checkpoint_data(checkpoint, &st->scsi_dma_ctrl, sizeof(st->scsi_dma_ctrl));
        system_read_checkpoint_data(checkpoint, &st->scsi_dma_count, sizeof(st->scsi_dma_count));
        system_read_checkpoint_data(checkpoint, &st->scsi_dma_addr, sizeof(st->scsi_dma_addr));
        system_read_checkpoint_data(checkpoint, &st->scsi_dma_timer, sizeof(st->scsi_dma_timer));
        system_read_checkpoint_data(checkpoint, &st->scsi_dma_test, sizeof(st->scsi_dma_test));
        system_read_checkpoint_data(checkpoint, &st->mmu->tc, sizeof(st->mmu->tc));
        system_read_checkpoint_data(checkpoint, &st->mmu->crp, sizeof(st->mmu->crp));
        system_read_checkpoint_data(checkpoint, &st->mmu->srp, sizeof(st->mmu->srp));
        system_read_checkpoint_data(checkpoint, &st->mmu->tt0, sizeof(st->mmu->tt0));
        system_read_checkpoint_data(checkpoint, &st->mmu->tt1, sizeof(st->mmu->tt1));
        system_read_checkpoint_data(checkpoint, &st->mmu->mmusr, sizeof(st->mmu->mmusr));
        system_read_checkpoint_data(checkpoint, &st->mmu->enabled, sizeof(st->mmu->enabled));
        mmu_invalidate_tlb(st->mmu);
        g_mmu = st->mmu;
        cfg->cpu->mmu = st->mmu;
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
    floppy_checkpoint(st->floppy, cp);
    oss_checkpoint(st->oss, cp);
    iop_checkpoint(st->scc_iop, cp);
    iop_checkpoint(st->swim_iop, cp);
    system_write_checkpoint_data(cp, &st->scsi_dma_ctrl, sizeof(st->scsi_dma_ctrl));
    system_write_checkpoint_data(cp, &st->scsi_dma_count, sizeof(st->scsi_dma_count));
    system_write_checkpoint_data(cp, &st->scsi_dma_addr, sizeof(st->scsi_dma_addr));
    system_write_checkpoint_data(cp, &st->scsi_dma_timer, sizeof(st->scsi_dma_timer));
    system_write_checkpoint_data(cp, &st->scsi_dma_test, sizeof(st->scsi_dma_test));
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
