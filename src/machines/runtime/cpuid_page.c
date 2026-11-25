// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpuid_page.c
// The CPU-ID register page at $5FFFF000 -- see cpuid_page.h.

#include "cpuid_page.h"

#include "log.h"
#include "regfile.h"

LOG_USE_CATEGORY_NAME("board");

// One byte of the page: the register's lane in its last longword, `fill`
// everywhere else.
static uint8_t cpuid_read8(void *ctx, uint32_t offset) {
    const cpuid_page_t *pg = ctx;
    if ((offset & 0xFFFu) >= CPUID_PAGE_REG)
        return be_lane8(pg->value, offset & 3u);
    return pg->fill;
}

// A word read: two byte lanes.
static uint16_t cpuid_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((cpuid_read8(ctx, offset) << 8) | cpuid_read8(ctx, offset + 1));
}

// A long read: two words -- or, long_low_half, the register's low half at the
// register and `fill` anywhere else (a long read never sees the signature,
// not even one straddling into the register).
static uint32_t cpuid_read32(void *ctx, uint32_t offset) {
    const cpuid_page_t *pg = ctx;
    if (pg->long_low_half)
        return (offset & 0xFFFu) >= CPUID_PAGE_REG ? pg->value & 0xFFFFu : pg->fill * 0x01010101u;
    return ((uint32_t)cpuid_read16(ctx, offset) << 16) | cpuid_read16(ctx, offset + 2);
}

// Writes are dropped: the register is read-only.
static void cpuid_write8(void *ctx, uint32_t offset, uint8_t value) {
    (void)ctx;
    LOG(2, "CPU-ID write $%03X = $%02X ignored (read-only)", offset, value);
}

// A word write, dropped.
static void cpuid_write16(void *ctx, uint32_t offset, uint16_t value) {
    cpuid_write8(ctx, offset, (uint8_t)value);
}

// A long write, dropped.
static void cpuid_write32(void *ctx, uint32_t offset, uint32_t value) {
    cpuid_write8(ctx, offset, (uint8_t)value);
}

void cpuid_page_install(cpuid_page_t *pg, memory_map_t *map, uint32_t value) {
    pg->value = value;
    pg->fill = 0xFF;
    pg->long_low_half = false;
    pg->iface = (memory_interface_t){
        .read_uint8 = cpuid_read8,
        .read_uint16 = cpuid_read16,
        .read_uint32 = cpuid_read32,
        .write_uint8 = cpuid_write8,
        .write_uint16 = cpuid_write16,
        .write_uint32 = cpuid_write32,
    };
    memory_map_add(map, CPUID_PAGE_BASE, 0x1000u, "CPU-ID", &pg->iface, pg);
}
