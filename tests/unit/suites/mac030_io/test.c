// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// Unit test for the GLUE family's data-driven dispatch tables (proposal §6.1,
// "address-map tests per board" + "IRQ-routing tests per board").
//
// The dispatcher's decode and the IRQ→IPL routing are now ordered tables
// walked by generic engines (mac030_glue_io.c).  Because they are data, the
// address map and the interrupt priority are directly checkable without
// booting a machine: assert every $50Fxxxxx window decodes to the expected
// device with the expected offset transform / penalty, the gaps decode to
// nothing, the 128 KB mirror aliases, and each IRQ source raises the right CPU
// IPL with correct priority. Deterministic, no emulator/ROM/MMU required.

#include "mac030_glue.h" // mac030_irq_route_t, resolver, MAC030_GLUE_IRQ_*
#include "mac030_glue_io.h" // decode/ranges, MAC030_DEV_*, xform, MIRROR
#include "test_assert.h"

#include "asc.h" // device typedefs for the linker stubs below
#include "floppy.h"
#include "scc.h"
#include "scsi.h"
#include "via.h"

#include <stdint.h>
#include <string.h>

// --- Stubs ----------------------------------------------------------------
// mac030_glue_io.c's bind() / byte dispatch reference these symbols, but this
// test only calls the pure decode / resolve functions, so trivial definitions
// satisfy the linker. (g_io_cpi == 0 makes memory_io_penalty a no-op anyway.)
uint32_t g_io_penalty_remainder = 0;
uint32_t g_io_phantom_instructions = 0;
uint32_t g_io_cpi = 0;
uint32_t *g_sprint_burndown_ptr = NULL;

const memory_interface_t *via_get_memory_interface(via_t *v) {
    (void)v;
    return NULL;
}
const memory_interface_t *scc_get_memory_interface(scc_t *s) {
    (void)s;
    return NULL;
}
const memory_interface_t *scsi_get_memory_interface(scsi_t *s) {
    (void)s;
    return NULL;
}
const memory_interface_t *asc_get_memory_interface(asc_t *a) {
    (void)a;
    return NULL;
}
const memory_interface_t *floppy_get_memory_interface(floppy_t *f) {
    (void)f;
    return NULL;
}

// --- Address map ----------------------------------------------------------

// Assert that `offset` decodes to a window with the given device, name,
// penalty and offset transform.
static void expect_window(uint32_t offset, mac030_dev_t dev, const char *name, uint16_t penalty,
                          mac030_io_xform_t xform) {
    const mac030_io_range_t *r = mac030_glue_io_decode(offset);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(r->device, dev);
    ASSERT_TRUE(strcmp(r->debug_name, name) == 0);
    ASSERT_EQ_INT(r->penalty, penalty);
    ASSERT_EQ_INT(r->xform, xform);
}

TEST(test_addr_map_windows) {
    // VIA windows: A0-masked, E-clock penalty 16.
    expect_window(0x00000, MAC030_DEV_VIA1, "via1", 16, MAC030_IO_MASK_A0);
    expect_window(0x01FFF, MAC030_DEV_VIA1, "via1", 16, MAC030_IO_MASK_A0); // last byte
    expect_window(0x02000, MAC030_DEV_VIA2, "via2", 16, MAC030_IO_MASK_A0); // boundary
    expect_window(0x03FFF, MAC030_DEV_VIA2, "via2", 16, MAC030_IO_MASK_A0);

    // SCC + SCSI register window + ASC + SWIM: normal offset, penalty 2.
    expect_window(0x04000, MAC030_DEV_SCC, "scc", 2, MAC030_IO_NORMAL);
    expect_window(0x10000, MAC030_DEV_SCSI, "scsi_reg", 2, MAC030_IO_NORMAL);
    expect_window(0x14000, MAC030_DEV_ASC, "asc", 2, MAC030_IO_NORMAL);
    expect_window(0x16000, MAC030_DEV_FLOPPY, "swim", 2, MAC030_IO_NORMAL);
    expect_window(0x17FFF, MAC030_DEV_FLOPPY, "swim", 2, MAC030_IO_NORMAL); // last live byte

    // SCSI pseudo-DMA "blind" windows: fixed register (read 0 / write $201).
    expect_window(0x06000, MAC030_DEV_SCSI, "scsi_drq", 2, MAC030_IO_FIXED);
    expect_window(0x12000, MAC030_DEV_SCSI, "scsi_blind", 2, MAC030_IO_FIXED);
}

TEST(test_addr_map_fixed_offsets) {
    // The blind windows pop FIFO at reg 0 on read, push at $201 on write.
    const mac030_io_range_t *drq = mac030_glue_io_decode(0x06000);
    ASSERT_EQ_INT(drq->read_off, 0x000);
    ASSERT_EQ_INT(drq->write_off, 0x201);
    const mac030_io_range_t *blind = mac030_glue_io_decode(0x12000);
    ASSERT_EQ_INT(blind->read_off, 0x000);
    ASSERT_EQ_INT(blind->write_off, 0x201);
}

TEST(test_addr_map_gaps) {
    // [$08000,$10000) between SCSI DRQ and SCSI REG is unmapped.
    ASSERT_TRUE(mac030_glue_io_decode(0x08000) == NULL);
    ASSERT_TRUE(mac030_glue_io_decode(0x0C000) == NULL);
    ASSERT_TRUE(mac030_glue_io_decode(0x0FFFF) == NULL);
    // Past the last window ($18000..) is unmapped (within the mirror).
    ASSERT_TRUE(mac030_glue_io_decode(0x18000) == NULL);
    ASSERT_TRUE(mac030_glue_io_decode(0x1FFFF) == NULL);
}

TEST(test_addr_map_mirror) {
    // The island repeats every 128 KB; decode masks to the low 17 bits, so an
    // address one mirror up decodes to the same window as its base.
    const uint32_t MIRROR = MAC030_GLUE_IO_MIRROR + 1; // 0x20000
    expect_window(0x00000 + MIRROR, MAC030_DEV_VIA1, "via1", 16, MAC030_IO_MASK_A0);
    expect_window(0x06000 + MIRROR, MAC030_DEV_SCSI, "scsi_drq", 2, MAC030_IO_FIXED);
    expect_window(0x16000 + 7 * MIRROR, MAC030_DEV_FLOPPY, "swim", 2, MAC030_IO_NORMAL);
}

// --- IRQ routing ----------------------------------------------------------

TEST(test_irq_single_sources) {
    const mac030_irq_route_t *routes = mac030_glue_irq_routes();
    ASSERT_TRUE(routes != NULL);
    ASSERT_EQ_INT(mac030_irq_resolve_ipl(routes, 0), 0); // none active
    ASSERT_EQ_INT(mac030_irq_resolve_ipl(routes, MAC030_GLUE_IRQ_VIA1), 1);
    ASSERT_EQ_INT(mac030_irq_resolve_ipl(routes, MAC030_GLUE_IRQ_VIA2), 2);
    ASSERT_EQ_INT(mac030_irq_resolve_ipl(routes, MAC030_GLUE_IRQ_SCC), 4);
    ASSERT_EQ_INT(mac030_irq_resolve_ipl(routes, MAC030_GLUE_IRQ_NMI), 7);
}

TEST(test_irq_priority) {
    const mac030_irq_route_t *routes = mac030_glue_irq_routes();
    // Highest-priority active source wins.
    ASSERT_EQ_INT(mac030_irq_resolve_ipl(routes, MAC030_GLUE_IRQ_VIA1 | MAC030_GLUE_IRQ_VIA2), 2);
    ASSERT_EQ_INT(mac030_irq_resolve_ipl(routes, MAC030_GLUE_IRQ_VIA1 | MAC030_GLUE_IRQ_SCC), 4);
    ASSERT_EQ_INT(mac030_irq_resolve_ipl(routes, MAC030_GLUE_IRQ_VIA2 | MAC030_GLUE_IRQ_SCC), 4);
    ASSERT_EQ_INT(mac030_irq_resolve_ipl(routes, MAC030_GLUE_IRQ_NMI | MAC030_GLUE_IRQ_VIA1), 7);
    ASSERT_EQ_INT(mac030_irq_resolve_ipl(routes, MAC030_GLUE_IRQ_VIA1 | MAC030_GLUE_IRQ_VIA2 | MAC030_GLUE_IRQ_SCC |
                                                     MAC030_GLUE_IRQ_NMI),
                  7);
}

int main(void) {
    RUN(test_addr_map_windows);
    RUN(test_addr_map_fixed_offsets);
    RUN(test_addr_map_gaps);
    RUN(test_addr_map_mirror);
    RUN(test_irq_single_sources);
    RUN(test_irq_priority);
    printf("[PASS] All mac030_io dispatch-table tests passed\n");
    return 0;
}
