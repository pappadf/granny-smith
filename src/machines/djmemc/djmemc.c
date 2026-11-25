// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// djmemc.c
// The djMEMC family substrate (Centris 610/650, Quadra 610/650/800, and the
// MEMCjr boards LC 475/575, Quadra 605) -- see djmemc.h.  Implements the
// family lifecycle and the pieces of this generation: the djMEMC and MEMCjr
// bank decodes and register files, the ROM-at-zero overlay, the CPU-ID
// register, the Kiwi NuBus controller's register file, and the I/O island
// decode tables run on the shared board_io engine.

#include "djmemc.h"

#include "config_seed.h"
#include "mac_host_io.h"
#include "machine_checkpoint.h"
#include "machine_teardown.h"
#include "mmu040.h"
#include "regfile.h"
#include "slot_irq.h"

#include "adb.h"
#include "asc.h"
#include "checkpoint.h"
#include "cpu.h"
#include "cpu_internal.h" // cpu->mmu (the 040 walker the bus resolver serves)
#include "cuda.h"
#include "dafb.h"
#include "dma_mem.h"
#include "floppy.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "rtc.h"
#include "scc.h"
#include "scheduler.h"
#include "scsi.h"
#include "scsi_53c96.h"
#include "sonic.h"
#include "via.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

// One-level family: the profile's board IS the descriptor.
static inline const djmemc_board_desc_t *djmemc_board(config_t *cfg) {
    return (const djmemc_board_desc_t *)cfg->machine->board;
}

// True on the MEMCjr boards.
static inline bool djmemc_is_jr(config_t *cfg) {
    return djmemc_board(cfg)->chip == DJMEMC_CHIP_MEMCJR;
}

// ============================================================
// RAM banks
// ============================================================

// MEMCjr: the soldered bank 0, then the one SIMM socket's sides in banks 1
// and 2.  The 1, 4 and 16 MB SIMMs are single-sided, the 2, 8 and 32 MB
// ones two sides of half that.
static bool memcjr_ram_banks(const djmemc_board_desc_t *desc, uint32_t ram, uint32_t size[DJMEMC_MAX_BANKS]) {
    if (ram < desc->ram_onboard)
        return false;
    size[0] = desc->ram_onboard;
    uint32_t simm = ram - desc->ram_onboard;
    if (!simm)
        return true;
    if (!desc->simm_slots || simm > 0x02000000u || (simm & (simm - 1)))
        return false;
    // 1, 4, 16 MB: one side (an even power of two in MB); 2, 8, 32 MB: two.
    bool one_side = ((__builtin_ctz(simm) - 20) & 1) == 0;
    size[1] = one_side ? simm : simm / 2;
    size[2] = one_side ? 0 : simm / 2;
    return true;
}

bool djmemc_ram_banks(const djmemc_board_desc_t *desc, uint32_t ram, uint32_t size[DJMEMC_MAX_BANKS]) {
    // A SIMM, largest first: its bytes and how many 4/16 MB banks it fills.
    static const struct {
        uint32_t bytes, bank;
    } simm[] = {
        {0x02000000u, 0x01000000u}, // 32 MB: two 16 MB banks
        {0x01000000u, 0x01000000u}, // 16 MB: one 16 MB bank
        {0x00800000u, 0x00400000u}, //  8 MB: two  4 MB banks
        {0x00400000u, 0x00400000u}, //  4 MB: one  4 MB bank
    };
    for (int i = 0; i < DJMEMC_MAX_BANKS; i++)
        size[i] = 0;
    if (desc->chip == DJMEMC_CHIP_MEMCJR)
        return memcjr_ram_banks(desc, ram, size);
    if (ram < desc->ram_onboard)
        return false;
    // The soldered RAM is banks 0 and 1, 4 MB each.
    uint32_t left = ram - desc->ram_onboard;
    for (uint32_t b = 0; b * 0x00400000u < desc->ram_onboard && b < 2; b++)
        size[b] = 0x00400000u;
    for (int slot = 0; slot < desc->simm_slots && left; slot++) {
        for (size_t k = 0; k < sizeof simm / sizeof simm[0]; k++) {
            if (simm[k].bytes > left)
                continue;
            size[2 + 2 * slot] = simm[k].bank;
            if (simm[k].bytes > simm[k].bank)
                size[3 + 2 * slot] = simm[k].bank;
            left -= simm[k].bytes;
            break;
        }
    }
    return left == 0;
}

// Work out the bank population and where each bank lives in the flat RAM
// image (banks in order, packed).  A total that is no population of the
// board is presented as one bank, for the ROM to judge.
static void djmemc_bank_layout(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    if (!djmemc_ram_banks(djmemc_board(cfg), cfg->ram_size, st->bank_size)) {
        LOG(1, "%s: %u KB is no SIMM population of this board; one bank", djmemc_is_jr(cfg) ? "MEMCjr" : "djMEMC",
            cfg->ram_size >> 10);
        for (int b = 0; b < DJMEMC_MAX_BANKS; b++)
            st->bank_size[b] = 0;
        st->bank_size[0] = cfg->ram_size;
    }
    uint32_t off = 0;
    for (int b = 0; b < DJMEMC_MAX_BANKS; b++) {
        st->bank_image_off[b] = off;
        off += st->bank_size[b];
        if (st->bank_size[b])
            LOG(2, "djMEMC bank %d: %u MB at image offset $%08X", b, st->bank_size[b] >> 20, st->bank_image_off[b]);
    }
}

// The start address bank `b`'s configuration register places it at: the
// djMEMC's 8-bit 4 MB field, or the MEMCjr's six register bits under the
// four the latch gave that bank, in 1 MB units.
static uint32_t djmemc_bank_start(const djmemc_board_desc_t *desc, const djmemc_state_t *st, int b) {
    uint32_t reg = st->memc_regs[(DJMEMC_REG_BANK0 >> 2) + b];
    if (desc->chip == DJMEMC_CHIP_MEMCJR) {
        if (b >= MEMCJR_MAX_BANKS)
            return 0;
        uint32_t mb =
            ((uint32_t)(st->jr_bank_hi[b] & MEMCJR_LATCH_HI_MSK) << MEMCJR_LATCH_HI_SHIFT) | (reg & MEMCJR_BANK_LO_MSK);
        return mb * MEMCJR_BANK_UNIT;
    }
    return (reg & DJMEMC_BANK_START_MSK) * DJMEMC_BANK_UNIT;
}

// The physical span the bank registers can place RAM in: ten 64 MB windows.
#define DJMEMC_RAM_SPAN (DJMEMC_MAX_BANKS * DJMEMC_BANK_WINDOW)

// Apply the bank configuration registers to the page table.
//
// A populated bank decodes from its start address up to whichever populated
// bank starts next above it, or 64 MB if none does, and mirrors its size
// through that span -- the boot ROM sizes a bank by parking it at a 64 MB
// boundary and finding where its image repeats.  Every page of the span is
// cleared first: a bank that moves (the ROM parks the banks, sizes them,
// then stitches them contiguous) must not leave its old image behind.
static void djmemc_map_ram(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    const djmemc_board_desc_t *desc = djmemc_board(cfg);
    uint8_t *ram_base = ram_native_pointer(cfg->memory_map, 0);
    uint32_t span_pages = DJMEMC_RAM_SPAN >> MEM_PAGE_SHIFT;
    for (uint32_t p = 0; p < span_pages && p < g_page_count; p++)
        page_clear(p);

    for (int b = 0; b < DJMEMC_MAX_BANKS; b++) {
        uint32_t size = st->bank_size[b];
        if (!size)
            continue;
        uint32_t start = djmemc_bank_start(desc, st, b);
        uint32_t end = start + DJMEMC_BANK_WINDOW;
        for (int o = 0; o < DJMEMC_MAX_BANKS; o++) {
            uint32_t os = djmemc_bank_start(desc, st, o);
            if (o != b && st->bank_size[o] && os > start && os < end)
                end = os;
        }
        if (end > DJMEMC_RAM_SPAN)
            end = DJMEMC_RAM_SPAN;
        if (start >= end)
            continue;
        m68k_board_map_mirrored(start >> MEM_PAGE_SHIFT, (end - start) >> MEM_PAGE_SHIFT,
                                ram_base + st->bank_image_off[b], size >> MEM_PAGE_SHIFT, page_fill_68k, true);
    }
    if (st->bus_mmu)
        mmu_invalidate_tlb(st->bus_mmu);
}

// The power-up bank placement: bank n on the n-th 64 MB boundary, the same
// split the ROM programs before it sizes.  So whatever the ROM touches
// before sizing finds bank 0 at zero.
static void djmemc_reset_regs(const djmemc_board_desc_t *desc, djmemc_state_t *st) {
    memset(st->memc_regs, 0, sizeof st->memc_regs);
    memset(st->jr_bank_hi, 0, sizeof st->jr_bank_hi);
    if (desc->chip == DJMEMC_CHIP_MEMCJR) {
        // 64 MB is 64 units: the register's six bits clear, the latch's n.
        for (int b = 0; b < MEMCJR_MAX_BANKS; b++)
            st->jr_bank_hi[b] = (uint8_t)b;
        st->memc_regs[MEMCJR_REG_LATCH >> 2] = 0xFFu;
    } else {
        for (int b = 0; b < DJMEMC_MAX_BANKS; b++)
            st->memc_regs[(DJMEMC_REG_BANK0 >> 2) + b] = 0x100u + (uint32_t)b * 0x10u;
    }
    st->memc_regs[DJMEMC_REG_MEMTOP >> 2] = 0xFFu;
}

// MEMCjr: a register read loads the latch with the register's bits 6-11.
static void memcjr_latch_load(djmemc_state_t *st, uint32_t value) {
    st->memc_regs[MEMCJR_REG_LATCH >> 2] = (value >> MEMCJR_LATCH_HI_SHIFT) & MEMCJR_LATCH_MSK;
}

// The register a window offset decodes to: the djMEMC repeats its sixteen
// longwords through $7F, the MEMCjr decodes thirty-two.
static uint32_t djmemc_reg_index(config_t *cfg, uint32_t win_off) {
    uint32_t count = djmemc_is_jr(cfg) ? MEMCJR_REG_COUNT : DJMEMC_REG_COUNT;
    return (win_off >> 2) & (count - 1);
}

// ============================================================
// djMEMC register window ($0E000)
// ============================================================

uint8_t djmemc_reg_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    djmemc_state_t *st = djmemc_st(cfg);
    uint32_t idx = djmemc_reg_index(cfg, win_off);
    uint32_t v = st->memc_regs[idx];
    // A MEMCjr register read hands its bits 6-11 to the latch (the read
    // completes on the low lane).
    if (djmemc_is_jr(cfg) && idx != (MEMCJR_REG_LATCH >> 2) && (win_off & 3u) == 3u)
        memcjr_latch_load(st, v);
    // Interleave register bits 9-11 are the chip's version.  Version 0 is
    // the part the ROM does not enable interleaving on, which keeps the bank
    // map linear (interleaving scrambles it, and is a timing feature only).
    if (idx == (DJMEMC_REG_INTERLEAVE >> 2) && !djmemc_is_jr(cfg))
        v &= ~0x0E00u;
    return be_lane8(v, win_off & 3u);
}

void djmemc_reg_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)addr;
    djmemc_state_t *st = djmemc_st(cfg);
    bool jr = djmemc_is_jr(cfg);
    uint32_t idx = djmemc_reg_index(cfg, win_off);
    be_lane8_set(&st->memc_regs[idx], win_off & 3u, value);
    // A bank register's start field is its low byte: the longword is
    // complete when lane 3 lands, and the bank moves then.
    int nbanks = jr ? MEMCJR_MAX_BANKS : DJMEMC_MAX_BANKS;
    bool bank = idx >= (DJMEMC_REG_BANK0 >> 2) && idx < (DJMEMC_REG_BANK0 >> 2) + (uint32_t)nbanks;
    if ((win_off & 3u) == 3u) {
        LOG(2, "%s $%02X = $%08X (pc=%08X)", jr ? "MEMCjr" : "djMEMC", idx << 2, st->memc_regs[idx],
            cpu_get_pc(cfg->cpu));
        // A MEMCjr bank takes its high start bits from the latch as it lands.
        if (bank && jr)
            st->jr_bank_hi[idx - (DJMEMC_REG_BANK0 >> 2)] =
                (uint8_t)(st->memc_regs[MEMCJR_REG_LATCH >> 2] & MEMCJR_LATCH_HI_MSK);
        if (bank)
            djmemc_map_ram(cfg);
    }
}

// ============================================================
// Kiwi NuBus controller ($28000)
// ============================================================
// Software-compatible with the Quadra 700's YANCC (Developer Note, "Kiwi
// IC"): its control/status bits are one per longword, bit n in bit 0 of
// the long at n*4.  Bit 7 is the write-buffer error flag and bit 10 its
// enable; with both set the controller requests level 7.  Nothing on the
// modelled NuBus reports a write-buffer error, so only software sets the
// flag.

#define DJMEMC_KIWI_BIT_ERR    7
#define DJMEMC_KIWI_BIT_ERR_EN 10

static bool djmemc_kiwi_err_irq(const djmemc_state_t *st) {
    return (st->kiwi_regs[DJMEMC_KIWI_BIT_ERR] & 1u) && (st->kiwi_regs[DJMEMC_KIWI_BIT_ERR_EN] & 1u);
}

static uint8_t djmemc_kiwi_access(config_t *cfg, uint32_t win_off, bool peek) {
    djmemc_state_t *st = djmemc_st(cfg);
    uint32_t idx = (win_off >> 2) % DJMEMC_KIWI_REG_COUNT;
    if (!peek && !(st->kiwi_touched & (1ull << idx))) {
        st->kiwi_touched |= 1ull << idx;
        LOG(2, "Kiwi read  $%04X -> $%08X (pc=%08X)", win_off, st->kiwi_regs[idx], cpu_get_pc(cfg->cpu));
    }
    return be_lane8(st->kiwi_regs[idx], win_off & 3u);
}

uint8_t djmemc_kiwi_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    return djmemc_kiwi_access(cfg, win_off, false);
}

uint8_t djmemc_kiwi_peek(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    return djmemc_kiwi_access(cfg, win_off, true);
}

void djmemc_kiwi_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)addr;
    djmemc_state_t *st = djmemc_st(cfg);
    uint32_t idx = (win_off >> 2) % DJMEMC_KIWI_REG_COUNT;
    bool was = djmemc_kiwi_err_irq(st);
    be_lane8_set(&st->kiwi_regs[idx], win_off & 3u, value);
    st->kiwi_touched |= 1ull << idx;
    bool now = djmemc_kiwi_err_irq(st);
    if (now != was)
        m68k_board_update_ipl(cfg, M68K_BOARD_IRQ_NMI, now);
}

// ============================================================
// SONIC ($0A000) and its MAC-address PROM ($08000)
// ============================================================
// The same presentation as the Quadra 700's (mcu.c): station address bytes
// bit-reversed, the XOR of all eight bytes $FF, the eight bytes repeating
// through the window.  08:00:07:09:07:02 -- Apple's OUI, one past the
// Quadra 700's, so two emulated Quadras on one network differ.
static const uint8_t djmemc_mac_prom[8] = {0x10, 0x00, 0xE0, 0x90, 0xE0, 0x40, 0x00, 0x3F};

uint8_t djmemc_prom_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)cfg;
    (void)addr;
    return djmemc_mac_prom[win_off & 7u];
}

void djmemc_prom_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)cfg;
    (void)addr;
    LOG(2, "MAC PROM write $%X = $%02X ignored (read-only)", win_off & 7u, value);
}

// 16-bit SONIC registers on 4-byte spacing, the value in the low half of
// the slot; a write commits when byte 3 lands (mcu.c has the full note).
uint8_t djmemc_sonic_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    djmemc_state_t *st = djmemc_st(cfg);
    uint32_t off = win_off & 0xFFFu;
    if ((off & 3u) < 2)
        return 0;
    uint16_t v = sonic_reg_read(st->sonic, off >> 2);
    return (off & 3u) == 2 ? (uint8_t)(v >> 8) : (uint8_t)v;
}

void djmemc_sonic_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)addr;
    djmemc_state_t *st = djmemc_st(cfg);
    uint32_t off = win_off & 0xFFFu;
    if ((off & 3u) == 2)
        st->sonic_byte2 = value;
    else if ((off & 3u) == 3)
        sonic_reg_write(st->sonic, off >> 2, (uint16_t)((st->sonic_byte2 << 8) | value));
}

// ============================================================
// IOSB rows: configuration registers and VIA2's DREQ flag
// ============================================================

static uint8_t djmemc_iosb_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    return iosb_reg_read(&djmemc_st(cfg)->iosb, win_off);
}

static void djmemc_iosb_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)addr;
    iosb_reg_write(&djmemc_st(cfg)->iosb, win_off, value);
}

// VIA2's IFR register (one $200 register slot): the 6522's flags with the
// 53C96 DREQ in bit 0.  Writes go straight to the 6522.
static uint8_t djmemc_via2_ifr_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off;
    (void)addr;
    const memory_interface_t *vi = via_get_memory_interface(cfg->via2);
    uint8_t ifr = vi->read_uint8(cfg->via2, IOSB_VIA2_IFR_OFF);
    return iosb_via2_ifr(&djmemc_st(cfg)->iosb, ifr);
}

static uint8_t djmemc_via2_ifr_peek(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off;
    (void)addr;
    return iosb_via2_ifr(&djmemc_st(cfg)->iosb, via_get_ifr(cfg->via2));
}

static void djmemc_via2_ifr_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off;
    (void)addr;
    const memory_interface_t *vi = via_get_memory_interface(cfg->via2);
    vi->write_uint8(cfg->via2, IOSB_VIA2_IFR_OFF, value);
}

// ============================================================
// I/O island decode ($50000000, 256 KiB, mirrored)
// ============================================================
// Apple, Macintosh Centris 610, Centris 650, and Quadra 800 Developer Note
// (1993), Figure 2-4 "Addresses of I/O devices", p. 36.  Penalties follow
// the Quadra 700's until the IOSB's wait states are measured.

#define DJMEMC_VIA_IO_PENALTY  16
#define DJMEMC_IO_PENALTY      2
#define DJMEMC_SWIM_IO_PENALTY 5

//   base     end      device            penalty               xform              rd wr  rd_fn/wr_fn  name
const board_io_range_t djmemc_io_ranges[] = {
    {0x00000, 0x02000, BOARD_DEV_VIA1, DJMEMC_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    // VIA2's IFR slot first: the narrow row wins (first match).
    {0x03A00, 0x03C00, 0, DJMEMC_VIA_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, djmemc_via2_ifr_read, djmemc_via2_ifr_write,
     "via2_ifr", .esync = 1, .peek_fn = djmemc_via2_ifr_peek},
    {0x02000, 0x04000, BOARD_DEV_VIA2, DJMEMC_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via2", .esync = 1},
    {0x08000, 0x0A000, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, djmemc_prom_read, djmemc_prom_write, "enet_prom"},
    {0x0A000, 0x0B100, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, djmemc_sonic_read, djmemc_sonic_write, "sonic"},
    {0x0C000, 0x0E000, BOARD_DEV_SCC, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x0E000, 0x10000, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, djmemc_reg_read, djmemc_reg_write, "djmemc"},
    {0x10000, 0x10100, BOARD_DEV_SCSI96, DJMEMC_IO_PENALTY, BOARD_IO_STRIDE_16, 0, 0, NULL, NULL, "scsi_53c96"},
    // The pseudo-DMA port: the SCSI Manager moves longwords through it.
    {0x10100, 0x10200, BOARD_DEV_SCSI96, DJMEMC_IO_PENALTY, BOARD_IO_FIXED, SCSI_53C96_PDMA_SUB, SCSI_53C96_PDMA_SUB,
     NULL, NULL, "scsi_pdma"},
    {0x14000, 0x16000, BOARD_DEV_ASC, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "sound"},
    {0x18000, 0x1A000, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, djmemc_iosb_read, djmemc_iosb_write, "iosb"},
    {0x1E000, 0x20000, BOARD_DEV_FLOPPY, DJMEMC_SWIM_IO_PENALTY, BOARD_IO_STRIDE_512, 0, 0, NULL, NULL, "swim2"},
    {0x28000, 0x2A000, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, djmemc_kiwi_read, djmemc_kiwi_write, "kiwi",
     .peek_fn = djmemc_kiwi_peek},
    {0}, // sentinel
};

// The communication slot's window (the LC 575's; the slot Apple's
// Ethernet card puts its SONIC at): with no card fitted nothing answers and
// the cycle bus-errors, which is how the ROM's probe finds the slot empty.
static uint8_t memcjr_commslot_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)win_off;
    memory_signal_bus_error(djmemc_st(cfg)->io.window_base + addr, false);
    return 0xFF;
}

static uint8_t memcjr_commslot_peek(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)cfg, (void)win_off, (void)addr;
    return 0xFF; // what the faulting read returns, without the fault
}

static void memcjr_commslot_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)win_off, (void)value;
    memory_signal_bus_error(djmemc_st(cfg)->io.window_base + addr, true);
}

// The MEMCjr boards' island: the same decode less the SONIC, its PROM and
// the Kiwi, which these boards do not carry (Apple, Macintosh LC 475 and
// Macintosh Quadra 605 Developer Note (1993), Figure 2-2, p. 27).
const board_io_range_t memcjr_io_ranges[] = {
    {0x00000, 0x02000, BOARD_DEV_VIA1, DJMEMC_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},
    {0x03A00, 0x03C00, 0, DJMEMC_VIA_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, djmemc_via2_ifr_read, djmemc_via2_ifr_write,
     "via2_ifr", .esync = 1, .peek_fn = djmemc_via2_ifr_peek},
    {0x02000, 0x04000, BOARD_DEV_VIA2, DJMEMC_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via2", .esync = 1},
    // An empty communication slot: no turnaround to charge, the cycle aborts.
    {0x0A000, 0x0C000, BOARD_DEV_VIA1, 0, BOARD_IO_NORMAL, 0, 0, memcjr_commslot_read, memcjr_commslot_write,
     "comm_slot", .berr = 1, .peek_fn = memcjr_commslot_peek},
    {0x0C000, 0x0E000, BOARD_DEV_SCC, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scc"},
    {0x0E000, 0x10000, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, djmemc_reg_read, djmemc_reg_write, "memcjr"},
    {0x10000, 0x10100, BOARD_DEV_SCSI96, DJMEMC_IO_PENALTY, BOARD_IO_STRIDE_16, 0, 0, NULL, NULL, "scsi_53c96"},
    {0x10100, 0x10200, BOARD_DEV_SCSI96, DJMEMC_IO_PENALTY, BOARD_IO_FIXED, SCSI_53C96_PDMA_SUB, SCSI_53C96_PDMA_SUB,
     NULL, NULL, "scsi_pdma"},
    {0x14000, 0x16000, BOARD_DEV_ASC, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "sound"},
    {0x18000, 0x1A000, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, djmemc_iosb_read, djmemc_iosb_write, "primetime"},
    {0x1E000, 0x20000, BOARD_DEV_FLOPPY, DJMEMC_SWIM_IO_PENALTY, BOARD_IO_STRIDE_512, 0, 0, NULL, NULL, "swim2"},
    {0}, // sentinel
};

static void djmemc_io_bind(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    board_io_t *io = &st->io;
    m68k_board_io_install(io, cfg, &djmemc_board(cfg)->common);
    board_io_bind_dev(io, BOARD_DEV_VIA1, cfg->via1, via_get_memory_interface(cfg->via1));
    board_io_bind_dev(io, BOARD_DEV_VIA2, cfg->via2, via_get_memory_interface(cfg->via2));
    board_io_bind_dev(io, BOARD_DEV_SCC, cfg->scc, scc_get_memory_interface(cfg->scc));
    board_io_bind_dev(io, BOARD_DEV_ASC, st->asc, asc_get_memory_interface(st->asc));
    board_io_bind_dev(io, BOARD_DEV_FLOPPY, cfg->floppy, floppy_get_memory_interface(cfg->floppy));
    board_io_bind_dev(io, BOARD_DEV_SCSI96, st->scsi96, scsi_53c96_get_memory_interface());
}

// ============================================================
// CPU-ID register ($5FFFFFFC)
// ============================================================

static uint8_t djmemc_cpuid_read8(void *ctx, uint32_t offset) {
    config_t *cfg = ctx;
    uint32_t id = djmemc_board(cfg)->cpuid ? djmemc_board(cfg)->cpuid : DJMEMC_CPUID_VALUE;
    if ((offset & 0xFFCu) == 0xFFCu)
        return be_lane8(id, offset & 3u);
    return 0xFF; // the rest of the page: nothing drives the bus
}

static uint16_t djmemc_cpuid_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((djmemc_cpuid_read8(ctx, offset) << 8) | djmemc_cpuid_read8(ctx, offset + 1));
}

static uint32_t djmemc_cpuid_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)djmemc_cpuid_read16(ctx, offset) << 16) | djmemc_cpuid_read16(ctx, offset + 2);
}

static void djmemc_cpuid_write8(void *ctx, uint32_t offset, uint8_t value) {
    (void)ctx;
    LOG(2, "CPU-ID write $%03X = $%02X ignored (read-only)", offset, value);
}

static void djmemc_cpuid_write16(void *ctx, uint32_t offset, uint16_t value) {
    djmemc_cpuid_write8(ctx, offset, (uint8_t)value);
}

static void djmemc_cpuid_write32(void *ctx, uint32_t offset, uint32_t value) {
    djmemc_cpuid_write8(ctx, offset, (uint8_t)value);
}

// ============================================================
// /SLOTIRQ aggregation
// ============================================================

void djmemc_slot_irq_source(config_t *cfg, int pa_bit, bool active) {
    slot_irq_source(cfg->via2, &djmemc_st(cfg)->slot_pa_mask, pa_bit, active);
}

// substrate.nubus_slot_irq: a card's /NMRQ lands on VIA2 PA(slot - 9).
static void djmemc_nubus_slot_irq(config_t *cfg, int slot, bool active) {
    int pa_bit = slot - 0x9;
    if (pa_bit < 1 || pa_bit > 5)
        return;
    djmemc_slot_irq_source(cfg, pa_bit, active);
}

// ============================================================
// VIA wiring and device interrupts
// ============================================================

// VIA1 outputs: PA5 = floppy head select; PB = the classic RTC serial
// (bits 0-2) and the ADB state lines (bits 4-5) -- the Quadra 700 wiring --
// or, on the MEMCjr boards, the Cuda handshake (PB3 TREQ in, PB4 BYTEACK,
// PB5 TIP out).
static void djmemc_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    djmemc_state_t *st = djmemc_st(cfg);
    if (port == 0) {
        if (cfg->floppy)
            floppy_set_sel_signal(cfg->floppy, (output & 0x20) != 0);
    } else if (st && djmemc_is_jr(cfg)) {
        cuda_via1_port_output(st->cuda, port, output);
    } else {
        if (cfg->adb)
            adb_port_b_output(cfg->adb, output);
        rtc_via1_pb_output(cfg->rtc, output);
    }
}

// VIA1 shift register out: a Cuda command byte (MEMCjr boards).
static void djmemc_via1_shift_out(void *context, uint8_t byte) {
    djmemc_state_t *st = djmemc_st((config_t *)context);
    cuda_via1_shift_input(st ? st->cuda : NULL, byte);
}

// VIA2 outputs: PB2 soft power, the rest unused by the model.
static void djmemc_via2_output(void *context, uint8_t port, uint8_t output) {
    (void)context;
    (void)port;
    (void)output;
}

// Sound interrupt -> VIA2 CB1 (active low).
static void djmemc_asc_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    via_input_c(cfg->via2, 1, 0, active ? 0 : 1);
}

// 53C96 INT -> VIA2 CB2.
static void djmemc_scsi96_irq(void *context, bool active) {
    config_t *cfg = (config_t *)context;
    via_input_c(cfg->via2, 1, 1, active ? 0 : 1);
}

// SONIC INT -> VIA2 PA0 through the /SLOTIRQ aggregate.
static void djmemc_sonic_irq(void *context, bool active) {
    djmemc_slot_irq_source((config_t *)context, 0, active);
}

// Built-in video interrupt -> VIA2 PA6 through the /SLOTIRQ aggregate.
static void djmemc_dafb_irq(void *context, bool active) {
    djmemc_slot_irq_source((config_t *)context, 6, active);
}

// SCSI DREQ, as VIA2 IFR bit 0 presents it.
static bool djmemc_scsi_dreq(void *context) {
    return scsi_53c96_dreq((scsi_53c96_t *)context);
}

// ============================================================
// MEMCjr video registers through the latch
// ============================================================
// The frame-buffer controller is the same DAFB-compatible block, but on the
// MEMCjr its registers are read six bits at a time: a read of the low half
// of a register loads the latch with bits 6-11, which the driver then reads
// from $50F0E07C.  Writes carry the full value (the driver writes the latch
// first and the whole value after), so they go straight through.

static uint32_t memcjr_video_read32(void *ctx, uint32_t addr) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    uint32_t v = st->dafb_iface->read_uint32(st->dafb, addr);
    memcjr_latch_load(st, v);
    return v;
}

static uint16_t memcjr_video_read16(void *ctx, uint32_t addr) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    uint16_t v = st->dafb_iface->read_uint16(st->dafb, addr);
    if (addr & 2u)
        memcjr_latch_load(st, v);
    return v;
}

static uint8_t memcjr_video_read8(void *ctx, uint32_t addr) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    return st->dafb_iface->read_uint8(st->dafb, addr);
}

static void memcjr_video_write8(void *ctx, uint32_t addr, uint8_t v) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    st->dafb_iface->write_uint8(st->dafb, addr, v);
}

static void memcjr_video_write16(void *ctx, uint32_t addr, uint16_t v) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    st->dafb_iface->write_uint16(st->dafb, addr, v);
}

static void memcjr_video_write32(void *ctx, uint32_t addr, uint32_t v) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    st->dafb_iface->write_uint32(st->dafb, addr, v);
}

static uint8_t memcjr_video_peek8(void *ctx, uint32_t addr) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    return memory_iface_read8(st->dafb_iface, st->dafb, addr, true);
}

static uint16_t memcjr_video_peek16(void *ctx, uint32_t addr) {
    return (uint16_t)((memcjr_video_peek8(ctx, addr) << 8) | memcjr_video_peek8(ctx, addr + 1));
}

static uint32_t memcjr_video_peek32(void *ctx, uint32_t addr) {
    return ((uint32_t)memcjr_video_peek16(ctx, addr) << 16) | memcjr_video_peek16(ctx, addr + 2);
}

// ============================================================
// Memory layout
// ============================================================

static void djmemc_memory_layout(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    const djmemc_board_desc_t *desc = djmemc_board(cfg);

    djmemc_bank_layout(cfg);

    // I/O island at $50000000; the published map repeats it through
    // $53FFFFFF.
    board_io_fill_interface(&st->io_interface);
    memory_map_add(cfg->memory_map, 0x50000000u, 0x04000000u, "I/O", &st->io_interface, &st->io);

    // CPU-ID register page at $5FFFF000 (the register is its last long).
    st->cpuid_interface.read_uint8 = djmemc_cpuid_read8;
    st->cpuid_interface.read_uint16 = djmemc_cpuid_read16;
    st->cpuid_interface.read_uint32 = djmemc_cpuid_read32;
    st->cpuid_interface.write_uint8 = djmemc_cpuid_write8;
    st->cpuid_interface.write_uint16 = djmemc_cpuid_write16;
    st->cpuid_interface.write_uint32 = djmemc_cpuid_write32;
    memory_map_add(cfg->memory_map, 0x5FFFF000u, 0x00001000u, "CPU-ID", &st->cpuid_interface, cfg);

    // Video: the frame-buffer controller's registers at $F9800000, VRAM
    // pages direct at $F9000000.
    if (desc->chip == DJMEMC_CHIP_MEMCJR) {
        st->dafb_iface = dafb_reg_interface(st->dafb);
        st->video_interface = (memory_interface_t){
            .read_uint8 = memcjr_video_read8,
            .read_uint16 = memcjr_video_read16,
            .read_uint32 = memcjr_video_read32,
            .write_uint8 = memcjr_video_write8,
            .write_uint16 = memcjr_video_write16,
            .write_uint32 = memcjr_video_write32,
            .peek_uint8 = memcjr_video_peek8,
            .peek_uint16 = memcjr_video_peek16,
            .peek_uint32 = memcjr_video_peek32,
        };
        memory_map_add(cfg->memory_map, DAFB_REG_BASE, DAFB_REG_APERTURE, "MEMCjr video regs", &st->video_interface,
                       cfg);
    } else {
        memory_map_add(cfg->memory_map, DAFB_REG_BASE, DAFB_REG_APERTURE, "djMEMC video regs",
                       (memory_interface_t *)dafb_reg_interface(st->dafb), st->dafb);
    }
    // The VRAM decode spans the whole 2 MB aperture whatever is fitted, the
    // installed VRAM repeating through it: PrimaryInit reads the top of
    // every possible VRAM size before it sizes (it must not bus-error), and
    // the repeat is what makes its write-then-compare sizing find the
    // fitted amount.
    uint8_t *vram = dafb_vram(st->dafb);
    m68k_board_map_mirrored(DAFB_VRAM_BASE >> MEM_PAGE_SHIFT, DAFB_VRAM_APERTURE >> MEM_PAGE_SHIFT, vram,
                            dafb_vram_size(st->dafb) >> MEM_PAGE_SHIFT, page_fill_68k, true);
    // ...and the same repeat for the bus resolver (the 040 walker and
    // translated accesses reach VRAM through it).
    for (uint32_t off = 0; off < DAFB_VRAM_APERTURE; off += dafb_vram_size(st->dafb))
        memory_map_host_region(cfg->memory_map, "djmemc_vram", vram, DAFB_VRAM_BASE + off, dafb_vram_size(st->dafb),
                               true);

    // The ROM-at-zero overlay: armed until the ROM aperture is touched.
    m68k_board_overlay_init(&st->overlay, cfg, desc->common.rom_base, desc->common.rom_end, djmemc_map_ram, "djMEMC");
    memory_map_add(cfg->memory_map, desc->common.rom_base, desc->common.rom_end - desc->common.rom_base, "ROM aperture",
                   &st->overlay.iface, &st->overlay);
    m68k_board_overlay_arm(&st->overlay);
}

// ============================================================
// Checkpoint: the substrate-private part
// ============================================================

#define DJMEMC_PRIVATE_CP_FIELDS(OP, cp, st)                                                                           \
    CP_ARRAY(OP, cp, (st)->memc_regs);                                                                                 \
    CP_ARRAY(OP, cp, (st)->kiwi_regs);                                                                                 \
    CP_FIELD(OP, cp, (st)->kiwi_touched);                                                                              \
    CP_FIELD(OP, cp, (st)->slot_pa_mask);                                                                              \
    CP_FIELD(OP, cp, (st)->sonic_byte2);                                                                               \
    CP_ARRAY(OP, cp, (st)->jr_bank_hi)

static void part_save_djmemc_private(void *obj, checkpoint_t *cp) {
    djmemc_state_t *st = obj;
    CP_FIELD(system_write_checkpoint_data, cp, st->overlay.armed);
    DJMEMC_PRIVATE_CP_FIELDS(system_write_checkpoint_data, cp, st);
    iosb_checkpoint(&st->iosb, cp);
}

static void djmemc_restore_private(config_t *cfg, checkpoint_t *cp) {
    djmemc_state_t *st = djmemc_st(cfg);
    bool overlay = false;
    CP_FIELD(system_read_checkpoint_data, cp, overlay);
    DJMEMC_PRIVATE_CP_FIELDS(system_read_checkpoint_data, cp, st);
    iosb_restore(&st->iosb, cp);
    if (overlay)
        m68k_board_overlay_arm(&st->overlay);
    else
        m68k_board_overlay_drop(&st->overlay); // maps RAM from the restored banks
    slot_irq_redrive(cfg->via2, st->slot_pa_mask);
    if (djmemc_kiwi_err_irq(st))
        m68k_board_update_ipl(cfg, M68K_BOARD_IRQ_NMI, true);
    mmu_invalidate_tlb(st->bus_mmu);
    via_redrive_outputs(cfg->via1);
}

// The DAFB's identity part: the monitor strapped to it.
static void part_save_djmemc_monitor(void *obj, checkpoint_t *cp) {
    uint8_t monitor = dafb_monitor(((djmemc_state_t *)obj)->dafb);
    system_write_checkpoint_data(cp, &monitor, sizeof monitor, "dafb.monitor");
}

MACHINE_PART_SAVE(dafb_checkpoint, dafb_t)

// ============================================================
// Device construction
// ============================================================

static int djmemc_build_devices(config_t *cfg, checkpoint_t *cp) {
    djmemc_state_t *st = djmemc_st(cfg);
    const djmemc_board_desc_t *desc = djmemc_board(cfg);

    bool jr = desc->chip == DJMEMC_CHIP_MEMCJR;
    if (!jr)
        rtc_set_via(cfg->rtc, cfg->via1);

    // VIA1 port A: the identity straps, PA7 and PA0 (the diagnostic strap)
    // high -- see mcu.h on PA0.
    uint8_t pa = (uint8_t)((desc->via1_id & DJMEMC_VIA1_ID_MASK) | 0x81u);
    for (int bit = 0; bit < 8; bit++)
        via_input(cfg->via1, 0, bit, (pa >> bit) & 1);
    via_input_c(cfg->via1, 0, 0, 1);
    via_input_c(cfg->via1, 1, 0, 1);
    // The Cuda's TREQ (VIA1 PB3) idles high.
    if (jr)
        via_input(cfg->via1, 1, 3, 1);

    // VIA2 port A: slot/video/Ethernet requests, active low, idle high.
    for (int bit = 0; bit < 8; bit++)
        via_input(cfg->via2, 0, bit, 1);
    via_input_c(cfg->via2, 0, 0, 1);
    via_input_c(cfg->via2, 1, 0, 1);

    // ADB: the VIA shifter and transceiver on the djMEMC boards; on the
    // MEMCjr ones the Cuda services it in packets (the VIA is not the ADB's).
    machine_part_begin(cfg, cp, "adb");
    cfg->adb = adb_init(jr ? NULL : cfg->via1, cfg->scheduler, cp);
    machine_part(cfg, cp, "adb", part_save_adb, cfg->adb);
    if (jr) {
        machine_part_begin(cfg, cp, "cuda");
        st->cuda = cuda_init(cfg->via1, cfg->rtc, cfg->adb, cfg->scheduler, cp, /*mode3_clock=*/false);
        if (!st->cuda) {
            LOG(0, "Error: out of memory constructing the Cuda");
            return -1;
        }
        machine_part(cfg, cp, "cuda", part_save_cuda, st->cuda);
    }

    machine_part_images(cfg, cp);

    // One 53C96 for the internal and external connectors.
    machine_part_begin(cfg, cp, "scsi");
    cfg->scsi = machine_scsi_bus_init(cfg, cp, "scsi");
    machine_part(cfg, cp, "scsi", part_save_scsi, cfg->scsi);
    machine_part_begin(cfg, cp, "scsi96");
    st->scsi96 = scsi_53c96_init(cfg->scheduler, 25000000, cp);
    machine_part(cfg, cp, "scsi96", part_save_scsi96, st->scsi96);
    scsi_53c96_set_irq_callback(st->scsi96, djmemc_scsi96_irq, cfg);
    scsi_53c96_attach_bus(st->scsi96, cfg->scsi);

    // The IOSB: its registers, and the DREQ it shows in VIA2's IFR.
    iosb_init(&st->iosb, cfg->machine->freq >= 30000000u);
    iosb_set_dreq(&st->iosb, djmemc_scsi_dreq, st->scsi96);

    // The SONIC (no wire), bus-mastering through the guest-physical port.
    // The MEMCjr boards have no Ethernet.
    if (!jr) {
        machine_part_begin(cfg, cp, "sonic");
        st->sonic = sonic_init(cp);
        machine_part(cfg, cp, "sonic", part_save_sonic, st->sonic);
        sonic_set_irq_callback(st->sonic, djmemc_sonic_irq, cfg);
        sonic_set_memory_port(st->sonic, &dma_mem_port_physical);
    }

    machine_part_begin(cfg, cp, "asc");
    st->asc = asc_init(NULL, cfg->scheduler, cp);
    machine_part(cfg, cp, "asc", part_save_asc, st->asc);
    asc_set_mix(st->asc, ASC_MIX_CH_A);
    asc_set_irq_handler(st->asc, djmemc_asc_irq, cfg);

    machine_part_begin(cfg, cp, "floppy");
    cfg->floppy =
        floppy_init(FLOPPY_TYPE_SWIM, NULL, cfg->scheduler, machine_floppy_count(cfg), cp, config_images(cfg));
    machine_part(cfg, cp, "floppy", part_save_floppy, cfg->floppy);

    // The frame-buffer controller and its monitor strap.
    uint8_t monitor = cfg->build_opts.builtin_sense;
    machine_part_begin(cfg, cp, "dafb.monitor");
    if (cp)
        system_read_checkpoint_data(cp, &monitor, sizeof monitor, "dafb.monitor");
    machine_part(cfg, cp, "dafb.monitor", part_save_djmemc_monitor, st);
    machine_part_begin(cfg, cp, "dafb");
    st->dafb = dafb_init(desc->vram_size, monitor, cp);
    if (!st->dafb) {
        LOG(0, "Error: out of memory constructing the djMEMC video");
        return -1;
    }
    machine_part(cfg, cp, "dafb", dafb_checkpoint_part, st->dafb);
    dafb_attach_scheduler(st->dafb, cfg->scheduler, cp);
    dafb_set_irq_callback(st->dafb, djmemc_dafb_irq, cfg);
    dafb_attach_objects(st->dafb);
    dafb_set_ac842a(st->dafb, true); // the Antelope answers the AC842a probe...
    dafb_set_antelope(st->dafb, true); // ...and reports a non-zero mfg/rev

    st->bus_mmu = m68k_board_build_bus_mmu040(cfg, 0x40000000u, desc->common.rom_base, desc->common.rom_end);
    if (!st->bus_mmu)
        return -1;

    djmemc_io_bind(cfg);
    djmemc_memory_layout(cfg);

    machine_part_begin(cfg, cp, "djmemc");
    if (cp)
        djmemc_restore_private(cfg, cp);
    machine_part(cfg, cp, "djmemc", part_save_djmemc_private, st);
    return 0;
}

// ============================================================
// Substrate lifecycle
// ============================================================

static int djmemc_init(config_t *cfg, checkpoint_t *cp) {
    const djmemc_board_desc_t *desc = djmemc_board(cfg);
    djmemc_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;
    djmemc_reset_regs(desc, st);

    m68k_board_build_core(cfg, &desc->common, cp);
    machine_part_irq(cfg, cp);
    m68k_board_build_lowspeed(cfg, cp, NULL);

    uint8_t via_ff = via_freq_factor_for_clock(cfg->machine->freq);
    machine_part_begin(cfg, cp, "via1");
    cfg->via1 = via_init(NULL, cfg->scheduler, via_ff, "via1", djmemc_via1_output,
                         desc->chip == DJMEMC_CHIP_MEMCJR ? djmemc_via1_shift_out : NULL, m68k_board_via1_irq, cfg, cp);
    machine_part(cfg, cp, "via1", part_save_via, cfg->via1);
    machine_part_begin(cfg, cp, "via2");
    cfg->via2 = via_init(NULL, cfg->scheduler, via_ff, "via2", djmemc_via2_output, NULL, m68k_board_via2_irq, cfg, cp);
    machine_part(cfg, cp, "via2", part_save_via, cfg->via2);
    via_set_exact_clock(cfg->via1, cfg->machine->freq);
    via_set_exact_clock(cfg->via2, cfg->machine->freq);

    if (djmemc_build_devices(cfg, cp) != 0)
        return -1;

    cfg->nubus = nubus_init(cfg, cfg->machine->nubus_slots, cp);
    mmu_host_regions_fill_pages(st->bus_mmu, page_fill_68k, /*mode24_alias*/ false);

    m68k_board_finish(cfg, cp, &st->io);
    return 0;
}

static void djmemc_bus_reset(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    // The overlay re-arms with /RESET.  The frame-buffer controller is NOT
    // on the 040's reset (Apple's PrimaryInit resets it by hand on this
    // board), so its registers survive; the ROM reprograms them.
    m68k_board_overlay_arm(&st->overlay);
    if (st->bus_mmu) {
        st->bus_mmu->enabled = false;
        mmu_invalidate_tlb(st->bus_mmu);
    }
    scsi_53c96_reset(st->scsi96);
    system_reset_common_devices(cfg);
}

static void djmemc_teardown(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    if (st) {
        if (st->cuda) {
            cuda_delete(st->cuda);
            st->cuda = NULL;
        }
        if (st->sonic) {
            sonic_delete(st->sonic);
            st->sonic = NULL;
        }
        if (st->scsi96) {
            scsi_53c96_delete(st->scsi96);
            st->scsi96 = NULL;
        }
        if (st->dafb) {
            dafb_delete(st->dafb);
            st->dafb = NULL;
        }
        if (st->bus_mmu) {
            mmu_delete(st->bus_mmu);
            st->bus_mmu = NULL;
        }
        if (st->asc) {
            asc_delete(st->asc);
            st->asc = NULL;
        }
    }
    machine_teardown_config_devices(cfg);
}

// VBL tick: VIA1 CA1 pulse (the 60.15 Hz chain); the video interrupt comes
// from the frame-buffer controller's own timing.
static void djmemc_trigger_vbl(config_t *cfg) {
    via_pulse_c(cfg->via1, 0, 0);
    nubus_tick_vbl(cfg->nubus);
    image_tick_all(cfg);
}

static struct display *djmemc_display(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    return (st && st->dafb) ? dafb_display(st->dafb) : NULL;
}

static void djmemc_power_on(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    if (st) {
        // Cuda stays powered; the host side of its handshake went down.
        if (st->cuda)
            cuda_host_power_cycle(st->cuda);
        dafb_power_on(st->dafb);
        dafb_reset(st->dafb);
    }
}

// === Built-in video as a display device ======================================
// The monitors the WombatDAFB drives, by passive sense code (dafb.h's
// indexed numbering), plus the 16" on the extended probe.

typedef struct djmemc_monitor {
    const char *monitor;
    uint8_t sense;
} djmemc_monitor_t;

static const djmemc_monitor_t k_djmemc_monitors[] = {
    {"13in_rgb",            0x6u                 },
    {"12in_rgb",            0x2u                 },
    {"15in_portrait",       0x1u                 },
    {"15in_portrait_color", 0x5u                 },
    {"16in_rgb",            DAFB_SENSE_INDEXED_GF},
    {"21in_rgb",            0x0u                 },
    {"21in_mono",           0x3u                 },
    {"none",                0x7u                 },
    {NULL,                  0                    },
};

static bool djmemc_monitor_at(size_t i, const char **id, const char **monitor) {
    for (size_t n = 0; k_djmemc_monitors[n].monitor; n++) {
        if (n == i) {
            *id = *monitor = k_djmemc_monitors[n].monitor;
            return true;
        }
    }
    return false;
}

static bool djmemc_monitor_sense(const char *id, uint8_t *out) {
    for (const djmemc_monitor_t *m = k_djmemc_monitors; m->monitor; m++) {
        if (strcmp(m->monitor, id) == 0) {
            *out = m->sense;
            return true;
        }
    }
    return false;
}

const builtin_video_desc_t djmemc_builtin_video = {
    .detail = "djMEMC (DAFB-compatible) + Antelope",
    .monitor_at = djmemc_monitor_at,
    .monitor_sense = djmemc_monitor_sense,
    .default_monitor = "13in_rgb",
    .indexed_sense = true,
};

const machine_substrate_t djmemc_substrate = {
    .init = djmemc_init,
    .bus_reset = djmemc_bus_reset,
    .power_on = djmemc_power_on,
    .teardown = djmemc_teardown,
    .seed = mac_seed_rtc_pram,
    .trigger_vbl = djmemc_trigger_vbl,
    .nubus_slot_irq = djmemc_nubus_slot_irq,
    MAC_SUBSTRATE_HOST_IO,
    MAC_SUBSTRATE_MEDIA_STD,
    .display = djmemc_display,
};
