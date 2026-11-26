// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mcu.h
// The MCU/Orwell family (Quadra 700/900/950): the 68040 + MCU + JDB/Relayer +
// YANCC + stand-alone-DAFB generation.  Chip-named like glue/, mdu/, oss/ —
// MCU is the memory controller that defines the generation.
//
// Family traits this substrate implements:
//   * access-triggered ROM-at-zero overlay: the MCU maps the 1 MiB boot ROM
//     at $00000000 out of reset; the FIRST access to the normal ROM aperture
//     ($40000000-$4FFFFFFF) restores RAM at zero — unlike GLUE/MDU,
//     where a VIA output bit controls the overlay
//   * MCU register file at $5000E000: accept-and-log with readback
//     (register semantics are not publicly documented: no silent zeros,
//     every access is loggable)
//   * the 256 KiB I/O island at $50000000 with the family decode,
//     run on the shared board_io engine
//   * interrupts: VIA1→1, VIA2→2, SCC→4, NMI→7 (same routing table
//     as GLUE, so m68k_board_update_ipl is reused verbatim)

#ifndef GS_MACHINES_MCU_H
#define GS_MACHINES_MCU_H

#include "checkpoint.h"
#include "m68k_board.h" // shared core builder + IRQ resolver + fill_page
#include "m68k_board_io.h" // the shared I/O dispatch engine, board-descriptor face
#include "m68k_board_overlay.h"
#include "memory.h"
#include "quadra040.h" // the device block and plumbing shared with the djMEMC family
#include "system_internal.h"

#include <stdbool.h>
#include <stdint.h>

struct adb;
struct asc;
struct dafb;
struct egret;
struct floppy;
struct iop;
struct mmu_state;
struct nubus_slot_decl;
struct scsi;
struct scsi_53c96;
struct sonic;

// Orwell memory-controller configuration register ($5000E000).
//
// Semantics recovered from the ROM sources (SuperMarioProj
// Internal/Asm/HardwarePrivateEqu.a "Orwell Memory Controller Equates", and
// its user OS/StartMgr/SizeMem.a).  Orwell has only ONE config data bit, so
// the register is written and read one bit at a time through a run of
// longword addresses: bit N lives at byte offset N*4, and only bit 0 of the
// accessed longword carries data.  A written value becomes active only when
// the matching latch address is poked.
//
//   bits  0..5   BANK_B start address    bits 19..20  DRAM_SPEED
//   bits  6..11  BANK_C start address    bits 21..23  ROM_SPEED
//   bits 12..17  BANK_D start address    bits 24..26  REFRESH
//   bit  18      CLK_SPEED               bit  27      PAR_ENB / 28 PAR_ODD
//   bits 29..33  page mode, wait states, RAS precharge
//
// A bank field holds the bank's start address in 4 MB units (SizeMem.a
// computes it as `offset >> 22`), so the power-up split the ROM programs —
// $10/$20/$30 — puts banks B/C/D at 64/128/192 MB.  Bank A is not
// programmable; it is always at physical 0.
#define ORWELL_CFG_BITS    34
#define ORWELL_BANK_BITS   6 // width of one bank's start field
#define ORWELL_BANK_UNIT   0x00400000u // a bank-field count is this many bytes (4 MB)
#define ORWELL_LATCH_BASE  0xA0u // OrwellLatches: $A0 banks, $A4 speeds, $A8 refresh, ...
#define ORWELL_LATCH_BANKS 0xA0u // OrLoadBanks
#define ORWELL_STATUS_BASE 0x100u // parity status/error registers live at $100+
#define ORWELL_MAX_BANKS   4 // banks A..D
#define ORWELL_BANK_WINDOW 0x04000000u // 64 MB decode window per bank

// The MCU-family board descriptor: per-machine hardware data consumed by the
// shared substrate (parallel to m68k_board_desc_t).
typedef struct mcu_board_desc {
    // The eight fields the shared 68k code reads -- see m68k_board_desc_t.
    // Embedded, not repeated: one definition, one place to document it.
    m68k_board_desc_t common;
    uint32_t ram_onboard_size; // soldered RAM forming bank A (Q700: 4 MB; 0 = SIMM banks only)
    uint8_t ram_bank_count; // physical banks the board decodes (Q700: 2, towers: 4)
    // VIA1 PA model sense ($C0 Q700, $D0 Q900, $90 Q950): the MODEL SENSE
    // BITS ONLY -- the ROM's identify table matches PA & $56, which does not
    // include bit 0.  PA0 is the diagnostic-mode strap and is a separate
    // fact: it idles HIGH for a normal boot, and driving it low sends the
    // ROM into its serial test manager (an endless SCC poll at $40847BFx
    // with no fault, observed during Q700 bring-up).  The boards raise it on
    // top of this field (quadra040_idle_lines(cfg, via1_pa_model | 1)).
    uint8_t via1_pa_model;
    uint8_t dafb_version; // DAFB_Test bits 11:9 (0 = Q700/Q900, 3 = Q950 "DAFB 3")
    bool has_ac842a; // AC842a RAMDAC (PCBR1 + x555 16-bit mode; Q950 only)
    // VRAM fitted, into the fixed 2 MiB CPU aperture every board decodes.
    // The BASE configurations genuinely differ -- the Q700 has one soldered
    // 512 KiB bank plus three optional 256 KiB-SIMM-pair banks, the towers
    // ship 1 MiB -- and all three expand to 2 MiB.  We currently model every
    // board MAXED, so these agree by choice rather than by hardware; this
    // field is what makes that a decision rather than a magic number, and
    // what a base-configuration model would change.
    uint32_t dafb_vram_size;
} mcu_board_desc_t;

// The .common block every MCU board shares (quadra040.h); only the island's
// decode table differs.
#define MCU_BOARD_COMMON(io_ranges_) M68K040_BOARD_COMMON("MCU+DAFB", io_ranges_)

// Per-machine hooks + data, named by hw_profile_t.board.
typedef struct mcu_board {
    const mcu_board_desc_t *desc;
    void (*via1_output)(void *context, uint8_t port, uint8_t value);
    void (*via1_shift_out)(void *context, uint8_t byte);
    void (*via2_output)(void *context, uint8_t port, uint8_t value);
    int (*build_devices)(config_t *cfg, checkpoint_t *cp); // machine-specific tail
    // SCC chip IRQ callback override (NULL → m68k_board_scc_irq).  The towers
    // OR the SCC chip INT with the SCC IOP host INT onto the level-4 source,
    // so they intercept the chip line here.
    void (*scc_irq)(void *context, bool active);
} mcu_board_t;

// Unified MCU-family machine state.
typedef struct mcu_state {
    quadra040_t q; // the shared 040 Quadra device block -- first, for quadra040_st()

    // --- Tower (Q900/Q950) devices — NULL on the Q700 ---
    struct egret *caboose; // Egret-protocol system manager ("Caboose" firmware)
    struct iop *scc_iop; // SCC behind an Apple PIC/IOP at island $C000
    struct iop *swim_iop; // SWIM/ADB behind the second IOP at island $1E000
    struct scsi_53c96 *scsi96_ext; // second 53C96 — external bus (regs $F402, pdma $F502)
    uint8_t scc_irq_or; // level-4 requesters: bit0 = SCC chip, bit1 = SCC IOP host INT
    uint8_t scsi_irq_or; // VIA2 CB2 requesters: bit0 = internal 53C96, bit1 = external

    // Orwell memory controller ($5000E000).  `orwell_cfg` is the 34-bit
    // shift-in register; `bank_start` holds the values latched from it.
    uint64_t orwell_cfg; // staged config bits (bit N = config bit N)
    uint32_t bank_start[ORWELL_MAX_BANKS]; // latched physical start of each bank
    uint32_t bank_size[ORWELL_MAX_BANKS]; // installed bytes in each bank (0 = empty socket)
    uint32_t bank_image_off[ORWELL_MAX_BANKS]; // where the bank lives in the flat RAM image
    int bank_count; // populated banks
    uint32_t ram_extent; // how far up the last bank mapping reached (the next remap clears below it)
} mcu_state_t;

// The MCU family's machine state (cfg->machine_context).
static inline mcu_state_t *mcu_st(config_t *cfg) {
    return (mcu_state_t *)cfg->machine_context;
}

// The one MCU-family substrate; q700/q900/q950 bind this.
extern const machine_substrate_t mcu_substrate;

// The DAFB as a display device: every monitor the Q700/Q900 ROM was measured
// with, or (the Q950, measured on the passive codes only) the passive ones.
extern const builtin_video_desc_t mcu_builtin_video_q700;
extern const builtin_video_desc_t mcu_builtin_video_q900;
extern const builtin_video_desc_t mcu_builtin_video_q950;

// Q700 I/O window table (exposed for the address-map unit test).
extern const board_io_range_t mcu_q700_io_ranges[];

// Q900/Q950 tower I/O window table: IOP apertures replace direct SCC/SWIM,
// second 53C96 windows at $F400/$F500.
extern const board_io_range_t mcu_q900_io_ranges[];

// Bind the I/O island: the shared device set (quadra040_io_bind) plus the
// towers' second 53C96.  The IOP apertures are the tower build's to bind.
void mcu_io_bind(config_t *cfg);

// Install the family memory layout: I/O island, DAFB apertures, ROM-aperture
// trigger device, and the armed overlay.  Called from a machine's
// build_devices once the DAFB and bus resolver exist.
void mcu_memory_layout(config_t *cfg);

// Overlay control (checkpoint restore / tests); layout arms it by default.
void mcu_set_overlay(config_t *cfg, bool on);

// The family's own checkpoint part (overlay, Orwell, YANCC, the IRQ
// aggregates): read it on a restore and re-drive the lines it derives,
// then register it.  The last thing each board's build_devices does.
void mcu_private_part(config_t *cfg, checkpoint_t *cp);

// Build the DAFB and everything that hangs off it, for every MCU board
// (quadra040_build_dafb with the descriptor's version, AC842a and VRAM, and
// the TurboSCSI DRQ readback wired to the 53C96s).
int mcu_build_dafb(config_t *cfg, checkpoint_t *cp);

#endif // GS_MACHINES_MCU_H
