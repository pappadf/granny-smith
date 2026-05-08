// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iicx_internal.h
// Shared internals for the IIcx / IIx pair.  Both machines run the same
// GLUE-driven I/O map, share dual-VIA / 68030 / Universal-ROM peripherals,
// and only diverge in a handful of decisions: slot table, machine-ID bits,
// soft-power policy, sound-jack policy.  iix.c reuses iicx.c's internals
// via this header so the family-shared code lives in one place — a
// pragmatic stand-in for the proper §3.1 glue030 extraction that lands
// when a third caller arrives.

#ifndef IICX_INTERNAL_H
#define IICX_INTERNAL_H

#include "common.h"
#include "memory.h"
#include "mmu.h"
#include "system_config.h" // for config_t.machine_context
#include <stdbool.h>
#include <stdint.h>

struct adb;
struct asc;
struct floppy;

// Per-machine state.  Layout is shared between IIcx and IIx — the IIx's
// missing PB2 / PB6 quirks just leave their state fields unused.
typedef struct iicx_state {
    struct adb *adb;
    struct asc *asc;
    struct floppy *floppy;

    bool rom_overlay;
    mmu_state_t *mmu;

    const memory_interface_t *via1_iface;
    const memory_interface_t *via2_iface;
    const memory_interface_t *scc_iface;
    const memory_interface_t *scsi_iface;
    const memory_interface_t *asc_iface;
    const memory_interface_t *floppy_iface;

    uint8_t last_port_b; // ADB ST filtering on VIA1 PB
    uint8_t last_via2_port_b; // last seen VIA2 port-B output

    // IIcx soft-power detection (unused on IIx).  The Universal ROM
    // touches VIA2 port B many times during early init with PB2 = 0
    // simply because nothing in the OS has written it yet, so we can't
    // fire on the first 1→0 transition.  The detector arms on the first
    // observed PB2 = 1 (the OS asserting "stay powered on") and only
    // then watches for the falling edge that means "shut down".
    bool soft_power_armed;

    memory_interface_t io_interface;
} iicx_state_t;

static inline iicx_state_t *iicx_state(config_t *cfg) {
    return (iicx_state_t *)cfg->machine_context;
}

// Family-shared helpers — defined in iicx.c, called from iix.c.

void iicx_fill_page(uint32_t page_index, uint8_t *host_ptr, bool writable);
void iicx_set_rom_overlay(struct config *cfg, bool overlay);
void iicx_update_ipl(struct config *cfg, int source, bool active);

// I/O dispatcher (same address map for IIcx and IIx).
uint8_t iicx_io_read_uint8(void *ctx, uint32_t addr);
uint16_t iicx_io_read_uint16(void *ctx, uint32_t addr);
uint32_t iicx_io_read_uint32(void *ctx, uint32_t addr);
void iicx_io_write_uint8(void *ctx, uint32_t addr, uint8_t value);
void iicx_io_write_uint16(void *ctx, uint32_t addr, uint16_t value);
void iicx_io_write_uint32(void *ctx, uint32_t addr, uint32_t value);

// VIA1 callbacks — identical between IIcx and IIx (no PA6 buffer-select).
void iicx_via1_output(void *context, uint8_t port, uint8_t output);
void iicx_via1_shift_out(void *context, uint8_t byte);
void iicx_via1_irq(void *context, bool active);

// SCC IRQ (identical).
void iicx_scc_irq(void *context, bool active);

// Memory layout (RAM/ROM aliasing + I/O dispatcher registration).
void iicx_memory_layout_init(struct config *cfg);

// IRQ source bit assignments.
#define IICX_IRQ_VIA1 (1 << 0)
#define IICX_IRQ_VIA2 (1 << 1)
#define IICX_IRQ_SCC  (1 << 2)
#define IICX_IRQ_NMI  (1 << 3)

// I/O bus penalty equates (used only by the dispatcher).
#define IICX_VIA_IO_PENALTY  16
#define IICX_SCC_IO_PENALTY  2
#define IICX_SCSI_IO_PENALTY 2
#define IICX_ASC_IO_PENALTY  2
#define IICX_SWIM_IO_PENALTY 2

// Address-space constants.
#define IICX_ROM_START 0x40000000UL
#define IICX_ROM_END   0x50000000UL
#define IICX_IO_BASE   0x50000000UL
#define IICX_IO_SIZE   0x10000000UL

#endif // IICX_INTERNAL_H
