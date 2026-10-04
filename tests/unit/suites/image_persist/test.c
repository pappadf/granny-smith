// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// A writable disk keeps its writes across close and reopen.
//
// A machine.boot or checkpoint.load replaces the running machine, which closes
// its disks; the next machine mounts the same files again.  Every mount used
// to mint a fresh delta, so the writes the previous machine made -- held in
// its delta, never in the base -- were not there.  A mount's delta is now
// named after its base and a close commits it, so the next mount finds them.

#include "image.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLK    512u
#define BLOCKS 64u

static char g_dir[] = "/tmp/gs-image-persist-XXXXXX";
static char g_base[512];
static char g_deltas[512];

static void make_base(void) {
    ASSERT_TRUE(mkdtemp(g_dir) != NULL);
    snprintf(g_base, sizeof g_base, "%s/disk.img", g_dir);
    snprintf(g_deltas, sizeof g_deltas, "%s/deltas", g_dir);
    FILE *f = fopen(g_base, "wb");
    ASSERT_TRUE(f != NULL);
    uint8_t blk[BLK];
    memset(blk, 0x11, sizeof blk);
    for (unsigned i = 0; i < BLOCKS; i++)
        ASSERT_TRUE(fwrite(blk, BLK, 1, f) == 1);
    fclose(f);
}

static uint8_t read_byte(image_t *img, unsigned block) {
    uint8_t buf[BLK];
    ASSERT_TRUE(disk_read_data(img, (size_t)block * BLK, buf, BLK) == BLK);
    return buf[0];
}

static void write_block(image_t *img, unsigned block, uint8_t v) {
    uint8_t buf[BLK];
    memset(buf, v, sizeof buf);
    ASSERT_TRUE(disk_write_data(img, (size_t)block * BLK, buf, BLK) == BLK);
}

// Write, close, mount again: the writes are there, the base is untouched.
TEST(writes_survive_close_and_reopen) {
    image_t *img = image_create(g_base, g_deltas);
    ASSERT_TRUE(img != NULL);
    write_block(img, 3, 0xA3);
    write_block(img, 40, 0xB4);
    image_close(img);

    img = image_create(g_base, g_deltas);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ_INT(read_byte(img, 3), 0xA3);
    ASSERT_EQ_INT(read_byte(img, 40), 0xB4);
    ASSERT_EQ_INT(read_byte(img, 4), 0x11); // unwritten blocks still read the base

    // A second round, overwriting a block the first round wrote.
    write_block(img, 3, 0xC3);
    image_close(img);
    img = image_create(g_base, g_deltas);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ_INT(read_byte(img, 3), 0xC3);
    ASSERT_EQ_INT(read_byte(img, 40), 0xB4);
    image_close(img);

    // The base file itself never changed.
    FILE *f = fopen(g_base, "rb");
    ASSERT_TRUE(f != NULL);
    uint8_t b[BLK];
    fseek(f, 3 * BLK, SEEK_SET);
    ASSERT_TRUE(fread(b, BLK, 1, f) == 1);
    fclose(f);
    ASSERT_EQ_INT(b[0], 0x11);
}

int main(void) {
    make_base();
    RUN(writes_survive_close_and_reopen);
    return 0;
}
