// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// MFS, read over volumes the test lays out (Inside Macintosh II, "Data
// Organization on Volumes"): a master directory block, the allocation map,
// a flat file directory, and forks whose allocation blocks are chained out
// of order -- then the same volume made corrupt in the ways a reader must
// refuse rather than trust.

#include "image_mfs.h"
#include "source.h"
#include "test_assert.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOL_BLOCKS 200 // 512-byte blocks
#define ALLOC_SIZE 1024 // two blocks per allocation block
#define ALLOC_ST   6 // allocation block 2 starts at block 6
#define N_ALLOC    90
#define DIR_ST     4
#define DIR_BLOCKS 2

static uint8_t vol[VOL_BLOCKS * 512];

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)(v >> 16));
    put16(p + 2, (uint16_t)v);
}

// Set allocation block n's map entry (12 bits, two entries in three bytes).
static void map_set(uint32_t n, uint16_t next) {
    uint8_t *m = vol + 1024 + 64;
    uint32_t i = n - 2;
    size_t o = (size_t)i * 3 / 2;
    if (i & 1) {
        m[o] = (uint8_t)((m[o] & 0xF0) | (next >> 8));
        m[o + 1] = (uint8_t)next;
    } else {
        m[o] = (uint8_t)(next >> 4);
        m[o + 1] = (uint8_t)((m[o + 1] & 0x0F) | (next & 0x0F) << 4);
    }
}

// The bytes of allocation block n.
static uint8_t *alloc_block(uint32_t n) {
    return vol + ALLOC_ST * 512 + (size_t)(n - 2) * ALLOC_SIZE;
}

// A directory entry at `p`; returns the next entry's position.
static size_t dir_entry(uint8_t *blk, size_t p, const char *name, uint32_t fnum, uint16_t dstart, uint32_t dlen,
                        uint16_t rstart, uint32_t rlen, const char *type, const char *creator) {
    uint8_t *e = blk + p;
    e[0] = 0x80;
    memcpy(e + 2, type, 4);
    memcpy(e + 6, creator, 4);
    put16(e + 10, 0x0100);
    put32(e + 18, fnum);
    put16(e + 22, dstart);
    put32(e + 24, dlen);
    put32(e + 28, (dlen + ALLOC_SIZE - 1) / ALLOC_SIZE * ALLOC_SIZE);
    put16(e + 32, rstart);
    put32(e + 34, rlen);
    put32(e + 38, (rlen + ALLOC_SIZE - 1) / ALLOC_SIZE * ALLOC_SIZE);
    size_t n = strlen(name);
    e[50] = (uint8_t)n;
    memcpy(e + 51, name, n);
    return p + ((51 + n + 1) & ~(size_t)1);
}

static uint8_t pattern(size_t i) {
    return (uint8_t)(i * 13 + 7);
}

// "Read/Me": 2500 data bytes in allocation blocks 2 -> 5 -> 3, a 100-byte
// resource fork in block 4.  "Second": empty.
static void build(void) {
    memset(vol, 0, sizeof(vol));
    uint8_t *mdb = vol + 1024;
    put16(mdb + 0, MFS_SIG);
    put16(mdb + 12, 2); // files
    put16(mdb + 14, DIR_ST);
    put16(mdb + 16, DIR_BLOCKS);
    put16(mdb + 18, N_ALLOC);
    put32(mdb + 20, ALLOC_SIZE);
    put16(mdb + 28, ALLOC_ST);
    mdb[36] = 4;
    memcpy(mdb + 37, "Test", 4);
    map_set(2, 5);
    map_set(5, 3);
    map_set(3, 1);
    map_set(4, 1);
    for (size_t i = 0; i < 2500; i++) {
        uint32_t blk = i < 1024 ? 2 : i < 2048 ? 5 : 3;
        alloc_block(blk)[i % ALLOC_SIZE] = pattern(i);
    }
    for (size_t i = 0; i < 100; i++)
        alloc_block(4)[i] = (uint8_t)(0xA0 ^ i);
    uint8_t *dir = vol + DIR_ST * 512;
    size_t p = dir_entry(dir, 0, "Read/Me", 17, 2, 2500, 4, 100, "TEXT", "ttxt");
    dir_entry(dir, p, "Second", 18, 0, 0, 0, 0, "APPL", "????");
}

static source_t *src(void) {
    return source_memory(vol, sizeof(vol), false, "mfs-test");
}

// The volume opens, lists its two files, and names a Mac '/' as ':'.
TEST(test_mfs_lists_files_and_finder_info) {
    build();
    source_t *s = src();
    ASSERT_TRUE(mfs_probe_source(s, 0, sizeof(vol)));
    mfs_volume_t *v = mfs_open_source(s, 0, sizeof(vol));
    ASSERT_TRUE(v != NULL);
    ASSERT_TRUE(strcmp(mfs_volume_name(v), "Test") == 0);
    ASSERT_EQ_INT(2, mfs_count(v));
    const mfs_dirent_t *e = mfs_entry(v, 0);
    ASSERT_TRUE(strcmp(e->name, "Read:Me") == 0);
    ASSERT_EQ_INT(2500, (int)e->data_len);
    ASSERT_EQ_INT(100, (int)e->rsrc_len);
    ASSERT_TRUE(memcmp(e->finder_info, "TEXTttxt", 8) == 0);
    ASSERT_TRUE(strcmp(mfs_entry(v, 1)->name, "Second") == 0);
    mfs_dirent_t f;
    ASSERT_EQ_INT(0, mfs_lookup(v, "read:me", &f)); // case-insensitive, ':' for '/'
    ASSERT_EQ_INT(0, mfs_lookup(v, "READ/ME", &f));
    ASSERT_EQ_INT(-ENOENT, mfs_lookup(v, "Nope", &f));
    mfs_close(v);
    source_release(s);
}

// Forks follow their allocation chains, out of order, across block edges.
TEST(test_mfs_reads_forks_through_the_chain) {
    build();
    source_t *s = src();
    mfs_volume_t *v = mfs_open_source(s, 0, sizeof(vol));
    mfs_dirent_t f;
    ASSERT_EQ_INT(0, mfs_lookup(v, "Read:Me", &f));
    uint8_t buf[3000];
    size_t got = 0;
    ASSERT_EQ_INT(0, mfs_read_fork(v, &f, false, 0, buf, sizeof(buf), &got));
    ASSERT_EQ_INT(2500, (int)got); // short at the fork's end
    for (size_t i = 0; i < got; i++)
        ASSERT_EQ_INT(pattern(i), buf[i]);
    ASSERT_EQ_INT(0, mfs_read_fork(v, &f, false, 2000, buf, 100, &got)); // across 5 -> 3
    ASSERT_EQ_INT(100, (int)got);
    for (size_t i = 0; i < got; i++)
        ASSERT_EQ_INT(pattern(2000 + i), buf[i]);
    ASSERT_EQ_INT(0, mfs_read_fork(v, &f, true, 0, buf, sizeof(buf), &got));
    ASSERT_EQ_INT(100, (int)got);
    ASSERT_EQ_INT(0xA0 ^ 99, buf[99]);
    mfs_close(v);
    source_release(s);
}

// A cycle in the allocation map, or a chain ending before the fork does,
// is an error -- never an endless walk or a read of the wrong blocks.
TEST(test_mfs_broken_chains_are_refused) {
    build();
    map_set(3, 2); // 2 -> 5 -> 3 -> 2 -> ...
    uint8_t *dir = vol + DIR_ST * 512;
    put32(dir + 24, 40000); // and a fork longer than the volume
    source_t *s = src();
    mfs_volume_t *v = mfs_open_source(s, 0, sizeof(vol));
    ASSERT_TRUE(v != NULL);
    mfs_dirent_t f;
    ASSERT_EQ_INT(0, mfs_lookup(v, "Read:Me", &f));
    static uint8_t buf[40000];
    size_t got = 0;
    ASSERT_EQ_INT(-EIO, mfs_read_fork(v, &f, false, 0, buf, sizeof(buf), &got));
    ASSERT_EQ_INT(-EIO, mfs_read_fork(v, &f, false, 39000, buf, 10, &got));
    mfs_close(v);
    source_release(s);

    build();
    map_set(5, 1); // the chain ends after two blocks of a three-block fork
    s = src();
    v = mfs_open_source(s, 0, sizeof(vol));
    ASSERT_EQ_INT(0, mfs_lookup(v, "Read:Me", &f));
    ASSERT_EQ_INT(-EIO, mfs_read_fork(v, &f, false, 0, buf, 2500, &got));
    mfs_close(v);
    source_release(s);
}

// A volume whose geometry does not fit, or whose directory entry spans a
// block, does not open.
TEST(test_mfs_bad_volumes_do_not_open) {
    build();
    put16(vol + 1024 + 18, 4000); // more allocation blocks than the volume holds
    source_t *s = src();
    ASSERT_TRUE(mfs_open_source(s, 0, sizeof(vol)) == NULL);
    source_release(s);

    build();
    uint8_t *dir = vol + DIR_ST * 512;
    char longname[256];
    memset(longname, 'x', 255);
    longname[255] = '\0';
    size_t p = dir_entry(dir, 0, longname, 17, 2, 2500, 4, 100, "TEXT", "ttxt"); // 306 bytes
    longname[200] = '\0';
    dir_entry(dir, p, longname, 18, 0, 0, 0, 0, "APPL", "????"); // 251 more: past the block
    s = src();
    ASSERT_TRUE(mfs_open_source(s, 0, sizeof(vol)) == NULL);
    source_release(s);

    build();
    vol[1024] = 0; // no signature
    s = src();
    ASSERT_TRUE(!mfs_probe_source(s, 0, sizeof(vol)));
    ASSERT_TRUE(mfs_open_source(s, 0, sizeof(vol)) == NULL);
    source_release(s);
}

int main(void) {
    RUN(test_mfs_lists_files_and_finder_info);
    RUN(test_mfs_reads_forks_through_the_chain);
    RUN(test_mfs_broken_chains_are_refused);
    RUN(test_mfs_bad_volumes_do_not_open);
    fprintf(stderr, "All MFS tests passed\n");
    return 0;
}
