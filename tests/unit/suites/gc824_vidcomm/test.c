// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The 8*24 GC's VidComm mode change goes through display_set_scanout
// (N-52, #175).  The request is guest-written geometry in the card's DRAM;
// it used to set the descriptor's bits and stride directly, so a raster
// that decodes but does not fit the 2 MB DRAM had the renderer (and the
// card's own drawing engine) run past it.  The handler is static: the card
// source is included here to reach it.

#include "../../../../src/core/peripherals/nubus/cards/display_card_824gc.c"

#include "test_assert.h"

// --- Stubs for the plumbing the card links against --------------------------

const uint8_t *declrom_builder_bytes(const declrom_builder_t *b, size_t *out_size) {
    (void)b;
    *out_size = 0;
    return NULL;
}
void declrom_builder_free(declrom_builder_t *b) {
    (void)b;
}
bool declrom_install_builtin(const char *card_id, const uint8_t *chip, size_t chip_size, uint8_t *bus_buf,
                             size_t bus_size) {
    (void)card_id, (void)chip, (void)chip_size, (void)bus_buf, (void)bus_size;
    return false;
}
bool declrom_load_vrom_card(const char *card_id, uint8_t *bus_buf, size_t bus_size, char **out_path) {
    (void)card_id, (void)bus_buf, (void)bus_size, (void)out_path;
    return false;
}
struct declrom_builder *gsvrom_generate(gsvrom_personality_t p, const struct nubus_monitor *monitors) {
    (void)p, (void)monitors;
    return NULL;
}
void jmfb_apply_scanout(jmfb_regs_t *r, const jmfb_bind_t *b) {
    (void)r, (void)b;
}
uint16_t jmfb_read16(jmfb_regs_t *r, const jmfb_bind_t *b, int blk, uint32_t off) {
    (void)r, (void)b, (void)blk, (void)off;
    return 0;
}
void jmfb_write16(jmfb_regs_t *r, const jmfb_bind_t *b, int blk, uint32_t off, uint16_t val) {
    (void)r, (void)b, (void)blk, (void)off, (void)val;
}
void nubus_assert_irq(nubus_card_t *card) {
    (void)card;
}
void nubus_deassert_irq(nubus_card_t *card) {
    (void)card;
}
bool nubus_monitor_mode_lookup(const nubus_monitor_t *list, const char *id, const nubus_monitor_t **out_monitor,
                               int *out_depth_bpp) {
    (void)list, (void)id, (void)out_monitor, (void)out_depth_bpp;
    return false;
}
bool rtc_pram_write(rtc_t *rtc, uint8_t addr, uint8_t value) {
    (void)rtc, (void)addr, (void)value;
    return false;
}
rtc_t *system_rtc(void) {
    return NULL;
}
struct object *object_new(const class_desc_t *cls, void *instance_data, const char *name) {
    (void)cls, (void)instance_data, (void)name;
    return NULL;
}
void object_attach(struct object *parent, struct object *child) {
    (void)parent, (void)child;
}
void *object_data(struct object *o) {
    (void)o;
    return NULL;
}
void object_set_category(struct object *o, uint16_t category) {
    (void)o, (void)category;
}
void object_set_label(struct object *o, const char *label) {
    (void)o, (void)label;
}
void object_set_order(struct object *o, int order) {
    (void)o, (void)order;
}

uint8_t memory_debug_read_uint8(uint32_t addr) {
    (void)addr;
    return 0;
}
void memory_debug_read_block(uint32_t addr, uint8_t *dst, uint32_t len) {
    (void)addr;
    memset(dst, 0, len);
}
uint16_t memory_debug_read_uint16(uint32_t addr) {
    (void)addr;
    return 0;
}
uint32_t memory_debug_read_uint32(uint32_t addr) {
    (void)addr;
    return 0;
}
bool memory_debug_write_uint32(uint32_t addr, uint32_t value) {
    (void)addr, (void)value;
    return false;
}
void memory_map_host_region(memory_map_t *m, const char *name, uint8_t *host_ptr, uint32_t phys_base, uint32_t size,
                            bool writable) {
    (void)m, (void)name, (void)host_ptr, (void)phys_base, (void)size, (void)writable;
}
void memory_map_host_region_alias(memory_map_t *m, uint32_t alias_phys_base, uint32_t original_phys_base) {
    (void)m, (void)alias_phys_base, (void)original_phys_base;
}
void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                    void *device) {
    (void)mem, (void)addr, (void)size, (void)name, (void)iface, (void)device;
}

// --- A card with just what gc_vidcomm reads: DRAM, the blank, the monitor ----

static display_card_824gc_priv_t *make_card(void) {
    display_card_824gc_priv_t *p = calloc(1, sizeof(*p));
    p->dram = calloc(1, GC824_DRAM_SIZE);
    p->blank = calloc(1, GC824_DRAM_SIZE);
    p->mon_w = 640;
    p->jmfb.raster_h = 480;
    p->display.width = 640;
    p->display.height = 480;
    p->display.stride = 1024;
    p->display.bits = p->dram + GC824_FB_OFFSET;
    p->display.format = PIXEL_1BPP_MSB;
    return p;
}

static void free_card(display_card_824gc_priv_t *p) {
    free(p->dram);
    free(p->blank);
    free(p);
}

// Publish a mode change the way the video driver does, and run it.
static void vidcomm(display_card_824gc_priv_t *p, uint32_t rowbytes, uint32_t bpp) {
    uint32_t vc = GC824_DRAM_VIDCOMM;
    dram_set_be32(p, vc + GC824_VC_FBBASE, 0x90000000u | (GC824_DRAM_OFFSET + GC824_FB_OFFSET));
    dram_set_be32(p, vc + GC824_VC_ROWBYTES, rowbytes);
    dram_set_be32(p, vc + GC824_VC_BPP, bpp);
    dram_set_be32(p, vc + GC824_VC_SCANLINES, 545);
    dram_set_be32(p, vc + GC824_VC_GO, 0x80000000u);
    gc_vidcomm(p);
}

// Whatever the request, the advertised raster lies inside the buffer behind it.
static void assert_backed(const display_card_824gc_priv_t *p) {
    const uint8_t *lo, *hi;
    if (p->display.bits >= p->dram && p->display.bits < p->dram + GC824_DRAM_SIZE) {
        lo = p->dram;
        hi = p->dram + GC824_DRAM_SIZE;
    } else {
        lo = p->blank;
        hi = p->blank + GC824_DRAM_SIZE;
    }
    ASSERT_TRUE(p->display.bits >= lo);
    ASSERT_TRUE(p->display.bits + (size_t)p->display.stride * p->display.height <= hi);
    // ...and the renderer's walk over it stays there too.
    volatile uint8_t sum = 0;
    for (size_t i = 0; i < (size_t)p->display.stride * p->display.height; i++)
        sum ^= p->display.bits[i];
    (void)sum;
}

TEST(a_legal_direct_mode_is_scanned_from_dram) {
    display_card_824gc_priv_t *p = make_card();
    vidcomm(p, 4096, 24); // 640x480 at 32 bpp with a 4096-byte row: fits
    ASSERT_TRUE(p->display.bits == p->dram + GC824_FB_OFFSET);
    ASSERT_EQ_INT((int)p->display.stride, 4096);
    ASSERT_EQ_INT((int)p->display.height, 480);
    ASSERT_EQ_INT((int)p->display.width, 640);
    ASSERT_TRUE(p->display.format == PIXEL_32BPP_XRGB);
    assert_backed(p);
    free_card(p);
}

TEST(a_raster_past_the_dram_is_refused) {
    display_card_824gc_priv_t *p = make_card();
    vidcomm(p, 8192, 24); // decodes (rowBytes <= 8192), but 8192 x 480 > 2 MB
    ASSERT_TRUE(p->display.bits == p->blank);
    assert_backed(p);
    // A later legal request is honoured again, at the monitor's full height.
    vidcomm(p, 1024, 8);
    ASSERT_TRUE(p->display.bits == p->dram + GC824_FB_OFFSET);
    ASSERT_EQ_INT((int)p->display.height, 480);
    free_card(p);
}

TEST(an_undecodable_depth_is_refused) {
    display_card_824gc_priv_t *p = make_card();
    vidcomm(p, 1024, 8);
    ASSERT_TRUE(p->display.format == PIXEL_8BPP);
    vidcomm(p, 1024, 12); // not a depth: used to be taken as 8 bpp
    ASSERT_TRUE(p->display.format == PIXEL_8BPP);
    ASSERT_EQ_INT((int)p->display.stride, 1024);
    vidcomm(p, 1024, 3);
    ASSERT_TRUE(p->display.format == PIXEL_8BPP);
    ASSERT_EQ_INT(dram_be32(p, GC824_DRAM_VIDCOMM + GC824_VC_ACK) >> 24, 0); // still acked
    assert_backed(p);
    free_card(p);
}

int main(void) {
    RUN(a_legal_direct_mode_is_scanned_from_dram);
    RUN(a_raster_past_the_dram_is_refused);
    RUN(an_undecodable_depth_is_refused);
    return 0;
}
