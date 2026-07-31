// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// av.h
// The Cyclone/Tempest AV family (Quadra 840AV / Centris 660AV): the 68040 +
// YMCA + PSC + CIVIC/Sebastian + Cuda generation (proposal-quadra-av.md).
// Chip-named like mcu/, mdu/, oss/ — YMCA is the memory controller that
// defines the generation, but the family is best known by its "AV" branding.
//
// Family traits this substrate implements:
//   * access-triggered ROM-at-zero overlay: the 2 MB boot ROM is readable at
//     $00000000 out of reset; the FIRST access to the ROM aperture
//     ($40800000-$40A00000) restores RAM at zero.  There is no software
//     overlay-disable on this platform (ymca.md §6 — YMCA_EPROMmode is never
//     written; vOverlay does not exist).
//   * YMCA register file at $50F30400: 1-bit registers accessed as longwords
//     with the value in bit 31 (ymca.md §1); machine-ID straps at +$38..$44,
//     speed/width latches, and the per-bank boundary/size shift registers
//     that drive RAM bank mapping (ymca.md §5).
//   * CPU-ID register $5FFFFFFC reading $A55A2830, not writable (ymca.md §2).
//   * the I/O island at $50F00000-$50F3FFFF with its non-serialized alias at
//     $50F40000 (mirror mask $3FFFF), run on the shared mac030 I/O engine.
//   * MUNI NuBus bridge latches; a 660AV without the NuBus adapter must
//     bus-error on MUNI_Control so the ROM's TestForMUNI clears MUNIExists.
//   * interrupts: VIA1→1, PSC-VIA2 window→2, MACE→3, PSC-L4→4, PSC-L5→5,
//     PSC-L6→6, NMI→7 (docs/README.md interrupt table), resolved through
//     mac030_irq_resolve_ipl with the family's own routing table.
//   * NO VIA2 chip, no ASC, no SWIM, no SONIC, no IOPs — the PSC replaces
//     them all (config_t.via2 stays NULL; the IPL-2 path is the PSC's
//     VIA2 window).

#ifndef GS_MACHINES_AV_H
#define GS_MACHINES_AV_H

#include "mac030_glue.h" // shared core builder + IRQ resolver + fill_page
#include "mac030_glue_io.h" // the shared I/O dispatch engine
#include "memory.h"
#include "system_config.h"

#include <stdbool.h>
#include <stdint.h>

struct adb;
struct av_civic;
struct av_cuda;
struct av_mace;
struct av_new_age;
struct av_psc;
struct mmu_state;
struct nubus_slot_decl;
struct scsi_53c96;

// YMCA register file ($50F30400, ymca.md §1): every register is 1 bit wide,
// accessed as a longword with the value in bit 31.  The file spans offsets
// $000-$197 (bank 7's last size bit at $18C, then Test_Mode/Refresh_Test).
#define AV_YMCA_REG_COUNT (0x198 / 4)

// Machine-ID strap registers (CPUID0..3 at $38/$3C/$40/$44; nibble bit n =
// strap register n's bit 31 — Tempest25 %1011 = $B, Cyclone40 %1111 = $F).
#define AV_YMCA_CPUID0 (0x38 / 4)

// Per-bank register blocks: bank n at $50 + n*$28; 7 boundary bits (A20-A26)
// then 3 size bits (Sz0-Sz2), one register (= one longword) per bit.
#define AV_YMCA_BANK_BASE   0x50
#define AV_YMCA_BANK_STRIDE 0x28
#define AV_YMCA_BANK_COUNT  8
#define AV_YMCA_BDRY_BITS   7
#define AV_YMCA_SIZE_BITS   3

// MUNI register block ($50F30000, muni.md): only two registers are used by
// the boot ROM — IntCntrl (+$00, written) and Control (+$08, read/written).
#define AV_MUNI_INTCNTRL 0x00
#define AV_MUNI_CONTROL  0x08

// IRQ source bits driven into cfg->irq (one per 68k IPL; docs/README.md).
#define AV_IRQ_VIA1 (1 << 0) // IPL 1: VIA1 (60 Hz tick, Cuda SR, one-second)
#define AV_IRQ_VIA2 (1 << 1) // IPL 2: PSC VIA2 window (SCSI, FDC, slots, VBL)
#define AV_IRQ_L3   (1 << 2) // IPL 3: MACE
#define AV_IRQ_L4   (1 << 3) // IPL 4: SCC / Singer / DMA-complete
#define AV_IRQ_L5   (1 << 4) // IPL 5: DSP
#define AV_IRQ_L6   (1 << 5) // IPL 6: 60.15 Hz
#define AV_IRQ_NMI  (1 << 6) // IPL 7: NMI switch

// The AV-family board descriptor: per-machine hardware data consumed by the
// shared substrate (parallel to mcu_board_desc_t).
typedef struct av_board_desc {
    const char *chipset; // "YMCA+PSC" (tracing/diagnostics)
    uint32_t rom_base, rom_end; // ROM aperture ($40800000-$40A00000)
    const mac030_io_range_t *io_ranges; // ordered I/O window table
    uint32_t io_mirror_mask; // island mirror mask ($3FFFF: $50F40000 alias)
    uint8_t io_unmapped_read; // unmapped-read value inside the island
    const struct nubus_slot_decl *slots; // NuBus slot table (NULL: no cards yet)
    uint32_t bus_err_lo, bus_err_hi; // unmapped-region bus-error window
    uint8_t strap_nibble; // YMCA machine-ID straps ($F 840AV, $B 660AV)
    bool muni_present; // false → bus-error on MUNI_Control (660AV default)
} av_board_desc_t;

// Per-machine hooks + data, named by hw_profile_t.board.
typedef struct av_board {
    const av_board_desc_t *desc;
    void (*via1_output)(void *context, uint8_t port, uint8_t value);
    void (*via1_shift_out)(void *context, uint8_t byte);
    void (*build_devices)(config_t *cfg, checkpoint_t *cp); // machine tail
} av_board_t;

// Unified AV-family machine state.
typedef struct av_state {
    struct adb *adb;
    struct av_cuda *cuda; // behavioral Cuda 2.37 model on VIA1's SR
    struct av_psc *psc; // DMA + interrupt controller (the platform's heart)
    struct av_civic *civic; // CIVIC frame buffer + Sebastian RAMDAC
    struct av_new_age *fdc; // New Age floppy controller stub
    struct av_mace *mace; // MACE Ethernet register stub
    struct scsi_53c96 *scsi96; // NCR 53C96 inside Curio (Phase E)

    bool rom_overlay; // true = ROM mapped at $00000000 (access-triggered drop)
    struct mmu_state *bus_mmu; // bus-side resolver; 040 walker regs on the CPU

    mac030_io_t io; // device handles for the shared I/O engine

    // YMCA register file: one latched bit per register (ymca.md §1).  Strap
    // registers read the board's ID nibble instead of the latch.
    uint8_t ymca_regs[AV_YMCA_REG_COUNT];

    // Physical RAM bank model (ymca.md §5): the installed RAM decomposed into
    // up to 8 banks of 1-16 MB; starts latched from the boundary registers.
    uint32_t bank_size[AV_YMCA_BANK_COUNT]; // installed bytes (0 = empty)
    uint32_t bank_image_off[AV_YMCA_BANK_COUNT]; // offset in the flat RAM image
    uint32_t bank_start[AV_YMCA_BANK_COUNT]; // decoded physical start
    int bank_count; // populated banks

    // MUNI latches (muni.md): IntCntrl write-only in practice, Control R/W.
    uint32_t muni_intcntrl;
    uint32_t muni_control;

    memory_interface_t io_interface; // registered at the $50F00000 island
    memory_interface_t overlay_interface; // ROM-aperture trigger while overlay on
    memory_interface_t cpuid_interface; // $5FFFFFFC CPU-ID register page
} av_state_t;

// The one AV-family substrate; q840av/q660av bind this.
extern const machine_substrate_t av_substrate;

// AV I/O window table (exposed for the address-map unit test).
extern const mac030_io_range_t av_io_ranges[];

// Shared device construction for both leaves (the boards differ only in
// their descriptor data).  Referenced from each leaf's av_board_t.
void av_build_devices(config_t *cfg, checkpoint_t *cp);

// Shared VIA1 callbacks (identical wiring on both boards): port-B output and
// SR shift-out carry the Cuda handshake.
void av_via1_output(void *context, uint8_t port, uint8_t value);
void av_via1_shift_out(void *context, uint8_t byte);

// Set/clear an IRQ source bit and re-derive the CPU IPL through the family
// routing table (highest active source wins).
void av_update_ipl(config_t *cfg, int source, bool active);

// The AV family's IRQ routing table (sentinel-terminated; for unit tests).
const mac030_irq_route_t *av_irq_routes(void);

// Overlay control (checkpoint restore / tests); layout arms it by default.
void av_set_overlay(config_t *cfg, bool on);

#endif // GS_MACHINES_AV_H
