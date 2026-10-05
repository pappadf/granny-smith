// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dma_mem_physical.c — the host-physical dma_mem_port_t.
//
// Split from dma_mem.c so a unit suite can link the generic port plumbing
// (which depends on nothing) without dragging in the MMU: the suites for
// SONIC, the PSC engine and DBDMA all install array-backed ports of their
// own and have no g_mmu to resolve against.

#include "dma_mem.h"

#include "mmu.h"

// === The host-physical port =================================================

static uint32_t phys_read(void *ctx, uint32_t phys, unsigned width) {
    (void)ctx;
    if (width == 1)
        return mmu_read_physical_uint8(g_mmu, phys);
    if (width == 2)
        return mmu_read_physical_uint16(g_mmu, phys);
    return mmu_read_physical_uint32(g_mmu, phys);
}

static void phys_write(void *ctx, uint32_t phys, uint32_t value, unsigned width) {
    (void)ctx;
    if (width == 1)
        mmu_write_physical_uint8(g_mmu, phys, (uint8_t)value);
    else if (width == 2)
        mmu_write_physical_uint16(g_mmu, phys, (uint16_t)value);
    else
        mmu_write_physical_uint32(g_mmu, phys, value);
}

const dma_mem_port_t dma_mem_port_physical = {
    .read = phys_read,
    .write = phys_write,
};
