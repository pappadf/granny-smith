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
#include "swim3.h"
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
    // Internal transfer offset (SCC channels): RST and any address-register
    // write zero it, transfers advance it, and the address bytes read back
    // as addr+offset — the live ring pointer.  The 8.1 SerialDMA HAL never
    // writes the address register at all; it relies on RST rewinding the
    // channel to its ring start for every frame.
    uint16_t xfer_off;
} pdm_dma_ch_t;

// SWIM3 floppy controller ($50F16000, 16 byte-wide registers at stride
// $200): the shared model (core/peripherals/swim3.h), bound to the AMIC
// floppy DMA channel and the pseudo-VIA2 interrupt bank in pdm/swim3.c.
// Drive and media state lives in the shared floppy module (cfg->floppy);
// the struct is checkpointed positionally inside pdm_amic_t and re-bound
// with pdm_swim3_bind() after a restore.

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
    swim3_t swim3; // floppy controller (core/peripherals/swim3.c)
} pdm_amic_t;

// === Monitor sense strap (ariel.c) ==========================================
// What is wired to the HDI-45.  The three open-collector sense lines are
// pulled up and a monitor grounds a subset, so the strap IS the monitor
// (Apple, *Designing Cards and Drivers for the Macintosh Family*, 3rd ed.,
// ch. 9).  Code 7 grounds nothing: no monitor connected.
//
// This is the switch that decides whether the machine HAS built-in video at
// all.  Given code 7 the shipping ROM does the rest by itself — it prunes
// every built-in video sResource, and its Start Manager skips allocating the
// DRAM framebuffer — so a NuBus display card becomes the machine's only
// screen, which is what a real PDM does with nothing plugged into the
// built-in port.
#define PDM_SENSE_NONE 0x7u // grounds nothing = nothing connected

typedef struct pdm_monitor_kind {
    const char *id; // config token ("hires", "none", ...)
    const char *name; // human-readable, for the object model
    uint8_t sense; // the 3-bit strap this monitor presents
} pdm_monitor_kind_t;

// Straps this model can express.  Only the eight 3-bit codes are reachable:
// the monitors Apple reached through the EXTENDED sense walk (VGA, GoldFish)
// need a per-line strap this model does not carry, and are deliberately
// absent rather than half-supported.
extern const pdm_monitor_kind_t pdm_monitors[];
const pdm_monitor_kind_t *pdm_monitor_lookup(const char *id);
// Stage the strap for the NEXT machine built (machine.boot `monitor=`).
void pdm_pending_monitor_set(uint8_t sense);

// === Video presentation state (ariel.c) =====================================
// Everything here is DERIVED from the amic register file (vid_mode/vid_depth/
// clut) and rebuilt on init, reset and checkpoint restore — never saved.
typedef struct pdm_video {
    display_t display; // the substrate .display descriptor
    rgba8_t clut_view[256]; // depth-windowed palette the renderer indexes
    uint8_t *blank; // black raster presented while the blank bit is set
    uint8_t sense; // monitor strap (PDM_SENSE_NONE = nothing connected)
} pdm_video_t;

// === BART state (bart.c) ====================================================

// Base of the BART register file — "slot $0 standard slot space" by NuBus
// numbering, and the address the ROM's presence probe reads.
#define PDM_BART_BASE 0xF0000000u

// One address window BART claims with nothing behind it (empty slot space,
// or the whole register page on a board with no bridge).  The context a
// fault handler needs: what address it was and what to call it in a log.
typedef struct pdm_bart_window {
    uint32_t base;
    char what[24]; // window name, used in the memory map and in fault logs
} pdm_bart_window_t;

// Three connectors x (standard + super slot space), the PDS slot-$E window,
// and the no-bridge register page.
#define PDM_BART_WINDOWS 8

typedef struct pdm_bart {
    uint8_t slow; // $F0000001 — wait-state bit (latch; the ROM never writes it)
    uint8_t slot_e_off; // $F0000011 — $80 = BART's slot-$E path disabled
    uint8_t burst[14]; // per-slot block-transfer enables, slot 1..14 (bit 0)
    uint32_t reset_pulses; // NuBus /RESET pulses issued (diagnostic)
} pdm_bart_t;

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

    // BART, the NuBus bridge: register file + the windows it claims with
    // nothing behind them (bart.c).  Absent on a 6100, whose bridge lives
    // on an adapter card we do not model.
    pdm_bart_t bart;
    pdm_bart_window_t bart_window[PDM_BART_WINDOWS];
    int bart_window_count;

    // Memory interfaces registered with the map
    memory_interface_t io_interface; // $50F00000..$50F4FFFF island
    memory_interface_t id_interface; // $5FFFF000 machine-ID page
    memory_interface_t wait_interface; // page-0 wait-state forwarder (§5.2)
    memory_interface_t bart_reg_interface; // $F0000000 BART register file

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
// NuBus slot /NMRQ levels into the pseudo-VIA2 slot bank (slot $B -> bit 2,
// $C -> 3, $D -> 4, $E -> 5; the register reads active-low).
void pdm_amic_set_slot_irq(config_t *cfg, int slot, bool level);
// SWIM3's IRQ pin into the pseudo-VIA2 device bank (bit 5, 68k level 2).
void pdm_amic_set_fdc_irq(config_t *cfg, bool level);
// The AMIC floppy DMA channel, as the SWIM3 engine uses it: RUN/direction
// state and one byte in either direction (address advance, count decrement
// and the terminal-count interrupt are the movers' business).
bool pdm_amic_fd_dma_running(config_t *cfg);
bool pdm_amic_fd_dma_to_device(config_t *cfg);
bool pdm_amic_fd_dma_get(config_t *cfg, uint8_t *out);
bool pdm_amic_fd_dma_put(config_t *cfg, uint8_t value);

// === bart.c =================================================================
// The NuBus '90 bridge: the $F0000000 register file, the slot-space windows,
// and the recoverable faults an empty slot answers with.  Call from the
// family memory layout, BEFORE nubus_init builds the cards.
void pdm_bart_init(config_t *cfg);
// A card's /NMRQ, which reaches the CPU through AMIC and not through BART.
void pdm_bart_slot_irq(config_t *cfg, int slot, bool active);

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
// The PDM face of the shared SWIM3 model: island offsets 0..$1FFF from
// $50F16000 (index = offset >> 9), the AMIC DMA movers and the pseudo-VIA2
// interrupt sink.
uint8_t pdm_swim3_read(config_t *cfg, uint32_t off);
void pdm_swim3_write(config_t *cfg, uint32_t off, uint8_t value);
void pdm_swim3_bind(config_t *cfg); // after floppy_init and after a restore
void pdm_swim3_register_events(config_t *cfg); // before scheduler_start
void pdm_swim3_xfer_register_events(config_t *cfg); // before scheduler_start

// === bart.c =================================================================
// The NuBus '90 bridge: the $F0000000 register file, the slot-space windows,
// and the recoverable faults an empty slot answers with.  Call from the
// family memory layout, BEFORE nubus_init builds the cards.
void pdm_bart_init(config_t *cfg);
// A card's /NMRQ, which reaches the CPU through AMIC and not through BART.
void pdm_bart_slot_irq(config_t *cfg, int slot, bool active);

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
// SWIM3 floppy controller register file + Sony-drive sense/strobe model
// (see pdm_swim3_t above).  Island offsets 0..$1FFF from $50F16000; byte
// accesses only.
uint8_t pdm_swim3_read(config_t *cfg, uint32_t off);
void pdm_swim3_write(config_t *cfg, uint32_t off, uint8_t value);
// Raise/clear the chip's IRQ line from the current Interrupt/IntMask/mode
// picture (§7.9: IRQ = EnableInts && (Interrupt & IntMask)).
void pdm_swim3_update_irq(config_t *cfg);
// Post an interrupt source (the I_* bits below) and re-evaluate the line.
void pdm_swim3_raise(config_t *cfg, uint8_t bits);

// Interrupt / IntMask bit assignments (§3.5)
#define SWIM3_INT_TIMER 0x01u
#define SWIM3_INT_STEP  0x02u
#define SWIM3_INT_ID    0x04u
#define SWIM3_INT_DONE  0x08u
#define SWIM3_INT_SENSE 0x10u

// Mode register bits (§3.2)
#define SWIM3_M_ENABLE_INTS 0x01u
#define SWIM3_M_DRIVE1      0x02u
#define SWIM3_M_DRIVE2      0x04u
#define SWIM3_M_ACTION      0x08u // GO
#define SWIM3_M_WRITE       0x10u
#define SWIM3_M_HEADSEL     0x20u
#define SWIM3_M_FORMAT      0x40u
#define SWIM3_M_GOSTEP      0x80u

// Setup register bits (§3.1)
#define SWIM3_S_COPYPROT   0x02u
#define SWIM3_S_GCR        0x04u
#define SWIM3_S_DISGCRCONV 0x10u
#define SWIM3_S_IBMDRIVE   0x20u

// Error register bits (§3.4)
#define SWIM3_E_UNDERRUN 0x01u
#define SWIM3_E_OVERRUN  0x04u
#define SWIM3_E_CRC_ADDR 0x40u
#define SWIM3_E_CRC_DATA 0x80u

// === swim3_xfer.c ===========================================================
// The media transfer engine: header hunt, sector read/write, whole-track
// format, raw (copy-protect) capture, and the GCR nibble codec.  It reads
// and writes the disk image through the shared floppy module and moves its
// bytes through the AMIC floppy DMA channel.
void pdm_swim3_register_events(config_t *cfg); // before scheduler_start
void pdm_swim3_xfer_register_events(config_t *cfg); // before scheduler_start
// Mode-register edges: GO or GoStep just became set / cleared.
void pdm_swim3_engine_update(config_t *cfg);
// Drive geometry answers the sense protocol needs (media present, density,
// write protection) — implemented next to the geometry table.
bool pdm_swim3_media_is_hd(config_t *cfg);
// The drive's index / tach line for sense address 11.
int pdm_swim3_index_pulse(config_t *cfg);

// === ariel.c ================================================================
// Onboard video: the Sonora-model control registers ($50F28000), the Ariel II
// CLUT/DAC ($50F24000), and the scanout descriptor over physical DRAM 0.
void pdm_video_init(config_t *cfg); // after the memory layout exists
void pdm_video_teardown(config_t *cfg);
// The monitor strapped to the HDI-45.  Set before the machine runs; with
// PDM_SENSE_NONE the substrate publishes no display and the ROM turns its
// own built-in video off (see the strap notes above).
void pdm_video_set_sense(config_t *cfg, uint8_t sense);
uint8_t pdm_video_sense(config_t *cfg);
void pdm_video_update(config_t *cfg); // re-derive the descriptor from the regs
void pdm_video_vbl(config_t *cfg); // per-VBL framebuffer re-upload mark
display_t *pdm_video_display(config_t *cfg);
uint8_t pdm_video_ctl_read(config_t *cfg, uint32_t off); // $50F28000 block
void pdm_video_ctl_write(config_t *cfg, uint32_t off, uint8_t value);
uint8_t pdm_ariel_read(config_t *cfg, uint32_t off); // $50F24000 block
void pdm_ariel_write(config_t *cfg, uint32_t off, uint8_t value);

#endif // GS_MACHINES_PDM_H
