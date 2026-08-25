// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// bandit.c
// Bandit (343S1126), the AR-to-PCI host bridge, and Chaos (343S1155), the
// same silicon family adapted to the display (VCI) bus.  The 7500 has one
// Bandit; the 8500/9500 add a second at $F4000000; all three TNT machines
// carry Chaos at $F0000000.
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
// Control's register and VRAM BARs, which delegate to control.c: the
// ROM's own probe-slots sizes and assigns them); and writes to the rest
// of its config space are ignored ("/chaos really hates writes to config
// space" — NetBSD; Linux's chaos_map_bus applies the same offsets).  It
// has no PCI I/O window at all.
//
// Empty PCI MEMORY space faults recoverably: Open Firmware and the OS
// probe with catchable machine checks (the BART precedent), so the two
// 256 MB memory windows are claimed with a fault interface here.

#include "tnt.h"

#include "log.h"

#include <stdio.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("bandit");

// PCI ids (config dword 0 = device<<16 | vendor, little-endian layout)
#define PCI_VENDOR_APPLE 0x106Bu
#define BANDIT_DEVICE_ID 0x0001u
#define CHAOS_DEVICE_ID  0x0003u
#define BANDIT_REVISION  0x03u

// Config-space offsets of the bridge's documented mode registers
#define BANDIT_ADDR_SELECT 0x48u
#define BANDIT_MODE_SELECT 0x50u
#define BANDIT_COHERENT    0x40u // mode-select bit every OS latches on

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
        // Second bridge: its $F4/$F5 windows only (no memory-space window
        // is decoded by this model yet — nothing is behind its slots).
        return 0x0030u;
    case TNT_CHAOS_BASE:
    default:
        // Chaos: 256 MB VCI memory at $90000000; windows $F0/$F1.
        return (0x0200u << 16) | 0x0003u;
    }
}

// One config register of the bridge's own header, as a little-endian
// dword.  Unimplemented registers read zero (the header exists; only
// absent DEVICES read all-ones).
static uint32_t bridge_own_config_read(tnt_bandit_t *b, uint32_t reg) {
    switch (reg) {
    case 0x00:
        return ((uint32_t)(b->is_chaos ? CHAOS_DEVICE_ID : BANDIT_DEVICE_ID) << 16) | PCI_VENDOR_APPLE;
    case 0x08:
        return 0x06000000u | BANDIT_REVISION; // host-bridge class, rev 3
    case BANDIT_ADDR_SELECT:
        return bandit_addr_select(b);
    case BANDIT_MODE_SELECT:
        return b->mode_select;
    default:
        return 0;
    }
}

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

// Full-dword config read at the latched address (little-endian value).
static uint32_t config_read(tnt_bandit_t *b) {
    int dev;
    uint32_t fn, reg;
    if (!decode_type0(b, &dev, &fn, &reg))
        return 0xFFFFFFFFu;
    if (dev != 11 || fn != 0)
        return 0xFFFFFFFFu; // only the bridge itself answers (empty slots)
    if (b->is_chaos) {
        // Restricted readability: $00-$0F, $14 and $18 only — most Chaos
        // BARs are not safely readable and reads of them return all-ones.
        // The two readable BARs are Control's (control.c).
        if (reg == 0x14u || reg == 0x18u)
            return tnt_control_cfg_read(b->cfg, reg);
        if (reg > 0x0Cu)
            return 0xFFFFFFFFu;
    }
    return bridge_own_config_read(b, reg);
}

static void config_write(tnt_bandit_t *b, uint32_t reg_base, uint32_t byte, uint32_t value, uint32_t mask) {
    int dev;
    uint32_t fn, reg;
    (void)reg_base;
    if (!decode_type0(b, &dev, &fn, &reg))
        return;
    if (dev != 11 || fn != 0)
        return; // writes to empty devices vanish
    if (b->is_chaos) {
        // Control's two BARs latch (the ROM's probe-slots sizes and
        // assigns them); the rest of Chaos config space ignores writes.
        if (reg == 0x14u || reg == 0x18u) {
            tnt_control_cfg_write(b->cfg, reg, byte, (uint8_t)(value & mask));
            return;
        }
        LOG(2, "chaos config write $%02X ignored", reg);
        return;
    }
    if (reg == BANDIT_MODE_SELECT) {
        // The documented latch: whatever the OS writes reads back (the
        // coherency handshake is "read, OR in $40, write, read back").
        uint32_t shifted = (value & mask) << (8 * byte);
        uint32_t smask = mask << (8 * byte);
        b->mode_select = (b->mode_select & ~smask) | shifted;
        LOG(2, "mode-select now $%08X (coherency %s)", b->mode_select,
            (b->mode_select & BANDIT_COHERENT) ? "on" : "off");
    } else {
        LOG(2, "config write dev 11 reg $%02X byte %u = $%02X ignored", reg, byte, value & mask);
    }
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
    config_write(b, 0, offset & 3u, value, 0xFFu);
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
// Empty PCI memory space — the recoverable fault windows
// ============================================================

static void pci_fault(void *ctx, uint32_t offset, bool write) {
    tnt_fault_window_t *w = (tnt_fault_window_t *)ctx;
    LOG(4, "%s: unclaimed %s $%08X", w->what, write ? "write" : "read", w->base + offset);
    memory_signal_bus_error(w->base + offset, write);
}

static uint8_t fault_read8(void *ctx, uint32_t offset) {
    pci_fault(ctx, offset, false);
    return 0xFF;
}

static uint16_t fault_read16(void *ctx, uint32_t offset) {
    pci_fault(ctx, offset, false);
    return 0xFFFF;
}

static uint32_t fault_read32(void *ctx, uint32_t offset) {
    pci_fault(ctx, offset, false);
    return 0xFFFFFFFFu;
}

static void fault_write8(void *ctx, uint32_t offset, uint8_t value) {
    (void)value;
    pci_fault(ctx, offset, true);
}

static void fault_write16(void *ctx, uint32_t offset, uint16_t value) {
    (void)value;
    pci_fault(ctx, offset, true);
}

static void fault_write32(void *ctx, uint32_t offset, uint32_t value) {
    (void)value;
    pci_fault(ctx, offset, true);
}

static memory_interface_t pci_fault_iface = {
    .read_uint8 = fault_read8,
    .read_uint16 = fault_read16,
    .read_uint32 = fault_read32,
    .write_uint8 = fault_write8,
    .write_uint16 = fault_write16,
    .write_uint32 = fault_write32,
};

// ============================================================
// Wiring
// ============================================================

static void bridge_add(config_t *cfg, uint32_t base, bool is_chaos, const char *name) {
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
}

void tnt_bandit_init(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    st->bridge_count = 0;

    bridge_add(cfg, TNT_CHAOS_BASE, true, "Chaos");
    bridge_add(cfg, TNT_BANDIT1_BASE, false, "Bandit 1");
    if (tnt_board(cfg)->bandit_count >= 2)
        bridge_add(cfg, TNT_BANDIT2_BASE, false, "Bandit 2");

    // Bandit 1's 256 MB PCI memory space, claimed empty: with no card
    // seated an access there takes the recoverable transfer error the
    // probe idioms expect.  A future PCI card's regions overlay these
    // pages.  The VCI memory space is control.c's — its BAR dispatcher
    // provides the same fault semantics for unclaimed addresses.
    st->fault[0].base = TNT_PCI_MEM1;
    snprintf(st->fault[0].what, sizeof(st->fault[0].what), "PCI memory (Bandit 1)");
    memory_map_add(cfg->mem_map, TNT_PCI_MEM1, 0x10000000u, st->fault[0].what, &pci_fault_iface, &st->fault[0]);
}
