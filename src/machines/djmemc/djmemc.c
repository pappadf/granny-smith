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
#include "cpuid_page.h"
#include "mac_host_io.h"
#include "machine_checkpoint.h"
#include "machine_teardown.h"
#include "regfile.h"

#include "adb.h"
#include "checkpoint.h"
#include "cpu.h"
#include "cuda.h"
#include "dafb.h"
#include "floppy.h"
#include "log.h"
#include "memory.h"
#include "rtc.h"
#include "scsi_53c96.h"
#include "via.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("board");

// One-level family: the profile's board IS the descriptor.
static inline const djmemc_board_desc_t *djmemc_board(config_t *cfg) {
    return (const djmemc_board_desc_t *)cfg->machine->board;
}

// ============================================================
// RAM banks
// ============================================================

// The djMEMC: the soldered RAM as banks 0 and 1 (4 MB each), then each SIMM
// socket's one or two banks.
static bool djmemc_chip_ram_banks(const djmemc_board_desc_t *desc, uint32_t ram, uint32_t size[DJMEMC_MAX_BANKS]) {
    // A SIMM, largest first: its bytes and how many 4/16 MB banks it fills.
    static const struct {
        uint32_t bytes, bank;
    } simm[] = {
        {0x02000000u, 0x01000000u}, // 32 MB: two 16 MB banks
        {0x01000000u, 0x01000000u}, // 16 MB: one 16 MB bank
        {0x00800000u, 0x00400000u}, //  8 MB: two  4 MB banks
        {0x00400000u, 0x00400000u}, //  4 MB: one  4 MB bank
    };
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

// MEMCjr: the soldered bank 0, then the one SIMM socket's sides in banks 1
// and 2.  The 1, 4 and 16 MB SIMMs are single-sided, the 2, 8 and 32 MB
// ones two sides of half that.
static bool memcjr_chip_ram_banks(const djmemc_board_desc_t *desc, uint32_t ram, uint32_t size[DJMEMC_MAX_BANKS]) {
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

// The start address bank `b`'s configuration register places it at: the
// djMEMC's 8-bit 4 MB field.
static uint32_t djmemc_chip_bank_start(const djmemc_state_t *st, int b) {
    return (st->memc_regs[(DJMEMC_REG_BANK0 >> 2) + b] & DJMEMC_BANK_START_MSK) * DJMEMC_BANK_UNIT;
}

// The MEMCjr's: six register bits under the four the latch gave that bank,
// in 1 MB units.
static uint32_t memcjr_chip_bank_start(const djmemc_state_t *st, int b) {
    uint32_t reg = st->memc_regs[(DJMEMC_REG_BANK0 >> 2) + b];
    uint32_t mb =
        ((uint32_t)(st->jr_bank_hi[b] & MEMCJR_LATCH_HI_MSK) << MEMCJR_LATCH_HI_SHIFT) | (reg & MEMCJR_BANK_LO_MSK);
    return mb * MEMCJR_BANK_UNIT;
}

// The djMEMC's power-up bank placement: bank n on the n-th 64 MB boundary,
// the same split the ROM programs before it sizes, so whatever the ROM
// touches before sizing finds bank 0 at zero.
static void djmemc_chip_reset_regs(djmemc_state_t *st) {
    for (int b = 0; b < DJMEMC_MAX_BANKS; b++)
        st->memc_regs[(DJMEMC_REG_BANK0 >> 2) + b] = 0x100u + (uint32_t)b * 0x10u;
}

// The MEMCjr's: 64 MB is 64 units -- the register's six bits clear, the
// latch's n.
static void memcjr_chip_reset_regs(djmemc_state_t *st) {
    for (int b = 0; b < MEMCJR_MAX_BANKS; b++)
        st->jr_bank_hi[b] = (uint8_t)b;
    st->memc_regs[MEMCJR_REG_LATCH >> 2] = 0xFFu;
}

// MEMCjr: a register read loads the latch with the register's bits 6-11.
static void memcjr_latch_load(djmemc_state_t *st, uint32_t value) {
    st->memc_regs[MEMCJR_REG_LATCH >> 2] = (value >> MEMCJR_LATCH_HI_SHIFT) & MEMCJR_LATCH_MSK;
}

// The djMEMC's register read: interleave register bits 9-11 are the chip's
// version.  Version 0 is the part the ROM does not enable interleaving on,
// which keeps the bank map linear (interleaving scrambles it, and is a
// timing feature only).
static uint32_t djmemc_chip_reg_value(djmemc_state_t *st, uint32_t idx, uint32_t lane) {
    (void)lane;
    uint32_t v = st->memc_regs[idx];
    if (idx == (DJMEMC_REG_INTERLEAVE >> 2))
        v &= ~0x0E00u;
    return v;
}

// The MEMCjr's: a register read hands its bits 6-11 to the latch (the read
// completes on the low lane).
static uint32_t memcjr_chip_reg_value(djmemc_state_t *st, uint32_t idx, uint32_t lane) {
    uint32_t v = st->memc_regs[idx];
    if (idx != (MEMCJR_REG_LATCH >> 2) && lane == 3u)
        memcjr_latch_load(st, v);
    return v;
}

// MEMCjr: a bank register takes its high start bits from the latch as it
// lands.
static void memcjr_chip_bank_landed(djmemc_state_t *st, int b) {
    st->jr_bank_hi[b] = (uint8_t)(st->memc_regs[MEMCJR_REG_LATCH >> 2] & MEMCJR_LATCH_HI_MSK);
}

// What differs between the two memory controllers.
typedef struct djmemc_chip_ops {
    const char *name; // "djMEMC" / "MEMCjr" (logs)
    uint32_t reg_count; // longwords the window decodes (a power of two; it repeats)
    int max_banks; // bank registers
    bool (*ram_banks)(const djmemc_board_desc_t *desc, uint32_t ram, uint32_t size[DJMEMC_MAX_BANKS]);
    uint32_t (*bank_start)(const djmemc_state_t *st, int b); // a bank's start address
    void (*reset_regs)(djmemc_state_t *st); // power-up values over a zeroed file
    uint32_t (*reg_value)(djmemc_state_t *st, uint32_t idx, uint32_t lane); // a read's value and side effects
    void (*bank_landed)(djmemc_state_t *st, int b); // a bank register completed (NULL: nothing)
} djmemc_chip_ops_t;

// The ops of each djmemc_chip_t.
static const djmemc_chip_ops_t k_djmemc_chip_ops[] = {
    [DJMEMC_CHIP_DJMEMC] = {"djMEMC", DJMEMC_REG_COUNT, DJMEMC_MAX_BANKS, djmemc_chip_ram_banks, djmemc_chip_bank_start,
                            djmemc_chip_reset_regs, djmemc_chip_reg_value, NULL                   },
    [DJMEMC_CHIP_MEMCJR] = {"MEMCjr", MEMCJR_REG_COUNT, MEMCJR_MAX_BANKS, memcjr_chip_ram_banks, memcjr_chip_bank_start,
                            memcjr_chip_reset_regs, memcjr_chip_reg_value, memcjr_chip_bank_landed},
};

// The running board's memory-controller ops.
static inline const djmemc_chip_ops_t *djmemc_ops(config_t *cfg) {
    return &k_djmemc_chip_ops[djmemc_board(cfg)->chip];
}

bool djmemc_ram_banks(const djmemc_board_desc_t *desc, uint32_t ram, uint32_t size[DJMEMC_MAX_BANKS]) {
    for (int i = 0; i < DJMEMC_MAX_BANKS; i++)
        size[i] = 0;
    return k_djmemc_chip_ops[desc->chip].ram_banks(desc, ram, size);
}

// Work out the bank population and where each bank lives in the flat RAM
// image (banks in order, packed).  A total that is no population of the
// board is presented as one bank, for the ROM to judge.
static void djmemc_bank_layout(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    if (!djmemc_ram_banks(djmemc_board(cfg), cfg->ram_size, st->bank_size)) {
        LOG(1, "%s: %u KB is no SIMM population of this board; one bank", djmemc_ops(cfg)->name, cfg->ram_size >> 10);
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

// The physical span the bank registers can place RAM in: ten 64 MB windows.
#define DJMEMC_RAM_SPAN (DJMEMC_MAX_BANKS * DJMEMC_BANK_WINDOW)

// Apply the bank configuration registers to the page table
// (m68k_board_map_banks): each populated bank mirrors through its span up to
// the next bank or 64 MB.  The pages the previous placement reached are
// cleared first: a bank that moves (the ROM parks the banks, sizes them,
// then stitches them contiguous) must not leave its old image behind.  Not
// while the ROM overlay holds low memory: dropping it maps RAM (the AV
// boards' rule, av_ymca_remap), so a bank write under the overlay takes
// effect then.
static void djmemc_map_ram(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    if (st->q.overlay.armed)
        return;
    const djmemc_chip_ops_t *ops = djmemc_ops(cfg);
    m68k_board_bank_t banks[DJMEMC_MAX_BANKS];
    for (int b = 0; b < DJMEMC_MAX_BANKS; b++) {
        // A bank the chip has no register for is empty.
        uint32_t start = b < ops->max_banks ? ops->bank_start(st, b) : 0;
        banks[b] = (m68k_board_bank_t){start, st->bank_size[b], st->bank_image_off[b]};
    }
    m68k_board_map_banks(cfg, banks, DJMEMC_MAX_BANKS, DJMEMC_BANK_WINDOW, DJMEMC_RAM_SPAN, &st->ram_extent,
                         st->q.bus_mmu);
}

// The register file's power-up state: zero, then the chip's bank placement,
// and MemTop at $FF.
static void djmemc_reset_regs(const djmemc_board_desc_t *desc, djmemc_state_t *st) {
    memset(st->memc_regs, 0, sizeof st->memc_regs);
    memset(st->jr_bank_hi, 0, sizeof st->jr_bank_hi);
    k_djmemc_chip_ops[desc->chip].reset_regs(st);
    st->memc_regs[DJMEMC_REG_MEMTOP >> 2] = 0xFFu;
}

// ============================================================
// djMEMC register window ($0E000)
// ============================================================
// The register a window offset decodes to: the djMEMC repeats its sixteen
// longwords through $7F, the MEMCjr decodes thirty-two.

uint8_t djmemc_reg_read(config_t *cfg, uint32_t win_off, uint32_t addr) {
    (void)addr;
    const djmemc_chip_ops_t *ops = djmemc_ops(cfg);
    uint32_t idx = (win_off >> 2) & (ops->reg_count - 1);
    uint32_t v = ops->reg_value(djmemc_st(cfg), idx, win_off & 3u);
    return be_lane8(v, win_off & 3u);
}

void djmemc_reg_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)addr;
    djmemc_state_t *st = djmemc_st(cfg);
    const djmemc_chip_ops_t *ops = djmemc_ops(cfg);
    uint32_t idx = (win_off >> 2) & (ops->reg_count - 1);
    be_lane8_set(&st->memc_regs[idx], win_off & 3u, value);
    // A bank register's start field is its low byte: the longword is
    // complete when lane 3 lands, and the bank moves then.
    if ((win_off & 3u) != 3u)
        return;
    LOG(2, "%s $%02X = $%08X (pc=%08X)", ops->name, idx << 2, st->memc_regs[idx], cpu_get_pc(cfg->cpu));
    int bank = (int)idx - (int)(DJMEMC_REG_BANK0 >> 2);
    if (bank < 0 || bank >= ops->max_banks)
        return;
    if (ops->bank_landed)
        ops->bank_landed(st, bank);
    djmemc_map_ram(cfg);
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

// The rows both islands decode: the IOSB's (or PrimeTime's) device windows,
// the memory controller's registers (`memc_name`) and the I/O chip's own
// (`iosb_name`).  VIA2's IFR slot comes before VIA2: the narrow row wins
// (first match).
//   base     end      device            penalty               xform              rd wr  rd_fn/wr_fn  name
#define DJMEMC_ISLAND_ROWS(memc_name, iosb_name)                                                                         \
    {0x00000, 0x02000, BOARD_DEV_VIA1, DJMEMC_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL, NULL, "via1", .esync = 1},   \
        {0x03A00,                                                                                                        \
         0x03C00,                                                                                                        \
         0,                                                                                                              \
         DJMEMC_VIA_IO_PENALTY,                                                                                          \
         BOARD_IO_NORMAL,                                                                                                \
         0,                                                                                                              \
         0,                                                                                                              \
         djmemc_via2_ifr_read,                                                                                           \
         djmemc_via2_ifr_write,                                                                                          \
         "via2_ifr",                                                                                                     \
         .esync = 1,                                                                                                     \
         .peek_fn = djmemc_via2_ifr_peek},                                                                               \
        {0x02000, 0x04000, BOARD_DEV_VIA2, DJMEMC_VIA_IO_PENALTY, BOARD_IO_MASK_A0, 0, 0, NULL,                          \
         NULL,    "via2",  .esync = 1},                                                                                  \
        {0x0C000, 0x0E000, BOARD_DEV_SCC, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "scc"},                  \
        {0x0E000, 0x10000, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, djmemc_reg_read, djmemc_reg_write, memc_name},   \
        {0x10000, 0x10100, BOARD_DEV_SCSI96, DJMEMC_IO_PENALTY, BOARD_IO_STRIDE_16, 0, 0,                                \
         NULL,    NULL,    "scsi_53c96"}, /* The pseudo-DMA port: the SCSI Manager moves longwords through it. */        \
        {0x10100,                                                                                                        \
         0x10200,                                                                                                        \
         BOARD_DEV_SCSI96,                                                                                               \
         DJMEMC_IO_PENALTY,                                                                                              \
         BOARD_IO_FIXED,                                                                                                 \
         SCSI_53C96_PDMA_SUB,                                                                                            \
         SCSI_53C96_PDMA_SUB,                                                                                            \
         NULL,                                                                                                           \
         NULL,                                                                                                           \
         "scsi_pdma"},                                                                                                   \
        {0x14000, 0x16000, BOARD_DEV_ASC, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "sound"},                \
        {0x18000, 0x1A000, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, djmemc_iosb_read, djmemc_iosb_write, iosb_name}, \
    {                                                                                                                    \
        0x1E000, 0x20000, BOARD_DEV_FLOPPY, DJMEMC_SWIM_IO_PENALTY, BOARD_IO_STRIDE_512, 0, 0, NULL, NULL, "swim2"       \
    }

// The djMEMC boards' island: the shared rows plus the SONIC, its MAC PROM
// and the Kiwi (quadra040's handler rows).
const board_io_range_t djmemc_io_ranges[] = {
    DJMEMC_ISLAND_ROWS("djmemc", "iosb"),
    {0x08000, 0x0A000, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, quadra040_prom_read, quadra040_prom_write,
                        "enet_prom"},
    {0x0A000, 0x0B100, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, quadra040_sonic_read, quadra040_sonic_write,
                        "sonic"},
    {0x28000, 0x2A000, 0, DJMEMC_IO_PENALTY, BOARD_IO_NORMAL, 0, 0, quadra040_yancc_read, quadra040_yancc_write, "kiwi",
                        .peek_fn = quadra040_yancc_peek},
    {0}, // sentinel
};

// The MEMCjr boards' island: the same decode less the SONIC, its PROM and
// the Kiwi, which these boards do not carry (Apple, Macintosh LC 475 and
// Macintosh Quadra 605 Developer Note (1993), Figure 2-2, p. 27).
const board_io_range_t memcjr_io_ranges[] = {
    DJMEMC_ISLAND_ROWS("memcjr", "primetime"),
    // The communication slot's window (the LC 575's; the slot Apple's
    // Ethernet card puts its SONIC at): with no card fitted nothing answers
    // and the cycle bus-errors, which is how the ROM's probe finds the slot
    // empty.  No turnaround to charge.
    {0x0A000, 0x0C000, 0, 0, BOARD_IO_NORMAL, 0, 0, NULL, NULL, "comm_slot", .berr = 1},
    {0}, // sentinel
};

// ============================================================
// VIA wiring
// ============================================================
// The VIA ADB boards take quadra040_via1_output (the Quadra 700 wiring).

// VIA1 outputs of the Cuda boards: PA5 = floppy head select; PB the Cuda
// handshake (PB3 TREQ in, PB4 BYTEACK, PB5 TIP out).
static void djmemc_cuda_via1_output(void *context, uint8_t port, uint8_t output) {
    config_t *cfg = (config_t *)context;
    djmemc_state_t *st = djmemc_st(cfg);
    if (port == 0) {
        if (cfg->floppy)
            floppy_set_sel_signal(cfg->floppy, (output & 0x20) != 0);
    } else if (st) {
        cuda_via1_port_output(st->cuda, port, output);
    }
}

// VIA1 shift register out: a Cuda command byte.
static void djmemc_cuda_via1_shift_out(void *context, uint8_t byte) {
    djmemc_state_t *st = djmemc_st((config_t *)context);
    cuda_via1_shift_input(st ? st->cuda : NULL, byte);
}

// SCSI DREQ, as VIA2 IFR bit 0 presents it.
static bool djmemc_scsi_dreq(void *context) {
    return scsi_53c96_dreq((scsi_53c96_t *)context);
}

// ============================================================
// Video registers through the latch (video_latch)
// ============================================================
// The frame-buffer controller is the same DAFB-compatible block, but on the
// MEMCjr its registers are read six bits at a time: a read of the low half
// of a register loads the latch with bits 6-11, which the driver then reads
// from $50F0E07C.  Writes carry the full value (the driver writes the latch
// first and the whole value after), so they go straight through.

static uint32_t memcjr_video_read32(void *ctx, uint32_t addr) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    uint32_t v = st->dafb_iface->read_uint32(st->q.dafb, addr);
    memcjr_latch_load(st, v);
    return v;
}

static uint16_t memcjr_video_read16(void *ctx, uint32_t addr) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    uint16_t v = st->dafb_iface->read_uint16(st->q.dafb, addr);
    if (addr & 2u)
        memcjr_latch_load(st, v);
    return v;
}

static uint8_t memcjr_video_read8(void *ctx, uint32_t addr) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    return st->dafb_iface->read_uint8(st->q.dafb, addr);
}

static void memcjr_video_write8(void *ctx, uint32_t addr, uint8_t v) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    st->dafb_iface->write_uint8(st->q.dafb, addr, v);
}

static void memcjr_video_write16(void *ctx, uint32_t addr, uint16_t v) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    st->dafb_iface->write_uint16(st->q.dafb, addr, v);
}

static void memcjr_video_write32(void *ctx, uint32_t addr, uint32_t v) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    st->dafb_iface->write_uint32(st->q.dafb, addr, v);
}

static uint8_t memcjr_video_peek8(void *ctx, uint32_t addr) {
    djmemc_state_t *st = djmemc_st((config_t *)ctx);
    return memory_iface_read8(st->dafb_iface, st->q.dafb, addr, true);
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
    // Nothing tracked yet: the first remap clears the controller's whole reach.
    st->ram_extent = DJMEMC_RAM_SPAN;

    // I/O island at $50000000.
    quadra040_map_island(cfg);

    // CPU-ID register page at $5FFFF000 (the register is its last long).
    cpuid_page_install(&st->cpuid, cfg->memory_map, desc->cpuid ? desc->cpuid : DJMEMC_CPUID_VALUE);

    // Video: the frame-buffer controller's registers at $F9800000 (through
    // the latch where the board reads them so), VRAM at $F9000000.
    if (desc->video_latch) {
        st->dafb_iface = dafb_reg_interface(st->q.dafb);
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
        quadra040_map_video(cfg, "MEMCjr video regs", &st->video_interface, cfg);
    } else {
        quadra040_map_video(cfg, "djMEMC video regs", NULL, NULL);
    }

    // The ROM-at-zero overlay: armed until the ROM aperture is touched.
    quadra040_map_overlay(cfg, djmemc_map_ram, "djMEMC");
}

// ============================================================
// Checkpoint: the substrate-private part
// ============================================================

#define DJMEMC_PRIVATE_CP_FIELDS(OP, cp, st)                                                                           \
    CP_ARRAY(OP, cp, (st)->memc_regs);                                                                                 \
    CP_ARRAY(OP, cp, (st)->q.yancc.regs);                                                                              \
    CP_FIELD(OP, cp, (st)->q.yancc.touched);                                                                           \
    CP_FIELD(OP, cp, (st)->q.slot_pa_mask);                                                                            \
    CP_FIELD(OP, cp, (st)->q.sonic_byte2);                                                                             \
    CP_ARRAY(OP, cp, (st)->jr_bank_hi)

// Saves the substrate-private part: the overlay flag, the register files,
// the IRQ mask and SONIC latch, the IOSB.
static void part_save_djmemc_private(void *obj, checkpoint_t *cp) {
    djmemc_state_t *st = obj;
    CP_FIELD(system_write_checkpoint_data, cp, st->q.overlay.armed);
    DJMEMC_PRIVATE_CP_FIELDS(system_write_checkpoint_data, cp, st);
    iosb_checkpoint(&st->iosb, cp);
}

// Reads the substrate-private part on a restore, then re-applies the bank map
// (through the overlay) and re-drives the lines it derives.
static void djmemc_restore_private(config_t *cfg, checkpoint_t *cp) {
    djmemc_state_t *st = djmemc_st(cfg);
    bool overlay = false;
    CP_FIELD(system_read_checkpoint_data, cp, overlay);
    DJMEMC_PRIVATE_CP_FIELDS(system_read_checkpoint_data, cp, st);
    iosb_restore(&st->iosb, cp);
    if (overlay)
        m68k_board_overlay_arm(&st->q.overlay);
    else
        m68k_board_overlay_drop(&st->q.overlay); // maps RAM from the restored banks
    quadra040_restore_lines(cfg);
}

// ============================================================
// Device construction
// ============================================================

static int djmemc_build_devices(config_t *cfg, checkpoint_t *cp) {
    djmemc_state_t *st = djmemc_st(cfg);
    const djmemc_board_desc_t *desc = djmemc_board(cfg);
    bool cuda = desc->adb == DJMEMC_ADB_CUDA;

    // The VIA ADB boards bit-bang the classic RTC on VIA1.
    if (!cuda)
        rtc_set_via(cfg->rtc, cfg->via1);

    // VIA1 port A: the identity straps, PA7 and PA0 (the diagnostic strap)
    // high -- see mcu.h on PA0; the VIA1/VIA2 lines idle.  The Cuda's TREQ
    // (VIA1 PB3) idles high.
    quadra040_idle_lines(cfg, (uint8_t)((desc->via1_id & DJMEMC_VIA1_ID_MASK) | 0x81u));
    if (cuda)
        via_input(cfg->via1, 1, 3, 1);

    // ADB: the VIA shifter and transceiver, or the Cuda servicing it in
    // packets (the VIA is not the ADB's then).
    machine_part_begin(cfg, cp, "adb");
    cfg->adb = adb_init(cuda ? NULL : cfg->via1, cfg->scheduler, cp);
    machine_part(cfg, cp, "adb", part_save_adb, cfg->adb);
    if (cuda) {
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
    quadra040_build_scsi0(cfg, cp, quadra040_scsi96_irq);

    // The IOSB: its registers, and the DREQ it shows in VIA2's IFR.
    iosb_init(&st->iosb, cfg->machine->freq >= 30000000u);
    iosb_set_dreq(&st->iosb, djmemc_scsi_dreq, st->q.scsi96);

    // The SONIC (where the board has one), the sound block and the SWIM II.
    quadra040_build_sonic_asc_floppy(cfg, cp, desc->has_sonic ? quadra040_sonic_irq : NULL, quadra040_asc_irq);

    // The frame-buffer controller and its monitor strap: the Antelope answers
    // the AC842a probe and reports a non-zero mfg/rev.
    const quadra040_video_t video = {.vram_size = desc->vram_size, .ac842a = true, .antelope = true};
    if (quadra040_build_dafb(cfg, cp, &video, NULL) != 0)
        return -1;

    if (quadra040_build_bus_mmu(cfg) != 0)
        return -1;

    quadra040_io_bind(cfg);
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

// The family's MAC-address PROM: the Quadra 700's presentation (quadra040.c)
// of 08:00:07:09:07:02 -- Apple's OUI, one past the Quadra 700's, so two
// emulated Quadras on one network differ.
static const uint8_t djmemc_mac_prom[8] = {0x10, 0x00, 0xE0, 0x90, 0xE0, 0x40, 0x00, 0x3F};

static int djmemc_init(config_t *cfg, checkpoint_t *cp) {
    const djmemc_board_desc_t *desc = djmemc_board(cfg);
    djmemc_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        LOG(0, "Error: out of memory allocating the machine state for %s", cfg->machine->name);
        return -1;
    }
    cfg->machine_context = st;
    djmemc_reset_regs(desc, st);
    st->q.mac_prom = djmemc_mac_prom;
    yancc_init(&st->q.yancc, "Kiwi", quadra040_yancc_irq, cfg);

    bool cuda = desc->adb == DJMEMC_ADB_CUDA;
    quadra040_build_core(cfg, cp, &desc->common, NULL, cuda ? djmemc_cuda_via1_output : quadra040_via1_output,
                         cuda ? djmemc_cuda_via1_shift_out : NULL, quadra040_via2_output);

    if (djmemc_build_devices(cfg, cp) != 0)
        return -1;

    quadra040_build_finish(cfg, cp);
    return 0;
}

static void djmemc_bus_reset(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    // The overlay re-arms with /RESET.  The frame-buffer controller is NOT
    // on the 040's reset (Apple's PrimaryInit resets it by hand on this
    // board), so its registers survive; the ROM reprograms them.
    quadra040_bus_reset(cfg, /*reset_video*/ false);
    st->ram_extent = DJMEMC_RAM_SPAN; // the ROM at zero: the next remap clears the whole reach
    // The Kiwi is on the NuBus /RESET: its register file clears, and an
    // error request pending at reset no longer holds level 7.  The memory
    // controller's bank registers are kept -- the RAM decode must survive a
    // RESET instruction, and the ROM reprograms them as it sizes -- and so
    // are the IOSB's timing registers, which the ROM sets once at start-up.
    yancc_reset(&st->q.yancc);
    system_reset_common_devices(cfg);
}

static void djmemc_teardown(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    if (st) {
        if (st->cuda) {
            cuda_delete(st->cuda);
            st->cuda = NULL;
        }
        quadra040_teardown(&st->q);
    }
    machine_teardown_config_devices(cfg);
}

// Power cycle: the video's VRAM and registers go with main RAM; the Cuda
// stays powered, the host side of its handshake went down.
static void djmemc_power_on(config_t *cfg) {
    djmemc_state_t *st = djmemc_st(cfg);
    if (st) {
        if (st->cuda)
            cuda_host_power_cycle(st->cuda);
        dafb_power_on(st->q.dafb);
        dafb_reset(st->q.dafb);
    }
}

// === Built-in video as a display device ======================================
// The WombatDAFB drives the DAFB's monitors (quadra040's table); the two
// chips' descriptions differ only in the name.

#define DJMEMC_BUILTIN_VIDEO(detail_)                                                                                  \
    {                                                                                                                  \
        .detail = (detail_),                                                                                           \
        .monitor_at = quadra040_monitor_at,                                                                            \
        .monitor_sense = quadra040_monitor_sense,                                                                      \
        .default_monitor = "13in_rgb",                                                                                 \
        .indexed_sense = true,                                                                                         \
    }

const builtin_video_desc_t djmemc_builtin_video = DJMEMC_BUILTIN_VIDEO("djMEMC (DAFB-compatible) + Antelope");
// The LC 475's and Quadra 605's: the same monitors on the MEMCjr's video.
const builtin_video_desc_t memcjr_builtin_video = DJMEMC_BUILTIN_VIDEO("MEMCjr (DAFB-compatible) + Antelope");

const machine_substrate_t djmemc_substrate = {
    .init = djmemc_init,
    .bus_reset = djmemc_bus_reset,
    .power_on = djmemc_power_on,
    .teardown = djmemc_teardown,
    .seed = mac_seed_rtc_pram,
    .trigger_vbl = quadra040_trigger_vbl,
    .nubus_slot_irq = quadra040_nubus_slot_irq,
    MAC_SUBSTRATE_HOST_IO,
    MAC_SUBSTRATE_MEDIA_STD,
    .display = quadra040_display,
};
