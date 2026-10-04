// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gossamer.h
// The Gossamer family (the beige Power Macintosh G3: desktop, mini tower,
// Server G3) — the third PowerPC machine family.  Where TNT was
// Hammerhead + Bandit + Grand Central, Gossamer is one merchant chip and
// one Apple chip on a single 33 MHz PCI bus:
//
//   * Grackle (Motorola MPC106): memory controller AND PCI host bridge in
//     one part, every register of it in PCI configuration space of device
//     0, reached through CONFIG_ADDR/CONFIG_DATA at $FEC00000/$FEE00000
//     (grackle.c);
//   * Heathrow (343S1201): Grand Central's register layout grown a second
//     interrupt bank, a Feature Control Register, two ATA cells and BMAC,
//     sitting on the PCI bus as device $10 with a 512 KB BAR the boot
//     program itself assigns to $F3000000 (heathrow.c).
//
// Around those: the PowerPC 750 (the ppc core's CPU_MODEL_PPC750), the
// board-identification halfword at $FF000004, Cuda (fourth instance of
// av/cuda.c) whose I2C bus carries the three DIMMs' SPD EEPROMs and the
// PERCH personality-card ID EEPROM (gossamer_i2c.c), and the cells the
// repository already models (6522, Z8530, MESH, SWIM3, DBDMA).
//
// On this platform hardware bring-up is guest code twice over: the boot
// program in the ROM's ExceptionTable programs Grackle and Heathrow, sizes
// RAM from the SPD bytes it reads over Cuda I2C and computes the clocks
// from the board register and HID1; then Open Firmware 2.4 (2.0f1 on the
// first ROM) probes the bus and builds the device tree.  The emulator
// implements the registers they touch and nothing they compute.
//
// Register truth: Motorola, "MPC106 PCI Bridge/Memory Controller User's
// Manual", MPC106UM/AD Rev 1 (1997); Apple, "Power Macintosh G3 Computers"
// Developer Note (1998); IBM/Motorola, "MPC750 RISC Microprocessor User's
// Manual" (MPC750UM/D); the shipping Gossamer ROMs (their boot program,
// NanoKernel and Open Firmware); and the OS driver corpus for this
// machine.  Per-register citations in the .c files.

#ifndef GS_MACHINES_GOSSAMER_H
#define GS_MACHINES_GOSSAMER_H

#include "ata.h" // the two ATA cells
#include "davbus.h" // the DAVbus sound cell (Screamer face)
#include "machine.h"
#include "machine_profile.h"
#include "memory.h"
#include "pci.h"
#include "swim3.h"
#include "system_config.h"

#include <stdbool.h>
#include <stdint.h>

struct av_cuda; // the shared behavioral Cuda model (machines/av/cuda.h)
struct dbdma; // the DBDMA engine (core/peripherals/dbdma.h)
struct object;

// === Endianness =============================================================
// The TNT rule, unchanged: a big-endian memory map with every register
// block that is little-endian on the chip (Grackle's configuration
// registers, the Heathrow control block, DBDMA registers and descriptors)
// swapped at its edge.  This is the one sanctioned swap point in the
// family.
#define GOS_LE32(x) __builtin_bswap32((uint32_t)(x))

// === Physical map (Grackle address map B, MPC106UM Table 3-4) ===============
#define GOS_PCI_MEM_BASE  0x80000000u // PCI memory, passed through 1:1
#define GOS_PCI_MEM_SIZE  0x7D000000u // ...up to $FCFFFFFF
#define GOS_PCI_ISA_MEM   0xFD000000u // PCI memory 0-16 MB (top byte cleared)
#define GOS_PCI_IO_BASE   0xFE000000u // PCI I/O 0-8 MB
#define GOS_PCI_IO_SIZE   0x00800000u
#define GOS_CFG_ADDR_BASE 0xFEC00000u // CONFIG_ADDR, every word aliased (2 MB)
#define GOS_CFG_ADDR_SIZE 0x00200000u
#define GOS_CFG_DATA_BASE 0xFEE00000u // CONFIG_DATA, every word aliased (1 MB)
#define GOS_CFG_DATA_SIZE 0x00100000u
#define GOS_INTACK_BASE   0xFEF00000u // PCI interrupt acknowledge (1 MB, nothing answers)
#define GOS_BOARD_BASE    0xFF000000u // ROM bank 1: the board-register page (8 MB)
#define GOS_BOARD_SIZE    0x00800000u
#define GOS_ROM_BANK0     0xFF800000u // ROM bank 0 (8 MB; the 4 MB image aliases through it)
#define GOS_ROM_BASE      0xFFC00000u // the 4 MB ROM; reset vector $FFF00100
#define GOS_HEATHROW_SIZE 0x00080000u // Heathrow BAR0: 512 KB

// PCI device numbers on the one Grackle bus (the ROM's probe list and the
// device tree; the ROM's device -> source literal pairs $C/$1C $D/$17 $E/$18
// $F/$19 $12/$16).
#define GOS_DEV_GRACKLE  0x00 // the bridge itself (no IDSEL)
#define GOS_DEV_PERCH    0x0C // the personality-card slot's PCI presence
#define GOS_DEV_SLOT_A1  0x0D
#define GOS_DEV_SLOT_B1  0x0E
#define GOS_DEV_SLOT_C1  0x0F
#define GOS_DEV_HEATHROW 0x10 // mac-io@10
#define GOS_DEV_ATI      0x12 // the on-board Rage, "slot" F1

#define GOS_PCI_BUS 0 // the family's only bus index (pci_slot_decl_t.bus)

// === Heathrow interrupt sources (bank 1 = 0-31, bank 2 = 32-63) ============
// Source n is bit (n & 31) of bank (n >> 5), little-endian bit numbering —
// the numbers every AAPL,interrupts property in the Rev C device tree uses.
// DBDMA completions are held levels on their own sources, and on Heathrow
// the channel index is NOT the source: see gos_dbdma_source().
#define GOS_INT_MESH    12 // $0C
#define GOS_INT_ATA0    13 // $0D
#define GOS_INT_ATA1    14 // $0E
#define GOS_INT_SCCA    15 // $0F
#define GOS_INT_SCCB    16 // $10
#define GOS_INT_DAVBUS  17 // $11 Screamer
#define GOS_INT_VIA     18 // $12 VIA / Cuda
#define GOS_INT_SWIM3   19 // $13
#define GOS_INT_NMI     20 // $14 programmer's switch
#define GOS_INT_PERCH2  21 // $15
#define GOS_INT_ATI     22 // $16 on-board graphics (device $12)
#define GOS_INT_SLOT_A1 23 // $17
#define GOS_INT_SLOT_B1 24 // $18
#define GOS_INT_SLOT_C1 25 // $19
#define GOS_INT_PERCH1  26 // $1A
#define GOS_INT_PERCH   28 // $1C the PERCH slot as PCI device $0C
#define GOS_INT_BMAC    42 // $2A (bank 2)
#define GOS_INT_SOURCES 64

// Heathrow's thirteen DBDMA channel blocks (the mac-io children's `reg`
// properties in the Rev C device tree).
#define GOS_DMA_MESH    0
#define GOS_DMA_SWIM3   1
#define GOS_DMA_BMAC_TX 2
#define GOS_DMA_BMAC_RX 3
#define GOS_DMA_SCCA_TX 4
#define GOS_DMA_SCCA_RX 5
#define GOS_DMA_SCCB_TX 6
#define GOS_DMA_SCCB_RX 7
#define GOS_DMA_AUD_OUT 8
#define GOS_DMA_AUD_IN  9
#define GOS_DMA_ATA0    11
#define GOS_DMA_ATA1    12

// === Per-model board descriptor =============================================
typedef struct gossamer_board_desc {
    // The board-identification halfword at $FF000004 (machine-
    // halfword the boot program decodes): bit 15 = SWIM3 floppy (clear = PC MFDC),
    // bit 14 clear = burst-capable ROM, bits 13-8 = PCI PRSNT# pairs (all
    // ones when the slots are empty), bits 7-5 = processor-module ID (the
    // index into the boot program's L2CR table), bit 4 = 1 unless an
    // All-in-One personality card is fitted, bits 3-1 = bus-clock code
    // (6 = 66.82 MHz), bit 0 = 1 unless a Bose module is fitted.
    uint16_t board_id;
    uint32_t bus_hz; // 60x bus clock (66,820,000 on every shipping board)
    // The 750's identity: PVR and HID1 (PLL_CFG[0-3] in bits 31-28 — the
    // index into the boot program's CPU-clock table; code 10 = 4x, 7 = 4.5x,
    // 11 = 5x on the 66.82 MHz bus).
    uint32_t pvr;
    uint32_t hid1;
    // The on-board ATI (device $12): PCI device id and revision.
    uint16_t ati_device;
    uint8_t ati_revision;
    // The PERCH card's ID EEPROM (I2C $53), 16 significant bytes; NULL =
    // no card fitted.
    const uint8_t *perch_eeprom;
} gossamer_board_desc_t;

// === Grackle state (grackle.c) ==============================================
// The 256-byte little-endian configuration space of device 0, held as the
// bytes a config read returns (byte i = config offset i), plus the
// CONFIG_ADDR latch and the DIMM inventory the memory controller decodes.
#define GOS_GRACKLE_CFG 256
#define GOS_DIMMS       3 // three 168-pin SDRAM DIMM slots
#define GOS_MEM_BANKS   8 // Grackle's eight chip-select banks

typedef struct gos_grackle {
    uint8_t cfg[GOS_GRACKLE_CFG];
    uint32_t cfg_addr; // CONFIG_ADDR latch (little-endian value)
    // Host RAM behind each Grackle bank (0 = no DIMM side there) and
    // where in host RAM its storage starts.  Carved from the profile's RAM
    // size at init; the ROM programs the bank registers from the matching
    // SPD bytes, and the decode follows the registers (grackle.c).
    uint32_t bank_size[GOS_MEM_BANKS];
    uint32_t bank_host_off[GOS_MEM_BANKS];
} gos_grackle_t;

// One DIMM as its SPD EEPROM describes it (gossamer_i2c.c builds the
// bytes from this).
typedef struct gos_dimm {
    uint32_t side_bytes; // bytes per side (0 = slot empty)
    uint8_t sides; // 1 or 2 module banks
    uint8_t rows, cols; // address bits
} gos_dimm_t;

// === Heathrow state (heathrow.c) ============================================
#define GOS_NVRAM_SIZE 0x2000u // 8 KB, byte n at +$60000 + 16n

typedef struct gos_heathrow {
    // Interrupt controller, two banks (index 0 = sources 0-31 at +$20..
    // +$2C, index 1 = sources 32-63 at +$10..+$1C).  Per bank: Events
    // edge-latches source rising edges, Levels is the live picture, and
    // the output latch / clear mode are Grand Central's (tnt.h), because
    // the Gossamer NanoKernel's kind-7 handler acknowledges each bank
    // with a $80000000 Clear write and classifies from Levels & Mask —
    // the TNT scheme, twice.
    uint32_t events[2];
    uint32_t mask[2];
    uint32_t levels[2];
    uint32_t latch[2];
    uint8_t mode1[2];
    uint8_t brightness, contrast; // +$32 / +$33 (All-in-One display)
    uint16_t reg30; // +$30..+$31 (no known user)
    uint32_t mbcr; // +$34 media-bay control / ID register
    uint32_t fcr; // +$38 Feature Control Register
    uint32_t aux; // +$3C aux control register
    uint8_t nvram[GOS_NVRAM_SIZE];
} gos_heathrow_t;

// === I2C devices behind Cuda (gossamer_i2c.c) ===============================
#define GOS_SPD_SIZE   256
#define GOS_PERCH_SIZE 256
typedef struct gos_i2c {
    uint8_t spd[GOS_DIMMS][GOS_SPD_SIZE]; // 7-bit $50-$52
    uint8_t perch[GOS_PERCH_SIZE]; // 7-bit $53
    bool perch_present;
    uint8_t tda7433[8]; // tone/volume chip at 8-bit $8A (write sink)
    uint8_t ptr; // current EEPROM byte pointer (last subaddress)
} gos_i2c_t;

// The SWIM3 byte ring between the chip and DBDMA channel 1 (the tnt.c
// shape).
#define GOS_FDRING_SIZE 4096u
typedef struct gos_fdring {
    uint8_t buf[GOS_FDRING_SIZE];
    uint32_t head, tail; // free-running; count = tail - head
} gos_fdring_t;

// ESCC DBDMA port contexts (one per SCC channel).
typedef struct gos_scc_dma_ctx {
    config_t *cfg;
    unsigned ch;
} gos_scc_dma_ctx_t;

// === Family state ===========================================================
// Callback context of one ATA cell (IRQ, DMA kick, the dma_pump event's
// source): names its machine and cell.  Owned by the machine's state, so two
// Gossamer machines alive at once (a restore) each wire their own.
typedef struct gos_ata_ctx {
    struct config *cfg;
    int cell;
} gos_ata_ctx_t;

typedef struct gossamer_state {
    gos_grackle_t grackle;
    gos_heathrow_t hr;
    gos_i2c_t i2c;
    gos_dimm_t dimm[GOS_DIMMS];
    struct pci_bus *bus; // the one Grackle PCI bus
    pci_device_t grackle_dev; // Grackle's own header at device 0
    pci_device_t heathrow_dev; // Heathrow's header at device $10
    struct av_cuda *cuda;
    struct dbdma *dbdma; // 13 channel blocks (Heathrow +$8000)
    struct mesh *mesh; // internal SCSI (core/peripherals/scsi_mesh.c)
    // The DAVbus sound cell at +$14000, Screamer variant, output on DBDMA
    // channel 8: the register block is checkpointed, the host is wiring.
    davbus_t screamer;
    davbus_host_t screamer_host;
    swim3_t swim3;
    gos_fdring_t fdring;
    gos_scc_dma_ctx_t scc_dma_ctx[2];
    // The two ATA cells (+$20000, +$21000) and the SCSI bus that carries
    // their ATAPI CD-ROMs (machine.atapi): gossamer_ata.c.
    ata_channel_t ata[2];
    gos_ata_ctx_t ata_ctx[2];
    struct scsi *atapi;
    bool ata_ready;
    struct object *ata_object; // machine.ata
    // The Ethernet cell at +$11000 (gossamer_bmac.c).
    struct bmac *bmac;
    struct object *bmac_object; // machine.bmac
    struct object *grackle_object; // machine.grackle
    struct object *hr_object; // machine.heathrow (the interrupt controller)
    struct object *nvram_object; // machine.nvram
    struct object *board_object; // machine.board (Grackle + board register)

    memory_interface_t cfg_addr_if; // $FEC00000
    memory_interface_t cfg_data_if; // $FEE00000
    memory_interface_t intack_if; // $FEF00000
    memory_interface_t board_if; // $FF000000 bank-1 page
    memory_interface_t heathrow_if; // BAR0 backing
} gossamer_state_t;

static inline gossamer_state_t *gos_st(config_t *cfg) {
    return (gossamer_state_t *)cfg->machine_context;
}

static inline const gossamer_board_desc_t *gos_board(config_t *cfg) {
    return (const gossamer_board_desc_t *)cfg->machine->board;
}

// === gossamer.c =============================================================

extern const machine_substrate_t gossamer_substrate;
extern const struct scsi_slot gossamer_scsi_slots[];
extern const pci_slot_decl_t gossamer_pci_slots[];

// Fill/clear one physical page in the AoS table + SoA fast-path arrays
// (the tnt_fill_page shape; the PPC MMU owns the user arrays).
void gos_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable);
void gos_clear_page(uint32_t page_index);

// DBDMA channel -> Heathrow interrupt source (the table, not the index:
// ATA 11/12 on sources 2/3, BMAC 2/3 on bank-2 sources 32/33).  -1 for a
// channel with no interrupt (10).
int gos_dbdma_source(int chan);

// === grackle.c ==============================================================

// Build the bridge: config ports, the PCI bus, Grackle's own header at
// device 0, the PCI memory/I/O windows, the board-register page.  Requires
// cfg->pci.
void gos_grackle_init(config_t *cfg, checkpoint_t *cp);
void gos_grackle_reset(config_t *cfg); // power-on register file (DIMM inventory survives)
void gos_grackle_remap(config_t *cfg); // rebuild the RAM decode from the bank registers
uint16_t gos_board_id(config_t *cfg); // the $FF000004 halfword

// === heathrow.c =============================================================

void gos_heathrow_init(config_t *cfg); // power-on registers (NVRAM survives)
void gos_heathrow_pci_attach(config_t *cfg, checkpoint_t *cp); // header + BAR0 backing at device $10
void gos_grackle_attach_objects(config_t *cfg);
void gos_grackle_detach_objects(config_t *cfg);
void gos_heathrow_attach_objects(config_t *cfg);
void gos_heathrow_detach_objects(config_t *cfg);
// Level of interrupt source n (0-63): updates Levels, edge-latches into
// Events, recomputes the CPU line.
void gos_set_source(config_t *cfg, int n, bool level);
void gos_recompute_irq(config_t *cfg);
void gos_scc_dma_init(config_t *cfg); // the ESCC's four DBDMA ports
void gos_nvram_clear(config_t *cfg);

// === gossamer_i2c.c =========================================================

// Carve the profile's RAM size into DIMMs and build their SPD bytes, and
// the PERCH EEPROM image.
void gos_i2c_init(config_t *cfg);
// The Cuda I2C transaction hooks (av_cuda_attach_i2c).
int gos_i2c_read(void *ctx, uint8_t addr8, bool has_sub, uint8_t sub, uint8_t *out, int max);
bool gos_i2c_write(void *ctx, uint8_t addr8, const uint8_t *data, int len);

// === gossamer_ata.c =========================================================

#define GOS_HR_ATA0    0x20000u // cell 0; cell 1 follows at +$1000
#define GOS_HR_ATA_END 0x22000u

// gossamer_bmac.c — the Ethernet cell
void gos_bmac_init(config_t *cfg, checkpoint_t *cp); // the cell, its IRQ and DBDMA ports 2/3
void gos_bmac_reset(config_t *cfg);
void gos_bmac_teardown(config_t *cfg);
void gos_bmac_checkpoint_save(config_t *cfg, checkpoint_t *cp);
void gos_bmac_fcr_changed(config_t *cfg, uint32_t old, uint32_t fcr);
void gos_bmac_attach_objects(config_t *cfg);
void gos_bmac_detach_objects(config_t *cfg);
uint8_t gos_bmac_read8(config_t *cfg, uint32_t off);
void gos_bmac_write8(config_t *cfg, uint32_t off, uint8_t value);
uint16_t gos_bmac_read16(config_t *cfg, uint32_t off);
void gos_bmac_write16(config_t *cfg, uint32_t off, uint16_t value);
uint32_t gos_bmac_read32(config_t *cfg, uint32_t off);
void gos_bmac_write32(config_t *cfg, uint32_t off, uint32_t value);

void gos_ata_init(config_t *cfg, checkpoint_t *cp); // channels, ATAPI bus, DBDMA ports
void gos_ata_reset(config_t *cfg);
void gos_ata_teardown(config_t *cfg);
void gos_ata_checkpoint_save(config_t *cfg, checkpoint_t *cp);
void gos_ata_fcr_changed(config_t *cfg, uint32_t old, uint32_t fcr);
void gos_ata_attach_objects(config_t *cfg);
void gos_ata_detach_objects(config_t *cfg);
uint8_t gos_ata_read8(config_t *cfg, uint32_t off);
void gos_ata_write8(config_t *cfg, uint32_t off, uint8_t value);
uint16_t gos_ata_read16(config_t *cfg, uint32_t off);
void gos_ata_write16(config_t *cfg, uint32_t off, uint16_t value);
uint32_t gos_ata_read32(config_t *cfg, uint32_t off);
void gos_ata_write32(config_t *cfg, uint32_t off, uint32_t value);
// The substrate's media hooks: the standard floppy/SCSI set plus the ATA units.
int gos_media_attach(config_t *cfg, const media_slot_t *slot);
bool gos_media_present(config_t *cfg, media_bus_t bus, int unit);
int gos_media_eject(config_t *cfg, media_bus_t bus, int unit);

// === swim3 glue (gossamer.c) ================================================
void gos_swim3_bind(config_t *cfg);

#endif // GS_MACHINES_GOSSAMER_H
