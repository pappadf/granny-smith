// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// Grand Central unit test (proposal-powermac-7500-8500-9500 §5.3/§8).
//
// Links the real tnt/grand_central.c against recording stubs and pins
// the interrupt-fabric semantics the Phase B boot debugging established
// (docs/machines/tnt/tnt.md "The interrupt fabric"; the dossier's
// interrupt-map §5.1), now as directed sequences:
//
//  1. The MkLinux initialisation sequence (mask 0 / clear-all / mask 0)
//     and its events-driven acknowledge — clear MODE 0, where the CPU
//     line is combinational ((events | levels) & mask).
//  2. The NanoKernel's $80000000 ifMode1Clear acknowledge — clear MODE 1,
//     where the line is an output latch: set by enabled source edges (or
//     unmask-of-pending), cleared by the acknowledge, re-asserted only
//     by the NEXT edge; the acknowledge must clear no device bits.
//  3. The banked two-aperture NVRAM (bank port + $10-centre data window,
//     byte and stwbrx bank selects).
//  4. BoxID longword/byte composition (little-endian register).
//  5. The island's DBDMA routing: little-endian register access through
//     the +$8000 channel windows, and a channel-completion pulse landing
//     in Events as an edge.
//
// All 32-bit island calls carry BUS-domain (big-endian) values, exactly
// what the dispatcher receives from a guest stwbrx/lwbrx — the test
// swaps with TNT_LE32 like a guest would.

#include "dbdma.h"
#include "ppc.h"
#include "tnt.h"

#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// ============================================================================
// Recording stubs
// ============================================================================

// --- the one external-interrupt wire into the CPU core ---
static int s_line = -1; // -1 = never driven
static int s_line_edges; // number of 0->1 transitions
void ppc_set_ext_irq(ppc_t *p, bool level) {
    (void)p;
    if (level && s_line != 1)
        s_line_edges++;
    s_line = level ? 1 : 0;
}

// --- the AWACS block is not exercised here (it has its own datapath in
// machines/tnt/awacs.c); the island dispatch references it, so stub it ---
uint32_t tnt_awacs_read32(config_t *cfg, uint32_t offset) {
    (void)cfg;
    (void)offset;
    return 0;
}
void tnt_awacs_write32(config_t *cfg, uint32_t offset, uint32_t value) {
    (void)cfg;
    (void)offset;
    (void)value;
}

// --- the RaDACal block belongs to the Control video model (control.c);
// the island dispatch references it, so stub it the same way ---
uint8_t tnt_control_rad_read(config_t *cfg, uint32_t offset) {
    (void)cfg;
    (void)offset;
    return 0;
}
void tnt_control_rad_write(config_t *cfg, uint32_t offset, uint8_t value) {
    (void)cfg;
    (void)offset;
    (void)value;
}

// --- VIA/SCC apertures are not exercised here; the island dispatch
// references the accessors, so give them inert interfaces ---
static uint8_t stub_read8(void *d, uint32_t a) {
    (void)d;
    (void)a;
    return 0;
}
static void stub_write8(void *d, uint32_t a, uint8_t v) {
    (void)d;
    (void)a;
    (void)v;
}
static memory_interface_t s_stub_if = {.read_uint8 = stub_read8, .write_uint8 = stub_write8};
const memory_interface_t *via_get_memory_interface(via_t *via) {
    (void)via;
    return &s_stub_if;
}
const memory_interface_t *scc_get_memory_interface(scc_t *scc) {
    (void)scc;
    return &s_stub_if;
}

// ============================================================================
// Fixture: a config with the family state and a board descriptor
// ============================================================================

// BoxID: bit 15 strap, bit 14 MESH, bits 11/13 clear (a fixture value —
// the tests below exercise the register's byte-lane composition, not the
// model decode; the real per-model values live in the board descs).
#define TEST_BOXID (0x8000u | 0x4000u)

static tnt_board_desc_t s_board = {.boxid = TEST_BOXID, .bus_hz = 50000000u, .bandit_count = 1};
static hw_profile_t s_prof;
static tnt_state_t s_st;
static config_t s_cfg;

// DBDMA guest memory for the island-routing test.
static uint8_t s_mem[0x1000];
static void mem_read(void *ctx, uint32_t phys, uint8_t *buf, uint32_t len) {
    (void)ctx;
    ASSERT_TRUE(phys + len <= sizeof(s_mem));
    memcpy(buf, s_mem + phys, len);
}
static void mem_write(void *ctx, uint32_t phys, const uint8_t *buf, uint32_t len) {
    (void)ctx;
    ASSERT_TRUE(phys + len <= sizeof(s_mem));
    memcpy(s_mem + phys, buf, len);
}

// Channel completion -> Grand Central event pulse (the tnt.c wiring).
static void dbdma_irq(void *ctx, int chan) {
    tnt_gc_pulse_event((config_t *)ctx, chan);
}

static void fixture(void) {
    memset(&s_st, 0, sizeof(s_st));
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(&s_prof, 0, sizeof(s_prof));
    s_prof.board = &s_board;
    s_cfg.machine = &s_prof;
    s_cfg.machine_context = &s_st;
    if (s_st.dbdma)
        tnt_dbdma_delete(s_st.dbdma);
    s_st.dbdma = tnt_dbdma_init(NULL);
    tnt_dbdma_set_memory_hooks(s_st.dbdma, mem_read, mem_write, NULL);
    tnt_dbdma_set_irq_hook(s_st.dbdma, dbdma_irq, &s_cfg);
    memset(s_mem, 0, sizeof(s_mem));
    tnt_gc_init(&s_cfg);
    tnt_gc_recompute(&s_cfg);
    s_line = -1;
    s_line_edges = 0;
}

// Guest-eye register access: compose/recover little-endian values the
// way lwbrx/stwbrx would (the dispatcher takes bus-domain words).
static uint32_t reg_read(uint32_t off) {
    return TNT_LE32(tnt_gc_read32(&s_cfg, off));
}
static void reg_write(uint32_t off, uint32_t le_value) {
    tnt_gc_write32(&s_cfg, off, TNT_LE32(le_value));
}

#define R_EVENTS 0x20u
#define R_MASK   0x24u
#define R_CLEAR  0x28u
#define R_LEVELS 0x2Cu

// ============================================================================
// Tests
// ============================================================================

// The MkLinux bring-up and handler, verbatim: clear mode 0 stays in
// effect (no bit-31 write), the line is combinational, and the handler's
// events-driven acknowledge works.
TEST(test_mklinux_init_and_ack) {
    fixture();
    // gc_ints->mask = 0; clear = 0xffffffff; mask = 0;
    reg_write(R_MASK, 0);
    reg_write(R_CLEAR, 0x7FFFFFFFu); // all source bits (31 would flip modes)
    reg_write(R_MASK, 0);
    ASSERT_EQ_INT((int)reg_read(R_EVENTS), 0);
    ASSERT_EQ_INT(s_line, 0);
    // A masked source edge latches into Events but does not assert.
    tnt_gc_set_source(&s_cfg, TNT_INT_SCSI0, true);
    ASSERT_EQ_INT((int)(reg_read(R_EVENTS) >> TNT_INT_SCSI0) & 1, 1);
    ASSERT_EQ_INT((int)(reg_read(R_LEVELS) >> TNT_INT_SCSI0) & 1, 1);
    ASSERT_EQ_INT(s_line, 0);
    // Unmasking makes the combinational line follow.
    reg_write(R_MASK, 1u << TNT_INT_SCSI0);
    ASSERT_EQ_INT(s_line, 1);
    // The MkLinux handler: read events, write them back to clear.
    uint32_t ev = reg_read(R_EVENTS);
    reg_write(R_CLEAR, ev);
    ASSERT_EQ_INT((int)reg_read(R_EVENTS), 0);
    ASSERT_EQ_INT(s_line, 1); // level still asserted: line stays (mode 0)
    tnt_gc_set_source(&s_cfg, TNT_INT_SCSI0, false);
    ASSERT_EQ_INT(s_line, 0);
}

// Mode 0 with a pulse source: the edge latch alone holds the line until
// the explicit W1C.
TEST(test_mode0_pulse_w1c) {
    fixture();
    reg_write(R_MASK, 1u << 8); // DBDMA audio-out channel bit, say
    tnt_gc_pulse_event(&s_cfg, 8);
    ASSERT_EQ_INT((int)(reg_read(R_EVENTS) >> 8) & 1, 1);
    ASSERT_EQ_INT((int)(reg_read(R_LEVELS) >> 8) & 1, 0); // pulses leave Levels alone
    ASSERT_EQ_INT(s_line, 1);
    reg_write(R_CLEAR, 1u << 8);
    ASSERT_EQ_INT((int)reg_read(R_EVENTS), 0);
    ASSERT_EQ_INT(s_line, 0);
}

// The NanoKernel acknowledge: $80000000 selects mode 1, clears no device
// bits, and drops the latch; a standing level does NOT re-fire until its
// next edge — the exact semantics the 60.15 Hz tick chain needed.
TEST(test_mode1_latch) {
    fixture();
    reg_write(R_MASK, 1u << TNT_INT_VIA1);
    // First tick: edge asserts the line.
    tnt_gc_set_source(&s_cfg, TNT_INT_VIA1, true);
    ASSERT_EQ_INT(s_line, 1);
    ASSERT_EQ_INT(s_line_edges, 1);
    // The kernel's acknowledge.
    reg_write(R_CLEAR, 0x80000000u);
    ASSERT_EQ_INT(s_line, 0); // latch dropped...
    ASSERT_EQ_INT((int)(reg_read(R_EVENTS) >> TNT_INT_VIA1) & 1, 1); // ...events untouched
    ASSERT_EQ_INT((int)(reg_read(R_LEVELS) >> TNT_INT_VIA1) & 1, 1); // level still up
    // No re-fire while the level merely stands (a combinational model
    // storms here — the Phase B failure mode).
    tnt_gc_recompute(&s_cfg);
    ASSERT_EQ_INT(s_line, 0);
    // The handler serviced the VIA (level drops), the next tick edges.
    tnt_gc_set_source(&s_cfg, TNT_INT_VIA1, false);
    ASSERT_EQ_INT(s_line, 0);
    tnt_gc_set_source(&s_cfg, TNT_INT_VIA1, true);
    ASSERT_EQ_INT(s_line, 1);
    ASSERT_EQ_INT(s_line_edges, 2);
}

// Mode 1: enabling a source whose event/level is already pending counts
// as the latch-setting edge (the unmask-of-pending rule).
TEST(test_mode1_unmask_pending) {
    fixture();
    reg_write(R_CLEAR, 0x80000000u); // select mode 1
    ASSERT_EQ_INT(s_line, 0);
    tnt_gc_set_source(&s_cfg, TNT_INT_MESH, true); // masked: no latch
    reg_write(R_CLEAR, 0x80000000u); // ack anything stale
    ASSERT_EQ_INT(s_line, 0);
    reg_write(R_MASK, 1u << TNT_INT_MESH); // unmask-of-pending = edge
    ASSERT_EQ_INT(s_line, 1);
    reg_write(R_CLEAR, 0x80000000u);
    ASSERT_EQ_INT(s_line, 0);
}

// Banked NVRAM through both apertures, byte and stwbrx bank selects.
TEST(test_nvram_banking) {
    fixture();
    // Bank 3, byte j at data-window offset j*$10.
    tnt_gc_write8(&s_cfg, 0x1D000, 3);
    ASSERT_EQ_INT(tnt_gc_read8(&s_cfg, 0x1D000), 3);
    tnt_gc_write8(&s_cfg, 0x1F000 + 0 * 0x10, 0xAB);
    tnt_gc_write8(&s_cfg, 0x1F000 + 31 * 0x10, 0xCD);
    // Different bank: different bytes.
    tnt_gc_write8(&s_cfg, 0x1D000, 4);
    ASSERT_EQ_INT(tnt_gc_read8(&s_cfg, 0x1F000), 0);
    tnt_gc_write8(&s_cfg, 0x1F000, 0x11);
    // Back to bank 3 — the POST logging helper selects with stwbrx.
    reg_write(0x1D000, 3);
    ASSERT_EQ_INT(tnt_gc_read8(&s_cfg, 0x1F000), 0xAB);
    ASSERT_EQ_INT(tnt_gc_read8(&s_cfg, 0x1F000 + 31 * 0x10), 0xCD);
    // Off-centre data-window bytes are not NVRAM.
    ASSERT_EQ_INT(tnt_gc_read8(&s_cfg, 0x1F008), 0xFF);
}

// BoxID: the lwbrx longword and the byte lanes compose the same
// little-endian register.
TEST(test_boxid) {
    fixture();
    ASSERT_EQ_INT((int)reg_read(0x1A000), (int)TEST_BOXID);
    ASSERT_EQ_INT(tnt_gc_read8(&s_cfg, 0x1A000), (int)(TEST_BOXID & 0xFF));
    ASSERT_EQ_INT(tnt_gc_read8(&s_cfg, 0x1A001), (int)((TEST_BOXID >> 8) & 0xFF));
    ASSERT_EQ_INT(tnt_gc_read8(&s_cfg, 0x1A002), 0);
    ASSERT_EQ_INT(tnt_gc_read8(&s_cfg, 0x1A003), 0);
}

// The island routes the +$8000 channel windows to the engine with the
// little-endian conversion, and a completion pulse lands in Events.
TEST(test_dbdma_island_routing) {
    fixture();
    reg_write(R_MASK, 0); // observe through Events only
    // Build OUTPUT(0 bytes)+intr-always then STOP at guest $100; channel
    // 8 (audio out) through its island window at +$8800.
    // Descriptor 0 @ $100: NOP with INTR_ALWAYS; descriptor 1: STOP.
    uint32_t op_nop = (6u << 28) | (3u << 20);
    s_mem[0x100] = (uint8_t)op_nop; // LE bytes of the operation word
    s_mem[0x101] = (uint8_t)(op_nop >> 8);
    s_mem[0x102] = (uint8_t)(op_nop >> 16);
    s_mem[0x103] = (uint8_t)(op_nop >> 24);
    s_mem[0x113] = 0x70; // STOP: cmd nibble 7 in the top byte
    reg_write(0x8800 + TNT_DBDMA_REG_CMDPTRLO, 0x100);
    ASSERT_EQ_INT((int)reg_read(0x8800 + TNT_DBDMA_REG_CMDPTRLO), 0x100);
    reg_write(0x8800 + TNT_DBDMA_REG_CONTROL, (TNT_DBDMA_RUN << 16) | TNT_DBDMA_RUN);
    // The program ran: parked on the STOP, RUN up / ACTIVE down.
    uint32_t stat = reg_read(0x8800 + TNT_DBDMA_REG_STATUS);
    ASSERT_TRUE(stat & TNT_DBDMA_RUN);
    ASSERT_EQ_INT((int)(stat & TNT_DBDMA_ACTIVE), 0);
    ASSERT_EQ_INT((int)reg_read(0x8800 + TNT_DBDMA_REG_CMDPTRLO), 0x110);
    // The NOP's interrupt pulsed Grand Central event bit 8 (channel ==
    // interrupt number).
    ASSERT_EQ_INT((int)(reg_read(R_EVENTS) >> 8) & 1, 1);
    // The canonical reset through the island: RUN drops synchronously.
    reg_write(0x8800 + TNT_DBDMA_REG_CONTROL, 0xFC000000u);
    ASSERT_EQ_INT((int)(reg_read(0x8800 + TNT_DBDMA_REG_STATUS) & TNT_DBDMA_RUN), 0);
}

int main(void) {
    RUN(test_mklinux_init_and_ack);
    RUN(test_mode0_pulse_w1c);
    RUN(test_mode1_latch);
    RUN(test_mode1_unmask_pending);
    RUN(test_nvram_banking);
    RUN(test_boxid);
    RUN(test_dbdma_island_routing);
    return 0;
}
