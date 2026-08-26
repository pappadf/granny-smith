// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// bandit.c
// Bandit (343S1126), the AR-to-PCI host bridge, and Chaos (343S1155), the
// same silicon family adapted to the display (VCI) bus.  The 7500 has one
// Bandit; the 8500/9500 add a second at $F4000000; all three TNT machines
// carry Chaos at $F0000000.
//
// This file is the family's PCI BRIDGE ADAPTER: it owns the chipset facts
// (where the config ports live, how the address latch decodes, which
// windows the bridge forwards, the Chaos quirks) and delegates every
// config cycle to the generic bus controller in core/peripherals/pci/.
// A bridge's own header is just another registered device — which is why
// "an empty slot reads all-ones" needs no code here at all: it is simply
// "no device is registered at that IDSEL" (pci_bus_cfg_read).
//
// Software sees exactly two ports per bridge plus a config header:
//
//   * config ADDRESS at base+$800000 (4 bytes, little-endian): type-0
//     cycles select a device by ONE-HOT IDSEL — device N is bit N — and
//     type-1 cycles (bit 0 set) address subordinate buses.  Zero means
//     IDLE, not "device 0": the classic probe sequence writes the address,
//     touches the data port, then writes zero (NetBSD macppc bandit.c).
//     The port reads back its latch.
//   * config DATA at base+$C00000 (8 bytes decoded): the low two bits of
//     the config offset are carried by WHICH BYTE of the port is touched
//     (data + (offset & 3)).
//   * the bridge's own header answers at device 11 — vendor $106B, device
//     $0001, revision $03 — with two documented mode registers: $48
//     "address select" (the coarse/fine decode masks; Apple's IOKit
//     AppleMacRiscPCI driver is the only documentation) and $50 "mode
//     select" whose bit $40 is the PCI coherency enable every OS sets and
//     reads back.
//
//   Devices 0-10 do not exist on the primary bus: config reads of them
//   return all-ones (Linux refuses dev_fn < 11<<3; NetBSD returns
//   $FFFFFFFF).  Absent devices likewise read all-ones — a probe must
//   never hang.
//
// Chaos differs in three attested ways: its header reads device $0003;
// most of its BARs are not safely readable (config reads outside
// $00-$0F/$14/$18 return all-ones — the two exceptions are exactly
// Control's register and VRAM BARs: the ROM's own probe-slots sizes and
// assigns them); and writes to the rest of its config space are ignored
// ("/chaos really hates writes to config space" — NetBSD; Linux's
// chaos_map_bus applies the same offsets).  It has no PCI I/O window at
// all.  Both quirks live in this adapter, applied around the generic
// device dispatch, because they are chipset facts about CHAOS, not about
// the device behind it.
//
// Empty PCI MEMORY space faults recoverably: Open Firmware and the OS
// probe with catchable machine checks (the BART precedent), which is what
// the bus's decode windows provide (pci_bus_add_window).

#include "tnt.h"

#include "log.h"
#include "pci.h"

#include <stdio.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("bandit");

// PCI ids (config dword 0 = device<<16 | vendor, little-endian layout)
#define PCI_VENDOR_APPLE 0x106Bu
#define BANDIT_DEVICE_ID 0x0001u
#define BANDIT_REVISION  0x03u

// Config-space offsets of the bridge's documented mode registers
#define BANDIT_ADDR_SELECT 0x48u
#define BANDIT_MODE_SELECT 0x50u
#define BANDIT_COHERENT    0x40u // mode-select bit every OS latches on

// The bridge's own device-11 header.  A host bridge's command register is
// not software-settable on this part (Apple documents only $48/$50), so no
// command bits are writable and none are hardwired on: the bridge decodes
// by strap, not by config.
static const pci_config_decl_t bandit_self_decl = {
    .vendor_id = PCI_VENDOR_APPLE,
    .device_id = BANDIT_DEVICE_ID,
    .revision = BANDIT_REVISION,
    .class_code = 0x060000u, // host bridge
    .header_type = 0x00u,
};

// ============================================================
// Config-space contents of the bridge's own device-11 header
// ============================================================

// The $48 address-select value for a bridge: upper halfword = coarse mask
// (256 MB windows at n<<28), lower = fine mask (16 MB windows at
// $F0000000 | n<<24).  Derived from what this model actually decodes so
// an OS computing its bridge ranges from the register agrees with the
// memory map (Apple's IOKit driver builds the ranges exactly this way).
static uint32_t bandit_addr_select(const tnt_bandit_t *b) {
    switch (b->base) {
    case TNT_BANDIT1_BASE:
        // 256 MB PCI memory at $80000000; 16 MB windows $F2 (bridge) and
        // $F3 (PCI I/O — the Grand Central island).
        return (0x0100u << 16) | 0x000Cu;
    case TNT_BANDIT2_BASE:
    default:
        // Second bridge: its $F4/$F5 windows only.  No 256 MB memory-space
        // window is decoded — which memory range Bandit 2 forwards is
        // still open (Apple's notes quote $90000000 for both Bandit 2 and
        // Chaos, and a board carrying both cannot have them collide;
        // proposal-pci-architecture §14 Q5).  The register and the decode
        // stay consistent: we claim nothing, so we advertise nothing.
        return 0x0030u;
    }
}

// The bridge's quirk registers.  Everything else falls through to the
// generic type-0 header (config_space.c).
static bool bridge_cfg_read(pci_device_t *dev, uint32_t reg, uint32_t *out) {
    tnt_bandit_t *b = (tnt_bandit_t *)dev->priv;
    switch (reg) {
    case BANDIT_ADDR_SELECT:
        *out = bandit_addr_select(b);
        return true;
    case BANDIT_MODE_SELECT:
        *out = b->mode_select;
        return true;
    default:
        return false;
    }
}

static bool bridge_cfg_write(pci_device_t *dev, uint32_t reg, uint32_t byte, uint8_t value) {
    tnt_bandit_t *b = (tnt_bandit_t *)dev->priv;
    if (reg != BANDIT_MODE_SELECT)
        return false;
    // The documented latch: whatever the OS writes reads back (the
    // coherency handshake is "read, OR in $40, write, read back").
    uint32_t shift = 8u * (byte & 3u);
    b->mode_select = (b->mode_select & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    LOG(2, "mode-select now $%08X (coherency %s)", b->mode_select, (b->mode_select & BANDIT_COHERENT) ? "on" : "off");
    return true;
}

static const char *bridge_name(const pci_device_t *dev) {
    const tnt_bandit_t *b = (const tnt_bandit_t *)dev->priv;
    return b->is_chaos ? "Chaos" : "Bandit";
}

static const pci_device_ops_t bandit_self_ops = {
    .cfg_read = bridge_cfg_read,
    .cfg_write = bridge_cfg_write,
    .name = bridge_name,
};

// Decode the address latch into (device, function, register), one-hot
// IDSEL.  Returns false when the cycle addresses nothing this model
// answers (idle latch, type-1, devices 0-10, empty IDSELs, multi-hot).
static bool decode_type0(tnt_bandit_t *b, int *dev, uint32_t *fn, uint32_t *reg) {
    uint32_t a = b->cfg_addr;
    if (a == 0 || (a & 1u))
        return false; // idle, or type-1: no subordinate buses exist
    uint32_t idsel = a & 0xFFFFF800u; // AD11..AD31 carry the one-hot select
    if (idsel == 0 || (idsel & (idsel - 1)) != 0)
        return false; // no device selected, or an illegal multi-hot cycle
    int d = 31 - __builtin_clz(idsel);
    *dev = d;
    *fn = (a >> 8) & 7u;
    *reg = a & 0xFCu;
    return true;
}

// Chaos restricts what of its config space is safely READABLE: $00-$0F,
// $14 and $18 only; everything else returns all-ones.  The two readable
// BARs are exactly Control's.
static bool chaos_readable(uint32_t reg) {
    return reg <= 0x0Cu || reg == 0x14u || reg == 0x18u;
}

// ...and it ignores writes outside those two BAR offsets.
static bool chaos_writable(uint32_t reg) {
    return reg == 0x14u || reg == 0x18u;
}

// Full-dword config read at the latched address (little-endian value).
static uint32_t config_read(tnt_bandit_t *b) {
    int dev;
    uint32_t fn, reg;
    if (!decode_type0(b, &dev, &fn, &reg))
        return 0xFFFFFFFFu;
    if (b->is_chaos && !chaos_readable(reg))
        return 0xFFFFFFFFu;
    return pci_bus_cfg_read(b->bus, dev, fn, reg);
}

static void config_write(tnt_bandit_t *b, uint32_t byte, uint32_t value, uint32_t mask) {
    int dev;
    uint32_t fn, reg;
    if (!decode_type0(b, &dev, &fn, &reg))
        return;
    if (b->is_chaos && !chaos_writable(reg)) {
        LOG(2, "chaos config write $%02X ignored", reg);
        return;
    }
    pci_bus_cfg_write(b->bus, dev, fn, reg, byte, (uint8_t)(value & mask));
}

// ============================================================
// The config ADDRESS port (base + $800000, 4 bytes, little-endian)
// ============================================================

static uint8_t addr_read8(void *ctx, uint32_t offset) {
    tnt_bandit_t *b = (tnt_bandit_t *)ctx;
    return (uint8_t)(b->cfg_addr >> (8 * (offset & 3u)));
}

static uint16_t addr_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((addr_read8(ctx, offset) << 8) | addr_read8(ctx, offset + 1));
}

static uint32_t addr_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)addr_read16(ctx, offset) << 16) | addr_read16(ctx, offset + 2);
}

static void addr_write8(void *ctx, uint32_t offset, uint8_t value) {
    tnt_bandit_t *b = (tnt_bandit_t *)ctx;
    uint32_t shift = 8 * (offset & 3u);
    b->cfg_addr = (b->cfg_addr & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    LOG(3, "$%08X config addr = $%08X", b->base, b->cfg_addr);
}

static void addr_write16(void *ctx, uint32_t offset, uint16_t value) {
    addr_write8(ctx, offset, (uint8_t)(value >> 8));
    addr_write8(ctx, offset + 1, (uint8_t)value);
}

static void addr_write32(void *ctx, uint32_t offset, uint32_t value) {
    addr_write16(ctx, offset, (uint16_t)(value >> 16));
    addr_write16(ctx, offset + 2, (uint16_t)value);
}

// ============================================================
// The config DATA port (base + $C00000, 8 bytes decoded)
// ============================================================
// Byte j of the port is byte j of the little-endian config dword; the port
// repeats across its 8 decoded bytes with (offset & 3) selecting the lane.

static uint8_t data_read8(void *ctx, uint32_t offset) {
    tnt_bandit_t *b = (tnt_bandit_t *)ctx;
    uint32_t v = config_read(b);
    uint8_t byte = (uint8_t)(v >> (8 * (offset & 3u)));
    LOG(3, "$%08X config data[%u] -> $%02X (addr $%08X)", b->base, offset & 7u, byte, b->cfg_addr);
    return byte;
}

static uint16_t data_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((data_read8(ctx, offset) << 8) | data_read8(ctx, offset + 1));
}

static uint32_t data_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)data_read16(ctx, offset) << 16) | data_read16(ctx, offset + 2);
}

static void data_write8(void *ctx, uint32_t offset, uint8_t value) {
    tnt_bandit_t *b = (tnt_bandit_t *)ctx;
    LOG(3, "$%08X config data[%u] = $%02X (addr $%08X)", b->base, offset & 7u, value, b->cfg_addr);
    config_write(b, offset & 3u, value, 0xFFu);
}

static void data_write16(void *ctx, uint32_t offset, uint16_t value) {
    data_write8(ctx, offset, (uint8_t)(value >> 8));
    data_write8(ctx, offset + 1, (uint8_t)value);
}

static void data_write32(void *ctx, uint32_t offset, uint32_t value) {
    data_write16(ctx, offset, (uint16_t)(value >> 16));
    data_write16(ctx, offset + 2, (uint16_t)value);
}

// ============================================================
// Wiring
// ============================================================

// One bridge: its two config ports, its bus, and its own device-11 header
// seated on that bus.
static tnt_bandit_t *bridge_add(config_t *cfg, uint32_t base, bool is_chaos, int bus_index, const char *name) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_bandit_t *b = &st->bridge[st->bridge_count++];
    memset(b, 0, sizeof(*b));
    b->base = base;
    b->is_chaos = is_chaos;
    b->cfg = cfg;
    b->addr_if.read_uint8 = addr_read8;
    b->addr_if.read_uint16 = addr_read16;
    b->addr_if.read_uint32 = addr_read32;
    b->addr_if.write_uint8 = addr_write8;
    b->addr_if.write_uint16 = addr_write16;
    b->addr_if.write_uint32 = addr_write32;
    b->data_if.read_uint8 = data_read8;
    b->data_if.read_uint16 = data_read16;
    b->data_if.read_uint32 = data_read32;
    b->data_if.write_uint8 = data_write8;
    b->data_if.write_uint16 = data_write16;
    b->data_if.write_uint32 = data_write32;
    char label[32];
    snprintf(label, sizeof(label), "%s cfg addr", name);
    memory_map_add(cfg->mem_map, base + TNT_PCI_CFG_ADDR, MEM_PAGE_SIZE, label, &b->addr_if, b);
    snprintf(label, sizeof(label), "%s cfg data", name);
    memory_map_add(cfg->mem_map, base + TNT_PCI_CFG_DATA, MEM_PAGE_SIZE, label, &b->data_if, b);

    b->bus = pci_bus_create(cfg->pci, name, bus_index);
    // The bridge's own header at device 11 (kPCIBridgeSelfDevice).  Chaos
    // gets none: through its restricted config space only $00-$0C are
    // readable, and today's model answers those from the CONTROL device
    // seated at the same IDSEL — the documented Chaos/Control conflation
    // (control.c, proposal §6.2 / §14 Q6).
    if (!is_chaos) {
        b->self_dev.ops = &bandit_self_ops;
        b->self_dev.decl = &bandit_self_decl;
        b->self_dev.priv = b;
        pci_cfg_reset(&b->self_dev);
        pci_bus_add_device(b->bus, &b->self_dev, 11);
    }
    return b;
}

void tnt_bandit_init(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    st->bridge_count = 0;

    bridge_add(cfg, TNT_CHAOS_BASE, true, TNT_PCI_BUS_VCI, "Chaos");
    tnt_bandit_t *bandit1 = bridge_add(cfg, TNT_BANDIT1_BASE, false, TNT_PCI_BUS_1, "Bandit 1");
    if (tnt_board(cfg)->bandit_count >= 2)
        bridge_add(cfg, TNT_BANDIT2_BASE, false, TNT_PCI_BUS_2, "Bandit 2");

    // Bandit 1's 256 MB PCI memory space and Chaos's 256 MB VCI memory
    // space: the bus claims both, dispatches an access to whichever seated
    // device's BAR decodes it, and takes the recoverable transfer error
    // the probe idioms expect when none does.
    //
    // Bandit 2 claims no memory window: which range it forwards is still
    // open (§14 Q5 — see bandit_addr_select), and its $48 says so too.
    // Its slots probe correctly regardless: config cycles are the
    // discovery path, and an unpopulated IDSEL reads all-ones.
    //
    // No I/O window is claimed either.  The generic layer supports one
    // (pci_bus_add_window with PCI_SPACE_IO over base+$000000..$7FFFFF),
    // but nothing on these machines decodes PCI I/O space yet — Grand
    // Central is reached by PASS-THROUGH MEMORY at $F3000000, not by I/O
    // cycles — and claiming the range would turn today's silent reads
    // into faults for no modelled device's benefit.  It lands with the
    // first card that declares an I/O BAR.
    pci_bus_add_window(bandit1->bus, PCI_SPACE_MEM, TNT_PCI_MEM1, 0x10000000u, TNT_PCI_MEM1, 0xFFFFFFFFu,
                       "PCI memory (Bandit 1)");
    pci_bus_add_window(pci_bus_by_index(cfg->pci, TNT_PCI_BUS_VCI), PCI_SPACE_MEM, TNT_PCI_MEM_VCI, 0x10000000u,
                       TNT_PCI_MEM_VCI, 0xFFFFFFFFu, "VCI memory (Chaos)");

    // Grand Central's config presence at device 16 (grand_central.c).
    tnt_gc_pci_attach(cfg, bandit1->bus);
}
