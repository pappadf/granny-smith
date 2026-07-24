// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mcu.h
// The MCU/Orwell family (Quadra 700/900/950): the 68040 + MCU + JDB/Relayer +
// YANCC + stand-alone-DAFB generation (proposal-machine-quadra-700-900-950.md
// §5).  Chip-named like glue/, mdu/, oss/ — MCU is the memory controller that
// defines the generation.
//
// Family traits this substrate implements:
//   * access-triggered ROM-at-zero overlay: the MCU maps the 1 MiB boot ROM
//     at $00000000 out of reset; the FIRST access to the normal ROM aperture
//     ($40000000-$4FFFFFFF) restores RAM at zero (ref §4.3) — unlike GLUE/MDU,
//     where a VIA output bit controls the overlay
//   * MCU register file at $5000E000: accept-and-log with readback (ref §8;
//     register semantics are not publicly documented — Trap 24: no silent
//     zeros, every access is loggable)
//   * the 256 KiB I/O island at $50000000 with the family decode (ref §6),
//     run on the shared mac030 I/O engine
//   * interrupts: VIA1→1, VIA2→2, SCC→4, NMI→7 (ref §13; same routing table
//     as GLUE, so mac030_glue_update_ipl is reused verbatim)

#ifndef GS_MACHINES_MCU_H
#define GS_MACHINES_MCU_H

#include "mac030_glue.h" // shared core builder + IRQ resolver + fill_page
#include "mac030_glue_io.h" // the shared I/O dispatch engine
#include "memory.h"
#include "system_config.h"

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

// Number of longword slots kept for the accept-and-log MCU register file
// ($5000E000; ref §8.2 — offsets/masks unresolved, so writes latch and read
// back verbatim while a log records the ROM's access sequence).
#define MCU_REG_COUNT 64

// Same policy for the YANCC system-bus/NuBus bridge register file
// ($50028000; ref §10.2 [A][U] — the address is Apple-documented, the bit
// layout is not, so writes latch and read back under a log).
#define MCU_YANCC_REG_COUNT 64

// The MCU-family board descriptor: per-machine hardware data consumed by the
// shared substrate (parallel to mac030_board_desc_t).
typedef struct mcu_board_desc {
    const char *chipset; // "MCU+DAFB" (tracing/diagnostics)
    uint32_t rom_base, rom_end; // ROM aperture ($40000000-$50000000)
    const mac030_io_range_t *io_ranges; // ordered I/O window table
    uint32_t io_mirror_mask; // I/O island mirror mask ($3FFFF)
    uint8_t io_unmapped_read; // unmapped-read value inside the island
    const struct nubus_slot_decl *slots; // NuBus slot table (NULL until Phase F)
    uint32_t bus_err_lo, bus_err_hi; // unmapped-region bus-error window
    uint8_t via1_pa_model; // VIA1 PA model sense ($C0 Q700, $D0 Q900, $90 Q950; ref §7.4 [R])
    uint8_t dafb_version; // DAFB_Test bits 11:9 (0 = Q700/Q900, 3 = Q950 "DAFB 3"; ref §11.8)
    bool has_ac842a; // AC842a RAMDAC (PCBR1 + x555 16-bit mode; Q950 only)
} mcu_board_desc_t;

// Per-machine hooks + data, named by hw_profile_t.board.
typedef struct mcu_board {
    const mcu_board_desc_t *desc;
    void (*via1_output)(void *context, uint8_t port, uint8_t value);
    void (*via1_shift_out)(void *context, uint8_t byte);
    void (*via2_output)(void *context, uint8_t port, uint8_t value);
    void (*build_devices)(config_t *cfg, checkpoint_t *cp); // machine-specific tail
    // SCC chip IRQ callback override (NULL → mac030_glue_scc_irq).  The towers
    // OR the SCC chip INT with the SCC IOP host INT onto the level-4 source
    // (ref §15.12), so they intercept the chip line here.
    void (*scc_irq)(void *context, bool active);
} mcu_board_t;

// Unified MCU-family machine state.
typedef struct mcu_state {
    struct adb *adb;
    struct asc *asc; // EASC (ASC-compatible core until Phase D)
    struct floppy *floppy;
    struct dafb *dafb; // DAFB video (register stub until Phase D)
    struct scsi_53c96 *scsi96; // NCR 53C96 (bus/targets attach in Phase E)
    struct sonic *sonic; // DP83932 SONIC Ethernet (Phase F; no wire in v1)
    uint8_t sonic_byte2; // high byte latched from an in-flight register write

    // --- Tower (Q900/Q950) devices — NULL on the Q700 (Phase G) ---
    struct egret *caboose; // Egret-protocol system manager ("Caboose" firmware; ref §15.14)
    struct iop *scc_iop; // SCC behind an Apple PIC/IOP at island $C000 (ref §6.3)
    struct iop *swim_iop; // SWIM/ADB behind the second IOP at island $1E000
    struct scsi_53c96 *scsi96_ext; // second 53C96 — external bus (regs $F402, pdma $F502)
    struct scsi *scsi_ext; // external SCSI bus (no default devices in v1)
    uint8_t scc_irq_or; // level-4 requesters: bit0 = SCC chip, bit1 = SCC IOP host INT
    uint8_t scsi_irq_or; // VIA2 CB2 requesters: bit0 = internal 53C96, bit1 = external

    bool rom_overlay; // true = ROM mapped at $00000000 (access-triggered drop)
    struct mmu_state *bus_mmu; // bus-side resolver; 040 walker regs live on the CPU

    mac030_io_t io; // device handles for the shared I/O engine

    uint8_t last_port_b; // VIA1 PB output, for ADB ST-transition filtering

    // Accept-and-log MCU register file ($5000E000, ref §8.2 [U]).
    uint32_t mcu_regs[MCU_REG_COUNT];
    uint64_t mcu_touched; // log-once bitmap (bit n = slot n already logged)

    // Accept-and-log YANCC bridge register file ($50028000, ref §10.2 [U]).
    uint32_t yancc_regs[MCU_YANCC_REG_COUNT];
    uint64_t yancc_touched; // log-once bitmap, parallel to mcu_touched

    // /SLOTIRQ aggregate (ref §13.3): bit n set = the VIA2 PA n source is
    // asserting (Ethernet 0, NuBus A-E 1-5, built-in video 6).  CA1 follows
    // the OR of the mask.
    uint8_t slot_pa_mask;

    memory_interface_t io_interface; // registered at the $50000000 island
    memory_interface_t overlay_interface; // ROM-aperture trigger while overlay on
} mcu_state_t;

// The one MCU-family substrate; q700/q900/q950 bind this.
extern const machine_substrate_t mcu_substrate;

// Q700 I/O window table (exposed for the address-map unit test).
extern const mac030_io_range_t mcu_q700_io_ranges[];

// Q900/Q950 tower I/O window table: IOP apertures replace direct SCC/SWIM,
// second 53C96 windows at $F400/$F500 (ref §6.3).
extern const mac030_io_range_t mcu_q900_io_ranges[];

// Bind the family device set + board tables into the shared I/O engine.
void mcu_io_bind(mac030_io_t *io, config_t *cfg, const mcu_board_desc_t *desc, void *asc, void *floppy);

// Install the family memory layout: I/O island, DAFB apertures, ROM-aperture
// trigger device, and the armed overlay.  Called from a machine's
// build_devices once the DAFB and bus resolver exist.
void mcu_memory_layout(config_t *cfg);

// Overlay control (checkpoint restore / tests); layout arms it by default.
void mcu_set_overlay(config_t *cfg, bool on);

// Read the substrate-private checkpoint tail and re-drive derived IRQ lines;
// each board's build_devices calls this at the end of its restore path.
void mcu_restore_private(config_t *cfg, checkpoint_t *cp);

// Drive one /SLOTIRQ source (VIA2 PA bit 0-6, `active` in source polarity):
// sets the active-low PA input and re-resolves the CA1 aggregate (ref §13.3).
void mcu_slot_irq_source(config_t *cfg, int pa_bit, bool active);

#endif // GS_MACHINES_MCU_H
