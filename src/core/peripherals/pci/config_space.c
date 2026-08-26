// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// config_space.c
// The generic PCI type-0 configuration header.  See config_space.h for
// the contract and proposal-pci-architecture.md §5.2 for the design.
//
// This file knows nothing about any particular device: it assembles reads
// out of the static declaration plus the live latches, applies the size
// mask that makes the universal $FFFFFFFF BAR-sizing probe work, and
// tells the bus whenever a write moved or gated a decoded region.  Device
// quirks (Bandit's $48/$50, Grand Central's all-ones presence) layer on
// top through ops->cfg_read / ops->cfg_write.

#include "config_space.h"

#include "card.h"
#include "pci.h"

#include <string.h>

// The low bits a BAR reads back, encoding what kind of space it decodes
// (PCI 2.0 §6.2.5.1).  The size mask covers them for every BAR we model
// (the smallest is 4 KB), so they only matter on the read path.
static uint32_t bar_type_bits(pci_bar_kind_t kind) {
    switch (kind) {
    case PCI_BAR_IO:
        return 0x1u;
    case PCI_BAR_MEM_PREFETCH:
        return 0x8u;
    case PCI_BAR_MEM:
    case PCI_BAR_NONE:
    default:
        return 0x0u;
    }
}

// The declaration of BAR `bar`, or NULL when the index names the
// expansion-ROM BAR or a BAR the device does not implement.
static const pci_bar_decl_t *bar_decl(const pci_device_t *dev, int bar) {
    if (!dev || !dev->decl || bar < 0 || bar >= PCI_NUM_BARS)
        return NULL;
    const pci_bar_decl_t *b = &dev->decl->bar[bar];
    return b->size ? b : NULL;
}

uint32_t pci_cfg_bar_size(const pci_device_t *dev, int bar) {
    if (bar == PCI_ROM_BAR_INDEX)
        return (dev && dev->decl) ? dev->decl->rom_size : 0;
    const pci_bar_decl_t *b = bar_decl(dev, bar);
    return b ? b->size : 0;
}

uint32_t pci_cfg_bar_base(const pci_device_t *dev, int bar) {
    uint32_t size = pci_cfg_bar_size(dev, bar);
    if (!size)
        return 0;
    uint32_t latch = (bar == PCI_ROM_BAR_INDEX) ? dev->cfg.rom_bar : dev->cfg.bar[bar];
    return latch & ~(size - 1u);
}

bool pci_cfg_bar_enabled(const pci_device_t *dev, int bar) {
    uint32_t size = pci_cfg_bar_size(dev, bar);
    if (!size)
        return false;
    // The expansion ROM has its own enable bit on top of the memory-space
    // gate; an unassigned (zero) base decodes nothing either way.
    if (bar == PCI_ROM_BAR_INDEX) {
        if (!(dev->cfg.rom_bar & PCI_ROM_BAR_ENABLE))
            return false;
        if (!(dev->cfg.command & PCI_CMD_MEM_SPACE))
            return false;
    } else {
        const pci_bar_decl_t *b = bar_decl(dev, bar);
        uint16_t gate = (b->kind == PCI_BAR_IO) ? PCI_CMD_IO_SPACE : PCI_CMD_MEM_SPACE;
        if (!(dev->cfg.command & gate))
            return false;
    }
    return pci_cfg_bar_base(dev, bar) != 0;
}

void pci_cfg_reset(pci_device_t *dev) {
    if (!dev)
        return;
    memset(&dev->cfg, 0, sizeof(dev->cfg));
    if (dev->decl) {
        dev->cfg.command = dev->decl->command_reset;
        dev->cfg.status = dev->decl->status_reset;
    }
}

uint32_t pci_cfg_read(pci_device_t *dev, uint32_t reg) {
    // Device quirks answer first (Bandit's $48/$50; Grand Central's
    // all-ones presence) — returning true claims the register entirely.
    uint32_t out = 0;
    if (dev->ops && dev->ops->cfg_read && dev->ops->cfg_read(dev, reg, &out))
        return out;
    const pci_config_decl_t *d = dev->decl;
    if (!d)
        return 0;
    if (reg >= PCI_CFG_BAR0 && reg <= PCI_CFG_BAR5) {
        int bar = (int)((reg - PCI_CFG_BAR0) >> 2);
        const pci_bar_decl_t *b = bar_decl(dev, bar);
        if (!b)
            return 0; // unimplemented BAR: reads zero, sizing sees "absent"
        return (dev->cfg.bar[bar] & ~(b->size - 1u)) | bar_type_bits(b->kind);
    }
    switch (reg) {
    case PCI_CFG_ID:
        return ((uint32_t)d->device_id << 16) | d->vendor_id;
    case PCI_CFG_COMMAND:
        return ((uint32_t)dev->cfg.status << 16) | dev->cfg.command;
    case PCI_CFG_CLASS:
        return ((d->class_code & 0x00FFFFFFu) << 8) | d->revision;
    case PCI_CFG_MISC:
        return ((uint32_t)d->header_type << 16) | ((uint32_t)dev->cfg.latency_timer << 8) | dev->cfg.cache_line_size;
    case PCI_CFG_ROM_BAR:
        if (!d->rom_size)
            return 0;
        return (dev->cfg.rom_bar & ~(d->rom_size - 1u)) | (dev->cfg.rom_bar & PCI_ROM_BAR_ENABLE);
    case PCI_CFG_INTERRUPT:
        return ((uint32_t)d->interrupt_pin << 8) | dev->cfg.interrupt_line;
    default:
        // The device exists but does not implement this register: zero.
        return 0;
    }
}

void pci_cfg_write(pci_device_t *dev, uint32_t reg, uint32_t byte, uint8_t value) {
    if (dev->ops && dev->ops->cfg_write && dev->ops->cfg_write(dev, reg, byte, value))
        return;
    const pci_config_decl_t *d = dev->decl;
    if (!d)
        return;
    uint32_t shift = 8u * (byte & 3u);
    uint32_t mask = 0xFFu << shift;

    if (reg >= PCI_CFG_BAR0 && reg <= PCI_CFG_BAR5) {
        int bar = (int)((reg - PCI_CFG_BAR0) >> 2);
        if (!bar_decl(dev, bar))
            return; // unimplemented BAR: writes vanish
        uint32_t was = dev->cfg.bar[bar];
        dev->cfg.bar[bar] = (was & ~mask) | ((uint32_t)value << shift);
        if (dev->cfg.bar[bar] != was)
            pci_device_regions_changed(dev);
        return;
    }
    switch (reg) {
    case PCI_CFG_COMMAND: {
        if (byte >= 2)
            return; // the status halfword is write-1-to-clear; nothing is set
        uint16_t was = dev->cfg.command;
        uint16_t incoming = (uint16_t)(((uint32_t)dev->cfg.command & ~mask) | ((uint32_t)value << shift));
        dev->cfg.command =
            (uint16_t)((dev->cfg.command & ~d->command_writable) | (incoming & d->command_writable) | d->command_reset);
        if (dev->cfg.command != was)
            pci_device_regions_changed(dev); // the space gates moved
        return;
    }
    case PCI_CFG_MISC:
        // Cache line size and latency timer are plain read/write bytes;
        // header type and BIST are read-only.
        if (byte == 0)
            dev->cfg.cache_line_size = value;
        else if (byte == 1)
            dev->cfg.latency_timer = value;
        return;
    case PCI_CFG_ROM_BAR: {
        if (!d->rom_size)
            return;
        uint32_t was = dev->cfg.rom_bar;
        dev->cfg.rom_bar = (was & ~mask) | ((uint32_t)value << shift);
        if (dev->cfg.rom_bar != was)
            pci_device_regions_changed(dev);
        return;
    }
    case PCI_CFG_INTERRUPT:
        // $3C is the line number the OS copies out of AAPL,interrupts;
        // the pin at $3D is strapped and read-only.
        if (byte == 0)
            dev->cfg.interrupt_line = value;
        return;
    default:
        return; // unimplemented register: writes vanish
    }
}
