// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// tnt.h
// The TNT family (Power Macintosh 7500/8500/9500) — the second PowerPC
// machine family, and the repository's first PCI platform.  Where PDM was
// "AMIC plus bespoke DMA per device", TNT is one DMA architecture (DBDMA)
// behind one I/O controller (Grand Central), reached through a PCI host
// bridge (Bandit), under a north bridge (Hammerhead).
//
// Board model: Hammerhead (343S1142: system bus / DRAM / ROM / L2
// controller) + one or two Bandits (343S1126: AR-to-PCI bridge) + Chaos
// (343S1155: the display-bus variant) + Grand Central (343S1125:
// interrupt controller, eleven DBDMA engines, and the apertures for every
// legacy I/O cell) around silicon the repo already models (Cuda, 6522,
// 53C96, Z8530).  On this platform even hardware bring-up is guest code:
// the ROM's own Open Firmware 1.0.5 probes the chipset, sizes memory and
// builds the device tree — the emulator implements only the registers it
// touches.
//
// Register truth: the shipping 4 MB TNT ROM (its Open Firmware device
// tree, 68k DecoderInfo tables and ConfigInfo page), Apple, "Power
// Macintosh 7500 and 8500 Computers" Developer Note (1995), Apple, "Power
// Macintosh 9500 Computer" Developer Note (1995), and the OS driver
// corpus for these exact machines (Linux powermac, NetBSD macppc,
// OSF/Apple MkLinux DR3).  Per-register citations in the .c files.
//
// Built through Phase C (proposal-powermac-7500-8500-9500 §7): Phase B —
// the machine skeleton and boot-ladder rungs T1-T8 (memory map,
// Hammerhead, Bandit config space, Grand Central decode + interrupt
// block, BoxID, banked NVRAM, Cuda/VIA); Phase C — the DBDMA engine
// (dbdma.c) behind the island's +$8000 channel window.  MESH, AWACS,
// Control video and the rest of the datapaths are later phases.

#ifndef GS_MACHINES_TNT_H
#define GS_MACHINES_TNT_H

#include "awacs.h" // shared ASCO codec semantics (core/peripherals/)
#include "display.h" // scanout descriptor (control.c presents through it)
#include "machine.h"
#include "memory.h"
#include "system_config.h"

#include <stdbool.h>
#include <stdint.h>

struct av_cuda; // the shared behavioral Cuda model (machines/av/cuda.h)
struct tnt_dbdma; // the DBDMA engine (dbdma.h)
struct scsi_53c96; // the external-bus SCSI chip (core scsi_53c96.h)

// === Endianness =============================================================
// The one structural rule of this platform: Grand Central, the Bandit/Chaos
// config ports, DBDMA (registers AND in-memory descriptors), Control and
// AWACS are little-endian devices behind a big-endian bus with NO byte-lane
// swapper — software swaps (the guest uses lwbrx/stwbrx).  The memory map
// stays big-endian; each LE register block applies TNT_LE32 at its edge,
// so a guest lwbrx of a register yields the little-endian value the model
// stores.  This macro is the ONLY sanctioned swap point in the family.
#define TNT_LE32(x) __builtin_bswap32((uint32_t)(x))

// === Physical map ===========================================================
// Chaos bridge + its device space, Bandit 1 + its PCI I/O window (Grand
// Central at the window base), Bandit 2 (8500/9500), Hammerhead, ROM.
#define TNT_CHAOS_BASE   0xF0000000u // Chaos bridge (config ports)
#define TNT_BANDIT1_BASE 0xF2000000u // Bandit 1 bridge (config ports)
#define TNT_GC_BASE      0xF3000000u // Grand Central: base of Bandit 1 PCI I/O
#define TNT_BANDIT2_BASE 0xF4000000u // Bandit 2 bridge (8500/9500)
#define TNT_HH_BASE      0xF8000000u // Hammerhead register window (2 KB)
#define TNT_ROM_BASE     0xFFC00000u // 4 MB ROM; reset vector $FFF00100
#define TNT_PCI_MEM1     0x80000000u // Bandit 1 PCI memory space (256 MB)
#define TNT_PCI_MEM_VCI  0x90000000u // Chaos/VCI PCI memory space (256 MB)

// Config-port offsets from a bridge base (identical on Bandit and Chaos).
#define TNT_PCI_CFG_ADDR 0x800000u // config address port (4 bytes, LE)
#define TNT_PCI_CFG_DATA 0xC00000u // config data port (8 bytes decoded)

// === Per-model board descriptor =============================================
typedef struct tnt_board_desc {
    // BoxID power-on value (little-endian bit numbering — the guest reads
    // the register with lwbrx).  Bit 8 is the factory-test strap (must be
    // clear); bit 14 = MESH present; bit 15 idles pulled high; LE bit 11
    // = 8500 (the 68k identification's second discriminator — decoded
    // from the shipping ROM at $FFC14844, see grand_central.c).
    uint32_t boxid;
    // Hammerhead identity: the part identifier at +$00 (first byte $39
    // selects the TNT identification path; $3001xxxx is the 7200), and
    // the +$20 register whose bit 30 ($40000000) marks the 9500.
    uint32_t hh_id;
    uint32_t hh_r20;
    uint32_t bus_hz; // processor (AR) bus clock: 50/40/44 MHz
    int bandit_count; // 1 (7500) or 2 (8500/9500)
} tnt_board_desc_t;

// === Hammerhead state (hammerhead.c) ========================================
// 128 x 32-bit registers on $10 centres, big-endian (processor bus — the
// one non-LE block), store-and-readback with a handful of special offsets.
#define TNT_HH_REGS 128

typedef struct tnt_hammerhead {
    uint32_t reg[TNT_HH_REGS]; // raw store; specials overlay on read
} tnt_hammerhead_t;

// === Bandit / Chaos state (bandit.c) ========================================
// Per-bridge software-visible state: the config address latch and the two
// documented config registers of the bridge's own device-11 header.
typedef struct tnt_bandit {
    uint32_t base; // bridge window base (identifies the instance in logs)
    bool is_chaos; // Chaos: restricted config space, writes ignored
    uint32_t cfg_addr; // config address port latch (LE value; 0 = idle)
    uint32_t mode_select; // config $50 (the $40 coherency bit latches)
    struct config *cfg; // back-pointer (Chaos delegates BARs to control.c)
    memory_interface_t addr_if; // +$800000 port
    memory_interface_t data_if; // +$C00000 port
} tnt_bandit_t;

#define TNT_MAX_BRIDGES 3 // Bandit 1, Bandit 2, Chaos

// === Grand Central state (grand_central.c) ==================================
// The interrupt block at +$20..$2C (LE), BoxID, and the banked NVRAM.
#define TNT_NVRAM_SIZE 0x2000u // 8 KB: 256 banks of 32 bytes

typedef struct tnt_gc {
    // Interrupt controller (little-endian bit numbering, bit 0 = LSB):
    // Events edge-latches source rising edges, Levels is the live source
    // picture.  Two clear modes (grand_central.c): mode 0 (power-on) has
    // the CPU line follow ((events | levels) & mask); a Clear write with
    // bit 31 — the NanoKernel's ifMode1Clear acknowledge — selects mode 1,
    // where the line follows (levels & mask) alone.
    uint32_t int_events;
    uint32_t int_mask;
    uint32_t int_levels; // live source levels (mirror of the source state)
    uint8_t int_mode1; // Clear-mode 1 selected (see above)
    uint8_t int_latch; // mode-1 output latch: set by enabled source edges,
                       // cleared by the $80000000 acknowledge
    uint8_t nvram_bank; // +$1D000 bank-select port (bank = offset / 32)
    uint8_t nvram[TNT_NVRAM_SIZE];
} tnt_gc_t;

// Grand Central interrupt numbers (little-endian bit positions; the same
// numbers appear in the device tree's AAPL,interrupts properties).  DBDMA
// channels are numbers 0-10; device interrupts follow.
#define TNT_INT_SCSI0 12 // 53C94 chip
#define TNT_INT_MESH  13 // MESH chip
#define TNT_INT_MACE  14 // MACE Ethernet chip
#define TNT_INT_SCCA  15 // SCC channel A
#define TNT_INT_SCCB  16 // SCC channel B
#define TNT_INT_AWACS 17 // AWACS codec
#define TNT_INT_VIA1  18 // VIA1/Cuda cascade (60 Hz tick, ADB, timers)
#define TNT_INT_SWIM3 19 // SWIM3 chip
#define TNT_INT_NMI   20 // External Int 0 — the NanoKernel's IPL-7 bit
#define TNT_INT_VBL   30 // External Int 10 — Control/Platinum video VBL

// === AWACS state (awacs.c) ==================================================
// The Grand Central sound face: five 32-bit LE registers on $10 centres
// at island +$14000, the shared ASCO codec shadows behind the NEWECMD
// command port, and the DBDMA channel-8 pacing state (a frame-credit
// gate the periodic tick refills exact-rationally off scheduler cycles).
typedef struct tnt_awacs {
    uint32_t sound_ctrl; // +$00: subframe selects, rate field (bits 10:8)
    uint32_t codec_ctrl; // +$10: last command (NEWECMD reads back clear)
    uint32_t byte_swap; // +$40: bit 0 = sample data is little-endian
    uint16_t codec[AWACS_CODEC_REGS]; // expanded-command shadows
    // Channel-8 pacing (exact-rational: frames = cycles*rate/freq)
    uint64_t tick_cycles; // cycle stamp of the last credit grant
    uint64_t tick_frac; // running remainder of (elapsed*rate) mod freq
    uint32_t credit; // frames the port may consume before the next grant
    uint8_t tick_armed; // the pacing event is pending (mirrors scheduler)
    uint8_t partial[4]; // sub-frame byte assembly across port calls
    uint32_t partial_len;
    // Diagnostics (machine.sound)
    uint64_t frames_pushed; // frames rendered into the host stream
    int32_t peak; // loudest |sample| pushed since power-on
} tnt_awacs_t;

// One empty PCI memory-space window: an access nothing answers takes a
// recoverable transfer error (the BART precedent — Open Firmware, NetBSD
// and the Slot Manager all probe under a fault catcher).
typedef struct tnt_fault_window {
    uint32_t base;
    char what[24]; // window name, used in the memory map and fault logs
} tnt_fault_window_t;

#define TNT_FAULT_WINDOWS 2 // Bandit 1 PCI memory + Chaos/VCI memory

// === Control video state (control.c) ========================================
// Control (343S1154) is PCI device 11 on the Chaos display bus: a 4 KB
// register block behind config BAR $14 and a 64 MB VRAM aperture behind
// BAR $18 (both assigned by Open Firmware inside the $90000000 VCI memory
// space), 32 little-endian 32-bit registers on $10 centres, plus the
// RaDACal RAMDAC whose byte registers sit in GRAND CENTRAL at +$1B000.
#define TNT_CONTROL_REGS 32
#define TNT_VRAM_SIZE    0x400000u // 4 MB: two 2 MB banks, both populated

typedef struct tnt_control {
    // PCI BAR latches (config $14 = registers, $18 = VRAM aperture).  The
    // stored value is the raw last write; reads mask to the BAR's size so
    // the standard write-ones sizing probe works.
    uint32_t bar_regs;
    uint32_t bar_vram;
    // The register file (index = offset/$10; §4 of control-chaos-video.md)
    uint32_t reg[TNT_CONTROL_REGS];
    uint8_t vbl_pending; // intr_stat: a VBL edge not yet acknowledged
    uint8_t vbl_armed; // the frame event is pending in the scheduler
    // RaDACal (byte registers on $10 centres at GC +$1B000)
    uint8_t rad_addr; // +$00 index register (CLUT entry / misc register)
    uint8_t rad_phase; // RGB byte phase of the CLUT data port
    uint8_t rad_ctrl; // misc $20: depth control (bits 3:2 = 8/16/32 bpp)
    uint8_t rad_bank; // misc $21: VRAM bank select
    uint8_t rad_misc[2]; // misc $10/$11 (cursor, cleared on mode set)
    uint8_t clut[256][3];
    uint8_t crsr[8][3]; // cursor palette (+$10 port)
    uint8_t crsr_phase;
} tnt_control_t;

// === MESH state (mesh.c) ====================================================
// MESH (343S1146) — Apple's fast internal-bus SCSI cell: sixteen
// byte-wide registers on $10 centres at island +$18000, a 16-byte FIFO
// for the non-data phases, and DBDMA channel 10 for the data phases.
// The register core drives the shared bus/target model through the same
// scsi_external_* API the 53C96 front-end uses (mesh-scsi.md §2-§8).
#define TNT_MESH_FIFO 16

typedef struct tnt_mesh {
    uint8_t fifo[TNT_MESH_FIFO];
    uint8_t fifo_rd, fifo_n;
    uint8_t sequence; // last written sequence-command byte
    uint8_t bus0_atn; // explicitly driven ATN (bus_status0 write)
    uint8_t exception, error; // W1C cause latches
    uint8_t intr_mask, interrupt; // W1C summary; mask gates GC line only
    uint8_t source_id, dest_id;
    uint8_t sync_params, sel_timeout;
    // Live transfer engine: the sequence command in progress and its
    // down-counter (count_lo/hi read back the live remainder).
    uint8_t active; // command nibble in progress (0 = idle)
    uint8_t active_dma; // SEQ_DMA_MODE was set on the active command
    uint32_t remaining; // bytes left on the active transfer
    uint8_t connected; // a target is selected (bus not free)
    uint8_t msgout_pending; // select-with-ATN: present MSG OUT until sent
    uint8_t resel_enabled, parity_enabled;
    // SDTR message engine (mesh.c §"Sync negotiation"): the assembled
    // message-out bytes of the current session and the virtual
    // message-in queue the target speaks through.  All of it is
    // per-connection state.
    uint8_t mo_buf[12];
    uint8_t mo_len;
    uint8_t mi_buf[8];
    uint8_t mi_n, mi_rd;
    uint8_t sdtr_await; // our SDTR request is out, awaiting the reply
    uint8_t msgin_taken; // the bus message byte was delivered (MESSAGE IN
                         // lingers in the bus model until release, but the
                         // target no longer REQs — busfree must succeed)
} tnt_mesh_t;

// === Family state ===========================================================
typedef struct tnt_state {
    tnt_hammerhead_t hh;
    tnt_gc_t gc;
    tnt_bandit_t bridge[TNT_MAX_BRIDGES];
    int bridge_count;
    tnt_fault_window_t fault[TNT_FAULT_WINDOWS];
    struct av_cuda *cuda;
    struct tnt_dbdma *dbdma; // the 11-channel DMA engine (island +$8000)
    tnt_awacs_t awacs;
    struct object *snd_object; // machine.sound node (awacs.c)
    int16_t *snd_stage; // gain-applied staging frames for audio_out_push

    // Control video (control.c): register/RaDACal state is checkpointed;
    // the VRAM blob follows it in the tail; the display descriptor and its
    // derived views are rebuilt from the registers on restore.
    tnt_control_t control;
    tnt_mesh_t mesh; // internal fast SCSI (mesh.c; bus = cfg->scsi)
    struct scsi_53c96 *scsi96; // external SCSI chip (no bus attached yet)
    uint8_t *vram; // TNT_VRAM_SIZE host buffer (bank 2 at +$200000)
    struct display display; // scanout descriptor (display.h)
    rgba8_t clut_view[256]; // materialized CLUT for the renderer
    uint8_t *blank; // black stub presented while the raster is blanked

    // Memory interfaces registered with the map
    memory_interface_t gc_interface; // $F3000000 island (128 KB)
    memory_interface_t hh_interface; // $F8000000 register window
    memory_interface_t pci_fault_interface; // empty PCI memory space
    memory_interface_t chaos_probe_interface; // unclaimed Chaos window (logged)
    memory_interface_t vci_interface; // $90000000 VCI memory (control.c)
} tnt_state_t;

static inline tnt_state_t *tnt_st(config_t *cfg) {
    return (tnt_state_t *)cfg->machine_context;
}

static inline const tnt_board_desc_t *tnt_board(config_t *cfg) {
    return (const tnt_board_desc_t *)cfg->machine->board;
}

// === tnt.c ==================================================================

extern const machine_substrate_t tnt_substrate;

// Fill/clear one physical page in the AoS table + SoA fast-path arrays
// (the pdm_fill_page shape; local so tnt stays free of 68K-family headers).
void tnt_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable);
void tnt_clear_page(uint32_t page_index);

// === hammerhead.c ===========================================================

void tnt_hh_init(config_t *cfg); // power-on register state + map claim
uint8_t tnt_hh_read(config_t *cfg, uint32_t offset); // window offsets 0..$7FF
void tnt_hh_write(config_t *cfg, uint32_t offset, uint8_t value);

// === bandit.c ===============================================================

// Build all bridge instances (per the board's bandit_count) and claim the
// config ports + the empty PCI memory-space fault windows.
void tnt_bandit_init(config_t *cfg);

// === awacs.c ================================================================

void tnt_awacs_register_events(config_t *cfg); // event type (before sched start)
void tnt_awacs_init(config_t *cfg); // stream, machine.sound, DBDMA ch-8 port
void tnt_awacs_reset(config_t *cfg); // power-on register state
void tnt_awacs_teardown(config_t *cfg);
// Island access for the +$14000 block (little-endian register domain).
uint32_t tnt_awacs_read32(config_t *cfg, uint32_t offset);
void tnt_awacs_write32(config_t *cfg, uint32_t offset, uint32_t value);

// === mesh.c =================================================================

void tnt_mesh_init(config_t *cfg); // power-on state + DBDMA ch-10 port
void tnt_mesh_reset(config_t *cfg);
// Island access for the +$18000 block (byte registers on $10 centres).
uint8_t tnt_mesh_read(config_t *cfg, uint32_t offset);
void tnt_mesh_write(config_t *cfg, uint32_t offset, uint8_t value);

// === control.c ==============================================================

void tnt_control_register_events(config_t *cfg); // event type (pre-start)
void tnt_control_init(config_t *cfg); // VRAM, display, VCI window claim
void tnt_control_reset(config_t *cfg); // power-on registers (VRAM survives)
void tnt_control_update(config_t *cfg); // re-derive the display descriptor
void tnt_control_teardown(config_t *cfg);
// Chaos config space, device 11 (control's PCI header): full-dword read of
// register `reg`; byte-lane write (the ports decompose to bytes).
uint32_t tnt_control_cfg_read(config_t *cfg, uint32_t reg);
void tnt_control_cfg_write(config_t *cfg, uint32_t reg, uint32_t byte, uint8_t value);
// RaDACal byte cells (Grand Central +$1B000, $10 centres).
uint8_t tnt_control_rad_read(config_t *cfg, uint32_t offset);
void tnt_control_rad_write(config_t *cfg, uint32_t offset, uint8_t value);
// Presentation: the primary display descriptor (NULL before init), and the
// host-frame dirty mark (guest CPU writes bypass the renderer).
struct display *tnt_control_display(config_t *cfg);
void tnt_control_host_vbl(config_t *cfg);

// === grand_central.c ========================================================

void tnt_gc_init(config_t *cfg); // power-on state (NVRAM contents survive)
// Island dispatch ($F3000000, offsets 0..$1FFFF).  Byte-wide cells decode
// bytes only; the 32-bit LE registers (interrupt block, BoxID) decode
// longwords only.
uint8_t tnt_gc_read8(config_t *cfg, uint32_t offset);
void tnt_gc_write8(config_t *cfg, uint32_t offset, uint8_t value);
uint32_t tnt_gc_read32(config_t *cfg, uint32_t offset);
void tnt_gc_write32(config_t *cfg, uint32_t offset, uint32_t value);
// Level-sensitive source line n (0..30): updates Levels, edge-latches into
// Events on assertion, recomputes the CPU line.
void tnt_gc_set_source(config_t *cfg, int n, bool level);
// Momentary event on line n (edge-latch only; Levels untouched).
void tnt_gc_pulse_event(config_t *cfg, int n);
// Recompute ((events | levels) & mask) and drive the CPU external line.
void tnt_gc_recompute(config_t *cfg);

#endif // GS_MACHINES_TNT_H
