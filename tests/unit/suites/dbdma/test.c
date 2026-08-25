// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// DBDMA engine unit test (proposal-powermac-7500-8500-9500 §5.4).
//
// Drives the real tnt/dbdma.c against a flat guest-memory array and a
// scripted device port.  Register values on this API are already in the
// little-endian register domain (the island dispatcher owns the bus-edge
// swap), so the test writes the same constants a driver would compose
// with stwbrx.  Sequences pinned here:
//
//  1. Control mask/value writes and the synchronous status transitions
//     behind the canonical Linux/OSF poll loops (stop, reset, start).
//  2. A full OUTPUT program: data delivered, resCount/xferStatus written
//     back, the channel interrupt raised AFTER the write-back, cmdptr
//     parked on the STOP descriptor.
//  3. Device stalls mid-command and tnt_dbdma_kick resumption (INPUT).
//  4. The NOP+branch ring idiom and the runaway-budget guard.
//  5. The STOP/WAKE descriptor-overwrite idiom (commit rule: descriptors
//     are refetched, never cached).
//  6. STORE_QUAD / LOAD_QUAD, conditional branch/wait/interrupt against
//     the select registers and the device's s-bits, host s-bit latches,
//     cmdptr write protection while running, partial-residual write-back
//     on stop.

#include "dbdma.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// ============================================================================
// Recording checkpoint stream (a real byte round-trip, unlike the no-op
// stub) — the engine only calls the two data movers.
// ============================================================================

static uint8_t s_cp_buf[4096];
static size_t s_cp_w, s_cp_r;

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *file, int line) {
    (void)cp;
    (void)file;
    (void)line;
    memcpy(s_cp_buf + s_cp_w, data, size);
    s_cp_w += size;
}

void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *file, int line) {
    (void)cp;
    (void)file;
    (void)line;
    memcpy(data, s_cp_buf + s_cp_r, size);
    s_cp_r += size;
}

// ============================================================================
// Guest memory + hooks
// ============================================================================

#define MEM_SIZE 0x10000
static uint8_t s_mem[MEM_SIZE];

static void mem_read(void *ctx, uint32_t phys, uint8_t *buf, uint32_t len) {
    (void)ctx;
    ASSERT_TRUE(phys + len <= MEM_SIZE);
    memcpy(buf, s_mem + phys, len);
}

static void mem_write(void *ctx, uint32_t phys, const uint8_t *buf, uint32_t len) {
    (void)ctx;
    ASSERT_TRUE(phys + len <= MEM_SIZE);
    memcpy(s_mem + phys, buf, len);
}

// Little-endian guest-memory accessors for building/reading descriptors.
static void poke32(uint32_t addr, uint32_t v) {
    s_mem[addr] = (uint8_t)v;
    s_mem[addr + 1] = (uint8_t)(v >> 8);
    s_mem[addr + 2] = (uint8_t)(v >> 16);
    s_mem[addr + 3] = (uint8_t)(v >> 24);
}

static uint32_t peek32(uint32_t addr) {
    return (uint32_t)s_mem[addr] | ((uint32_t)s_mem[addr + 1] << 8) | ((uint32_t)s_mem[addr + 2] << 16) |
           ((uint32_t)s_mem[addr + 3] << 24);
}

// Build one descriptor at `addr` in the driver's commit order (result,
// data32, address first; operation LAST — Apple's MakeCCDescriptor rule).
static void desc(uint32_t addr, uint32_t op, uint32_t data_addr, uint32_t cmd_dep) {
    poke32(addr + 12, 0);
    poke32(addr + 8, cmd_dep);
    poke32(addr + 4, data_addr);
    poke32(addr, op);
}

// Operation-word builder: cmd nibble 31:28, key 26:24, i 21:20, b 19:18,
// w 17:16, reqCount 15:0 (Linux struct dbdma_cmd bit positions).
static uint32_t op(uint32_t cmd, uint32_t i, uint32_t b, uint32_t w, uint32_t req) {
    return (cmd << 28) | (i << 20) | (b << 18) | (w << 16) | (req & 0xFFFFu);
}

#define OUTPUT_MORE 0u
#define OUTPUT_LAST 1u
#define INPUT_MORE  2u
#define INPUT_LAST  3u
#define STORE_QUAD  4u
#define LOAD_QUAD   5u
#define NOP_CMD     6u
#define STOP_CMD    7u

#define NEVER  0u
#define IFSET  1u
#define IFCLR  2u
#define ALWAYS 3u

// ============================================================================
// Scripted device port
// ============================================================================

static uint8_t s_dev_data[1024]; // bytes the device produced/consumed
static int s_dev_len; // bytes recorded (out) / staged (in)
static int s_dev_pos; // in-side read cursor
static int s_dev_budget; // max bytes the port moves per call batch (-1 = all)
static uint8_t s_dev_sbits; // live device status bits

static int dev_out(void *ctx, const uint8_t *buf, int len) {
    (void)ctx;
    int n = (s_dev_budget < 0 || len <= s_dev_budget) ? len : s_dev_budget;
    if (s_dev_budget > 0)
        s_dev_budget -= n;
    memcpy(s_dev_data + s_dev_len, buf, (size_t)n);
    s_dev_len += n;
    return n;
}

static int dev_in(void *ctx, uint8_t *buf, int len) {
    (void)ctx;
    int avail = s_dev_len - s_dev_pos;
    int n = len < avail ? len : avail;
    if (s_dev_budget >= 0 && n > s_dev_budget)
        n = s_dev_budget;
    if (s_dev_budget > 0)
        s_dev_budget -= n;
    memcpy(buf, s_dev_data + s_dev_pos, (size_t)n);
    s_dev_pos += n;
    return n;
}

static uint8_t dev_s_bits(void *ctx) {
    (void)ctx;
    return s_dev_sbits;
}

static void dev_reset(void) {
    memset(s_dev_data, 0, sizeof(s_dev_data));
    s_dev_len = 0;
    s_dev_pos = 0;
    s_dev_budget = -1;
    s_dev_sbits = 0;
}

// ============================================================================
// Interrupt recorder — snapshots the result field AT interrupt time so
// the write-back-before-interrupt order is directly asserted.
// ============================================================================

static int s_irq_count;
static int s_irq_chan;
static uint32_t s_irq_watch_addr; // descriptor whose result we snapshot
static uint32_t s_irq_result; // result field content at irq delivery

static void irq_hook(void *ctx, int chan) {
    (void)ctx;
    s_irq_count++;
    s_irq_chan = chan;
    if (s_irq_watch_addr)
        s_irq_result = peek32(s_irq_watch_addr + 12);
}

static void irq_reset(void) {
    s_irq_count = 0;
    s_irq_chan = -1;
    s_irq_watch_addr = 0;
    s_irq_result = 0;
}

// ============================================================================
// Fixture
// ============================================================================

static tnt_dbdma_t *s_d;

// Fresh engine with hooks and a port on channel 0 (and none on 1).
static void fixture(void) {
    static tnt_dbdma_port_t port;
    if (s_d)
        tnt_dbdma_delete(s_d);
    s_d = tnt_dbdma_init(NULL);
    tnt_dbdma_set_memory_hooks(s_d, mem_read, mem_write, NULL);
    tnt_dbdma_set_irq_hook(s_d, irq_hook, NULL);
    port.out = dev_out;
    port.in = dev_in;
    port.s_bits = dev_s_bits;
    port.ctx = NULL;
    tnt_dbdma_set_port(s_d, 0, &port);
    memset(s_mem, 0, sizeof(s_mem));
    dev_reset();
    irq_reset();
}

static uint32_t status(int chan) {
    return tnt_dbdma_reg_read(s_d, chan, TNT_DBDMA_REG_STATUS);
}

static void control(int chan, uint32_t v) {
    tnt_dbdma_reg_write(s_d, chan, TNT_DBDMA_REG_CONTROL, v);
}

static void start(int chan, uint32_t cmdptr) {
    tnt_dbdma_reg_write(s_d, chan, TNT_DBDMA_REG_CMDPTRLO, cmdptr);
    control(chan, (TNT_DBDMA_RUN << 16) | TNT_DBDMA_RUN); // the canonical start
}

// ============================================================================
// Tests
// ============================================================================

// Mask/value convention: only masked bits change; value bits without a
// mask bit are ignored; host s-bits latch and clear.
TEST(test_control_mask_value) {
    fixture();
    control(0, 0x00010001); // set s0
    ASSERT_EQ_INT((int)(status(0) & 0xFF), 0x01);
    control(0, 0x00020002); // set s1 — s0 must survive (unmasked)
    ASSERT_EQ_INT((int)(status(0) & 0xFF), 0x03);
    control(0, 0x0001FFFE); // mask s0, value bit clear -> s0 drops, s1 stays
    ASSERT_EQ_INT((int)(status(0) & 0xFF), 0x02);
    control(0, 0x0000FFFF); // empty mask: no change
    ASSERT_EQ_INT((int)(status(0) & 0xFF), 0x02);
    // Engine-owned bits cannot be SET from the host side.
    control(0, (TNT_DBDMA_ACTIVE << 16) | TNT_DBDMA_ACTIVE);
    ASSERT_EQ_INT((int)(status(0) & TNT_DBDMA_ACTIVE), 0);
}

// The canonical start: RUN set -> ACTIVE synchronously; program runs to
// its STOP; both canonical poll conditions read terminal immediately.
TEST(test_output_program) {
    fixture();
    for (int i = 0; i < 8; i++)
        s_mem[0x2000 + i] = (uint8_t)(0xA0 + i);
    desc(0x1000, op(OUTPUT_LAST, ALWAYS, NEVER, NEVER, 8), 0x2000, 0);
    desc(0x1010, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    s_irq_watch_addr = 0x1000;
    start(0, 0x1000);
    // Transfer happened, in order.
    ASSERT_EQ_INT(s_dev_len, 8);
    ASSERT_EQ_INT(memcmp(s_dev_data, s_mem + 0x2000, 8), 0);
    // STOP parked the channel ON the STOP descriptor, ACTIVE dropped.
    ASSERT_EQ_INT((int)(status(0) & TNT_DBDMA_ACTIVE), 0);
    ASSERT_EQ_INT((int)tnt_dbdma_reg_read(s_d, 0, TNT_DBDMA_REG_CMDPTRLO), 0x1010);
    // One interrupt, and the result field ALREADY held the write-back
    // when it fired: resCount 0, xferStatus with ACTIVE set.
    ASSERT_EQ_INT(s_irq_count, 1);
    ASSERT_EQ_INT(s_irq_chan, 0);
    ASSERT_EQ_INT((int)(s_irq_result & 0xFFFF), 0);
    ASSERT_TRUE(s_irq_result & ((uint32_t)TNT_DBDMA_ACTIVE << 16));
}

// The canonical stop and reset sequences terminate synchronously with a
// partial transfer's residual made guest-visible.
TEST(test_stop_reset_sequences) {
    fixture();
    desc(0x1000, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 8), 0x2000, 0);
    desc(0x1010, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    // Device has only 3 bytes -> the command stalls mid-transfer.
    s_dev_data[0] = 0x11;
    s_dev_data[1] = 0x22;
    s_dev_data[2] = 0x33;
    s_dev_len = 3;
    start(0, 0x1000);
    ASSERT_TRUE(status(0) & TNT_DBDMA_ACTIVE);
    ASSERT_EQ_INT(s_irq_count, 0);
    ASSERT_EQ_INT(s_mem[0x2000], 0x11);
    ASSERT_EQ_INT(s_mem[0x2002], 0x33);
    // Canonical stop: clear RUN|FLUSH; poll condition (ACTIVE|FLUSH)==0
    // must hold on the very next read, and the residual is written back.
    control(0, (uint32_t)(TNT_DBDMA_RUN | TNT_DBDMA_FLUSH) << 16);
    ASSERT_EQ_INT((int)(status(0) & (TNT_DBDMA_ACTIVE | TNT_DBDMA_FLUSH)), 0);
    ASSERT_EQ_INT((int)(peek32(0x100C) & 0xFFFF), 5); // 8 requested - 3 moved
    // Canonical reset: clear everything; poll condition (RUN)==0.
    control(0, (uint32_t)(TNT_DBDMA_ACTIVE | TNT_DBDMA_DEAD | TNT_DBDMA_WAKE | TNT_DBDMA_FLUSH | TNT_DBDMA_PAUSE |
                          TNT_DBDMA_RUN)
                   << 16);
    ASSERT_EQ_INT((int)(status(0) & TNT_DBDMA_RUN), 0);
}

// A stalled INPUT resumes on the device's kick and completes.
TEST(test_stall_and_kick) {
    fixture();
    desc(0x1000, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 6), 0x2000, 0);
    desc(0x1010, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    s_dev_len = 2; // two bytes now, four later
    s_dev_data[0] = 0x51;
    s_dev_data[1] = 0x52;
    start(0, 0x1000);
    ASSERT_TRUE(tnt_dbdma_active(s_d, 0));
    ASSERT_EQ_INT(s_irq_count, 0);
    // Device produces the rest and kicks.
    memcpy(s_dev_data + 2, "\x53\x54\x55\x56", 4);
    s_dev_len = 6;
    s_irq_watch_addr = 0x1000;
    tnt_dbdma_kick(s_d, 0);
    ASSERT_EQ_INT(s_irq_count, 1);
    ASSERT_EQ_INT((int)(s_irq_result & 0xFFFF), 0); // full transfer: residual 0
    ASSERT_EQ_INT(memcmp(s_mem + 0x2000, "\x51\x52\x53\x54\x55\x56", 6), 0);
    ASSERT_EQ_INT((int)(status(0) & TNT_DBDMA_ACTIVE), 0); // parked on STOP
}

// A channel with no device port stalls its data command honestly.
TEST(test_no_port_stalls) {
    fixture();
    desc(0x1000, op(OUTPUT_LAST, ALWAYS, NEVER, NEVER, 4), 0x2000, 0);
    start(1, 0x1000); // channel 1 has no port
    ASSERT_TRUE(status(1) & TNT_DBDMA_ACTIVE);
    ASSERT_EQ_INT(s_irq_count, 0);
}

// NOP+BR_ALWAYS is the ring jump; a ring with a data command drains the
// device each lap, and a ring of pure NOPs trips the runaway guard
// without hanging the host.
TEST(test_nop_branch_ring) {
    fixture();
    // Ring: OUTPUT 4 @ $2000 -> NOP jump back to head.
    desc(0x1000, op(OUTPUT_MORE, NEVER, NEVER, NEVER, 4), 0x2000, 0);
    desc(0x1010, op(NOP_CMD, NEVER, ALWAYS, NEVER, 0), 0, 0x1000);
    poke32(0x2000, 0x64636261);
    s_dev_budget = 12; // three laps' worth, then the device stalls
    start(0, 0x1000);
    ASSERT_EQ_INT(s_dev_len, 12); // three laps drained
    ASSERT_TRUE(status(0) & TNT_DBDMA_ACTIVE); // stalled mid-lap 4
    ASSERT_TRUE(status(0) & TNT_DBDMA_BT); // last branch was taken
    // Pure-NOP ring on channel 1: the budget guard must return control.
    desc(0x3000, op(NOP_CMD, NEVER, ALWAYS, NEVER, 0), 0, 0x3000);
    start(1, 0x3000);
    ASSERT_TRUE(status(1) & TNT_DBDMA_ACTIVE); // parked, not dead, not hung
}

// The STOP/WAKE idiom: overwrite the parked-on STOP descriptor (commit
// order: operation last), then WAKE — the engine must refetch, not
// replay a cached copy.
TEST(test_stop_wake_overwrite) {
    fixture();
    poke32(0x2000, 0x71717171);
    desc(0x1000, op(OUTPUT_LAST, NEVER, NEVER, NEVER, 4), 0x2000, 0);
    desc(0x1010, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    start(0, 0x1000);
    ASSERT_EQ_INT(s_dev_len, 4);
    ASSERT_EQ_INT((int)(status(0) & TNT_DBDMA_ACTIVE), 0);
    ASSERT_TRUE(status(0) & TNT_DBDMA_RUN); // still enabled, just parked
    // Driver appends: the old STOP becomes OUTPUT_LAST(2), a new STOP
    // follows; then the WAKE control write restarts the channel.
    poke32(0x2100, 0x0000A2A1);
    desc(0x1020, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    desc(0x1010, op(OUTPUT_LAST, NEVER, NEVER, NEVER, 2), 0x2100, 0);
    control(0, (TNT_DBDMA_WAKE << 16) | TNT_DBDMA_WAKE);
    ASSERT_EQ_INT(s_dev_len, 6);
    ASSERT_EQ_INT(s_dev_data[4], 0xA1);
    ASSERT_EQ_INT(s_dev_data[5], 0xA2);
    ASSERT_EQ_INT((int)tnt_dbdma_reg_read(s_d, 0, TNT_DBDMA_REG_CMDPTRLO), 0x1020);
}

// STORE_QUAD writes its little-endian quad to memory; LOAD_QUAD reads
// one back into the descriptor's cmdDep field.
TEST(test_quads) {
    fixture();
    desc(0x1000, op(STORE_QUAD, NEVER, NEVER, NEVER, 4), 0x2000, 0xDEADBEEF);
    desc(0x1010, op(LOAD_QUAD, NEVER, NEVER, NEVER, 4), 0x2004, 0);
    desc(0x1020, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    poke32(0x2004, 0xCAFEF00D);
    start(0, 0x1000);
    ASSERT_EQ_INT((int)peek32(0x2000), (int)0xDEADBEEF);
    ASSERT_EQ_INT((int)peek32(0x1018), (int)0xCAFEF00D); // cmdDep write-back
    ASSERT_EQ_INT((int)tnt_dbdma_reg_read(s_d, 0, TNT_DBDMA_REG_CMDPTRLO), 0x1020);
}

// Conditional branch/wait/interrupt against the select registers and the
// device's live s-bits (mask 23:16 / value 7:0; cond is masked equality).
TEST(test_conditions) {
    fixture();
    // Branch: br_sel selects s0==1.  With s0 clear, BR_IFSET falls
    // through; with s0 set, it branches (and sets BT).
    tnt_dbdma_reg_write(s_d, 0, TNT_DBDMA_REG_BRSEL, 0x00010001);
    desc(0x1000, op(NOP_CMD, NEVER, IFSET, NEVER, 0), 0, 0x1030);
    desc(0x1010, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    desc(0x1030, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    start(0, 0x1000);
    ASSERT_EQ_INT((int)tnt_dbdma_reg_read(s_d, 0, TNT_DBDMA_REG_CMDPTRLO), 0x1010); // fell through
    ASSERT_EQ_INT((int)(status(0) & TNT_DBDMA_BT), 0);
    control(0, (uint32_t)TNT_DBDMA_RUN << 16); // rundown
    s_dev_sbits = 0x01;
    start(0, 0x1000);
    ASSERT_EQ_INT((int)tnt_dbdma_reg_read(s_d, 0, TNT_DBDMA_REG_CMDPTRLO), 0x1030); // branched
    ASSERT_TRUE(status(0) & TNT_DBDMA_BT);
    control(0, (uint32_t)TNT_DBDMA_RUN << 16);
    // Wait: WAIT_IFSET on s1 parks the command until the bit drops.
    tnt_dbdma_reg_write(s_d, 0, TNT_DBDMA_REG_WAITSEL, 0x00020002);
    s_dev_sbits = 0x02;
    desc(0x4000, op(STORE_QUAD, NEVER, NEVER, IFSET, 4), 0x2000, 0x1234);
    desc(0x4010, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    poke32(0x2000, 0);
    start(0, 0x4000);
    ASSERT_TRUE(status(0) & TNT_DBDMA_ACTIVE); // waiting, not executed
    ASSERT_EQ_INT((int)peek32(0x2000), 0);
    s_dev_sbits = 0; // condition clears; the device kicks
    tnt_dbdma_kick(s_d, 0);
    ASSERT_EQ_INT((int)peek32(0x2000), 0x1234);
    control(0, (uint32_t)TNT_DBDMA_RUN << 16);
    // Interrupt: INTR_IFCLR on s2 — fires only while the bit is clear.
    tnt_dbdma_reg_write(s_d, 0, TNT_DBDMA_REG_INTRSEL, 0x00040004);
    desc(0x5000, op(NOP_CMD, IFCLR, NEVER, NEVER, 0), 0, 0);
    desc(0x5010, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    s_dev_sbits = 0x04;
    start(0, 0x5000);
    ASSERT_EQ_INT(s_irq_count, 0); // condition true -> IFCLR suppressed
    control(0, (uint32_t)TNT_DBDMA_RUN << 16);
    s_dev_sbits = 0;
    start(0, 0x5000);
    ASSERT_EQ_INT(s_irq_count, 1);
}

// cmdptr is load-protected while the channel runs; PAUSE suspends the
// engine at a command boundary and resuming continues the program.
TEST(test_cmdptr_protect_and_pause) {
    fixture();
    desc(0x1000, op(OUTPUT_LAST, NEVER, NEVER, NEVER, 4), 0x2000, 0);
    desc(0x1010, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    s_dev_budget = 0; // stall immediately
    start(0, 0x1000);
    ASSERT_TRUE(status(0) & TNT_DBDMA_ACTIVE);
    tnt_dbdma_reg_write(s_d, 0, TNT_DBDMA_REG_CMDPTRLO, 0x9999); // must be ignored
    ASSERT_EQ_INT((int)tnt_dbdma_reg_read(s_d, 0, TNT_DBDMA_REG_CMDPTRLO), 0x1000);
    // Pause, then feed the device: the kick must NOT move data while
    // paused; unpausing resumes and completes.
    control(0, (TNT_DBDMA_PAUSE << 16) | TNT_DBDMA_PAUSE);
    s_dev_budget = -1;
    tnt_dbdma_kick(s_d, 0);
    ASSERT_EQ_INT(s_dev_len, 0);
    control(0, (uint32_t)TNT_DBDMA_PAUSE << 16);
    ASSERT_EQ_INT(s_dev_len, 4);
    ASSERT_EQ_INT((int)(status(0) & TNT_DBDMA_ACTIVE), 0);
}

// Checkpoint round-trip: a mid-stall channel survives save/restore (the
// stream carries status, pointers, selects and the byte cursor).
TEST(test_checkpoint_roundtrip) {
    fixture();
    desc(0x1000, op(INPUT_LAST, ALWAYS, NEVER, NEVER, 8), 0x2000, 0);
    desc(0x1010, op(STOP_CMD, NEVER, NEVER, NEVER, 0), 0, 0);
    s_dev_data[0] = 0x77;
    s_dev_len = 1;
    start(0, 0x1000);
    ASSERT_TRUE(status(0) & TNT_DBDMA_ACTIVE);
    // Save, rebuild, restore through the recording byte stream.
    s_cp_w = s_cp_r = 0;
    tnt_dbdma_checkpoint(s_d, (checkpoint_t *)1);
    tnt_dbdma_delete(s_d);
    s_d = tnt_dbdma_init((checkpoint_t *)1);
    tnt_dbdma_set_memory_hooks(s_d, mem_read, mem_write, NULL);
    tnt_dbdma_set_irq_hook(s_d, irq_hook, NULL);
    static tnt_dbdma_port_t port = {dev_out, dev_in, dev_s_bits, NULL};
    tnt_dbdma_set_port(s_d, 0, &port);
    ASSERT_TRUE(status(0) & TNT_DBDMA_ACTIVE); // still mid-program
    // The rest of the data arrives; the transfer completes from byte 1.
    memcpy(s_dev_data + 1, "\x78\x79\x7A\x7B\x7C\x7D\x7E", 7);
    s_dev_len = 8;
    tnt_dbdma_kick(s_d, 0);
    ASSERT_EQ_INT(s_irq_count, 1);
    ASSERT_EQ_INT(memcmp(s_mem + 0x2000, "\x77\x78\x79\x7A\x7B\x7C\x7D\x7E", 8), 0);
}

int main(void) {
    RUN(test_control_mask_value);
    RUN(test_output_program);
    RUN(test_stop_reset_sequences);
    RUN(test_stall_and_kick);
    RUN(test_no_port_stalls);
    RUN(test_nop_branch_ring);
    RUN(test_stop_wake_overwrite);
    RUN(test_quads);
    RUN(test_conditions);
    RUN(test_cmdptr_protect_and_pause);
    RUN(test_checkpoint_roundtrip);
    return 0;
}
