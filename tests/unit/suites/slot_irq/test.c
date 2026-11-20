// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// /SLOTIRQ aggregate unit test.  Links the real via.c and the shared
// aggregate (src/machines/runtime/slot_irq.c) against recording stubs, and
// pins the OR on VIA2 CA1: a level, held low while ANY source requests.
//
// The defect pinned here: the GLUE family (SE/30, IIcx, IIx) never saved or
// re-derived its mask, so after a restore it was 0 while VIA2 came back with
// a source's port-A line still low.  The next other source to assert and
// deassert then drove CA1 high with the first source still requesting, and
// the next assert after that forged a fresh CA1 edge.  The MCU saved its mask
// all along.  The restore case below fails with the mask left at 0.

#include "gs_assert.h"
#include "object.h"
#include "slot_irq.h"
#include "test_assert.h"
#include "value.h"
#include "via.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// VIA register selects, as via.c decodes them from address lines 9-12.
#define REG_IFR 13

// IFR bit for CA1 (via.c's private numbering, mirrored here).
#define IFR_CA1 0x02

// ============================================================================
// Stubs — via.c reaches the scheduler, the memory map and the object tree.
// ============================================================================

// A settable cycle counter and the most recently armed event, so a test can
// run a timer out and then keep the clock moving past the timeout.
static uint64_t s_cycles;
static event_callback_t s_armed_cb;
static void *s_armed_src;

void scheduler_new_event_type(scheduler_t *sch, const char *source_name, void *source, const char *event_name,
                              event_callback_t callback) {
    (void)sch;
    (void)source_name;
    (void)source;
    (void)event_name;
    (void)callback;
}
event_t *scheduler_new_cpu_event_ex(scheduler_t *sch, event_callback_t callback, void *source, uint64_t data,
                                    uint64_t cycles, uint64_t ns, bool periodic) {
    (void)periodic;
    (void)sch;
    (void)data;
    (void)ns;
    (void)cycles;
    s_armed_cb = callback;
    s_armed_src = source;
    return NULL;
}
void remove_event(scheduler_t *sch, event_callback_t callback, void *source) {
    (void)sch;
    (void)callback;
    (void)source;
    s_armed_cb = NULL;
}
void scheduler_forget_source(scheduler_t *sch, void *source) {
    (void)sch;
    (void)source;
}
uint64_t scheduler_cpu_cycles(scheduler_t *sch) {
    (void)sch;
    return s_cycles;
}
// via_t is opaque to callers, so the test reaches its registers the way the
// machine does: through the memory_interface_t via_init registers here.
static memory_interface_t *s_iface;
static void *s_device;
void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, const memory_interface_t *iface,
                    void *device) {
    (void)mem;
    (void)addr;
    (void)size;
    (void)name;
    s_iface = iface;
    s_device = device;
}
struct object *object_new(const class_desc_t *cls, void *instance_data, const char *name) {
    (void)cls;
    (void)instance_data;
    (void)name;
    return NULL;
}
void object_delete(struct object *o) {
    (void)o;
}
void object_destroy(struct object **slot) {
    if (slot)
        *slot = NULL;
}
void object_attach(struct object *parent, struct object *child) {
    (void)parent;
    (void)child;
}
void object_detach(struct object *child) {
    (void)child;
}
void *object_data(struct object *o) {
    (void)o;
    return NULL;
}
void object_set_order(struct object *o, int order) {
    (void)o;
    (void)order;
}
struct object *machine_object(void) {
    return NULL;
}
value_t val_uint(uint8_t width, uint64_t u) {
    (void)width;
    (void)u;
    value_t v;
    memset(&v, 0, sizeof(v));
    return v;
}

// ============================================================================
// Harness
// ============================================================================

// A non-NULL map token: via_init only passes it straight to memory_map_add,
// which the stub above intercepts to capture the register interface.
static memory_map_t *const s_dummy_map_token = (memory_map_t *)(uintptr_t)1;

// The VIA's IRQ, output and shift callbacks: nothing to record here.
static void irq_sink(void *ctx, bool active) {
    (void)ctx;
    (void)active;
}
static void output_sink(void *ctx, uint8_t port, uint8_t value) {
    (void)ctx;
    (void)port;
    (void)value;
}
static void shift_sink(void *ctx, uint8_t value) {
    (void)ctx;
    (void)value;
}

// The VIA sits on 512-byte register strides (address lines 9-12), even bytes.
static uint8_t rd(int rs) {
    return s_iface->read_uint8(s_device, (uint32_t)rs << 9);
}
static void wr(int rs, uint8_t value) {
    s_iface->write_uint8(s_device, (uint32_t)rs << 9, value);
}

// A fresh VIA2 at power-on: every port-A line and CA1 idle high, PCR 0
// (CA1 negative edge, as the ROMs program it).
static via_t *make_via2(void) {
    s_cycles = 1000;
    via_t *via = via_init(s_dummy_map_token, NULL, 1, "via2", output_sink, shift_sink, irq_sink, NULL, NULL);
    ASSERT_TRUE(via != NULL);
    return via;
}

// True when a CA1 active (falling) edge has been latched since the last clear
static bool ca1_flagged(void) {
    return (rd(REG_IFR) & IFR_CA1) != 0;
}

// Clear every IFR flag, so the next check sees only new edges
static void clear_ifr(void) {
    wr(REG_IFR, 0x7F);
}

// ============================================================================
// Tests
// ============================================================================

// The first source to assert pulls CA1 low (one edge); a second source adds no
// edge; CA1 rises only when the last one withdraws.
TEST(test_aggregate_is_the_or_of_every_source) {
    via_t *via = make_via2();
    uint8_t mask = 0;
    slot_irq_source(via, &mask, 0, true);
    ASSERT_EQ_INT(mask, 0x01);
    ASSERT_EQ_INT(via_port_input(via, 0) & 0x01, 0); // active-low line
    ASSERT_TRUE(ca1_flagged());
    clear_ifr();
    slot_irq_source(via, &mask, 5, true);
    ASSERT_TRUE(!ca1_flagged()); // already low: no second edge
    slot_irq_source(via, &mask, 5, false);
    slot_irq_source(via, &mask, 5, true);
    ASSERT_TRUE(!ca1_flagged()); // PA0 still holds /SLOTIRQ low throughout
    slot_irq_source(via, &mask, 5, false);
    slot_irq_source(via, &mask, 0, false);
    ASSERT_EQ_INT(mask, 0);
    ASSERT_EQ_INT(via_port_input(via, 0) & 0x7F, 0x7F);
    slot_irq_source(via, &mask, 0, true);
    ASSERT_TRUE(ca1_flagged()); // a fresh assert from idle is an edge again
    via_delete(via);
}

// A source outside PA0-PA6 changes nothing.
TEST(test_out_of_range_source_is_ignored) {
    via_t *via = make_via2();
    uint8_t mask = 0;
    slot_irq_source(via, &mask, 7, true);
    slot_irq_source(via, &mask, -1, true);
    ASSERT_EQ_INT(mask, 0);
    ASSERT_TRUE(!ca1_flagged());
    via_delete(via);
}

// A restore re-derives the mask from the VIA2 pins (the GLUE family saves
// none): with slot $9 (PA0) requesting at save time, a built-in video VBL
// pulse on PA5 after the restore must not raise CA1 under it.
TEST(test_restore_derives_the_mask_from_via2) {
    via_t *via = make_via2();
    uint8_t mask = 0;
    slot_irq_source(via, &mask, 0, true); // card in slot $9 requesting at save
    // The restore: VIA2 comes back with its pins, the family state does not
    uint8_t restored = slot_irq_mask_from_via(via);
    ASSERT_EQ_INT(restored, 0x01);
    clear_ifr();
    slot_irq_source(via, &restored, 5, true); // VBL asserts ...
    slot_irq_source(via, &restored, 5, false); // ... and withdraws
    slot_irq_source(via, &restored, 5, true); // the next frame
    ASSERT_TRUE(!ca1_flagged()); // PA0 kept /SLOTIRQ low: no forged edge
    ASSERT_EQ_INT(restored, 0x21);
    via_delete(via);
}

// The MCU saves its mask: re-driving from it restores the lines and CA1.
TEST(test_redrive_from_saved_mask) {
    via_t *via = make_via2();
    slot_irq_redrive(via, 0x41); // SONIC (PA0) and DAFB (PA6) requesting
    ASSERT_EQ_INT(via_port_input(via, 0) & 0x7F, 0x3E);
    ASSERT_TRUE(ca1_flagged()); // CA1 went low from its idle-high power-on level
    ASSERT_EQ_INT(slot_irq_mask_from_via(via), 0x41);
    slot_irq_redrive(via, 0);
    ASSERT_EQ_INT(via_port_input(via, 0) & 0x7F, 0x7F);
    ASSERT_EQ_INT(slot_irq_mask_from_via(via), 0);
    via_delete(via);
}

int main(void) {
    RUN(test_aggregate_is_the_or_of_every_source);
    RUN(test_out_of_range_source_is_ignored);
    RUN(test_restore_derives_the_mask_from_via2);
    RUN(test_redrive_from_saved_mask);
    fprintf(stderr, "All /SLOTIRQ aggregate tests passed\n");
    return 0;
}
