// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// config_space.h
// The generic PCI type-0 configuration header (proposal-pci-architecture
// §5.2).  ONE implementation serves every device on every bus — the host
// bridges' own headers, Grand Central's config presence, Control, and
// every future slot card — so that "absent devices read all-ones" means
// "no device is registered at this IDSEL", never a hard-coded literal.
//
// The contract, attested by the shipping ROM's own probe and by every OS
// driver for these machines (bandit-chaos-pci.md):
//   * Reads are dword-assembled from the static declaration (IDs, class,
//     header type, interrupt pin) plus live state (command, status, the
//     BAR latches, the expansion-ROM BAR, interrupt line).  Registers the
//     device does not implement read ZERO — the device exists; only
//     absent DEVICES read all-ones.
//   * Writes arrive one byte at a time: the config DATA port carries the
//     low two offset bits on the port address, so a bridge adapter
//     decomposes every access into (reg, byte, value).
//   * A BAR latches the written value with the size mask applied on
//     READ-BACK, which is what makes the universal $FFFFFFFF sizing probe
//     work with no per-device code.

#ifndef PCI_CONFIG_SPACE_H
#define PCI_CONFIG_SPACE_H

#include <stdbool.h>
#include <stdint.h>

struct pci_device;

// Header-space register offsets this module implements.
#define PCI_CFG_ID          0x00u // device<<16 | vendor
#define PCI_CFG_COMMAND     0x04u // status<<16 | command
#define PCI_CFG_CLASS       0x08u // class<<8 | revision
#define PCI_CFG_MISC        0x0Cu // BIST | header type | latency | cache line
#define PCI_CFG_BAR0        0x10u
#define PCI_CFG_BAR5        0x24u
#define PCI_CFG_SUBSYSTEM   0x2Cu
#define PCI_CFG_ROM_BAR     0x30u
#define PCI_CFG_CAP_POINTER 0x34u
#define PCI_CFG_INTERRUPT   0x3Cu // max-lat | min-gnt | int pin | int line

// Command-register bits (PCI 2.0 §6.2.2); only the three the guests on
// these machines actually drive are named.
#define PCI_CMD_IO_SPACE  0x0001u
#define PCI_CMD_MEM_SPACE 0x0002u
#define PCI_CMD_MASTER    0x0004u

#define PCI_NUM_BARS       6
#define PCI_ROM_BAR_INDEX  6 // index of the expansion-ROM BAR in backing[]
#define PCI_BAR_SLOTS      7 // BARs 0..5 plus the expansion-ROM BAR
#define PCI_ROM_BAR_ENABLE 0x1u // expansion-ROM BAR bit 0

// What kind of space one BAR decodes.  The low bits of a BAR read back
// the encoding, which is how the guest tells memory from I/O.
typedef enum pci_bar_kind {
    PCI_BAR_NONE = 0, // BAR absent — reads zero, writes vanish
    PCI_BAR_MEM, // 32-bit non-prefetchable memory (low bits 0)
    PCI_BAR_MEM_PREFETCH, // 32-bit prefetchable memory (low bits $8)
    PCI_BAR_IO, // I/O space (bit 0 set)
} pci_bar_kind_t;

// One BAR's static geometry.  `size` is a power of two; 0 = absent.
typedef struct pci_bar_decl {
    uint32_t size;
    pci_bar_kind_t kind;
} pci_bar_decl_t;

// Everything the generic header needs to answer a probe.  One static
// instance per device model.
typedef struct pci_config_decl {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint32_t class_code; // 24-bit class / subclass / prog-if
    uint8_t header_type; // $00 for everything this proposal covers
    uint8_t interrupt_pin; // 0 = none, 1 = INTA (slots strap INTA-D together)
    uint16_t command_writable; // mask of command bits the device latches
    uint16_t command_reset; // command bits that are hardwired ON at power-on
                            // (Control: the Chaos bus ignores config writes
                            // outside its two BAR offsets, so the device
                            // always decodes — proposal §6.2)
    pci_bar_decl_t bar[PCI_NUM_BARS];
    uint32_t rom_size; // expansion-ROM BAR ($30); 0 = absent
} pci_config_decl_t;

// Live header state.  Checkpointed verbatim by the bus controller.
typedef struct pci_cfg_state {
    uint16_t command;
    uint16_t status;
    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t interrupt_line; // $3C — the GC line number the OS stores here
    uint32_t bar[PCI_NUM_BARS]; // raw latches (masked on read-back)
    uint32_t rom_bar;
} pci_cfg_state_t;

// Power-on state from the declaration (command_reset, zeroed latches).
void pci_cfg_reset(struct pci_device *dev);

// Full-dword read of header register `reg` (already masked to a dword
// boundary by the caller).  Device ops->cfg_read intercepts first.
uint32_t pci_cfg_read(struct pci_device *dev, uint32_t reg);

// Byte-lane write: byte `byte` (0..3) of header register `reg` takes
// `value`.  Device ops->cfg_write intercepts first.  Region-affecting
// changes are reported to the bus through pci_device_regions_changed().
void pci_cfg_write(struct pci_device *dev, uint32_t reg, uint32_t byte, uint8_t value);

// The decoded base of BAR `bar` (or PCI_ROM_BAR_INDEX), and whether the
// device currently decodes it (space-enable bit set, latch non-zero).
uint32_t pci_cfg_bar_base(const struct pci_device *dev, int bar);
bool pci_cfg_bar_enabled(const struct pci_device *dev, int bar);
uint32_t pci_cfg_bar_size(const struct pci_device *dev, int bar);

#endif // PCI_CONFIG_SPACE_H
