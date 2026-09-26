// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The Voodoo2's scanout raster is a fixed allocation, and videoDimensions is
// guest-programmed: 11 bits of height (up to 2047) against a raster sized for
// 1024 x 1024.  A card driving the monitor at 1024 x 1100 wrote the rows past
// 1024 beyond the allocation every frame (#174).  The card must really drive
// the monitor here -- pass-through set, blanking and reset clear, outputs
// enabled -- or the conversion never runs and the test passes whatever the code
// does.

#include "card.h"
#include "display.h"
#include "pci.h"
#include "system_config.h"
#include "test_assert.h"

#include <stdlib.h>
#include <string.h>

extern const pci_card_kind_t voodoo2_kind;

// --- Stubs for what the card touches outside itself -------------------------

static const memory_interface_t *s_bar_if;
static void *s_bar_ctx;

void pci_bar_backing_iface(pci_device_t *dev, int bar, const memory_interface_t *iface, void *ctx) {
    (void)dev;
    (void)bar;
    s_bar_if = iface;
    s_bar_ctx = ctx;
}
void pci_cfg_reset(pci_device_t *dev) {
    (void)dev;
}
void pci_card_set_framebuffer_object(pci_device_t *dev, struct object *fb) {
    (void)dev;
    (void)fb;
}
uint64_t scheduler_cpu_cycles(struct scheduler *s) {
    (void)s;
    return 0;
}
bool gs_v2gpu_available(void) {
    return false;
}
bool gs_v2gpu_attach(void *ctrl, uint32_t bytes) {
    (void)ctrl;
    (void)bytes;
    return false;
}
void gs_v2gpu_detach(void *ctrl) {
    (void)ctrl;
}
void gs_v2gpu_notify(volatile uint32_t *addr) {
    (void)addr;
}
int gs_v2gpu_wait(volatile uint32_t *addr, uint32_t expected, uint32_t timeout_ms) {
    (void)addr;
    (void)expected;
    (void)timeout_ms;
    return -1;
}
struct object *object_new(const class_desc_t *cls, void *data, const char *name) {
    (void)cls;
    (void)data;
    (void)name;
    return NULL;
}
void object_attach(struct object *parent, struct object *child) {
    (void)parent;
    (void)child;
}
void *object_data(struct object *o) {
    (void)o;
    return NULL;
}
void object_set_category(struct object *o, uint16_t c) {
    (void)o;
    (void)c;
}
void object_set_label(struct object *o, const char *l) {
    (void)o;
    (void)l;
}
void object_set_order(struct object *o, int order) {
    (void)o;
    (void)order;
}

// --- The card's register face, as the bus sees it ---------------------------

#define R_VIDEODIM 0x83
#define R_FBIINIT0 0x84
#define R_FBIINIT1 0x85

static void reg_write(uint32_t idx, uint32_t value) {
    // The Mac's big-endian bus: the card swaps at its own edge.
    s_bar_if->write_uint32(s_bar_ctx, idx * 4u, __builtin_bswap32(value));
}

static pci_device_t *seat_driving(uint32_t width, uint32_t height) {
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    voodoo2_kind.stage_option("raster", "sw"); // no worker thread
    pci_device_t *dev = voodoo2_kind.factory(0, &cfg, NULL);
    ASSERT_TRUE(dev && s_bar_if);
    dev->ops->cfg_write(dev, 0x40, 0, 0x01); // initEnable: fbiInit writes on
    reg_write(R_FBIINIT1, 0x0001E000u); // outputs enabled; no reset, no blank
    reg_write(R_FBIINIT0, 0x00000001u); // the Voodoo drives the monitor
    reg_write(R_VIDEODIM, (height << 16) | (width - 1u));
    return dev;
}

static void unseat(pci_device_t *dev) {
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (dev->ops->teardown)
        dev->ops->teardown(dev, &cfg);
    free(dev); // the bus owns the wrapper
}

// Taller than the raster: the height is not taken as programmed.
TEST(a_height_past_the_raster_is_not_scanned_out) {
    pci_device_t *dev = seat_driving(1024, 1100);
    display_t *d = dev->ops->display(dev); // one frame: the conversion runs
    ASSERT_TRUE(d != NULL); // the card really drives the monitor
    ASSERT_TRUE(d->width == 1024);
    ASSERT_TRUE(d->height <= 1024);
    unseat(dev);
}

// The largest shape the raster holds is scanned out as programmed.
TEST(the_full_raster_is_scanned_out) {
    pci_device_t *dev = seat_driving(1024, 1024);
    display_t *d = dev->ops->display(dev);
    ASSERT_TRUE(d != NULL);
    ASSERT_EQ_INT((int)d->width, 1024);
    ASSERT_EQ_INT((int)d->height, 1024);
    unseat(dev);
}

int main(void) {
    RUN(a_height_past_the_raster_is_not_scanned_out);
    RUN(the_full_raster_is_scanned_out);
    return 0;
}
