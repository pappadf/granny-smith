// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// floppy_iwm.c
// Mac Plus IWM memory-mapped I/O interface (address decoding).

#include "floppy_internal.h"
#include "log.h"

#include <assert.h>

LOG_USE_CATEGORY_NAME("floppy");

// Memory interface handler for 8-bit reads from IWM address space
static uint8_t iwm_read_uint8(void *floppy, uint32_t addr) {
    floppy_t *s = (floppy_t *)floppy;

    // [3]: the IWM sits on the lower byte of the data bus, so only odd-addressed
    // byte accesses reach it.  That is a property of how THIS board wired /LDS,
    // which is why it is checked here and not in the chip -- and it is logged
    // rather than asserted, because a guest must not be able to pause the
    // emulator by executing a wrong instruction (02-floppy F-32).
    if (!(addr & 1))
        LOG(1, "IWM: even-address byte read at 0x%08X; the chip is on the low byte", addr);

    // [5]: A1-A4 of the IWM are connected to A9-A12 of the CPU bus
    return floppy_iwm_read(s, (addr >> 9) & 0x0F);
}

// The chip is on one byte of the data bus, so a wide access reaches nothing.
// These used to GS_ASSERT(0) -- which prints and PAUSES THE SCHEDULER rather
// than aborting, so any guest executing `move.w $D80000,d0`, buggy or hostile,
// halted the emulator and surfaced in CI as an unexplained hang (02-floppy
// F-32).  Log it and return open bus, as grand_central.c does.
static uint16_t iwm_read_uint16(void *floppy, uint32_t addr) {
    (void)floppy;
    LOG(1, "%s: 16-bit access at 0x%08X is not decoded; reading open bus", "''' + name + r'''", addr);
    return 0xFFFF;
}

static uint32_t iwm_read_uint32(void *floppy, uint32_t addr) {
    (void)floppy;
    LOG(1, "%s: 32-bit access at 0x%08X is not decoded; reading open bus", "''' + name + r'''", addr);
    return 0xFFFFFFFFu;
}

// Memory interface handler for 8-bit writes to IWM address space
static void iwm_write_uint8(void *floppy, uint32_t addr, uint8_t value) {
    floppy_t *s = (floppy_t *)floppy;

    if (!(addr & 1))
        LOG(1, "IWM: even-address byte write at 0x%08X; the chip is on the low byte", addr);

    // [5]: A1-A4 of the IWM are connected to A9-A12 of the CPU bus
    floppy_iwm_write(s, (addr >> 9) & 0x0F, value);
}

static void iwm_write_uint16(void *floppy, uint32_t addr, uint16_t value) {
    (void)floppy;
    (void)value;
    LOG(1, "%s: 16-bit write at 0x%08X is not decoded; dropped", "''' + name + r'''", addr);
}

static void iwm_write_uint32(void *floppy, uint32_t addr, uint32_t value) {
    (void)floppy;
    (void)value;
    LOG(1, "%s: 32-bit write at 0x%08X is not decoded; dropped", "''' + name + r'''", addr);
}

// Sets up the IWM memory interface callbacks on the floppy controller
void floppy_iwm_setup(floppy_t *floppy, memory_map_t *map) {
    floppy->memory_interface.read_uint8 = &iwm_read_uint8;
    floppy->memory_interface.read_uint16 = &iwm_read_uint16;
    floppy->memory_interface.read_uint32 = &iwm_read_uint32;
    floppy->memory_interface.write_uint8 = &iwm_write_uint8;
    floppy->memory_interface.write_uint16 = &iwm_write_uint16;
    floppy->memory_interface.write_uint32 = &iwm_write_uint32;

    memory_map_add(map, 0x00d80000, 0x00080000, "floppy", &floppy->memory_interface, floppy);
}
