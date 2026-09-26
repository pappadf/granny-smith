// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_rom_overlay.c
// The access-triggered ROM-at-zero overlay, shared by the AV and MCU families.
// See mac030_rom_overlay.h for why one copy replaced two.

#include "mac030_rom_overlay.h"

#include "cpu.h"
#include "log.h"
#include "mac030_glue.h"
#include "system.h"
#include "system_config.h"

LOG_USE_CATEGORY_NAME("board");

// Base of the ROM image inside the RAM+ROM allocation.
static uint8_t *overlay_rom_data(const mac030_rom_overlay_t *ov) {
    return ram_native_pointer(ov->cfg->mem_map, ov->cfg->ram_size);
}

// Point the aperture at direct ROM pages, mirroring the image across it.
void mac030_rom_overlay_fill_aperture(const mac030_rom_overlay_t *ov) {
    uint32_t rom_pages = ov->cfg->machine->rom_size >> PAGE_SHIFT;
    uint8_t *rom_data = overlay_rom_data(ov);
    uint32_t start_page = ov->rom_base >> PAGE_SHIFT;
    uint32_t end_page = ov->rom_end >> PAGE_SHIFT;
    for (uint32_t p = start_page; p < end_page && p < g_page_count; p++)
        mac030_fill_page(p, rom_data + (((p - start_page) % rom_pages) << PAGE_SHIFT), false);
}

// RAM at zero, aperture direct.  Idempotent.
void mac030_rom_overlay_drop(mac030_rom_overlay_t *ov) {
    if (!ov->armed)
        return;
    ov->armed = false;
    LOG(1, "%s overlay drop: RAM at $00000000, ROM direct in aperture (pc=%08X)", ov->name, cpu_get_pc(ov->cfg->cpu));
    ov->map_ram(ov->cfg);
    mac030_rom_overlay_fill_aperture(ov);
}

// ROM readable at zero; aperture pages routed to the trigger device.
void mac030_rom_overlay_arm(mac030_rom_overlay_t *ov) {
    uint32_t rom_pages = ov->cfg->machine->rom_size >> PAGE_SHIFT;
    uint8_t *rom_data = overlay_rom_data(ov);

    ov->armed = true;

    // ROM mapped read-only at zero.
    for (uint32_t p = 0; p < rom_pages && p < g_page_count; p++)
        mac030_fill_page(p, rom_data + (p << PAGE_SHIFT), false);

    // Route the aperture through the trigger device: memory_map_add did the
    // page plumbing once, and later arms re-point the pages by hand.
    uint32_t start_page = ov->rom_base >> PAGE_SHIFT;
    uint32_t end_page = ov->rom_end >> PAGE_SHIFT;
    for (uint32_t p = start_page; p < end_page && p < g_page_count; p++) {
        g_page_table[p].host_base = NULL;
        g_page_table[p].dev = &ov->iface;
        g_page_table[p].dev_context = ov;
        g_page_table[p].base_addr = ov->rom_base;
        g_page_table[p].writable = false;
        if (g_supervisor_read)
            g_supervisor_read[p] = 0;
        if (g_supervisor_write)
            g_supervisor_write[p] = 0;
        if (g_user_read)
            g_user_read[p] = 0;
        if (g_user_write)
            g_user_write[p] = 0;
    }
}

// Trigger-device handlers: any access drops the overlay and completes from the
// ROM image.  `offset` is relative to the aperture base.
static inline uint8_t *overlay_rom_ptr(const mac030_rom_overlay_t *ov, uint32_t offset) {
    return overlay_rom_data(ov) + (offset % ov->cfg->machine->rom_size);
}

static uint8_t overlay_read8(void *ctx, uint32_t offset) {
    mac030_rom_overlay_t *ov = (mac030_rom_overlay_t *)ctx;
    mac030_rom_overlay_drop(ov);
    return overlay_rom_ptr(ov, offset)[0];
}

// Composed from byte reads so every byte wraps within the mirror
// independently.  Indexing p[1..3] off a single wrapped base instead would
// read past the end of the RAM+ROM allocation when the base landed on the last
// bytes of a mirror period -- the ROM sits at the top of that one calloc.
//
// That was not reachable: the memory dispatcher only calls a device's 16/32-bit
// handler when (addr & PAGE_MASK) <= MEM_PAGE_SIZE - 2/-4 and splits anything
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
    mac030_rom_overlay_drop((mac030_rom_overlay_t *)ctx); // a write also triggers the switch
    LOG(2, "ROM aperture write $%X ignored", offset);
}

static void overlay_write16(void *ctx, uint32_t offset, uint16_t value) {
    overlay_write8(ctx, offset, (uint8_t)value);
}

static void overlay_write32(void *ctx, uint32_t offset, uint32_t value) {
    overlay_write8(ctx, offset, (uint8_t)value);
}

// Fill in `ov` and its trigger interface.  Does not arm.
void mac030_rom_overlay_init(mac030_rom_overlay_t *ov, struct config *cfg, uint32_t rom_base, uint32_t rom_end,
                             void (*map_ram)(struct config *cfg), const char *name) {
    ov->cfg = cfg;
    ov->rom_base = rom_base;
    ov->rom_end = rom_end;
    ov->map_ram = map_ram;
    ov->name = name;
    ov->armed = false;
    ov->iface.read_uint8 = overlay_read8;
    ov->iface.read_uint16 = overlay_read16;
    ov->iface.read_uint32 = overlay_read32;
    ov->iface.write_uint8 = overlay_write8;
    ov->iface.write_uint16 = overlay_write16;
    ov->iface.write_uint32 = overlay_write32;
}
