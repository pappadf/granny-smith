// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the shared bus-master memory port (05-chipsets-irq F-16).
//
// Four engines used to declare their own hook typedefs in two different
// shapes -- `(phys, value, width)` for SONIC and the PSC, `(phys, buf, len)`
// for DBDMA.  One type carries both now, and the point of these tests is the
// bridge between them: an engine calls whichever shape its command makes
// natural, whatever shape the machine's port happens to implement.  Without
// the bridge, "one type" would just be two typedefs in one struct.

#include "dma_mem.h"
#include "test_assert.h"

#include <string.h>

static uint8_t s_mem[64];
static int s_scalar_reads, s_scalar_writes, s_block_reads, s_block_writes;

static uint32_t scalar_read(void *ctx, uint32_t phys, unsigned width) {
    (void)ctx;
    s_scalar_reads++;
    uint32_t v = 0;
    for (unsigned i = 0; i < width; i++)
        v = (v << 8) | s_mem[phys + i];
    return v;
}
static void scalar_write(void *ctx, uint32_t phys, uint32_t value, unsigned width) {
    (void)ctx;
    s_scalar_writes++;
    for (unsigned i = 0; i < width; i++)
        s_mem[phys + i] = (uint8_t)(value >> ((width - 1 - i) * 8));
}
static void block_read(void *ctx, uint32_t phys, uint8_t *buf, uint32_t len) {
    (void)ctx;
    s_block_reads++;
    memcpy(buf, s_mem + phys, len);
}
static void block_write(void *ctx, uint32_t phys, const uint8_t *buf, uint32_t len) {
    (void)ctx;
    s_block_writes++;
    memcpy(s_mem + phys, buf, len);
}

static const dma_mem_port_t k_scalar_only = {.read = scalar_read, .write = scalar_write};
static const dma_mem_port_t k_block_only = {.read_block = block_read, .write_block = block_write};

static void reset(void) {
    memset(s_mem, 0, sizeof s_mem);
    s_scalar_reads = s_scalar_writes = s_block_reads = s_block_writes = 0;
}

// A SONIC-shaped port (scalar only) must still serve a DBDMA-shaped block
// move, one byte at a time.
TEST(test_scalar_only_port_serves_block_moves) {
    reset();
    const uint8_t src[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    dma_mem_write_block(&k_scalar_only, 8, src, 4);
    ASSERT_EQ_INT(s_scalar_writes, 4); // byte at a time, as it must be
    ASSERT_EQ_INT(s_block_writes, 0);
    ASSERT_TRUE(memcmp(s_mem + 8, src, 4) == 0);

    uint8_t out[4] = {0};
    dma_mem_read_block(&k_scalar_only, 8, out, 4);
    ASSERT_EQ_INT(s_scalar_reads, 4);
    ASSERT_TRUE(memcmp(out, src, 4) == 0);
}

// ...and a DBDMA-shaped port (block only) must serve a descriptor-field
// scalar read, composed big-endian like every bus in this machine.
TEST(test_block_only_port_serves_scalar_access_big_endian) {
    reset();
    s_mem[16] = 0x12;
    s_mem[17] = 0x34;
    s_mem[18] = 0x56;
    s_mem[19] = 0x78;
    ASSERT_EQ_INT((int)dma_mem_read(&k_block_only, 16, 1), 0x12);
    ASSERT_EQ_INT((int)dma_mem_read(&k_block_only, 16, 2), 0x1234);
    ASSERT_EQ_INT((int)dma_mem_read(&k_block_only, 16, 4), 0x12345678);
    ASSERT_EQ_INT(s_scalar_reads, 0); // never reached the scalar slot
    ASSERT_TRUE(s_block_reads == 3);

    dma_mem_write(&k_block_only, 32, 0xAABBCCDDu, 4);
    ASSERT_EQ_INT(s_mem[32], 0xAA);
    ASSERT_EQ_INT(s_mem[35], 0xDD);
    dma_mem_write(&k_block_only, 40, 0xBEEFu, 2);
    ASSERT_EQ_INT(s_mem[40], 0xBE);
    ASSERT_EQ_INT(s_mem[41], 0xEF);
}

// A port a machine never wired: reads zero, writes vanish, and the engine
// can tell -- which is how sonic.c and dbdma.c log "no memory port".
TEST(test_unbound_port_is_inert_and_says_so) {
    reset();
    static const dma_mem_port_t none = {0};
    ASSERT_TRUE(!dma_mem_port_bound(&none));
    ASSERT_TRUE(!dma_mem_port_bound(NULL));
    ASSERT_TRUE(dma_mem_port_bound(&k_scalar_only));
    ASSERT_TRUE(dma_mem_port_bound(&k_block_only));

    ASSERT_EQ_INT((int)dma_mem_read(&none, 0, 4), 0);
    ASSERT_EQ_INT((int)dma_mem_read(NULL, 0, 4), 0);
    uint8_t out[4] = {1, 2, 3, 4};
    dma_mem_read_block(&none, 0, out, 4); // must not write through a NULL hook
    dma_mem_write_block(&none, 0, out, 4);
    dma_mem_write(&none, 0, 0xFF, 1);
    ASSERT_EQ_INT(s_mem[0], 0);
}

// The port's ctx reaches the callbacks: tnt.c installs one shared static
// descriptor and only varies ctx, so a port that dropped it would hand the
// wrong machine's config to the mover.
static void *s_seen_ctx;
static uint32_t ctx_read(void *ctx, uint32_t phys, unsigned width) {
    (void)phys;
    (void)width;
    s_seen_ctx = ctx;
    return 0;
}

TEST(test_ctx_reaches_the_callbacks) {
    int marker = 0;
    dma_mem_port_t p = {.read = ctx_read, .ctx = &marker};
    s_seen_ctx = NULL;
    (void)dma_mem_read(&p, 0, 1);
    ASSERT_TRUE(s_seen_ctx == &marker);
}

int main(void) {
    RUN(test_scalar_only_port_serves_block_moves);
    RUN(test_block_only_port_serves_scalar_access_big_endian);
    RUN(test_unbound_port_is_inert_and_says_so);
    RUN(test_ctx_reaches_the_callbacks);
    printf("[PASS] All dma_mem tests passed\n");
    return 0;
}
