// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// Unit test for the mac030 I/O dispatch engine + the GLUE and MDU+RBV family
// tables (proposal §6.1, "address-map tests per board" + "IRQ-routing tests
// per board").
//
// The dispatcher's decode and the IRQ→IPL routing are ordered tables walked by
// generic engines (mac030_glue_io.c).  Because they are data, the address map
// and the interrupt priority are directly checkable without booting a machine:
// assert every $50Fxxxxx window decodes to the expected device with the
// expected offset transform / penalty, the gaps decode to nothing, the mirror
// aliases, and each IRQ source raises the right CPU IPL with correct priority.
// Deterministic, no emulator/ROM/MMU required.

#include "mac030_glue.h" // mac030_irq_route_t, resolver, MAC030_GLUE_IRQ_*
#include "mac030_glue_io.h" // engine + GLUE table
#include "mdu_io.h" // MDU table
#include "test_assert.h"

#include "asc.h" // device typedefs for the linker stubs below
#include "builtin_rbv_video.h"
#include "floppy.h"
#include "rbv.h"
#include "scc.h"
#include "scsi.h"
#include "via.h"

#include <stdint.h>
#include <string.h>

#define GLUE_MIRROR MAC030_GLUE_IO_MIRROR
#define MDU_MIRROR  0x0003FFFFUL

// --- Stubs ----------------------------------------------------------------
// The bind() / byte-dispatch code in mac030_glue_io.c + mdu_io.c references
// these symbols, but this test only calls the pure decode / resolve functions,
// so trivial definitions satisfy the linker. (g_io_cpi_x256 == 0 makes
// memory_io_penalty a no-op anyway.)
uint32_t g_io_penalty_remainder = 0;
uint32_t g_io_phantom_instructions = 0;
uint32_t g_io_cpi_x256 = 0;
uint32_t *g_sprint_burndown_ptr = NULL;
uint64_t g_sprint_base_cycles = 0;
uint32_t g_sprint_frac_x256 = 0;
uint32_t g_sprint_total_slots = 0;
uint32_t g_esync_period_x256 = 0;

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
const memory_interface_t *rbv_get_memory_interface(rbv_t *r) {
    (void)r;
    return NULL;
}
uint8_t builtin_rbv_video_vdac_read(nubus_card_t *c, uint32_t off) {
    (void)c;
    (void)off;
    return 0;
}
void builtin_rbv_video_vdac_write(nubus_card_t *c, uint32_t off, uint8_t v) {
    (void)c;
    (void)off;
    (void)v;
}

// --- Address map ----------------------------------------------------------

// Assert that `offset` in `ranges` (under `mirror`) decodes to a window with
// the given device, name, penalty and offset transform.
static void expect_window(const mac030_io_range_t *ranges, uint32_t mirror, uint32_t offset, mac030_dev_t dev,
                          const char *name, uint16_t penalty, mac030_io_xform_t xform) {
    const mac030_io_range_t *r = mac030_io_decode(ranges, mirror, offset);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(r->device, dev);
    ASSERT_TRUE(strcmp(r->debug_name, name) == 0);
    ASSERT_EQ_INT(r->penalty, penalty);
    ASSERT_EQ_INT(r->xform, xform);
}

// The stride is only useful if it actually produces a register index.  Every
// one of the SWIM's sixteen registers must be reachable and distinct: baking
// the SE/30 decode into the chip instead is what collapsed all sixteen on to
// index 0 through the IIfx/Q900 IOP bypass (02-floppy F-03).
TEST(test_swim_window_decodes_register_index) {
    const mac030_io_range_t *g = mac030_glue_io_ranges();
    int seen[16] = {0};
    for (unsigned reg = 0; reg < 16; reg++) {
        uint32_t offset = 0x16000u + (reg << 9);
        const mac030_io_range_t *r = mac030_io_decode(g, GLUE_MIRROR, offset);
        ASSERT_TRUE(r != NULL);
        ASSERT_EQ_INT(r->device, MAC030_DEV_FLOPPY);
        ASSERT_EQ_INT((int)(((offset - r->base) >> 9) & 0x0Fu), (int)reg);
        seen[reg]++;
    }
    for (int i = 0; i < 16; i++)
        ASSERT_EQ_INT(seen[i], 1);
}

TEST(test_glue_addr_map) {
    const mac030_io_range_t *g = mac030_glue_io_ranges();
    // VIA windows: A0-masked, E-clock penalty 16.
    expect_window(g, GLUE_MIRROR, 0x00000, MAC030_DEV_VIA1, "via1", 16, MAC030_IO_MASK_A0);
    expect_window(g, GLUE_MIRROR, 0x01FFF, MAC030_DEV_VIA1, "via1", 16, MAC030_IO_MASK_A0);
    expect_window(g, GLUE_MIRROR, 0x02000, MAC030_DEV_VIA2, "via2", 16, MAC030_IO_MASK_A0);
    expect_window(g, GLUE_MIRROR, 0x03FFF, MAC030_DEV_VIA2, "via2", 16, MAC030_IO_MASK_A0);
    // SCC + SCSI register window + ASC + SWIM: normal offset, penalty 2.
    expect_window(g, GLUE_MIRROR, 0x04000, MAC030_DEV_SCC, "scc", 2, MAC030_IO_NORMAL);
    expect_window(g, GLUE_MIRROR, 0x10000, MAC030_DEV_SCSI, "scsi_reg", 2, MAC030_IO_NORMAL);
    expect_window(g, GLUE_MIRROR, 0x14000, MAC030_DEV_ASC, "asc", 2, MAC030_IO_NORMAL);
    // The SWIM window carries its own stride: the chip's A0-A3 are wired to
    // A9-A12, so the table decodes the register index rather than handing the
    // chip a bus offset (02-floppy F-03/F-44).
    expect_window(g, GLUE_MIRROR, 0x16000, MAC030_DEV_FLOPPY, "swim", 2, MAC030_IO_STRIDE_512);
    expect_window(g, GLUE_MIRROR, 0x17FFF, MAC030_DEV_FLOPPY, "swim", 2, MAC030_IO_STRIDE_512);
    // SCSI pseudo-DMA "blind" windows: fixed register (read 0 / write $201).
    expect_window(g, GLUE_MIRROR, 0x06000, MAC030_DEV_SCSI, "scsi_drq", 2, MAC030_IO_FIXED);
    expect_window(g, GLUE_MIRROR, 0x12000, MAC030_DEV_SCSI, "scsi_blind", 2, MAC030_IO_FIXED);

    // Blind windows pop FIFO at reg 0 on read, push at $201 on write.
    const mac030_io_range_t *drq = mac030_io_decode(g, GLUE_MIRROR, 0x06000);
    ASSERT_EQ_INT(drq->read_off, 0x000);
    ASSERT_EQ_INT(drq->write_off, 0x201);

    // Gaps: [$08000,$10000) and past $18000 are unmapped.
    ASSERT_TRUE(mac030_io_decode(g, GLUE_MIRROR, 0x08000) == NULL);
    ASSERT_TRUE(mac030_io_decode(g, GLUE_MIRROR, 0x0FFFF) == NULL);
    ASSERT_TRUE(mac030_io_decode(g, GLUE_MIRROR, 0x18000) == NULL);
    ASSERT_TRUE(mac030_io_decode(g, GLUE_MIRROR, 0x1FFFF) == NULL);

    // The island repeats every 128 KB; decode masks, so a mirror up aliases.
    expect_window(g, GLUE_MIRROR, 0x00000 + 0x20000, MAC030_DEV_VIA1, "via1", 16, MAC030_IO_MASK_A0);
    expect_window(g, GLUE_MIRROR, 0x06000 + 0x20000, MAC030_DEV_SCSI, "scsi_drq", 2, MAC030_IO_FIXED);
}

TEST(test_mdu_addr_map) {
    const mac030_io_range_t *m = mdu_io_ranges();
    // VIA1 (A0-masked, pen 16); NO VIA2 window (the RBV replaces it).
    expect_window(m, MDU_MIRROR, 0x00000, MAC030_DEV_VIA1, "via1", 16, MAC030_IO_MASK_A0);
    ASSERT_TRUE(mac030_io_decode(m, MDU_MIRROR, 0x02000) == NULL); // VIA2 hole
    ASSERT_TRUE(mac030_io_decode(m, MDU_MIRROR, 0x03FFF) == NULL);
    // SCC / SCSI {reg,drq,blind} / ASC / SWIM, same as GLUE.
    expect_window(m, MDU_MIRROR, 0x04000, MAC030_DEV_SCC, "scc", 2, MAC030_IO_NORMAL);
    expect_window(m, MDU_MIRROR, 0x06000, MAC030_DEV_SCSI, "scsi_drq", 2, MAC030_IO_FIXED);
    expect_window(m, MDU_MIRROR, 0x10000, MAC030_DEV_SCSI, "scsi_reg", 2, MAC030_IO_NORMAL);
    expect_window(m, MDU_MIRROR, 0x12000, MAC030_DEV_SCSI, "scsi_blind", 2, MAC030_IO_FIXED);
    expect_window(m, MDU_MIRROR, 0x14000, MAC030_DEV_ASC, "asc", 2, MAC030_IO_NORMAL);
    expect_window(m, MDU_MIRROR, 0x16000, MAC030_DEV_FLOPPY, "swim", 2, MAC030_IO_STRIDE_512);
    // The two MDU-only windows: VDAC ($24000) + RBV ($26000).
    expect_window(m, MDU_MIRROR, 0x24000, MAC030_DEV_VDAC, "vdac", 2, MAC030_IO_NORMAL);
    expect_window(m, MDU_MIRROR, 0x26000, MAC030_DEV_RBV, "rbv", 2, MAC030_IO_NORMAL);
    // Gaps unmapped; the 18-bit ($40000) mirror aliases.
    ASSERT_TRUE(mac030_io_decode(m, MDU_MIRROR, 0x08000) == NULL);
    ASSERT_TRUE(mac030_io_decode(m, MDU_MIRROR, 0x28000) == NULL);
    expect_window(m, MDU_MIRROR, 0x26000 + 0x40000, MAC030_DEV_RBV, "rbv", 2, MAC030_IO_NORMAL);
}

// --- Handler rows + decode-miss bookkeeping -------------------------------
// These two go through the real byte dispatcher rather than mac030_io_decode,
// because what they pin is what the ENGINE hands a handler row and what it
// records when nothing matches.

static uint32_t g_probe_win_off, g_probe_addr;
static int g_probe_reads, g_probe_writes;
static uint8_t g_probe_value;

static uint8_t probe_read(struct config *cfg, uint32_t win_off, uint32_t addr) {
    (void)cfg;
    g_probe_win_off = win_off;
    g_probe_addr = addr;
    g_probe_reads++;
    return 0x5A;
}
static void probe_write(struct config *cfg, uint32_t win_off, uint32_t addr, uint8_t value) {
    (void)cfg;
    g_probe_win_off = win_off;
    g_probe_addr = addr;
    g_probe_value = value;
    g_probe_writes++;
}

// A deliberately narrow island: $1FFFF, half the AV family's $3FFFF.  A
// handler that re-derives its own offset has to name SOME mask, and any
// board whose mask differs from the one it named is then decoded twice,
// differently.  That is exactly what psc.c and new_age.c were doing with a
// hardcoded `(addr & 0x3FFFFu) - <base>` (05-chipsets-irq F-22).
#define PROBE_MIRROR 0x0001FFFFu
#define PROBE_BASE   0x00001000u
static const mac030_io_range_t k_probe_ranges[] = {
    {.base = PROBE_BASE, .end = 0x00002000u, .read_fn = probe_read, .write_fn = probe_write, .debug_name = "probe"},
    {0},
};

TEST(test_handler_row_gets_engine_decoded_sub_offset) {
    mac030_io_t io = {.ranges = k_probe_ranges, .mirror_mask = PROBE_MIRROR, .unmapped_read = 0xFF};

    // Inside the first copy of the island: re-derivation and the engine agree.
    ASSERT_EQ_INT(mac030_io_read_uint8(&io, 0x50F01004u), 0x5A);
    ASSERT_EQ_INT(g_probe_win_off, 0x004u);
    ASSERT_EQ_INT(g_probe_addr, 0x50F01004u); // raw address, for fault reporting

    // Now the same register through the $20000 mirror, which this board folds
    // away but a hardcoded $3FFFF mask would not: the engine still says $004,
    // where `(addr & 0x3FFFF) - $1000` would say $20004 and fall off the end
    // of every register switch.
    mac030_io_write_uint8(&io, 0x50F21004u, 0xC3);
    ASSERT_EQ_INT(g_probe_win_off, 0x004u);
    ASSERT_EQ_INT(g_probe_addr, 0x50F21004u);
    ASSERT_EQ_INT(g_probe_value, 0xC3);
    ASSERT_EQ_INT((0x50F21004u & 0x0003FFFFu) - PROBE_BASE, 0x20004u); // what the old code computed
    ASSERT_EQ_INT(g_probe_reads, 1);
    ASSERT_EQ_INT(g_probe_writes, 1);
}

// The five families on this engine used to return `unmapped_read` in silence.
// Each first touch of an unwired 4K now logs once; the bitmaps are the
// observable half of that (the log call itself is a no-op in this harness).
TEST(test_decode_miss_is_recorded_once_per_4k) {
    mac030_io_t io = {.ranges = k_probe_ranges, .mirror_mask = PROBE_MIRROR, .unmapped_read = 0xFF};

    ASSERT_EQ_INT(mac030_io_read_uint8(&io, 0x50F00000u), 0xFF); // unwired: below the window
    ASSERT_EQ_INT((uint32_t)io.miss_logged_read, 0x1u); // 4K #0
    ASSERT_EQ_INT((uint32_t)io.miss_logged_write, 0x0u); // reads and writes count apart

    mac030_io_read_uint8(&io, 0x50F00FFFu); // same 4K: no second bit
    ASSERT_EQ_INT((uint32_t)io.miss_logged_read, 0x1u);

    mac030_io_read_uint8(&io, 0x50F03000u); // 4K #3
    ASSERT_EQ_INT((uint32_t)io.miss_logged_read, 0x9u);

    mac030_io_write_uint8(&io, 0x50F03000u, 0x11); // same 4K, other direction
    ASSERT_EQ_INT((uint32_t)io.miss_logged_write, 0x8u);

    // A hit inside the handler window records nothing.
    mac030_io_read_uint8(&io, 0x50F01004u);
    ASSERT_EQ_INT((uint32_t)io.miss_logged_read, 0x9u);

    // Per instance, not per process: a second board starts clean.
    mac030_io_t io2 = {.ranges = k_probe_ranges, .mirror_mask = PROBE_MIRROR, .unmapped_read = 0xFF};
    mac030_io_read_uint8(&io2, 0x50F00000u);
    ASSERT_EQ_INT((uint32_t)io2.miss_logged_read, 0x1u);
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
    RUN(test_glue_addr_map);
    RUN(test_swim_window_decodes_register_index);
    RUN(test_mdu_addr_map);
    RUN(test_handler_row_gets_engine_decoded_sub_offset);
    RUN(test_decode_miss_is_recorded_once_per_4k);
    RUN(test_irq_single_sources);
    RUN(test_irq_priority);
    printf("[PASS] All mac030_io dispatch-table tests passed\n");
    return 0;
}
