// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// grackle.c
// Grackle — Motorola's MPC106 PCI bridge / memory controller, the beige
// G3's north bridge.  One chip does what Hammerhead (memory, ROM) and
// Bandit (60x <-> PCI) do on the TNT boards, and every register it has is
// in PCI configuration space of bus 0, device 0: there are no memory-
// mapped registers, only the two configuration ports.
//
// This file is the family's PCI bridge adapter and memory controller:
//
//   * CONFIG_ADDR (any word of $FEC00000-$FEDFFFFF) and CONFIG_DATA (any
//     byte/half/word of $FEE00000-$FEEFFFFF), both little-endian.  The
//     address word is the standard PCI form (MPC106UM Table 7-3): bit 31
//     enable, bus 23-16, device 15-11, function 10-8, register 7-2.  With
//     the enable bit clear no cycle is generated (reads all-ones, writes
//     vanish) — three OS drivers write 0 after every access.  Bus 0
//     device 0 is Grackle itself; devices 11-30 are IDSEL AD11-AD30
//     (Table 7-4); anything else, and every bus other than 0 (no PCI-PCI
//     bridge exists on this board), reads all-ones.  The byte within the
//     dword is chosen by which byte of CONFIG_DATA is touched.
//   * Grackle's own 256-byte configuration register file, reset values
//     from MPC106UM Table 3-10 with the Gossamer straps (map B, 64-bit ROM
//     on the memory bus): identity $1057:$0002 revision $40, PICR1/PICR2,
//     the memory boundary and bank-enable registers, MCCR1-4, the error
//     registers.  Everything is store-and-readback except the read-only
//     identity and pin bits and the write-1-to-clear status registers.
//   * The memory controller: eight chip-select banks, each an [start,
//     end] window of 1 MB granularity (§3.2.6), enabled by $A0 and live
//     once MCCR1[MEMGO] is set.  The DIMMs themselves are the profile's RAM
//     carved by gossamer_i2c.c into up to three SDRAM modules whose SPD
//     bytes the boot program reads to program these registers; each bank
//     register decodes the host RAM of the module side the ROM sized it
//     from, aliased modulo its size (the row/column lines it lacks).
//   * The address map (map B, Table 3-4): PCI memory at $80000000-
//     $FCFFFFFF passed through 1:1, PCI memory 0-16 MB at $FD000000, PCI
//     I/O 0-8 MB at $FE000000, the interrupt-acknowledge window at
//     $FEF00000, ROM bank 1 (the board register) at $FF000000 and ROM
//     bank 0 (the 4 MB image, aliased through its 8 MB) at $FF800000.
//   * Master aborts: with PICR1 TEA_EN and MCP_EN both clear (the reset
//     state) nothing traps — reads return all-ones and writes are dropped
//     (§9); once the guest enables either, an unclaimed PCI access takes
//     the machine check the fault path delivers.

#include "gossamer.h"

#include "log.h"
#include "machine.h"
#include "object.h"
#include "pci.h"
#include "ppc.h"

#include <stdio.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("grackle");

// Identity (MPC106UM Table 3-10; revision $40 = Rev 4.0, the lspci of a
// real Rev A/B board).
#define GRACKLE_VENDOR   0x1057u
#define GRACKLE_DEVICE   0x0002u
#define GRACKLE_REVISION 0x40u

// Register offsets in device 0's configuration space.
#define G_CMD      0x04u // PCI command (2 bytes)
#define G_STATUS   0x06u // PCI status (2 bytes, write-1-to-clear)
#define G_MSTART   0x80u // memory starting address, banks 0-3 / 4-7 (+4)
#define G_XMSTART  0x88u // extended memory starting address
#define G_MEND     0x90u // memory ending address
#define G_XMEND    0x98u // extended memory ending address
#define G_BANK_EN  0xA0u // memory bank enable
#define G_PGMAX    0xA3u
#define G_PICR1    0xA8u
#define G_PICR2    0xACu
#define G_ERREN1   0xC0u
#define G_ERRDR1   0xC1u // write-1-to-clear
#define G_60X_ERR  0xC3u // write-1-to-clear
#define G_ERREN2   0xC4u
#define G_ERRDR2   0xC5u // write-1-to-clear
#define G_PCI_ERR  0xC7u // write-1-to-clear
#define G_ERR_ADDR 0xC8u // read-only
#define G_ESCR1    0xE0u
#define G_ESCR2    0xE8u
#define G_MCCR1    0xF0u
#define G_MCCR2    0xF4u
#define G_MCCR3    0xF8u
#define G_MCCR4    0xFCu

// PICR1 bits (Table 3-35).
#define PICR1_TEA_EN   0x00000400u
#define PICR1_MCP_EN   0x00000800u
#define PICR1_RCS0     0x00100000u // read-only strap: ROM on the memory bus
#define PICR1_MAP_A    0x00010000u // 1 = address map A (never on a Mac)
#define MCCR1_MEMGO    0x00080000u
#define MCCR1_8N64     0x00200000u // read-only strap: 1 = 8-bit ROM
#define PCI_STATUS_W1C 0xF900u // parity, SERR, the abort bits (Table 3-13)

// Little-endian register access into the byte file.
static uint32_t cfg32(const gos_grackle_t *g, uint32_t reg) {
    return (uint32_t)g->cfg[reg] | ((uint32_t)g->cfg[reg + 1] << 8) | ((uint32_t)g->cfg[reg + 2] << 16) |
           ((uint32_t)g->cfg[reg + 3] << 24);
}

static void set_cfg32(gos_grackle_t *g, uint32_t reg, uint32_t v) {
    for (int i = 0; i < 4; i++)
        g->cfg[reg + i] = (uint8_t)(v >> (8 * i));
}

// ============================================================
// The memory controller
// ============================================================

// A bank's decoded [lower, upper] window (§3.2.6.1-3): 8-bit start/end
// in 1 MB units, extended by two bits each.
static void bank_window(const gos_grackle_t *g, unsigned n, uint32_t *lo, uint32_t *hi) {
    unsigned q = n & 3u, off = (n >> 2) * 4u;
    uint32_t start = g->cfg[G_MSTART + off + q], end = g->cfg[G_MEND + off + q];
    uint32_t xstart = (g->cfg[G_XMSTART + off + q]) & 3u, xend = (g->cfg[G_XMEND + off + q]) & 3u;
    *lo = ((xstart << 8) | start) << 20;
    *hi = (((xend << 8) | end) << 20) | 0xFFFFFu;
}

void gos_grackle_remap(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    gos_grackle_t *g = &st->grackle;
    uint8_t *ram = ram_native_pointer(cfg->memory_map, 0);
    // The whole RAM decode space goes quiet first: an access outside every
    // enabled bank reads all-ones and drops writes (§6), which an empty
    // page gives by default.
    for (uint32_t p = 0; p < (0x40000000u >> PAGE_SHIFT); p++)
        gos_clear_page(p);
    bool memgo = (cfg32(g, G_MCCR1) & MCCR1_MEMGO) != 0;
    uint8_t enables = g->cfg[G_BANK_EN];
    if (!memgo) {
        LOG(3, "remap: MEMGO clear, no RAM decoded (enables $%02X)", enables);
        return;
    }
    for (unsigned n = 0; n < GOS_MEM_BANKS; n++) {
        if (!(enables & (1u << n)))
            continue;
        uint32_t lo, hi;
        bank_window(g, n, &lo, &hi);
        if (hi < lo) {
            LOG(1, "bank %u enabled with end $%08X below start $%08X: decodes nothing", n, hi, lo);
            continue;
        }
        uint32_t size = g->bank_size[n];
        if (!size) {
            LOG(2, "bank %u enabled at $%08X-$%08X but no DIMM side sits there", n, lo, hi);
            continue;
        }
        uint8_t *host = ram + g->bank_host_off[n];
        uint32_t span = hi - lo + 1u;
        for (uint32_t p = 0; p < (span >> PAGE_SHIFT); p++)
            gos_fill_page((lo >> PAGE_SHIFT) + p, host + (((uint64_t)p << PAGE_SHIFT) % size), true);
        LOG(2, "bank %u: $%08X-$%08X -> %u MB of DIMM storage at host +$%X", n, lo, hi, size >> 20,
            g->bank_host_off[n]);
    }
    // The PPC core caches translations of the physical map.
    if (cfg->ppc)
        ppc_mmu_invalidate_all(cfg->ppc);
}

// Assign each populated DIMM side to a Grackle chip select, in slot order:
// DIMM k side s drives bank 2k+s (the developer note's linear, non-
// interleaved organisation).  Storage is contiguous in host RAM, in the
// same order, so a ROM that stacks the banks from 0 upward sees one
// contiguous array.
static void bank_inventory(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    gos_grackle_t *g = &st->grackle;
    uint32_t off = 0;
    memset(g->bank_size, 0, sizeof(g->bank_size));
    memset(g->bank_host_off, 0, sizeof(g->bank_host_off));
    for (unsigned k = 0; k < GOS_DIMMS; k++) {
        const gos_dimm_t *d = &st->dimm[k];
        for (unsigned s = 0; s < d->sides && s < 2; s++) {
            unsigned n = 2 * k + s;
            g->bank_size[n] = d->side_bytes;
            g->bank_host_off[n] = off;
            off += d->side_bytes;
        }
    }
}

// ============================================================
// Grackle's own configuration space (bus 0, device 0)
// ============================================================

void gos_grackle_reset(config_t *cfg) {
    gos_grackle_t *g = &gos_st(cfg)->grackle;
    memset(g->cfg, 0, sizeof(g->cfg));
    g->cfg_addr = 0;
    // Identity, command/status, class (Table 3-10).
    g->cfg[0x00] = (uint8_t)GRACKLE_VENDOR;
    g->cfg[0x01] = (uint8_t)(GRACKLE_VENDOR >> 8);
    g->cfg[0x02] = (uint8_t)GRACKLE_DEVICE;
    g->cfg[0x03] = (uint8_t)(GRACKLE_DEVICE >> 8);
    g->cfg[G_CMD] = 0x06; // bus master + memory space
    g->cfg[G_STATUS] = 0x80; // fast back-to-back capable
    g->cfg[0x08] = GRACKLE_REVISION;
    g->cfg[0x0B] = 0x06; // bridge / host bridge / 0
    g->cfg[0x0C] = 0x08; // cache line size
    g->cfg[0x73] = 0xCD; // ODCR (Rev 4.0 addendum)
    // PICR1 $FF000010 with the straps: RCS0 = 1 (ROM on the memory bus),
    // ADDRESS_MAP = 0 (map B).  PICR2 $000C060C.
    set_cfg32(g, G_PICR1, 0xFF000010u | PICR1_RCS0);
    set_cfg32(g, G_PICR2, 0x000C060Cu);
    g->cfg[0xBA] = 0x04; // alternate OS-visible parameters 1
    g->cfg[G_ERREN1] = 0x01; // 60x bus-error reporting enabled
    set_cfg32(g, G_ESCR1, 0x0FFF0042u);
    set_cfg32(g, G_ESCR2, 0x00000020u);
    // MCCR1 $FF820000 with 8N64 = 0 (64-bit ROM), MCCR2 3, MCCR4 RCBUF.
    set_cfg32(g, G_MCCR1, 0xFF820000u);
    set_cfg32(g, G_MCCR2, 0x00000003u);
    set_cfg32(g, G_MCCR4, 0x00100000u);
    gos_grackle_remap(cfg);
}

// Read-only bytes of the file (identity, class, the error address) and the
// pin-strap bits inside writable registers.
static bool byte_read_only(uint32_t reg) {
    return reg < 0x04u || (reg >= 0x08u && reg < 0x0Cu) || reg == 0x0Eu || (reg >= 0x3Cu && reg < 0x40u) ||
           (reg >= G_ERR_ADDR && reg < G_ERR_ADDR + 4u) || reg == 0x40u || reg == 0x42u || reg == 0x44u ||
           reg == 0x45u || (reg >= 0xE4u && reg < 0xE8u) || (reg >= 0xECu && reg < 0xF0u);
}

static bool grackle_cfg_read(pci_device_t *dev, uint32_t reg, uint32_t *out) {
    config_t *cfg = (config_t *)dev->priv;
    *out = cfg32(&gos_st(cfg)->grackle, reg & 0xFCu);
    return true; // the whole 256-byte file is Grackle's own
}

static bool grackle_cfg_write(pci_device_t *dev, uint32_t reg, uint32_t byte, uint8_t value) {
    config_t *cfg = (config_t *)dev->priv;
    gos_grackle_t *g = &gos_st(cfg)->grackle;
    uint32_t at = (reg & 0xFCu) + (byte & 3u);
    if (byte_read_only(at)) {
        LOG(3, "write to read-only config $%02X = $%02X ignored", at, value);
        return true;
    }
    uint8_t old = g->cfg[at];
    switch (at) {
    case G_STATUS:
    case G_STATUS + 1: {
        // Write-1-to-clear error bits; the capability bits are hardwired.
        uint16_t w1c = (uint16_t)(at == G_STATUS ? (PCI_STATUS_W1C & 0xFFu) : (PCI_STATUS_W1C >> 8));
        g->cfg[at] = (uint8_t)(old & ~(value & w1c));
        return true;
    }
    case G_ERRDR1:
    case G_60X_ERR:
    case G_ERRDR2:
    case G_PCI_ERR:
        g->cfg[at] = (uint8_t)(old & ~value); // write-1-to-clear
        return true;
    case G_CMD:
        // I/O space is hardwired 0 (Table 3-12); memory and master latch.
        g->cfg[at] = (uint8_t)(value & 0x46u);
        return true;
    case G_PICR1 + 2:
        // RCS0 (bit 20) is a strap; ADDRESS_MAP (bit 16) latches but map A
        // is never decoded — log if a guest ever asks for it.
        value = (uint8_t)((value & ~0x10u) | 0x10u);
        if (value & 0x01u)
            LOG(0, "PICR1 selects address map A; only map B is modelled");
        break;
    case G_MCCR1 + 2:
        value = (uint8_t)(value & ~(uint8_t)(MCCR1_8N64 >> 16)); // 8N64 = 0: a 64-bit ROM
        break;
    default:
        break;
    }
    g->cfg[at] = value;
    if (at >= 0xA8u && at < 0xB0u)
        LOG(2, "%s = $%08X (pc $%08X)", at < 0xACu ? "PICR1" : "PICR2", cfg32(g, at & 0xFCu),
            cfg->ppc ? ppc_get_pc(cfg->ppc) : 0);
    // The memory map follows the bank registers, the enables and MEMGO.
    if ((at >= G_MSTART && at < G_BANK_EN + 1u) || at == G_MCCR1 + 2u) {
        if (old != value) {
            LOG(2, "memory config $%02X: $%02X -> $%02X", at, old, value);
            gos_grackle_remap(cfg);
        }
    } else if (at >= G_MCCR1 && at < G_MCCR4 + 4u && old != value) {
        LOG(3, "MCCR%u = $%08X", 1u + ((at - G_MCCR1) >> 2), cfg32(g, at & 0xFCu));
    }
    return true;
}

static const char *grackle_name(const pci_device_t *dev) {
    (void)dev;
    return "Grackle";
}

// The generic header answers nothing here (the ops claim every register);
// the declaration only feeds the object model's identity readout.
static const pci_config_decl_t grackle_decl = {
    .vendor_id = GRACKLE_VENDOR,
    .device_id = GRACKLE_DEVICE,
    .revision = GRACKLE_REVISION,
    .class_code = 0x060000u,
    .header_type = 0x00u,
};

static const pci_device_ops_t grackle_ops = {
    .cfg_read = grackle_cfg_read,
    .cfg_write = grackle_cfg_write,
    .name = grackle_name,
};

// ============================================================
// The configuration mechanism
// ============================================================

// Decode the CONFIG_ADDR latch: false when no cycle is generated or it
// addresses nothing that can answer (enable clear, a secondary bus, or a
// device number with no IDSEL).
static bool cfg_decode(const gos_grackle_t *g, int *dev, uint32_t *fn, uint32_t *reg) {
    uint32_t a = g->cfg_addr;
    if (!(a & 0x80000000u))
        return false; // E = 0: no cycle
    if ((a >> 16) & 0xFFu)
        return false; // type 1: no PCI-PCI bridge exists on this board
    int d = (int)((a >> 11) & 0x1Fu);
    if (d != 0 && (d < 11 || d > 30))
        return false; // devices 1-10 and 31 have no IDSEL line (Table 7-4)
    *dev = d;
    *fn = (a >> 8) & 7u;
    *reg = a & 0xFCu;
    return true;
}

static uint8_t data_read8(config_t *cfg, uint32_t offset) {
    gossamer_state_t *st = gos_st(cfg);
    int dev;
    uint32_t fn, reg;
    if (!cfg_decode(&st->grackle, &dev, &fn, &reg))
        return 0xFF;
    uint32_t v = pci_bus_cfg_read(st->bus, dev, fn, reg);
    return (uint8_t)(v >> (8 * (offset & 3u)));
}

static void data_write8(config_t *cfg, uint32_t offset, uint8_t value) {
    gossamer_state_t *st = gos_st(cfg);
    int dev;
    uint32_t fn, reg;
    if (!cfg_decode(&st->grackle, &dev, &fn, &reg))
        return;
    LOG(3, "config write dev $%02X reg $%02X+%u = $%02X", dev, reg, offset & 3u, value);
    pci_bus_cfg_write(st->bus, dev, fn, reg, offset & 3u, value);
}

// CONFIG_ADDR: byte i of the port is byte i of the little-endian latch.
static uint8_t addr_read8(void *ctx, uint32_t offset) {
    return (uint8_t)(gos_st((config_t *)ctx)->grackle.cfg_addr >> (8 * (offset & 3u)));
}

static void addr_write8(void *ctx, uint32_t offset, uint8_t value) {
    gos_grackle_t *g = &gos_st((config_t *)ctx)->grackle;
    uint32_t shift = 8 * (offset & 3u);
    g->cfg_addr = (g->cfg_addr & ~(0xFFu << shift)) | ((uint32_t)value << shift);
}

static uint16_t addr_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((addr_read8(ctx, offset) << 8) | addr_read8(ctx, offset + 1));
}

static uint32_t addr_read32(void *ctx, uint32_t offset) {
    (void)offset;
    return GOS_LE32(gos_st((config_t *)ctx)->grackle.cfg_addr); // the stwbrx/lwbrx view
}

static void addr_write16(void *ctx, uint32_t offset, uint16_t value) {
    addr_write8(ctx, offset, (uint8_t)(value >> 8));
    addr_write8(ctx, offset + 1, (uint8_t)value);
}

static void addr_write32(void *ctx, uint32_t offset, uint32_t value) {
    (void)offset;
    gos_grackle_t *g = &gos_st((config_t *)ctx)->grackle;
    g->cfg_addr = GOS_LE32(value);
    LOG(4, "CONFIG_ADDR = $%08X", g->cfg_addr);
}

// CONFIG_DATA: byte n of the port is configuration byte (reg + n).
static uint8_t cdata_read8(void *ctx, uint32_t offset) {
    return data_read8((config_t *)ctx, offset);
}

static uint16_t cdata_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((cdata_read8(ctx, offset) << 8) | cdata_read8(ctx, offset + 1));
}

static uint32_t cdata_read32(void *ctx, uint32_t offset) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; i++)
        v = (v << 8) | cdata_read8(ctx, offset + i);
    return v;
}

static void cdata_write8(void *ctx, uint32_t offset, uint8_t value) {
    data_write8((config_t *)ctx, offset, value);
}

static void cdata_write16(void *ctx, uint32_t offset, uint16_t value) {
    cdata_write8(ctx, offset, (uint8_t)(value >> 8));
    cdata_write8(ctx, offset + 1, (uint8_t)value);
}

static void cdata_write32(void *ctx, uint32_t offset, uint32_t value) {
    for (uint32_t i = 0; i < 4; i++)
        cdata_write8(ctx, offset + i, (uint8_t)(value >> (24 - 8 * i)));
}

// ============================================================
// Interrupt acknowledge ($FEF00000) and the board page ($FF000000)
// ============================================================

// PCI interrupt acknowledge: no 8259 answers on this board, so the cycle
// master-aborts; reads float to all-ones, writes are an unsupported 60x
// cycle that nothing reports with the reset error enables.
static uint8_t ia_read8(void *ctx, uint32_t o) {
    (void)ctx;
    LOG(2, "interrupt-acknowledge read +$%X", o);
    return 0xFF;
}
static uint16_t ia_read16(void *ctx, uint32_t o) {
    return (uint16_t)((ia_read8(ctx, o) << 8) | 0xFFu);
}
static uint32_t ia_read32(void *ctx, uint32_t o) {
    (void)ia_read8(ctx, o);
    return 0xFFFFFFFFu;
}
static void ia_write8(void *ctx, uint32_t o, uint8_t v) {
    (void)ctx;
    LOG(2, "interrupt-acknowledge write +$%X = $%02X dropped", o, v);
}
static void ia_write16(void *ctx, uint32_t o, uint16_t v) {
    ia_write8(ctx, o, (uint8_t)v);
}
static void ia_write32(void *ctx, uint32_t o, uint32_t v) {
    ia_write8(ctx, o, (uint8_t)v);
}

uint16_t gos_board_id(config_t *cfg) {
    return gos_board(cfg)->board_id;
}

// ROM bank 1: the board-identification latch answers the halfword at +4
// (the boot program reads it with lhz, Darwin as the high half of a word
// at +4, the nvramrc with `4+ w@`); the rest of the 8 MB is an absent ROM
// and reads all-ones.
static uint8_t board_read8(void *ctx, uint32_t o) {
    config_t *cfg = (config_t *)ctx;
    uint16_t id = gos_board_id(cfg);
    if (o == 4)
        return (uint8_t)(id >> 8);
    if (o == 5)
        return (uint8_t)id;
    return 0xFF;
}
static uint16_t board_read16(void *ctx, uint32_t o) {
    return (uint16_t)((board_read8(ctx, o) << 8) | board_read8(ctx, o + 1));
}
static uint32_t board_read32(void *ctx, uint32_t o) {
    return ((uint32_t)board_read16(ctx, o) << 16) | board_read16(ctx, o + 2);
}
static void board_write8(void *ctx, uint32_t o, uint8_t v) {
    (void)ctx;
    LOG(2, "write to ROM bank 1 +$%X = $%02X dropped (FLASH_WR_EN clear)", o, v);
}
static void board_write16(void *ctx, uint32_t o, uint16_t v) {
    board_write8(ctx, o, (uint8_t)v);
}
static void board_write32(void *ctx, uint32_t o, uint32_t v) {
    board_write8(ctx, o, (uint8_t)v);
}

// Master-abort policy for the bus windows: trap only once the guest has
// enabled TEA or MCP reporting (PICR1 bits 10/11).
static bool grackle_abort_faults(void *ctx, bool write) {
    (void)write;
    config_t *cfg = (config_t *)ctx;
    return (cfg32(&gos_st(cfg)->grackle, G_PICR1) & (PICR1_TEA_EN | PICR1_MCP_EN)) != 0;
}

// ============================================================
// Wiring
// ============================================================

static void iface_set(memory_interface_t *mi, uint8_t (*r8)(void *, uint32_t), uint16_t (*r16)(void *, uint32_t),
                      uint32_t (*r32)(void *, uint32_t), void (*w8)(void *, uint32_t, uint8_t),
                      void (*w16)(void *, uint32_t, uint16_t), void (*w32)(void *, uint32_t, uint32_t)) {
    mi->read_uint8 = r8;
    mi->read_uint16 = r16;
    mi->read_uint32 = r32;
    mi->write_uint8 = w8;
    mi->write_uint16 = w16;
    mi->write_uint32 = w32;
}

void gos_grackle_init(config_t *cfg, checkpoint_t *cp) {
    gossamer_state_t *st = gos_st(cfg);
    bank_inventory(cfg);

    // The configuration ports: every word of each range aliases the port
    // (the decode ignores the low address bits beyond the byte lane).
    iface_set(&st->cfg_addr_if, addr_read8, addr_read16, addr_read32, addr_write8, addr_write16, addr_write32);
    iface_set(&st->cfg_data_if, cdata_read8, cdata_read16, cdata_read32, cdata_write8, cdata_write16, cdata_write32);
    iface_set(&st->intack_if, ia_read8, ia_read16, ia_read32, ia_write8, ia_write16, ia_write32);
    iface_set(&st->board_if, board_read8, board_read16, board_read32, board_write8, board_write16, board_write32);
    memory_map_add(cfg->memory_map, GOS_CFG_ADDR_BASE, GOS_CFG_ADDR_SIZE, "Grackle CONFIG_ADDR", &st->cfg_addr_if, cfg);
    memory_map_add(cfg->memory_map, GOS_CFG_DATA_BASE, GOS_CFG_DATA_SIZE, "Grackle CONFIG_DATA", &st->cfg_data_if, cfg);
    memory_map_add(cfg->memory_map, GOS_INTACK_BASE, 0x00100000u, "Grackle INT ACK", &st->intack_if, cfg);
    memory_map_add(cfg->memory_map, GOS_BOARD_BASE, GOS_BOARD_SIZE, "board register", &st->board_if, cfg);

    // The one PCI bus, and Grackle's own header at device 0.
    st->bus = pci_bus_create(cfg->pci, "Grackle", GOS_PCI_BUS);
    pci_bus_set_abort_policy(st->bus, grackle_abort_faults, cfg);
    st->grackle_dev.ops = &grackle_ops;
    st->grackle_dev.decl = &grackle_decl;
    st->grackle_dev.priv = cfg;
    pci_bus_add_device(st->bus, &st->grackle_dev, GOS_DEV_GRACKLE);
    pci_device_part(cfg, cp, &st->grackle_dev, "pci.grackle");

    // Map B's windows.  PCI memory passes through 1:1; the 16 MB ISA-memory
    // alias reaches PCI memory 0; PCI I/O is 8 MB from 0 (the ROM's own
    // `ranges`: `01000000 0 0 fe000000 0 00800000`).
    pci_bus_add_window(st->bus, PCI_SPACE_MEM, GOS_PCI_MEM_BASE, GOS_PCI_MEM_SIZE, GOS_PCI_MEM_BASE, 0xFFFFFFFFu,
                       "PCI memory (Grackle)");
    pci_bus_add_window(st->bus, PCI_SPACE_MEM, GOS_PCI_ISA_MEM, 0x01000000u, 0x0u, 0x00FFFFFFu,
                       "PCI ISA memory (Grackle)");
    pci_bus_add_window(st->bus, PCI_SPACE_IO, GOS_PCI_IO_BASE, GOS_PCI_IO_SIZE, 0x0u, 0x007FFFFFu, "PCI I/O (Grackle)");

    gos_grackle_reset(cfg);
}

// ============================================================
// Object node: machine.grackle (read-only register view)
// ============================================================

static gos_grackle_t *grk_obj(struct object *self) {
    gossamer_state_t *st = gos_st((config_t *)object_data(self));
    return st ? &st->grackle : NULL;
}

// Bytes of RAM the controller decodes right now: the enabled banks'
// windows that have a DIMM side behind them, with MEMGO set.
static uint32_t grk_decoded_bytes(const gos_grackle_t *g) {
    if (!(cfg32(g, G_MCCR1) & MCCR1_MEMGO))
        return 0;
    uint32_t total = 0;
    for (unsigned n = 0; n < GOS_MEM_BANKS; n++) {
        uint32_t lo, hi;
        if (!(g->cfg[G_BANK_EN] & (1u << n)) || !g->bank_size[n])
            continue;
        bank_window(g, n, &lo, &hi);
        if (hi >= lo)
            total += hi - lo + 1u;
    }
    return total;
}

#define GRK_U32_ATTR(NAME, EXPR)                                                                                       \
    static value_t grk_attr_##NAME(struct object *self, const member_t *m) {                                           \
        (void)m;                                                                                                       \
        const gos_grackle_t *g = grk_obj(self);                                                                        \
        value_t v = val_uint(4, g ? (EXPR) : 0u);                                                                      \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }

GRK_U32_ATTR(picr1, cfg32(g, G_PICR1))
GRK_U32_ATTR(picr2, cfg32(g, G_PICR2))
GRK_U32_ATTR(mccr1, cfg32(g, G_MCCR1))
GRK_U32_ATTR(mccr2, cfg32(g, G_MCCR2))
GRK_U32_ATTR(mccr3, cfg32(g, G_MCCR3))
GRK_U32_ATTR(mccr4, cfg32(g, G_MCCR4))
GRK_U32_ATTR(bank_enable, g->cfg[G_BANK_EN])
GRK_U32_ATTR(ram_decoded, grk_decoded_bytes(g))
GRK_U32_ATTR(config_address, g->cfg_addr)

static value_t grk_method_config(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    const gos_grackle_t *g = grk_obj(self);
    if (!g)
        return val_err("grackle not available");
    uint64_t reg = argv[0].u;
    if (reg > 0xFCu || (reg & 3u))
        return val_err("grackle.config: register $%llX is not a dword offset in $00-$FC", (unsigned long long)reg);
    value_t v = val_uint(4, cfg32(g, (uint32_t)reg));
    v.flags |= VAL_HEX;
    return v;
}

#define GRK_RO_ATTR(NAME, DOC)                                                                                         \
    {                                                                                                                  \
        .kind = M_ATTR, .name = #NAME, .doc = DOC, .attr = {                                                           \
            .type = V_UINT,                                                                                            \
            .presentation_flags = VAL_HEX,                                                                             \
            .get = grk_attr_##NAME,                                                                                    \
            .set = NULL                                                                                                \
        }                                                                                                              \
    }

static const arg_decl_t grk_config_args[] = {
    {.name = "reg", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "dword-aligned register offset ($00-$FC)"},
};

static const member_t grk_members[] = {
    GRK_RO_ATTR(picr1, "Processor interface configuration 1 ($A8)"),
    GRK_RO_ATTR(picr2, "Processor interface configuration 2 ($AC)"),
    GRK_RO_ATTR(mccr1, "Memory control configuration 1 ($F0; bit 19 = MEMGO)"),
    GRK_RO_ATTR(mccr2, "Memory control configuration 2 ($F4)"),
    GRK_RO_ATTR(mccr3, "Memory control configuration 3 ($F8)"),
    GRK_RO_ATTR(mccr4, "Memory control configuration 4 ($FC)"),
    GRK_RO_ATTR(bank_enable, "Memory bank enable ($A0)"),
    GRK_RO_ATTR(ram_decoded, "Bytes of RAM the enabled banks decode (0 while MEMGO is clear)"),
    GRK_RO_ATTR(config_address, "The CONFIG_ADDR latch (CF8 format)"),
    {.kind = M_METHOD,
                                                            .name = "config",
                                                            .doc = "Read one of Grackle's own configuration dwords (device 0)",
                                                            .method = {.args = grk_config_args, .nargs = 1, .result = V_UINT, .fn = grk_method_config}},
};

static const class_desc_t grk_class = {
    .name = "grackle",
    .members = grk_members,
    .n_members = sizeof(grk_members) / sizeof(grk_members[0]),
};

void gos_grackle_attach_objects(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (!st || st->grackle_object)
        return;
    st->grackle_object = object_new(&grk_class, cfg, "grackle");
    if (st->grackle_object) {
        object_set_label(st->grackle_object, "Grackle");
        object_set_order(st->grackle_object, 44);
        object_attach(machine_object(), st->grackle_object);
    }
}

void gos_grackle_detach_objects(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (st && st->grackle_object) {
        object_detach(st->grackle_object);
        object_delete(st->grackle_object);
        st->grackle_object = NULL;
    }
}
