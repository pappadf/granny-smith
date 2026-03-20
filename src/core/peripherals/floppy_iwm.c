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

    // [3]: The IWM is on the lower byte of the data bus, so use odd-addressed byte accesses only
    GS_ASSERT(addr & 1);

    // [5]: A1-A4 of the IWM are connected to A9-A12 of the CPU bus
    addr >>= 9;

    return floppy_iwm_read(s, addr & 0x0F);
}

// Memory interface handler for 16-bit reads (not supported)
static uint16_t iwm_read_uint16(void *floppy, uint32_t addr) {
    (void)floppy;
    (void)addr;
    GS_ASSERT(0);
    return 0;
}

// Memory interface handler for 32-bit reads (not supported)
static uint32_t iwm_read_uint32(void *floppy, uint32_t addr) {
    (void)floppy;
    (void)addr;
    GS_ASSERT(0);
    return 0;
}

// Memory interface handler for 8-bit writes to IWM address space
static void iwm_write_uint8(void *floppy, uint32_t addr, uint8_t value) {
    floppy_t *s = (floppy_t *)floppy;

    // [3]: The IWM is on the lower byte of the data bus, so use odd-addressed byte accesses only
    GS_ASSERT(addr & 1);

    // [5]: A1-A4 of the IWM are connected to A9-A12 of the CPU bus
    addr >>= 9;

    floppy_iwm_write(s, addr & 0x0F, value);
}

// Memory interface handler for 16-bit writes (not supported)
static void iwm_write_uint16(void *floppy, uint32_t addr, uint16_t value) {
    (void)floppy;
    (void)addr;
    (void)value;
    GS_ASSERT(0);
}

// Memory interface handler for 32-bit writes (not supported)
static void iwm_write_uint32(void *floppy, uint32_t addr, uint32_t value) {
    (void)floppy;
    (void)addr;
    (void)value;
    GS_ASSERT(0);
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
