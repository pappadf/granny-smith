// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The Apple Network Server's on-board Cirrus Logic 54M30
// (src/core/peripherals/pci/cards/cirrus54m30.c).
//
// The card has no mode register: c54m30_update() derives the scanout from the
// VGA CRTC, the sequencer and the Cirrus extension registers every time a
// guest writes one.  These rows program those registers through the card's
// own I/O windows, exactly as a guest's port writes arrive, and read the
// result back through the card's display op -- the descriptor the renderer
// and `machine.screen` see.  Config space is the real config_space.c, so the
// BAR, expansion-ROM and interrupt rows read what Open Firmware's config
// cycles read.
//
// Expected values come from two places, cited per row: the card's own
// derivation (cirrus54m30.c, "Deriving the mode") and the VGA / Alpine
// register semantics it implements (Cirrus Logic, "Alpine VGA Family
// CL-GD543X/4X Technical Reference Manual", 4th ed., sections 4.14-4.20).

#include "checkpoint.h"
#include "config_space.h"
#include "display.h"
#include "display_class.h"
#include "pci.h"
#include "pci_card.h"
#include "system_config.h"
#include "test_assert.h"

#include <stdlib.h>
#include <string.h>

extern const pci_card_kind_t cirrus_54m30_kind;

// --- Stubs for what the card touches outside itself -------------------------

// The card declares three decodes at seating: BAR0 (display memory), BAR1
// (the relocatable VGA I/O range) and the strapped legacy block at $3B0.
// Each is captured so a row can drive it the way the bus would.
static const memory_interface_t *s_fb_if, *s_io_if, *s_vga_if;
static void *s_fb_ctx, *s_io_ctx, *s_vga_ctx;
static uint32_t s_vga_base, s_vga_span;
static pci_space_t s_vga_space;

void pci_bar_backing_iface(pci_device_t *dev, int bar, const memory_interface_t *iface, void *ctx) {
    (void)dev;
    if (bar == 0) {
        s_fb_if = iface;
        s_fb_ctx = ctx;
    } else if (bar == 1) {
        s_io_if = iface;
        s_io_ctx = ctx;
    }
}
void pci_device_add_fixed_region(pci_device_t *dev, pci_space_t space, uint32_t base, uint32_t span,
                                 uint32_t match_mask, uint32_t match_value, const memory_interface_t *iface,
                                 void *ctx) {
    (void)dev;
    (void)match_mask;
    (void)match_value;
    s_vga_space = space;
    s_vga_base = base;
    s_vga_span = span;
    s_vga_if = iface;
    s_vga_ctx = ctx;
}
// config_space.c tells the bus when a BAR or the command register moved;
// nothing here maps regions, so the notification is only counted.
static int s_regions_changed;
void pci_device_regions_changed(pci_device_t *dev) {
    (void)dev;
    s_regions_changed++;
}
void pci_card_set_framebuffer_object(pci_device_t *dev, struct object *fb) {
    (void)dev;
    (void)fb;
}

// Input Status 1 is a function of the scheduler's cycle count.
static uint64_t s_cycles;
uint64_t scheduler_cpu_cycles(struct scheduler *s) {
    (void)s;
    return s_cycles;
}

// The object tree: object_new() hands back the card's display_fb_node_t, so
// the framebuffer node's resolve/base hooks are reachable from a row.
const class_desc_t display_fb_class = {0};
static display_fb_node_t *s_fb_node;
static char s_fake_object;
struct object *object_new(const class_desc_t *cls, void *data, const char *name) {
    (void)cls;
    (void)name;
    s_fb_node = (display_fb_node_t *)data;
    return (struct object *)&s_fake_object;
}
void object_attach(struct object *parent, struct object *child) {
    (void)parent;
    (void)child;
}
void object_set_label(struct object *o, const char *l) {
    (void)o;
    (void)l;
}
void object_set_order(struct object *o, int order) {
    (void)o;
    (void)order;
}

// The checkpoint stream: a flat byte buffer, written in order and read back
// in the same order, which is all the card's save/restore pair relies on.
#define CP_CAP (2u * 1024u * 1024u)
static uint8_t *s_cp_buf;
static size_t s_cp_len, s_cp_pos;
static char s_cp_token;
#define TEST_CP ((checkpoint_t *)&s_cp_token)

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)cp;
    (void)tag;
    (void)file;
    (void)line;
    ASSERT_TRUE(s_cp_len + size <= CP_CAP);
    memcpy(s_cp_buf + s_cp_len, data, size);
    s_cp_len += size;
}
void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)cp;
    (void)tag;
    (void)file;
    (void)line;
    ASSERT_TRUE(s_cp_pos + size <= s_cp_len);
    memcpy(data, s_cp_buf + s_cp_pos, size);
    s_cp_pos += size;
}

// --- Seating ----------------------------------------------------------------

// The card keeps its config_t for Input Status 1, so it must outlive the
// device.  The scheduler is never dereferenced (scheduler_cpu_cycles is the
// stub above); the profile supplies the core clock.
static config_t s_cfg;
static hw_profile_t s_profile;
static char s_sched_token;

static pci_device_t *seat(void) {
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_profile, 0, sizeof(s_profile));
    s_fb_if = s_io_if = s_vga_if = NULL;
    s_cycles = 0;
    pci_device_t *dev = cirrus_54m30_kind.factory(1, &s_cfg, NULL);
    ASSERT_TRUE(dev && s_fb_if && s_io_if && s_vga_if);
    return dev;
}

static void unseat(pci_device_t *dev) {
    dev->ops->teardown(dev, &s_cfg);
    free(dev); // the bus owns the wrapper
}

// --- The legacy VGA ports, as the guest reaches them ------------------------

// Byte offsets into the strapped $3B0-$3DF block.
#define P_STATUS1_MONO 0x0Au // $3BA
#define P_ATTR         0x10u // $3C0
#define P_ATTR_READ    0x11u // $3C1
#define P_SEQ_INDEX    0x14u // $3C4
#define P_SEQ_DATA     0x15u // $3C5
#define P_DAC_RINDEX   0x17u // $3C7
#define P_DAC_WINDEX   0x18u // $3C8
#define P_DAC_DATA     0x19u // $3C9
#define P_GR_INDEX     0x1Eu // $3CE
#define P_GR_DATA      0x1Fu // $3CF
#define P_CRTC_INDEX   0x24u // $3D4
#define P_CRTC_DATA    0x25u // $3D5
#define P_STATUS1      0x2Au // $3DA

static void port_out(uint32_t port, uint8_t v) {
    s_vga_if->write_uint8(s_vga_ctx, port, v);
}
static uint8_t port_in(uint32_t port) {
    return s_vga_if->read_uint8(s_vga_ctx, port);
}
static void seq(uint8_t i, uint8_t v) {
    port_out(P_SEQ_INDEX, i);
    port_out(P_SEQ_DATA, v);
}
static void crtc(uint8_t i, uint8_t v) {
    port_out(P_CRTC_INDEX, i);
    port_out(P_CRTC_DATA, v);
}
static void gr(uint8_t i, uint8_t v) {
    port_out(P_GR_INDEX, i);
    port_out(P_GR_DATA, v);
}

// What Open Firmware 1.1.22 programs on a cold boot (cirrus54m30.c,
// "Deriving the mode"; also docs/machines/tnt/tnt.md): 640x480, 8 bpp.
static void program_of_640x480x8(void) {
    seq(0x01, 0x01); // SR01 bit 0: 8 dots per character clock
    seq(0x07, 0xF1); // SR07 bit 0 = extended mode, bits [3:1] = 000 = 8 bpp
    crtc(0x01, 0x4F); // CR01 horizontal display end: (79 + 1) * 8 = 640
    crtc(0x07, 0x02); // CR07 bit 1 = vertical display end bit 8
    crtc(0x12, 0xDF); // CR12: VDE = $1DF = 479 -> 480 lines
    crtc(0x13, 0x50); // CR13 offset 80, eight-byte units -> 640 bytes
    crtc(0x0C, 0x00); // CR0C/CR0D start address 0
    crtc(0x0D, 0x00);
    gr(0x05, 0x40); // GR05 256-colour shift mode
}

// 1024x768x8, the part's advertised big-endian maximum: CR01 = 127 ->
// 128 * 8 = 1024; VDE 767 = $2FF, i.e. CR12 = $FF with bit 9 in CR07 bit 6
// and bit 8 (CR07 bit 1) clear; offset 128 -> 1024 bytes.
static void program_1024x768x8(void) {
    seq(0x01, 0x01);
    seq(0x07, 0xF1);
    crtc(0x13, 0x80);
    crtc(0x01, 0x7F);
    crtc(0x07, 0x40);
    crtc(0x12, 0xFF);
}

static void fb_poke(uint32_t off, uint8_t v) {
    s_fb_if->write_uint8(s_fb_ctx, off, v);
}

// --- Mode derivation --------------------------------------------------------

// Before a guest programs anything the card advertises no display, so
// pci_primary_display falls through to whatever else the machine has
// (c54m30_display's contract).
TEST(no_display_before_a_mode_is_programmed) {
    pci_device_t *dev = seat();
    ASSERT_TRUE(dev->ops->display(dev) == NULL);
    unseat(dev);
}

// The Open Firmware sequence yields 640x480x8 at a 640-byte stride, scanned
// from the bottom of display memory.
TEST(open_firmware_sequence_gives_640x480x8) {
    pci_device_t *dev = seat();
    fb_poke(0, 0xA5); // a marker at VRAM offset 0 = the start address
    program_of_640x480x8();
    display_t *d = dev->ops->display(dev);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ_INT((int)d->width, 640);
    ASSERT_EQ_INT((int)d->height, 480);
    ASSERT_EQ_INT((int)d->stride, 640);
    ASSERT_EQ_INT((int)d->format, (int)PIXEL_8BPP);
    ASSERT_TRUE(d->bits != NULL);
    ASSERT_EQ_INT(d->bits[0], 0xA5);
    ASSERT_TRUE(d->shape_dirty);
    unseat(dev);
}

// 1024x768x8 -- VDE bit 9 from CR07 bit 6, not bit 8 -- fits the 1 MB store
// (786,432 bytes) and is taken as programmed.
TEST(mode_1024x768x8_uses_vde_bit_9) {
    pci_device_t *dev = seat();
    program_1024x768x8();
    display_t *d = dev->ops->display(dev);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ_INT((int)d->width, 1024);
    ASSERT_EQ_INT((int)d->height, 768);
    ASSERT_EQ_INT((int)d->stride, 1024);
    unseat(dev);
}

// SR01 bit 0 clear selects nine-dot character clocks (VGA clocking mode
// register): CR01 = 79 then gives 80 * 9 = 720 pixels.  Offset 90 -> 720
// bytes; VDE $18F = 399 -> 400 lines.
TEST(nine_dot_clocks_give_720_pixels) {
    pci_device_t *dev = seat();
    seq(0x01, 0x00);
    seq(0x07, 0xF1);
    crtc(0x13, 0x5A);
    crtc(0x01, 0x4F);
    crtc(0x07, 0x02);
    crtc(0x12, 0x8F);
    display_t *d = dev->ops->display(dev);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ_INT((int)d->width, 720);
    ASSERT_EQ_INT((int)d->height, 400);
    ASSERT_EQ_INT((int)d->stride, 720);
    unseat(dev);
}

// Start address: CR0C/CR0D in doubleword units, extended by CR1B bit 0
// (address bit 16) and CR1B bits 3:2 (bits 18:17).  $0100 -> byte 1024;
// CR1B = $01 -> $10000 * 4 = byte $40000.
TEST(start_address_and_its_cirrus_extension) {
    pci_device_t *dev = seat();
    fb_poke(1024, 0x11);
    fb_poke(0x40000, 0x22);
    fb_poke(0x80000, 0x33);
    program_of_640x480x8();
    crtc(0x0C, 0x01);
    display_t *d = dev->ops->display(dev);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ_INT(d->bits[0], 0x11);
    crtc(0x0C, 0x00);
    crtc(0x1B, 0x01);
    ASSERT_EQ_INT(d->bits[0], 0x22);
    // CR1B bit 2 is address bit 17: $20000 * 4 = byte $80000.
    crtc(0x1B, 0x04);
    ASSERT_EQ_INT(d->bits[0], 0x33);
    unseat(dev);
}

// A start address that pushes the raster off the end of the 1 MB store is
// the base being wrong, not the mode: the card scans from 0 instead.  CR1B =
// $0D sets address bits 16-18: $70000 * 4 = byte $1C0000, past 1 MB.
TEST(start_past_the_store_falls_back_to_zero) {
    pci_device_t *dev = seat();
    fb_poke(0, 0x5A);
    program_of_640x480x8();
    crtc(0x1B, 0x0D);
    display_t *d = dev->ops->display(dev);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ_INT((int)d->width, 640);
    ASSERT_EQ_INT((int)d->height, 480);
    ASSERT_EQ_INT(d->bits[0], 0x5A);
    unseat(dev);
}

// Not a packed-pixel mode: SR07 bit 0 clear is plain VGA text/planar, which
// c54m30_bpp() reports as depth 0.  Nothing is presented, however complete
// the CRTC geometry.
TEST(standard_vga_mode_presents_nothing) {
    pci_device_t *dev = seat();
    seq(0x01, 0x01);
    seq(0x07, 0xF0); // extended-mode enable clear
    crtc(0x01, 0x4F);
    crtc(0x07, 0x02);
    crtc(0x12, 0xDF);
    crtc(0x13, 0x50);
    ASSERT_TRUE(dev->ops->display(dev) == NULL);
    unseat(dev);
}

// Deeper colour needs a little-endian framebuffer window the display layer
// does not have (Apple: big-endian hosts are limited to 8 bpp on this part),
// so a 16/24/32 bpp selection is refused and the last good mode is kept.
// SR07 bits [3:1] = 001 is 16 bpp 5-5-5, 011 is 32 bpp.
TEST(deeper_colour_keeps_the_last_good_mode) {
    pci_device_t *dev = seat();
    program_of_640x480x8();
    seq(0x07, 0xF3); // 16 bpp
    crtc(0x13, 0xA0); // offset 160 -> a 1280-byte 16 bpp stride
    display_t *d = dev->ops->display(dev);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ_INT((int)d->stride, 640);
    ASSERT_EQ_INT((int)d->format, (int)PIXEL_8BPP);
    seq(0x07, 0xF7); // 32 bpp
    ASSERT_EQ_INT((int)dev->ops->display(dev)->stride, 640);
    unseat(dev);

    // ...and from reset, with no good mode to keep, nothing at all.
    dev = seat();
    seq(0x01, 0x01);
    seq(0x07, 0xF3);
    crtc(0x01, 0x4F);
    crtc(0x07, 0x02);
    crtc(0x12, 0xDF);
    crtc(0x13, 0xA0);
    ASSERT_TRUE(dev->ops->display(dev) == NULL);
    unseat(dev);
}

// A half-programmed CRTC -- a stride shorter than a line -- is waited out,
// not presented: the last good mode stays.
TEST(stride_shorter_than_a_line_is_ignored) {
    pci_device_t *dev = seat();
    program_of_640x480x8();
    crtc(0x13, 0x40); // 512 bytes for a 640-pixel line
    display_t *d = dev->ops->display(dev);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ_INT((int)d->width, 640);
    ASSERT_EQ_INT((int)d->stride, 640);
    unseat(dev);
}

// A raster the 1 MB store cannot back scans nothing: offset 255 (a 2040-byte
// stride) at 768 lines is 1,566,720 bytes.  The card has no blank buffer, so
// display_set_scanout zeroes the height and the display op returns NULL.
TEST(raster_larger_than_vram_presents_nothing) {
    pci_device_t *dev = seat();
    program_1024x768x8();
    ASSERT_TRUE(dev->ops->display(dev) != NULL);
    crtc(0x13, 0xFF);
    ASSERT_TRUE(dev->ops->display(dev) == NULL);
    // Bringing the offset back restores the picture.
    crtc(0x13, 0x80);
    display_t *d = dev->ops->display(dev);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ_INT((int)d->height, 768);
    ASSERT_EQ_INT((int)d->stride, 1024);
    unseat(dev);
}

// --- Config space -----------------------------------------------------------

static uint32_t cfg_read(pci_device_t *dev, uint32_t reg) {
    return pci_cfg_read(dev, reg);
}
static void cfg_write32(pci_device_t *dev, uint32_t reg, uint32_t v) {
    for (uint32_t b = 0; b < 4; b++)
        pci_cfg_write(dev, reg, b, (uint8_t)(v >> (8u * b)));
}

// Identity: vendor $1013 (Cirrus Logic), device $00A0 (the Alpine TRM's PCI
// ID reset value), class $030000 (VGA-compatible display), revision 0.
TEST(config_identity) {
    pci_device_t *dev = seat();
    ASSERT_TRUE(cfg_read(dev, PCI_CFG_ID) == 0x00A01013u);
    ASSERT_TRUE(cfg_read(dev, PCI_CFG_CLASS) == 0x03000000u);
    ASSERT_EQ_INT((int)((cfg_read(dev, PCI_CFG_MISC) >> 16) & 0xFFu), 0x00); // type-0 header
    unseat(dev);
}

// BAR sizing, the universal all-ones probe (PCI 2.0 section 6.2.5.1), against
// c54m30_decl: BAR0 is a 16 MB prefetchable memory aperture (base in bits
// 31:24, type bits 1000b); BAR1 a 512-byte I/O range (size mask $FFFFFE00,
// bit 0 set for I/O space); BARs 2-5 are unimplemented and read zero.
TEST(bar_sizing_matches_the_declaration) {
    pci_device_t *dev = seat();
    for (uint32_t reg = PCI_CFG_BAR0; reg <= PCI_CFG_BAR5; reg += 4)
        cfg_write32(dev, reg, 0xFFFFFFFFu);
    ASSERT_TRUE(cfg_read(dev, PCI_CFG_BAR0) == 0xFF000008u);
    ASSERT_TRUE(cfg_read(dev, PCI_CFG_BAR0 + 4) == 0xFFFFFE01u);
    for (uint32_t reg = PCI_CFG_BAR0 + 8; reg <= PCI_CFG_BAR5; reg += 4)
        ASSERT_TRUE(cfg_read(dev, reg) == 0u);
    ASSERT_TRUE(pci_cfg_bar_size(dev, 0) == 0x01000000u);
    ASSERT_TRUE(pci_cfg_bar_size(dev, 1) == 0x200u);
    // An assigned base reads back masked to the BAR's alignment: Open
    // Firmware's I/O assignment of $00010000 (docs/machines/tnt/tnt.md).
    cfg_write32(dev, PCI_CFG_BAR0 + 4, 0x00010000u);
    ASSERT_TRUE(cfg_read(dev, PCI_CFG_BAR0 + 4) == 0x00010001u);
    unseat(dev);
}

// The strapped legacy decode is the VGA block $3B0-$3DF in I/O space, not a
// BAR (cirrus54m30.c, "The VGA I/O ranges").
TEST(legacy_vga_block_is_a_fixed_io_region) {
    pci_device_t *dev = seat();
    ASSERT_EQ_INT((int)s_vga_space, (int)PCI_SPACE_IO);
    ASSERT_TRUE(s_vga_base == 0x3B0u);
    ASSERT_TRUE(s_vga_span == 0x30u);
    unseat(dev);
}

// No expansion ROM: Open Firmware builds the node from the main ROM's
// `54m30-config`, so the ROM BAR is unimplemented -- a sizing probe reads
// zero and the declared size is zero.
TEST(no_expansion_rom) {
    pci_device_t *dev = seat();
    cfg_write32(dev, PCI_CFG_ROM_BAR, 0xFFFFFFFFu);
    ASSERT_TRUE(cfg_read(dev, PCI_CFG_ROM_BAR) == 0u);
    ASSERT_TRUE(pci_cfg_bar_size(dev, PCI_ROM_BAR_INDEX) == 0u);
    ASSERT_TRUE(!pci_cfg_bar_enabled(dev, PCI_ROM_BAR_INDEX));
    unseat(dev);
}

// No interrupt: the pin byte ($3D) reads 0, "uses no interrupt pin" in the
// PCI 2.0 header, and stays 0 through a write -- it is strapped.
TEST(no_interrupt_pin) {
    pci_device_t *dev = seat();
    ASSERT_EQ_INT((int)((cfg_read(dev, PCI_CFG_INTERRUPT) >> 8) & 0xFFu), 0);
    cfg_write32(dev, PCI_CFG_INTERRUPT, 0xFFFFFFFFu);
    ASSERT_EQ_INT((int)((cfg_read(dev, PCI_CFG_INTERRUPT) >> 8) & 0xFFu), 0);
    unseat(dev);
}

// The command register keeps only what the part implements: I/O space,
// memory space and bus master (c54m30_decl.command_writable).
TEST(command_register_writable_bits) {
    pci_device_t *dev = seat();
    ASSERT_EQ_INT((int)(cfg_read(dev, PCI_CFG_COMMAND) & 0xFFFFu), 0);
    cfg_write32(dev, PCI_CFG_COMMAND, 0x0000FFFFu);
    ASSERT_EQ_INT((int)(cfg_read(dev, PCI_CFG_COMMAND) & 0xFFFFu),
                  (int)(PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_MASTER));
    unseat(dev);
}

// --- The register file ------------------------------------------------------

// The indexed blocks round-trip, and both windows -- the strapped legacy
// block and the relocatable BAR1 range, indexed there by the port's low
// byte -- reach one register file.
TEST(indexed_registers_round_trip_through_both_windows) {
    pci_device_t *dev = seat();
    seq(0x07, 0xF1);
    gr(0x05, 0x40);
    crtc(0x13, 0x50);
    port_out(P_SEQ_INDEX, 0x07);
    ASSERT_EQ_INT(port_in(P_SEQ_DATA), 0xF1);
    port_out(P_GR_INDEX, 0x05);
    ASSERT_EQ_INT(port_in(P_GR_DATA), 0x40);
    port_out(P_CRTC_INDEX, 0x13);
    ASSERT_EQ_INT(port_in(P_CRTC_DATA), 0x50);
    // BAR1: $3C4/$3C5 at +$C4/+$C5.
    s_io_if->write_uint8(s_io_ctx, 0xC4, 0x07);
    ASSERT_EQ_INT(s_io_if->read_uint8(s_io_ctx, 0xC5), 0xF1);
    s_io_if->write_uint8(s_io_ctx, 0xC5, 0xE1);
    ASSERT_EQ_INT(port_in(P_SEQ_DATA), 0xE1);
    unseat(dev);
}

// A halfword store to the index port loads the index and the data in one
// cycle, the byte at the lower address first -- the order the card's 16-bit
// handlers split a big-endian halfword in.
TEST(halfword_store_sets_index_then_data) {
    pci_device_t *dev = seat();
    s_vga_if->write_uint16(s_vga_ctx, P_SEQ_INDEX, 0x07F1u);
    ASSERT_EQ_INT(port_in(P_SEQ_INDEX), 0x07);
    ASSERT_EQ_INT(port_in(P_SEQ_DATA), 0xF1); // SR07, without a second index write
    unseat(dev);
}

// The attribute controller shares one port for index and data, alternating;
// a read of Input Status 1 resets the flip-flop to "index" (VGA attribute
// controller semantics).
TEST(attribute_flip_flop_resets_on_status_read) {
    pci_device_t *dev = seat();
    port_out(P_ATTR, 0x10); // index $10 (mode control)
    port_out(P_ATTR, 0x41); // data
    port_out(P_ATTR, 0x12); // the flip-flop is back at "index": select $12
    port_in(P_STATUS1); // ...and a status read forces "index" again
    port_out(P_ATTR, 0x10);
    ASSERT_EQ_INT(port_in(P_ATTR_READ), 0x41);
    port_out(P_ATTR, 0x55); // data for $10
    ASSERT_EQ_INT(port_in(P_ATTR_READ), 0x55);
    port_out(P_ATTR, 0x11); // index $11; the flip-flop now expects data
    port_in(P_STATUS1_MONO); // the mono address resets it too...
    port_out(P_ATTR, 0x10); // ...so this is an index, not $11's data
    ASSERT_EQ_INT(port_in(P_ATTR_READ), 0x55);
    unseat(dev);
}

// The DAC: one write index, then R, G, B per entry with auto-advance.
// Values are six bits (the high two are dropped) and the renderer's palette
// expands them by replicating the top two bits into the bottom, so $3F is
// $FF and $20 is $82.  The read port walks the same way.
TEST(dac_palette_load_and_expansion) {
    pci_device_t *dev = seat();
    program_of_640x480x8();
    port_out(P_DAC_WINDEX, 5);
    port_out(P_DAC_DATA, 0xFF); // -> $3F
    port_out(P_DAC_DATA, 0x20);
    port_out(P_DAC_DATA, 0x00);
    port_out(P_DAC_DATA, 0x01); // entry 6 follows without a new index
    port_out(P_DAC_DATA, 0x02);
    port_out(P_DAC_DATA, 0x03);
    display_t *d = dev->ops->display(dev);
    ASSERT_TRUE(d != NULL && d->clut != NULL);
    ASSERT_EQ_INT(d->clut_len, 256);
    ASSERT_EQ_INT(d->clut[5].r, 0xFF);
    ASSERT_EQ_INT(d->clut[5].g, 0x82);
    ASSERT_EQ_INT(d->clut[5].b, 0x00);
    ASSERT_EQ_INT(d->clut[5].a, 0xFF);
    ASSERT_EQ_INT(d->clut[6].r, 0x04); // $01 -> 0000 0100
    ASSERT_EQ_INT(d->clut[6].b, 0x0C); // $03 -> 0000 1100
    ASSERT_TRUE(d->clut_dirty);
    port_out(P_DAC_RINDEX, 5);
    ASSERT_EQ_INT(port_in(P_DAC_DATA), 0x3F);
    ASSERT_EQ_INT(port_in(P_DAC_DATA), 0x20);
    ASSERT_EQ_INT(port_in(P_DAC_DATA), 0x00);
    ASSERT_EQ_INT(port_in(P_DAC_DATA), 0x01); // entry 6's red
    unseat(dev);
}

// Input Status 1 toggles vertical retrace (bit 3) with display-enable
// inactive (bit 0) over the last 1/14 of each 1/60 s frame of emulated
// time, so a wait-for-retrace loop terminates.  A 840 kHz clock makes a
// 14,000-cycle frame with retrace from cycle 13,000.
TEST(input_status_1_follows_emulated_time) {
    pci_device_t *dev = seat();
    ASSERT_EQ_INT(port_in(P_STATUS1), 0); // no scheduler: never in retrace
    s_profile.freq = 60u * 14000u;
    s_cfg.machine = &s_profile;
    s_cfg.scheduler = (scheduler_t *)&s_sched_token;
    s_cycles = 12999;
    ASSERT_EQ_INT(port_in(P_STATUS1), 0x00);
    s_cycles = 13000;
    ASSERT_EQ_INT(port_in(P_STATUS1), 0x09);
    ASSERT_EQ_INT(port_in(P_STATUS1_MONO), 0x09);
    s_cycles = 14000 + 5; // next frame, active display again
    ASSERT_EQ_INT(port_in(P_STATUS1), 0x00);
    unseat(dev);
}

// The display-memory aperture: the fitted 1 MB answers, the rest of the
// 16 MB decode reads zero and swallows writes; wider accesses are
// big-endian over the byte lanes.
TEST(aperture_beyond_the_fitted_megabyte_is_empty) {
    pci_device_t *dev = seat();
    s_fb_if->write_uint32(s_fb_ctx, 0x100, 0x11223344u);
    ASSERT_EQ_INT(s_fb_if->read_uint8(s_fb_ctx, 0x100), 0x11);
    ASSERT_EQ_INT(s_fb_if->read_uint16(s_fb_ctx, 0x102), 0x3344);
    ASSERT_TRUE(s_fb_if->read_uint32(s_fb_ctx, 0x100) == 0x11223344u);
    s_fb_if->write_uint8(s_fb_ctx, 0x100000, 0x77);
    ASSERT_EQ_INT(s_fb_if->read_uint8(s_fb_ctx, 0x100000), 0);
    ASSERT_EQ_INT(s_fb_if->read_uint8(s_fb_ctx, 0xFFFFFF), 0);
    unseat(dev);
}

// PCI RST# clears the register file and the palette (to opaque black);
// display memory is DRAM and survives.
TEST(reset_clears_registers_not_vram) {
    pci_device_t *dev = seat();
    program_of_640x480x8();
    port_out(P_DAC_WINDEX, 0);
    port_out(P_DAC_DATA, 0x3F);
    port_out(P_DAC_DATA, 0x3F);
    port_out(P_DAC_DATA, 0x3F);
    fb_poke(0x10, 0x99);
    dev->ops->reset(dev, &s_cfg);
    port_out(P_SEQ_INDEX, 0x07);
    ASSERT_EQ_INT(port_in(P_SEQ_DATA), 0);
    port_out(P_CRTC_INDEX, 0x01);
    ASSERT_EQ_INT(port_in(P_CRTC_DATA), 0);
    port_out(P_DAC_RINDEX, 0);
    ASSERT_EQ_INT(port_in(P_DAC_DATA), 0);
    ASSERT_EQ_INT(s_fb_if->read_uint8(s_fb_ctx, 0x10), 0x99);
    unseat(dev);
}

// PCI reset clears the registers the mode is derived from, so the card has
// no mode until the firmware programs one.  It used to keep presenting the
// old geometry over stale memory: "keep the last good mode" swallowed the
// reset state too.
TEST(reset_withdraws_the_mode) {
    pci_device_t *dev = seat();
    program_of_640x480x8();
    ASSERT_TRUE(dev->ops->display(dev) != NULL);
    dev->ops->reset(dev, &s_cfg);
    ASSERT_TRUE(dev->ops->display(dev) == NULL);
    program_of_640x480x8(); // and the firmware's next mode set brings it back
    display_t *d = dev->ops->display(dev);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ_INT((int)d->width, 640);
    unseat(dev);
}

// --- Checkpoints and the framebuffer node -----------------------------------

// The mode is DERIVED state: restore rebuilds it from the saved registers
// into the restoring card's own display memory, together with the palette
// view and the VRAM contents.
TEST(checkpoint_round_trips_the_mode) {
    s_cp_buf = (uint8_t *)malloc(CP_CAP);
    ASSERT_TRUE(s_cp_buf != NULL);
    s_cp_len = s_cp_pos = 0;

    pci_device_t *a = seat();
    program_1024x768x8();
    port_out(P_DAC_WINDEX, 7);
    port_out(P_DAC_DATA, 0x3F);
    port_out(P_DAC_DATA, 0x00);
    port_out(P_DAC_DATA, 0x20);
    fb_poke(0, 0xC3);
    fb_poke(0xFFFFF, 0x3C);
    a->ops->checkpoint_save(a, TEST_CP);
    unseat(a);

    pci_device_t *b = seat();
    ASSERT_TRUE(b->ops->display(b) == NULL);
    b->ops->checkpoint_restore(b, TEST_CP);
    ASSERT_TRUE(s_cp_pos == s_cp_len); // read exactly what was written
    display_t *d = b->ops->display(b);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ_INT((int)d->width, 1024);
    ASSERT_EQ_INT((int)d->height, 768);
    ASSERT_EQ_INT((int)d->stride, 1024);
    ASSERT_EQ_INT(d->bits[0], 0xC3); // the restoring card's VRAM, restored
    ASSERT_EQ_INT(s_fb_if->read_uint8(s_fb_ctx, 0xFFFFF), 0x3C);
    ASSERT_EQ_INT(d->clut[7].r, 0xFF);
    ASSERT_EQ_INT(d->clut[7].g, 0x00);
    ASSERT_EQ_INT(d->clut[7].b, 0x82);
    ASSERT_TRUE(d->shape_dirty && d->clut_dirty && d->fb_dirty);
    unseat(b);
    free(s_cp_buf);
    s_cp_buf = NULL;
}

// A checkpoint taken between an index write and its data write, or partway
// through a palette entry, resumes there: the index latches, the attribute
// flip-flop and the DAC position are part of the state.  They were not
// saved, so after a restore $3C5 answered SR00 instead of the selected SR07,
// and the rest of an interrupted palette load landed in entry 0.
TEST(checkpoint_keeps_the_port_latches) {
    s_cp_buf = (uint8_t *)malloc(CP_CAP);
    ASSERT_TRUE(s_cp_buf != NULL);
    s_cp_len = s_cp_pos = 0;

    pci_device_t *a = seat();
    seq(0x07, 0xF1); // leaves the sequencer index at 7
    port_out(P_DAC_WINDEX, 9);
    port_out(P_DAC_DATA, 0x3F); // entry 9, red written: green is next
    a->ops->checkpoint_save(a, TEST_CP);
    unseat(a);

    pci_device_t *b = seat();
    b->ops->checkpoint_restore(b, TEST_CP);
    ASSERT_TRUE(s_cp_pos == s_cp_len);
    ASSERT_EQ_INT(port_in(P_SEQ_DATA), 0xF1);
    port_out(P_DAC_DATA, 0x3F);
    port_out(P_DAC_DATA, 0x3F);
    port_out(P_DAC_RINDEX, 9);
    ASSERT_EQ_INT(port_in(P_DAC_DATA), 0x3F);
    ASSERT_EQ_INT(port_in(P_DAC_DATA), 0x3F);
    ASSERT_EQ_INT(port_in(P_DAC_DATA), 0x3F);
    unseat(b);
    free(s_cp_buf);
    s_cp_buf = NULL;
}

// The shared framebuffer node resolves to the live descriptor and reports
// the CR0C/CR0D start address as a byte offset into display memory.
TEST(framebuffer_node_resolves_the_descriptor) {
    pci_device_t *dev = seat();
    s_fb_node = NULL;
    cirrus_54m30_kind.attach_objects(dev, (struct object *)&s_fake_object);
    ASSERT_TRUE(s_fb_node && s_fb_node->resolve && s_fb_node->base);
    program_of_640x480x8();
    crtc(0x0D, 0x40); // $0040 doublewords = byte $100
    ASSERT_TRUE(s_fb_node->resolve(s_fb_node->owner) == dev->ops->display(dev));
    ASSERT_TRUE(s_fb_node->base(s_fb_node->owner) == 0x100u);
    unseat(dev);
}

// The node's base is where the scanout starts, not the CR0C/CR0D pair alone:
// it includes the CR1B extension, and follows the fall-back to 0 when the
// start would run the raster off the end of display memory.
TEST(framebuffer_node_base_is_the_scanout_start) {
    pci_device_t *dev = seat();
    s_fb_node = NULL;
    cirrus_54m30_kind.attach_objects(dev, (struct object *)&s_fake_object);
    program_of_640x480x8();
    fb_poke(0x40000, 0x22);
    crtc(0x1B, 0x01); // start bit 16: doubleword $10000 = byte $40000
    display_t *d = dev->ops->display(dev);
    ASSERT_EQ_INT(d->bits[0], 0x22);
    ASSERT_TRUE(s_fb_node->base(s_fb_node->owner) == 0x40000u);
    crtc(0x1B, 0x0C); // bits 18:17 set: byte $C0000, and 640x480 does not fit
    ASSERT_TRUE(s_fb_node->base(s_fb_node->owner) == 0u);
    unseat(dev);
}

int main(void) {
    RUN(no_display_before_a_mode_is_programmed);
    RUN(open_firmware_sequence_gives_640x480x8);
    RUN(mode_1024x768x8_uses_vde_bit_9);
    RUN(nine_dot_clocks_give_720_pixels);
    RUN(start_address_and_its_cirrus_extension);
    RUN(start_past_the_store_falls_back_to_zero);
    RUN(standard_vga_mode_presents_nothing);
    RUN(deeper_colour_keeps_the_last_good_mode);
    RUN(stride_shorter_than_a_line_is_ignored);
    RUN(raster_larger_than_vram_presents_nothing);
    RUN(config_identity);
    RUN(bar_sizing_matches_the_declaration);
    RUN(legacy_vga_block_is_a_fixed_io_region);
    RUN(no_expansion_rom);
    RUN(no_interrupt_pin);
    RUN(command_register_writable_bits);
    RUN(indexed_registers_round_trip_through_both_windows);
    RUN(halfword_store_sets_index_then_data);
    RUN(attribute_flip_flop_resets_on_status_read);
    RUN(dac_palette_load_and_expansion);
    RUN(input_status_1_follows_emulated_time);
    RUN(aperture_beyond_the_fitted_megabyte_is_empty);
    RUN(reset_clears_registers_not_vram);
    RUN(reset_withdraws_the_mode);
    RUN(checkpoint_round_trips_the_mode);
    RUN(checkpoint_keeps_the_port_latches);
    RUN(framebuffer_node_resolves_the_descriptor);
    RUN(framebuffer_node_base_is_the_scanout_start);
    return 0;
}
