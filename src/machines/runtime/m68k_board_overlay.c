// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// m68k_board_overlay.c
// The ROM-at-zero overlay of the 68030/040 boards, with its two policies.
// See m68k_board_overlay.h.

#include "m68k_board_overlay.h"

#include "cpu.h"
#include "log.h"
#include "page_fill.h"
#include "system.h"
#include "system_internal.h"

LOG_USE_CATEGORY_NAME("board");

// Base of the ROM image inside the RAM+ROM allocation.
static uint8_t *overlay_rom_data(const m68k_board_overlay_t *ov) {
    return ram_native_pointer(ov->cfg->memory_map, ov->cfg->ram_size);
}

// Point the aperture at direct ROM pages, mirroring the image across it.
void m68k_board_overlay_fill_aperture(const m68k_board_overlay_t *ov) {
    uint32_t rom_pages = ov->cfg->machine->rom_size >> MEM_PAGE_SHIFT;
    uint8_t *rom_data = overlay_rom_data(ov);
    uint32_t start_page = ov->rom_base >> MEM_PAGE_SHIFT;
    uint32_t end_page = ov->rom_end >> MEM_PAGE_SHIFT;
    for (uint32_t p = start_page; p < end_page && p < g_page_count; p++)
        page_fill_68k(p, rom_data + (((p - start_page) % rom_pages) << MEM_PAGE_SHIFT), false);
}

// RAM at zero, aperture direct.  Idempotent.
void m68k_board_overlay_drop(m68k_board_overlay_t *ov) {
    if (!ov->armed)
        return;
    ov->armed = false;
    if (ov->policy == M68K_BOARD_OVERLAY_FOLLOW_LINE) {
        LOG(2, "%s overlay off: RAM at $00000000 (pc=%08X)", ov->name, cpu_get_pc(ov->cfg->cpu));
        ov->map_ram(ov->cfg);
        return;
    }
    LOG(1, "%s overlay drop: RAM at $00000000, ROM direct in aperture (pc=%08X)", ov->name, cpu_get_pc(ov->cfg->cpu));
    ov->map_ram(ov->cfg);
    m68k_board_overlay_fill_aperture(ov);
}

void m68k_board_overlay_set_line(m68k_board_overlay_t *ov, bool on) {
    if (ov->armed == on)
        return;
    if (on)
        m68k_board_overlay_arm(ov);
    else
        m68k_board_overlay_drop(ov);
}

// ROM readable at zero; aperture pages routed to the trigger device.
void m68k_board_overlay_arm(m68k_board_overlay_t *ov) {
    uint32_t rom_pages = ov->cfg->machine->rom_size >> MEM_PAGE_SHIFT;
    uint8_t *rom_data = overlay_rom_data(ov);

    ov->armed = true;

    // ROM mapped read-only at zero.
    for (uint32_t p = 0; p < rom_pages && p < g_page_count; p++)
        page_fill_68k(p, rom_data + (p << MEM_PAGE_SHIFT), false);

    // A line-following overlay has no trigger: the ROM window stays direct
    if (ov->policy == M68K_BOARD_OVERLAY_FOLLOW_LINE)
        return;

    // Route the aperture through the trigger device: memory_map_add did the
    // page plumbing once, and later arms re-point the pages by hand.
    uint32_t start_page = ov->rom_base >> MEM_PAGE_SHIFT;
    uint32_t end_page = ov->rom_end >> MEM_PAGE_SHIFT;
    for (uint32_t p = start_page; p < end_page && p < g_page_count; p++)
        page_route_device(p, &ov->iface, ov, ov->rom_base);
}

// Trigger-device handlers: any access drops the overlay and completes from the
// ROM image.  `offset` is relative to the aperture base.
static inline uint8_t *overlay_rom_ptr(const m68k_board_overlay_t *ov, uint32_t offset) {
    return overlay_rom_data(ov) + (offset % ov->cfg->machine->rom_size);
}

static uint8_t overlay_read8(void *ctx, uint32_t offset) {
    m68k_board_overlay_t *ov = (m68k_board_overlay_t *)ctx;
    m68k_board_overlay_drop(ov);
    return overlay_rom_ptr(ov, offset)[0];
}

// An inspection reads the ROM byte without throwing the switch.
static uint8_t overlay_peek8(void *ctx, uint32_t offset) {
    return overlay_rom_ptr((m68k_board_overlay_t *)ctx, offset)[0]; // wider peeks compose
}

// Composed from byte reads so every byte wraps within the mirror
// independently.  Indexing p[1..3] off a single wrapped base instead would
// read past the end of the RAM+ROM allocation when the base landed on the last
// bytes of a mirror period -- the ROM sits at the top of that one calloc.
//
// That was not reachable: the memory dispatcher only calls a device's 16/32-bit
// handler when (addr & MEM_PAGE_MASK) <= MEM_PAGE_SIZE - 2/-4 and splits anything
// closer to a page end, and rom_size is a multiple of the 4 KiB page size, so
// "within 3 bytes of a mirror top" is always also "within 3 bytes of a page
// end".  Confirmed under ASAN: a long read at the top of a q700 mirror reaches
// this handler only after being decomposed.  But that safety lives in another
// file and depends on an unstated size relationship, so it is made local here.
// (the drop is idempotent, so the repeated call costs nothing.)
static uint16_t overlay_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((overlay_read8(ctx, offset) << 8) | overlay_read8(ctx, offset + 1));
}

static uint32_t overlay_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)overlay_read16(ctx, offset) << 16) | overlay_read16(ctx, offset + 2);
}

static void overlay_write8(void *ctx, uint32_t offset, uint8_t value) {
    (void)value;
    m68k_board_overlay_drop((m68k_board_overlay_t *)ctx); // a write also triggers the switch
    LOG(2, "ROM aperture write $%X ignored", offset);
}

static void overlay_write16(void *ctx, uint32_t offset, uint16_t value) {
    overlay_write8(ctx, offset, (uint8_t)value);
}

static void overlay_write32(void *ctx, uint32_t offset, uint32_t value) {
    overlay_write8(ctx, offset, (uint8_t)value);
}

// Fill in `ov` and its trigger interface.  Does not arm.
void m68k_board_overlay_init(m68k_board_overlay_t *ov, struct config *cfg, uint32_t rom_base, uint32_t rom_end,
                             void (*map_ram)(struct config *cfg), const char *name) {
    ov->cfg = cfg;
    ov->rom_base = rom_base;
    ov->rom_end = rom_end;
    ov->map_ram = map_ram;
    ov->name = name;
    ov->policy = M68K_BOARD_OVERLAY_DROP_ON_ACCESS;
    ov->armed = false;
    ov->iface.read_uint8 = overlay_read8;
    ov->iface.read_uint16 = overlay_read16;
    ov->iface.read_uint32 = overlay_read32;
    ov->iface.peek_uint8 = overlay_peek8;
    ov->iface.peek_uint16 = NULL;
    ov->iface.peek_uint32 = NULL;
    ov->iface.write_uint8 = overlay_write8;
    ov->iface.write_uint16 = overlay_write16;
    ov->iface.write_uint32 = overlay_write32;
}

// RAM back at zero under a line-following overlay: the identity map of the
// pages the overlay covered (ROM size from zero).
static void overlay_map_ram_identity(struct config *cfg) {
    uint32_t rom_pages = cfg->machine->rom_size >> MEM_PAGE_SHIFT;
    uint8_t *ram_base = ram_native_pointer(cfg->memory_map, 0);
    for (uint32_t p = 0; p < rom_pages && p < g_page_count; p++)
        page_fill_68k(p, ram_base + (p << MEM_PAGE_SHIFT), true);
}

void m68k_board_overlay_init_line(m68k_board_overlay_t *ov, struct config *cfg, const char *name) {
    // No aperture: rom_base/rom_end stay 0 and the trigger interface unused
    m68k_board_overlay_init(ov, cfg, 0, 0, overlay_map_ram_identity, name);
    ov->policy = M68K_BOARD_OVERLAY_FOLLOW_LINE;
}
