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
#include "gbus.h" // the ANS GBUS island: board registers, keyswitch, LCD
#include "machine.h"
#include "memory.h"
#include "pci.h" // the generic PCI core: bus, device, config header
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
// Chaos bridge + its device space, Bandit 1 (config ports + its 8 MB PCI
// I/O window at the same base), the Grand Central island, Bandit 2
// (8500/9500), Hammerhead, ROM.
//
// The two 16 MB windows above each Bandit are NOT interchangeable, and a
// real 9500's own `ranges` property (Apple TN1062) says which is which:
//
//     01000000 00000000 00000000  F2000000  00000000 00800000   I/O,  8 MB
//     02000000 00000000 F3000000  F3000000  00000000 01000000   mem, 16 MB
//
// so the bridge base carries PCI I/O and the next 16 MB is pass-through
// MEMORY — which is how Grand Central is reached.  (This comment used to
// call $F3000000 "the base of Bandit 1 PCI I/O"; it is the opposite.)
#define TNT_CHAOS_BASE     0xF0000000u // Chaos bridge (config ports)
#define TNT_BANDIT1_BASE   0xF2000000u // Bandit 1 bridge: config ports + PCI I/O
#define TNT_GC_BASE        0xF3000000u // Grand Central: Bandit 1 pass-through memory
#define TNT_GC_ISLAND_SIZE 0x00020000u // the 128 KB Grand Central decodes at that base
#define TNT_BANDIT2_BASE   0xF4000000u // Bandit 2 bridge (8500/9500): ports + PCI I/O
#define TNT_HH_BASE        0xF8000000u // Hammerhead register window (2 KB)
#define TNT_ROM_BASE       0xFFC00000u // 4 MB ROM; reset vector $FFF00100
#define TNT_PCI_MEM1       0x80000000u // Bandit 1 PCI memory space (256 MB)
#define TNT_PCI_MEM_VCI    0x90000000u // Chaos/VCI PCI memory space (256 MB)
// A Bandit's PCI I/O window: 8 MB at the bridge base, of which only the
// low 16 address bits are driven, so the 64 KB I/O space aliases through
// it 128 times (TN1062's `ranges`, above).
#define TNT_PCI_IO_SIZE 0x00800000u

// Config-port offsets from a bridge base (identical on Bandit and Chaos).
#define TNT_PCI_CFG_ADDR 0x800000u // config address port (4 bytes, LE)
#define TNT_PCI_CFG_DATA 0xC00000u // config data port (8 bytes decoded)

// Family bus numbering — what a pci_slot_decl_t's `.bus` field names.
#define TNT_PCI_BUS_1   0 // Bandit 1 (all machines)
#define TNT_PCI_BUS_2   1 // Bandit 2 (8500/9500)
#define TNT_PCI_BUS_VCI 2 // Chaos, the display bus

// === Grand Central external-interrupt map, Network Server personality =======
// Apple, "Network Server Hardware Developer Notes", 1996, §4.2, p. 16: the
// ANS "keeps the critical positions of PowerMac 9500; however, F/W SCSI
// interrupts are moved to Bandit's positions."  Exactly three lines differ
// from the Macintosh boards — EXT1 (was Reserved) takes BOTH Bandits'
// ganged bus-timeout error line, and EXT2/EXT6 (were Ban1_Int/Ban2_Int)
// take the two fast/wide 53C825A controllers.  The internal Grand Central
// assignments (TNT_INT_SCSI0/MACE/SCCA/SCCB/AWACS/VIA1/SWIM3) are
// explicitly unchanged; TNT_INT_MESH simply goes unused.
//
// NOTE the trap in §10: a slot's line does NOT follow its bridge.  Slot 3
// sits on Bandit 2 but keeps EXT5 — the line a 9500 gives Bandit 1's third
// slot — so the map is DATA in the profile's slot table, never derived.
#define ANS_INT_ERROR    21 // EXT1: Error_Int, both Bandits ganged
#define ANS_INT_FW0      22 // EXT2: FW0_Int — 53C825A #0 (IDSEL 17)
#define ANS_INT_SLOT1    23 // EXT3
#define ANS_INT_SLOT2    24 // EXT4
#define ANS_INT_SLOT3    25 // EXT5 (on Bandit 2 — see above)
#define ANS_INT_FW1      26 // EXT6: FW1_Int — 53C825A #1 (IDSEL 18)
#define ANS_INT_SLOT4    27 // EXT7
#define ANS_INT_SLOT5    28 // EXT8
#define ANS_INT_SLOT6    29 // EXT9
#define ANS_INT_SECTOPRI 30 // EXT10: the MP doorbell (GBUS $19000 access)

// Which board personality a profile describes.  The ANS is a fourth board
// on this family, not a new one: same Hammerhead, same two Bandits, same
// Grand Central and its entire internal subtree, same 60x bus, same DBDMA
// (Apple, ibid., §1: "much of the hardware detail which is fully documented
// in the PowerMac family is not repeated here").  What differs is the
// sixteen-item delta this enum selects.
typedef enum tnt_board_kind {
    TNT_BOARD_MAC = 0, // Power Macintosh 7500/8500/9500
    TNT_BOARD_SHINER, // Apple Network Server 500/700 ("Shiner")
} tnt_board_kind_t;

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

    // === The Network Server delta (Apple, ibid., §1.1 table) ================
    tnt_board_kind_t kind; // MAC (default) or SHINER
    // MESH, the internal fast-SCSI cell.  Delta #4: the ANS has no MESH at
    // all — two 53C825A PCI controllers replace it — so the cell must not be
    // constructed, must not decode island +$18000, and must not appear in
    // the device tree.  A board flag rather than a #ifdef, because one
    // binary serves both boards.
    bool has_mesh;
    // The GBUS island (delta #6/#9/#13/#14): Grand Central's Generic Bus
    // chip selects, idle on a Macintosh, carrying the server's front-panel
    // LCD, board registers, keyswitch, Ethernet PROM and NVRAM ports here.
    bool has_gbus;
    // Parity DRAM (delta #7).  Counterintuitively the FASTER configuration:
    // "If parity is detected, 60 ns timing is set.  If parity is not
    // detected, 70 ns timing is set" (ibid., §5).
    bool has_parity;
    // L2 cache DIMM size in KB; 0 = no cache DIMM.  The ANS's DIMM is
    // "fit, form and function compatible with the PowerMac 8500 cache slot"
    // — 512 KB on the 500, 1 MB on the 700 (ibid., §6).  Reported through
    // Hammerhead +$E0.
    uint32_t l2_kb;
    // TwoSuppliesH — Board Register 1 bit 15, the redundant-PSU report and
    // the one register-level difference between a 700 and a 500 (§5.6).
    bool two_supplies;
} tnt_board_desc_t;

// === Hammerhead state (hammerhead.c) ========================================
// 128 x 32-bit registers on $10 centres, big-endian (processor bus — the
// one non-LE block), store-and-readback with a handful of special offsets.
#define TNT_HH_REGS 128

typedef struct tnt_hammerhead {
    uint32_t reg[TNT_HH_REGS]; // raw store; specials overlay on read
    bool l2cfg_sticky; // TEMP diagnostic: +$E0 ignores writes (GS_HH_L2CFG)
} tnt_hammerhead_t;

// === Bandit / Chaos state (bandit.c) ========================================
// Per-bridge software-visible state: the config address latch and the two
// documented config registers of the bridge's own device-11 header.
typedef struct tnt_bandit {
    uint32_t base; // bridge window base (identifies the instance in logs)
    bool is_chaos; // Chaos: restricted config space, writes ignored
    bool claims_mem; // took the $90000000 memory window (Bandit 2 only)
    uint32_t cfg_addr; // config address port latch (LE value; 0 = idle)
    uint32_t mode_select; // config $50 (the $40 coherency bit latches)
    struct config *cfg; // back-pointer
    pci_bus_t *bus; // the generic bus this bridge fronts
    pci_device_t self_dev; // the bridge's own device-11 header
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
    // The mode-1 output latch, PER SOURCE: a bit is set by that source's
    // enabled change and cleared by the $80000000 acknowledge, and the CPU
    // line follows `latch & mask`.  Per-source rather than a single flag
    // because masking a source has to quiet it: AIX services the fast/wide
    // controllers by polling and leaves their externals masked, so a single
    // sticky flag left the line asserted for a source the guest had
    // deliberately turned off and the machine took nothing but external
    // interrupts from then on.
    uint32_t int_latch;
    uint8_t int_mode1; // Clear-mode 1 selected (see above)
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
// Control video VBL.  The dossier's interrupt map guessed 30, but the
// shipping System's video driver is authoritative: right as it writes
// Control INTR_ENA it toggles GC mask BIT 26 through the kernel's
// Enable/DisableInterruptSource path (live at 962.6M of the 7.6 boot,
// mask writes at $FFE68D5C/$FFE68DE8) — the VCI/control interrupt rides
// line 26.  With the pulse on 30 the driver's VBL never delivered, the
// cursor task chain stayed dead, and the Finder had no mouse pointer.
#define TNT_INT_VBL 26

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

// === Control video state (control.c) ========================================
// Control (343S1154) is PCI device 11 on the Chaos display bus: a 4 KB
// register block behind config BAR $14 and a 64 MB VRAM aperture behind
// BAR $18 (both assigned by Open Firmware inside the $90000000 VCI memory
// space), 32 little-endian 32-bit registers on $10 centres, plus the
// RaDACal RAMDAC whose byte registers sit in GRAND CENTRAL at +$1B000.
#define TNT_CONTROL_REGS 32
#define TNT_VRAM_SIZE    0x400000u // 4 MB: two 2 MB banks, both populated

typedef struct tnt_control {
    // The BAR latches ($14 = registers, $18 = VRAM aperture) live in the
    // generic config header now (tnt_state_t.control_dev.cfg), which also
    // owns the sizing mask and the decode.  What stays here is the chip.
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
    pci_device_t gc_dev; // Grand Central's config presence (device 16)
    struct av_cuda *cuda;
    struct tnt_dbdma *dbdma; // the 11-channel DMA engine (island +$8000)
    tnt_awacs_t awacs;
    struct object *snd_object; // machine.sound node (awacs.c)
    int16_t *snd_stage; // gain-applied staging frames for audio_out_push

    // Control video (control.c): register/RaDACal state is checkpointed;
    // the VRAM blob follows it in the tail; the display descriptor and its
    // derived views are rebuilt from the registers on restore.
    tnt_control_t control;
    pci_device_t *control_dev; // Control as a device on the Chaos bus (owned
                               // by the bus: its factory allocated it)
    tnt_mesh_t mesh; // internal fast SCSI (mesh.c; bus = cfg->scsi)
    // The Network Server's GBUS island (gbus.c / lcd.c).  Built only for
    // TNT_BOARD_SHINER; inert and unread on the Macintosh boards.
    tnt_gbus_t gbus;
    tnt_lcd_t lcd;
    struct object *board_object; // machine.board node (gbus.c)
    struct object *lcd_object; // machine.lcd node (lcd.c)
    struct scsi_53c96 *scsi96; // external SCSI chip (no bus attached yet)
    // The Network Servers' second fast/wide bus (`machine.scsi2`, Open
    // Firmware's `scsi-int2` / `probe-scsi2`), carrying backplane bays 4-6.
    // NULL on the Macintosh boards, which have exactly one visible bus.
    struct scsi *scsi2;
    uint8_t *vram; // TNT_VRAM_SIZE host buffer (bank 2 at +$200000)
    struct display display; // scanout descriptor (display.h)
    rgba8_t clut_view[256]; // materialized CLUT for the renderer
    uint8_t *blank; // black stub presented while the raster is blanked
    uint8_t *compose; // hardware-cursor composite (derived, not checkpointed)

    // Memory interfaces registered with the map
    memory_interface_t gc_interface; // $F3000000 island (128 KB)
    memory_interface_t hh_interface; // $F8000000 register window
    memory_interface_t chaos_probe_interface; // unclaimed Chaos window (logged)
    memory_interface_t control_regs_if; // Control BAR $14 (register block)
    memory_interface_t control_vram_if; // Control BAR $18 (VRAM aperture)
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

// Build all bridge instances (per the board's bandit_count): the config
// ports, one generic PCI bus per bridge, each bridge's own device-11
// header and the PCI memory windows the buses claim.  Requires cfg->pci.
void tnt_bandit_init(config_t *cfg);
// The PCI memory windows, claimed AFTER pci_seat_slots(): which bridge
// owns $90000000 depends on whether the VCI bus seated anything.
void tnt_bandit_claim_memory(config_t *cfg);

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

// Control is a registered PCI card kind ("tnt_control", BUILTIN attach):
// the machine's slot table names it, and pci_seat_slots runs its factory,
// which is what calls the three functions below.
void tnt_control_register_events(config_t *cfg); // event type (pre-start)
void tnt_control_init(config_t *cfg); // VRAM, display, BAR backings
void tnt_control_reset(config_t *cfg); // power-on registers (VRAM survives)
void tnt_control_update(config_t *cfg); // re-derive the display descriptor
void tnt_control_teardown(config_t *cfg);
// RaDACal byte cells (Grand Central +$1B000, $10 centres).
uint8_t tnt_control_rad_read(config_t *cfg, uint32_t offset);
void tnt_control_rad_write(config_t *cfg, uint32_t offset, uint8_t value);
// Presentation: the primary display descriptor (NULL before init), and the
// host-frame dirty mark (guest CPU writes bypass the renderer).
struct display *tnt_control_display(config_t *cfg);
void tnt_control_host_vbl(config_t *cfg);

// === grand_central.c ========================================================

void tnt_gc_init(config_t *cfg); // power-on state (NVRAM contents survive)
// Seat Grand Central's config presence at device 16 on Bandit 1's bus.
void tnt_gc_pci_attach(config_t *cfg, pci_bus_t *bus);
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
// Clear the non-volatile store and its process-lifetime carry: the
// documented effect of removing the logic board's battery (tnt.c).
void tnt_nvram_clear(config_t *cfg);

// Board Register 1 / BoxID as software reads it: the board straps, the live
// PCI slot-presence pins, and (on a Network Server) the GBUS top byte.
uint32_t tnt_gc_boxid(config_t *cfg);

// === gbus.c (GBUS device 3's non-LCD registers; lcd.c routes them here) ====

void tnt_gbus_tben_write(config_t *cfg, uint16_t value);
uint16_t tnt_gbus_tben_read(config_t *cfg);
void tnt_gbus_misc_write(config_t *cfg, uint16_t value);
uint16_t tnt_gbus_misc_read(config_t *cfg);

#endif // GS_MACHINES_TNT_H
