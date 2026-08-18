// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pdm.h
// The PDM family (Power Macintosh 6100/7100/8100) — the first machines whose
// main CPU is the PowerPC 601 (proposal-powerpc-601-pdm.md Phase C).
//
// Board model: HMC (memory controller: serial config, RAM bank windows,
// machine ID) + AMIC (everything I/O: decode, pseudo-VIA1/2, interrupt
// control, DMA register file, sound engine, video control) around silicon
// the repo already models (Cuda, 6522, 53C96, SCC).  Register truth comes
// from the shipping-ROM-verified dossier; source citations in the .c files
// use the underlying primary documents (Apple Developer Notes, schematics,
// MPC601 UM).
//
// Phase C scope: the machine skeleton and the HWInit boot ladder (rungs
// L1-L12) — memory map with all ROM alias windows, HMC with both boot-time
// measurement mechanisms, the AMIC register file (datapaths stubbed), and
// Cuda on the pseudo-VIA1 transport.

#ifndef GS_MACHINES_PDM_H
#define GS_MACHINES_PDM_H

#include "display.h"
#include "machine.h"
#include "memory.h"
#include "system_config.h"

#include <stdbool.h>
#include <stdint.h>

struct av_cuda; // the shared behavioral Cuda model (machines/av/cuda.h)
struct object;
struct scsi_53c96; // the shared 53C9x register core (peripherals/scsi_53c96.h)

// How the memory controller places SIMM banks (hmc.c).
typedef enum pdm_bank_layout {
    PDM_BANKS_MOVABLE = 0, // 6100: placement follows the SIMM_BANK_SIZE code
    PDM_BANKS_FIXED, // 7100/8100: fixed windows at $01000000 + n*$04000000
} pdm_bank_layout_t;

// Up to 8 SIMM banks (8100) plus the soldered motherboard bank.
#define PDM_MAX_BANKS 8

// Per-model board descriptor (pure data; the shared substrate interprets it).
typedef struct pdm_board_desc {
    uint16_t machine_id; // low half of $5FFFFFFC: $3010 / $3012 / $3013
    uint32_t bus_hz; // bus clock (30/33.33/40 MHz)
    pdm_bank_layout_t bank_layout;
    int bank_count; // SIMM bank windows this board decodes
    // Extra bus cycles charged per load while the HMC wait-state config bit
    // is set — sized so HWInit's bus-ratio measurement lands on the real
    // machine's CPU:bus ratio (proposal §5.2; pinned at rung L7).
    uint32_t wait_state_penalty;
    // 8100 only: the discrete 53CF96 on the fast internal bus (SCSI bus 0,
    // register file at island +$11000, AMIC DMA channel B).
    bool has_fast_scsi;
} pdm_board_desc_t;

// === HMC state (hmc.c) ======================================================

typedef struct pdm_hmc {
    // The one software-visible register: 35 bits accessed serially, LSB
    // first (bits 0-31 in `cfg_lo`, 32-34 in `cfg_hi`), plus the bit
    // pointer reset by any byte write to +8.
    uint32_t cfg_lo;
    uint32_t cfg_hi;
    uint32_t bit_ptr;
    // Derived placement state (recomputed on config writes)
    uint32_t active_code; // SIMM_BANK_SIZE code currently mapped (0-3)
    bool wait_state; // config bit 8 — the bus-ratio measurement wait state
    // Bank inventory carved from cfg->ram_size at init (host offsets into
    // the RAM allocation; motherboard bank is host offset 0)
    uint32_t mb_size; // soldered bank size (8 MB, or 4 MB small configs)
    uint32_t bank_size[PDM_MAX_BANKS];
    uint32_t bank_host_off[PDM_MAX_BANKS];
} pdm_hmc_t;

// === AMIC state (amic.c) ====================================================

// Pseudo-VIA2 / slot+device interrupt bank ($50F26000)
typedef struct pdm_via2 {
    uint8_t slot_ifr; // $50F26002 — active-low slot levels + VBL latch; reset $7F
    uint8_t slot_ier; // $50F26012 — enables (mask $78)
    uint8_t dev_ier; // $50F26013 — enables (mask $3B)
    uint8_t dev_levels; // active-high internal view of the device sources
    uint8_t misc[8]; // +0/+1/+4/+5/+10... benign byte registers
} pdm_via2_t;

// One AMIC DMA channel's software-visible register set (control byte plus
// the address/count bytes the drivers program; datapaths are later phases).
typedef struct pdm_dma_ch {
    uint32_t addr;
    uint16_t count;
    uint8_t ctrl;
} pdm_dma_ch_t;

// SWIM3 floppy controller ($50F16000, 16 byte-wide registers at stride
// $200).  Phase-G scope: the register file + drive-sense model the .Sony
// driver's open/idle path needs — internal SuperDrive present, no disk,
// drive 2 absent, no interrupt sources.  Media datapaths are Phase H.
typedef struct pdm_swim3 {
    uint8_t timer; // reg 1 (storage only; no countdown modelled)
    uint8_t param; // reg 3 ParamData
    uint8_t phase; // reg 4 CA0-2/LSTRB lines (probe loopback readback)
    uint8_t setup; // reg 5 (bit 7 SoftReset self-clears)
    uint8_t mode; // reg 6 read; written via Zeroes ($C00) / Ones ($E00)
    uint8_t intr; // reg 8, read-to-clear (never set in this model)
    uint8_t step, ctrack, csect, gap, sector, nsect; // regs 9-14 storage
    uint8_t intmask; // reg 15, R/W
    uint8_t error; // reg 2, read-to-clear (never set in this model)
    uint8_t motor_on; // drive-1 spindle latch (strobe-controlled)
    uint8_t mfm_mode; // drive mode latch; forgotten at motor-off (§11.12)
    uint8_t step_dir; // step-direction latch (sense addr 0)
} pdm_swim3_t;

typedef struct pdm_amic {
    // Interrupt control register $50F2A000
    uint8_t icr_mode; // INTMODE (bit 6)
    uint8_t icr_latch; // CPUINT latch (bit 7)
    uint8_t icr_seen; // source picture at the last recompute (edge detect)
    // DMA engine registers
    uint8_t dma_base[4]; // $50F31000-3: window base bytes [31:24]..[7:0]
    pdm_dma_ch_t scsi[2]; // $50F32000/4 addr, $50F32008/9 ctrl
    pdm_dma_ch_t enet_rx; // $50F32028 (+ head/tail)
    pdm_dma_ch_t enet_tx; // $50F31C20 (+ two count sets)
    pdm_dma_ch_t floppy; // $50F32060/64/68
    pdm_dma_ch_t scc[4]; // $50F32080/90/A0/B0 (TxA RxA TxB RxB)
    uint8_t enet_rx_head, enet_rx_tail;
    uint16_t enet_tx_count[2];
    uint16_t dma_berr_en, dma_berr_flag; // $50F32100/2 (inert)
    // Sound block $50F14000 (registers; engine + codec model in awacs.c)
    uint8_t snd[0x20];
    // Video control $50F28000 + Ariel CLUT $50F24000 (handlers in ariel.c)
    uint8_t vid_mode; // $50F28000 (reset $9F: blanked)
    uint8_t vid_depth; // $50F28001
    uint8_t vid_sense; // $50F28002 (drive bits; sense readback stubbed)
    uint8_t vid_test; // $50F28003
    uint8_t clut_addr, clut_phase, clut_ctrl, clut_key;
    uint8_t clut[256][3];
    uint8_t snd_out_buf; // sound-out ping-pong: next buffer to complete (0/1)
    pdm_via2_t via2;
    // AWACS codec register shadows (write-only on hardware, loaded through
    // the $40/hi/lo/$C0 command-port handshake; awacs.c).  Only 0/1/2/4 are
    // ever addressed by PDM software.
    uint16_t codec[8];
    double snd_half_start_ns; // when the in-flight output half began playing
    uint32_t snd_halves; // output half-buffers rendered since power-on
    int32_t snd_peak; // loudest |sample| pushed to the host since power-on
    pdm_swim3_t swim3; // floppy controller register file (swim3.c)
} pdm_amic_t;

// === Video presentation state (ariel.c) =====================================
// Everything here is DERIVED from the amic register file (vid_mode/vid_depth/
// clut) and rebuilt on init, reset and checkpoint restore — never saved.
typedef struct pdm_video {
    display_t display; // the substrate .display descriptor
    rgba8_t clut_view[256]; // depth-windowed palette the renderer indexes
    uint8_t *blank; // black raster presented while the blank bit is set
} pdm_video_t;

// === Family state ===========================================================

typedef struct pdm_state {
    pdm_hmc_t hmc;
    pdm_amic_t amic;
    struct av_cuda *cuda;
    // SCSI: [0] = the Curio 53C94 cell (all models, island +$10000, AMIC
    // DMA channel A, bus = cfg->scsi); [1] = the 8100's discrete 53CF96
    // (island +$11000, channel B) — instantiated with no bus attached, so
    // every select on the fast bus times out like an empty bus.
    struct scsi_53c96 *scsi96[2];

    // ICR source levels (bits 0-5), recomputed by pdm_amic_recompute
    uint8_t icr_sources;

    // Memory interfaces registered with the map
    memory_interface_t io_interface; // $50F00000..$50F4FFFF island
    memory_interface_t id_interface; // $5FFFF000 machine-ID page
    memory_interface_t wait_interface; // page-0 wait-state forwarder (§5.2)

    // Derived presentation state, never checkpointed
    pdm_video_t video; // scanout descriptor (ariel.c)
    int16_t *snd_stage; // one half-buffer of staged stereo samples (awacs.c)
    struct object *snd_object; // the machine.sound node (awacs.c)
} pdm_state_t;

static inline pdm_state_t *pdm_st(config_t *cfg) {
    return (pdm_state_t *)cfg->machine_context;
}

static inline const pdm_board_desc_t *pdm_board(config_t *cfg) {
    return (const pdm_board_desc_t *)cfg->machine->board;
}

// === pdm.c ==================================================================

extern const machine_substrate_t pdm_substrate;

// Fill one physical page in the AoS table + SoA fast-path arrays (the
// mac030_fill_page shape, local so pdm stays free of 68K-family headers).
void pdm_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable);

// Clear a physical page back to unmapped (reads 0 outside the bus-error
// range — how empty RAM windows fail the probe signature compare).
void pdm_clear_page(uint32_t page_index);

// === hmc.c ==================================================================

void pdm_hmc_init(config_t *cfg);
uint8_t pdm_hmc_read(config_t *cfg, uint32_t offset); // island offset $40000+
void pdm_hmc_write(config_t *cfg, uint32_t offset, uint8_t value);
// (Re)build the RAM decode per the current config code; also the cold-boot
// power-on mapping when called with the reset config.
void pdm_hmc_remap(config_t *cfg);
// Machine-ID page handlers ($5FFFF000; the register itself is $5FFFFFFC)
uint8_t pdm_id_read8(void *ctx, uint32_t offset);
uint32_t pdm_id_read32(void *ctx, uint32_t offset);

// === amic.c =================================================================

void pdm_amic_init(config_t *cfg);
void pdm_amic_register_events(config_t *cfg); // before scheduler_start
void pdm_amic_start_vbl(config_t *cfg); // fresh boot: free-running raster
uint8_t pdm_amic_read(config_t *cfg, uint32_t offset); // island offsets < $40000
void pdm_amic_write(config_t *cfg, uint32_t offset, uint8_t value);
// Recompute the ICR source levels and drive the 601 EXT line (level-
// sensitive; called after every flag/enable write — proposal §4.6).
void pdm_amic_recompute(config_t *cfg);
// External source lines into the ICR (bit numbers per the dossier)
#define PDM_ICR_VIA1 0
#define PDM_ICR_VIA2 1
#define PDM_ICR_SCC  2
#define PDM_ICR_ENET 3
#define PDM_ICR_DMA  4
#define PDM_ICR_NMI  5
void pdm_amic_set_source(config_t *cfg, int bit, bool level);
// 53C9x INT pin levels into the pseudo-VIA2 device bank (chip 0 = Curio →
// bit 3, chip 1 = 53CF96 → bit 6; the DRQ bits 0/2 are read live from the
// chips' DREQ outputs, never latched).
void pdm_amic_set_scsi_irq(config_t *cfg, int chip, bool level);

// === awacs.c ================================================================
// The AWACS codec + AMIC sound engine: the $50F14000 register block, the
// command-port handshake, and the output datapath (half-buffer render into
// the host audio stream).  State lives in pdm_amic_t; these are the
// behavior.
void pdm_awacs_register_events(config_t *cfg); // before scheduler_start
void pdm_awacs_init(config_t *cfg); // staging buffer + machine.sound node
void pdm_awacs_teardown(config_t *cfg);
uint8_t pdm_awacs_read(config_t *cfg, uint32_t offset); // block offsets 0..$1F
void pdm_awacs_write(config_t *cfg, uint32_t offset, uint8_t value);
// Combinational ICR mirror bytes ($50F2A008/$50F2A00A): per-channel flag
// AND enable summaries the interrupt fabric folds into the DMA source bit.
uint8_t pdm_awacs_irq_summary(pdm_amic_t *a); // the $0A sound byte

// === swim3.c ================================================================
// SWIM3 floppy controller register file + Sony-drive sense model (Phase-G
// no-media scope; see pdm_swim3_t above).  Island offsets 0..$1FFF from
// $50F16000; byte accesses only.
uint8_t pdm_swim3_read(config_t *cfg, uint32_t off);
void pdm_swim3_write(config_t *cfg, uint32_t off, uint8_t value);

// === ariel.c ================================================================
// Onboard video: the Sonora-model control registers ($50F28000), the Ariel II
// CLUT/DAC ($50F24000), and the scanout descriptor over physical DRAM 0.
void pdm_video_init(config_t *cfg); // after the memory layout exists
void pdm_video_teardown(config_t *cfg);
void pdm_video_update(config_t *cfg); // re-derive the descriptor from the regs
void pdm_video_vbl(config_t *cfg); // per-VBL framebuffer re-upload mark
display_t *pdm_video_display(config_t *cfg);
uint8_t pdm_video_ctl_read(config_t *cfg, uint32_t off); // $50F28000 block
void pdm_video_ctl_write(config_t *cfg, uint32_t off, uint8_t value);
uint8_t pdm_ariel_read(config_t *cfg, uint32_t off); // $50F24000 block
void pdm_ariel_write(config_t *cfg, uint32_t off, uint8_t value);

#endif // GS_MACHINES_PDM_H
