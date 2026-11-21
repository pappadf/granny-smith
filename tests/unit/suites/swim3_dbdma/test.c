// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// SWIM3-over-DBDMA port unit test.
//
// Drives the shared machines/runtime/swim3_dbdma.c through the real
// core/peripherals/dbdma.c, with the SWIM3 chip reduced to the backend it
// is handed (swim3_bind is stubbed below: the test plays the chip by
// calling that backend directly).  Pinned:
//
//  1. A write's leftover: an OUTPUT command larger than the sector tops the
//     ring up past what the chip consumes; when the driver stops the
//     channel those bytes are dropped, so the next write starts with its
//     own data (MacTest Pro's floppy seek test, which once failed with
//     every sector carrying the previous one's data -- on Gossamer, whose
//     port had no stop hook, until the two copies were merged).
//  2. The read path: bytes the chip offers land in memory through INPUT.
//  3. No program, no transfer: put is refused and get underruns.
//  4. The IRQ pin reaches the board's sink with its context.

#include "checkpoint.h"
#include "dbdma.h"
#include "swim3_dbdma.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// ============================================================================
// Link stubs
// ============================================================================

// The engine's checkpoint movers (dbdma_init with NULL never calls them)
void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)cp;
    (void)data;
    (void)size;
    (void)tag;
    (void)file;
    (void)line;
}

void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)cp;
    (void)data;
    (void)size;
    (void)tag;
    (void)file;
    (void)line;
}

// The chip side: keep the backend so the test can play the SWIM3 engine
void swim3_bind(swim3_t *sw, struct floppy *fd, struct scheduler *sched, const swim3_backend_t *be) {
    sw->fd = fd;
    sw->sched = sched;
    sw->be = *be;
}

// ============================================================================
// Guest memory
// ============================================================================

#define MEM_SIZE 0x10000
static uint8_t s_mem[MEM_SIZE];

// Engine block read from the flat guest array
static void mem_read(void *ctx, uint32_t phys, uint8_t *buf, uint32_t len) {
    (void)ctx;
    ASSERT_TRUE(phys + len <= MEM_SIZE);
    memcpy(buf, s_mem + phys, len);
}

// Engine block write into the flat guest array
static void mem_write(void *ctx, uint32_t phys, const uint8_t *buf, uint32_t len) {
    (void)ctx;
    ASSERT_TRUE(phys + len <= MEM_SIZE);
    memcpy(s_mem + phys, buf, len);
}

// Little-endian guest store (descriptors are little-endian)
static void poke32(uint32_t addr, uint32_t v) {
    s_mem[addr] = (uint8_t)v;
    s_mem[addr + 1] = (uint8_t)(v >> 8);
    s_mem[addr + 2] = (uint8_t)(v >> 16);
    s_mem[addr + 3] = (uint8_t)(v >> 24);
}

// One descriptor at `addr`, operation word last (the commit rule)
static void desc(uint32_t addr, uint32_t op, uint32_t data_addr) {
    poke32(addr + 12, 0);
    poke32(addr + 8, 0);
    poke32(addr + 4, data_addr);
    poke32(addr, op);
}

// Operation word: command nibble 31:28, reqCount 15:0, no conditions
static uint32_t op(uint32_t cmd, uint32_t req) {
    return (cmd << 28) | (req & 0xFFFFu);
}

#define OUTPUT_LAST 1u
#define INPUT_LAST  3u
#define STOP_CMD    7u

#define FD_CHAN 1 // the floppy channel on both MacRISC controllers

// ============================================================================
// Fixture
// ============================================================================

static dbdma_t *s_d;
static swim3_dbdma_t s_port;
static swim3_t s_chip;
static int s_irq_level = -1;
static void *s_irq_ctx;

// The board's interrupt sink: record level and context
static void irq_sink(void *ctx, bool level) {
    s_irq_ctx = ctx;
    s_irq_level = level ? 1 : 0;
}

// Fresh engine, port wired to channel 1, chip bound to the port
static void fixture(void) {
    if (s_d)
        dbdma_delete(s_d);
    s_d = dbdma_init(NULL, DBDMA_CHANNELS_GRAND_CENTRAL);
    static const dma_mem_port_t mem_port = {.read_block = mem_read, .write_block = mem_write};
    dbdma_set_memory_port(s_d, &mem_port);
    memset(s_mem, 0, sizeof(s_mem));
    memset(&s_chip, 0, sizeof(s_chip));
    swim3_dbdma_init(&s_port, s_d, FD_CHAN, irq_sink, &s_irq_level);
    swim3_dbdma_bind(&s_port, &s_chip, NULL, NULL);
    s_irq_level = -1;
    s_irq_ctx = NULL;
}

// Write the channel's control register (mask hi 16 / value lo 16)
static void control(uint32_t v) {
    dbdma_reg_write(s_d, FD_CHAN, DBDMA_REG_CONTROL, v);
}

// The canonical start: command pointer, then RUN
static void start(uint32_t cmdptr) {
    dbdma_reg_write(s_d, FD_CHAN, DBDMA_REG_CMDPTRLO, cmdptr);
    control((DBDMA_RUN << 16) | DBDMA_RUN);
}

// The driver's stop: clear RUN
static void stop(void) {
    control(DBDMA_RUN << 16);
}

// ============================================================================
// Tests
// ============================================================================

// An OUTPUT larger than the sector, stopped after the chip took one sector:
// the next write must see its own bytes, not the first write's tail.
TEST(test_stop_drops_staged_output) {
    fixture();
    memset(s_mem + 0x2000, 0xAA, 1024); // first buffer: 1024 bytes
    desc(0x1000, op(OUTPUT_LAST, 1024), 0x2000);
    desc(0x1010, op(STOP_CMD, 0), 0);
    start(0x1000);
    // The chip consumes one 512-byte sector of it
    uint8_t b = 0;
    for (int i = 0; i < 512; i++) {
        ASSERT_TRUE(s_chip.be.dma_get(s_chip.be.ctx, &b));
        ASSERT_EQ_INT(b, 0xAA);
    }
    stop();
    ASSERT_EQ_INT(s_port.ring.tail - s_port.ring.head, 0); // the leftover is gone
    // The next write's program: its first byte is the chip's next byte
    memset(s_mem + 0x3000, 0x55, 512);
    desc(0x1100, op(OUTPUT_LAST, 512), 0x3000);
    desc(0x1110, op(STOP_CMD, 0), 0);
    start(0x1100);
    ASSERT_TRUE(s_chip.be.dma_get(s_chip.be.ctx, &b));
    ASSERT_EQ_INT(b, 0x55);
}

// Bytes the chip reads off the disk reach memory through an INPUT command.
TEST(test_read_path_fills_memory) {
    fixture();
    desc(0x1000, op(INPUT_LAST, 4), 0x2000);
    desc(0x1010, op(STOP_CMD, 0), 0);
    start(0x1000);
    ASSERT_TRUE(s_chip.be.dma_running(s_chip.be.ctx));
    const uint8_t data[4] = {0x11, 0x22, 0x33, 0x44};
    for (int i = 0; i < 4; i++)
        ASSERT_TRUE(s_chip.be.dma_put(s_chip.be.ctx, data[i]));
    ASSERT_EQ_INT(memcmp(s_mem + 0x2000, data, 4), 0);
}

// With no program running, a read byte overruns and a write byte underruns.
TEST(test_idle_channel_refuses) {
    fixture();
    uint8_t b = 0;
    ASSERT_TRUE(!s_chip.be.dma_running(s_chip.be.ctx));
    ASSERT_TRUE(!s_chip.be.dma_put(s_chip.be.ctx, 0x12));
    ASSERT_TRUE(!s_chip.be.dma_get(s_chip.be.ctx, &b));
}

// The chip's IRQ pin reaches the board's sink with the board's context.
TEST(test_irq_forwarded) {
    fixture();
    s_chip.be.set_irq(s_chip.be.ctx, true);
    ASSERT_EQ_INT(s_irq_level, 1);
    ASSERT_TRUE(s_irq_ctx == &s_irq_level);
    s_chip.be.set_irq(s_chip.be.ctx, false);
    ASSERT_EQ_INT(s_irq_level, 0);
}

int main(void) {
    RUN(test_stop_drops_staged_output);
    RUN(test_read_path_fills_memory);
    RUN(test_idle_channel_refuses);
    RUN(test_irq_forwarded);
    return 0;
}
