// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ISO 9660, read over discs the test lays out (ECMA-119): a primary volume
// descriptor, optionally a Joliet supplementary one, directory records in
// both byte orders, Rock Ridge NM names, and Apple's associated file (a
// resource fork) -- then records made corrupt in the ways a reader must
// refuse rather than trust.

#include "image_iso9660.h"
#include "source.h"
#include "test_assert.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEC     2048
#define SECTORS 40

static uint8_t disc[SECTORS * SEC];

// A both-byte-order field (ECMA-119 7.2.3 / 7.3.3).
static void both32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
        p[7 - i] = (uint8_t)(v >> (8 * i));
    }
}
static void both16(uint8_t *p, uint16_t v) {
    p[0] = p[3] = (uint8_t)v;
    p[1] = p[2] = (uint8_t)(v >> 8);
}

// A directory record at `p` of `dir`; returns its length.  `su` (with
// `su_len` bytes) is its system use area -- Rock Ridge entries.
static size_t record(uint8_t *p, const uint8_t *id, size_t idlen, uint32_t extent, uint32_t size, uint8_t flags,
                     const uint8_t *su, size_t su_len) {
    size_t len = 33 + idlen + (idlen % 2 == 0 ? 1 : 0) + su_len;
    len += len & 1;
    memset(p, 0, len);
    p[0] = (uint8_t)len;
    both32(p + 2, extent);
    both32(p + 10, size);
    p[25] = flags;
    both16(p + 28, 1);
    p[32] = (uint8_t)idlen;
    memcpy(p + 33, id, idlen);
    if (su_len)
        memcpy(p + 33 + idlen + (idlen % 2 == 0 ? 1 : 0), su, su_len);
    return len;
}

static size_t rec_s(uint8_t *p, const char *id, uint32_t extent, uint32_t size, uint8_t flags) {
    return record(p, (const uint8_t *)id, strlen(id), extent, size, flags, NULL, 0);
}

// A volume descriptor: type 1 (primary) or 2 (supplementary, Joliet when
// `joliet`), its root at `root_extent`.
static void descriptor(int sector, int type, bool joliet, uint32_t root_extent, uint32_t root_size) {
    uint8_t *vd = disc + sector * SEC;
    vd[0] = (uint8_t)type;
    memcpy(vd + 1, "CD001", 5);
    vd[6] = 1;
    memset(vd + 40, ' ', 32);
    memcpy(vd + 40, "TESTDISC", 8);
    both32(vd + 80, SECTORS);
    both16(vd + 128, SEC);
    if (joliet)
        memcpy(vd + 88, "%/E", 3);
    uint8_t dot = 0;
    record(vd + 156, &dot, 1, root_extent, root_size, 0x02, NULL, 0);
}

static uint8_t pattern(size_t i) {
    return (uint8_t)(i * 31 + 5);
}

// Root (sector 20): README.TXT;1 with its associated file (a resource fork
// at sector 24), and DIR (sector 21) holding INNER.TXT;1.  With `joliet`, a
// second tree at sector 22 names the same files "Read Me.txt" and "Folder".
// With `rock_ridge`, the root's "." carries SUSP "SP" and the files NM names.
static void build(bool joliet, bool rock_ridge) {
    memset(disc, 0, sizeof(disc));
    descriptor(16, 1, false, 20, SEC);
    int term = 17;
    if (joliet) {
        descriptor(17, 2, true, 22, SEC);
        term = 18;
    }
    disc[term * SEC] = 255;
    memcpy(disc + term * SEC + 1, "CD001", 5);
    disc[term * SEC + 6] = 1;

    // File data: README at 25 (3000 bytes, two sectors), INNER at 27.
    for (size_t i = 0; i < 3000; i++)
        disc[25 * SEC + i] = pattern(i);
    memcpy(disc + 27 * SEC, "inner", 5);
    memset(disc + 24 * SEC, 0xEE, 100); // the resource fork

    uint8_t *root = disc + 20 * SEC;
    size_t p = 0;
    if (rock_ridge) {
        static const uint8_t sp[7] = {'S', 'P', 7, 1, 0xBE, 0xEF, 0};
        uint8_t dot = 0;
        p += record(root + p, &dot, 1, 20, SEC, 0x02, sp, sizeof(sp));
    } else {
        uint8_t dot = 0;
        p += record(root + p, &dot, 1, 20, SEC, 0x02, NULL, 0);
    }
    uint8_t dotdot = 1;
    p += record(root + p, &dotdot, 1, 20, SEC, 0x02, NULL, 0);
    p += rec_s(root + p, "README.TXT;1", 24, 100, 0x04); // its associated file
    if (rock_ridge) {
        static const uint8_t nm[] = {'N', 'M', 15, 1, 0, 'R', 'e', 'a', 'd', 'M', 'e', '.', 't', 'x', 't'};
        p += record(root + p, (const uint8_t *)"README.TXT;1", 12, 25, 3000, 0, nm, sizeof(nm));
    } else {
        p += rec_s(root + p, "README.TXT;1", 25, 3000, 0);
    }
    p += rec_s(root + p, "DIR", 21, SEC, 0x02);

    uint8_t *dir = disc + 21 * SEC;
    uint8_t dot = 0;
    p = record(dir, &dot, 1, 21, SEC, 0x02, NULL, 0);
    p += record(dir + p, &dotdot, 1, 20, SEC, 0x02, NULL, 0);
    p += rec_s(dir + p, "INNER.TXT;1", 27, 5, 0);

    if (joliet) {
        uint8_t *jr = disc + 22 * SEC;
        p = record(jr, &dot, 1, 22, SEC, 0x02, NULL, 0);
        p += record(jr + p, &dotdot, 1, 22, SEC, 0x02, NULL, 0);
        static const uint8_t readme[] = {0,   'R', 0,   'e', 0,   'a', 0,   'd', 0,   ' ', 0,   'M', 0,
                                         'e', 0,   '.', 0,   't', 0,   'x', 0,   't', 0,   ';', 0,   '1'};
        p += record(jr + p, readme, sizeof(readme), 25, 3000, 0, NULL, 0);
        static const uint8_t folder[] = {0, 'F', 0, 'o', 0, 'l', 0, 'd', 0, 'e', 0, 'r'};
        p += record(jr + p, folder, sizeof(folder), 21, SEC, 0x02, NULL, 0);
    }
}

static gs_source_t *src(void) {
    return gs_source_memory(disc, sizeof(disc), false, "iso-test");
}

// Collect a directory's names (", "-joined).
static void names(iso_volume_t *v, uint32_t extent, uint64_t size, char *out, size_t cap) {
    iso_dir_iter_t *it = iso_opendir(v, extent, size);
    ASSERT_TRUE(it != NULL);
    out[0] = '\0';
    iso_dirent_t e;
    int rc;
    while ((rc = iso_readdir_next(it, &e)) > 0)
        snprintf(out + strlen(out), cap - strlen(out), "%s%s", out[0] ? ", " : "", e.name);
    ASSERT_EQ_INT(0, rc);
    iso_closedir(it);
}

// The plain ISO tree: version numbers dropped, the associated file folded
// into its file as a resource fork, a subdirectory, case-insensitive lookup.
TEST(test_iso_plain_tree) {
    build(false, false);
    gs_source_t *s = src();
    ASSERT_TRUE(iso_probe_source(s, 0, sizeof(disc)));
    iso_volume_t *v = iso_open_source(s, 0, sizeof(disc));
    ASSERT_TRUE(v != NULL);
    ASSERT_TRUE(strcmp(iso_volume_name(v), "TESTDISC") == 0);
    char list[256];
    names(v, 20, SEC, list, sizeof(list));
    ASSERT_TRUE(strcmp(list, "README.TXT, DIR") == 0);
    iso_dirent_t e;
    const char *path[] = {"readme.txt"};
    ASSERT_EQ_INT(0, iso_lookup(v, path, 1, &e));
    ASSERT_EQ_INT(3000, (int)e.size);
    ASSERT_EQ_INT(100, (int)e.rsrc_size);
    uint8_t buf[4000];
    size_t got = 0;
    ASSERT_EQ_INT(0, iso_read(v, e.extent, e.size, 0, buf, sizeof(buf), &got));
    ASSERT_EQ_INT(3000, (int)got);
    for (size_t i = 0; i < got; i++)
        ASSERT_EQ_INT(pattern(i), buf[i]);
    ASSERT_EQ_INT(0, iso_read(v, e.rsrc_extent, e.rsrc_size, 0, buf, sizeof(buf), &got));
    ASSERT_EQ_INT(100, (int)got);
    ASSERT_EQ_INT(0xEE, buf[0]);
    const char *inner[] = {"DIR", "inner.txt"};
    ASSERT_EQ_INT(0, iso_lookup(v, inner, 2, &e));
    ASSERT_EQ_INT(0, iso_read(v, e.extent, e.size, 0, buf, sizeof(buf), &got));
    ASSERT_TRUE(got == 5 && memcmp(buf, "inner", 5) == 0);
    const char *missing[] = {"DIR", "nope"};
    ASSERT_EQ_INT(-ENOENT, iso_lookup(v, missing, 2, &e));
    const char *through_file[] = {"README.TXT", "x"};
    ASSERT_EQ_INT(-ENOTDIR, iso_lookup(v, through_file, 2, &e));
    iso_close(v);
    gs_source_release(s);
}

// Joliet's UCS-2 names are preferred over the 8.3 ISO ones.
TEST(test_iso_joliet_names_win) {
    build(true, false);
    gs_source_t *s = src();
    iso_volume_t *v = iso_open_source(s, 0, sizeof(disc));
    ASSERT_TRUE(v != NULL);
    char list[256];
    names(v, 22, SEC, list, sizeof(list));
    ASSERT_TRUE(strcmp(list, "Read Me.txt, Folder") == 0);
    iso_dirent_t e;
    const char *path[] = {"folder", "INNER.TXT"}; // the Joliet tree's folder, the ISO tree below it
    ASSERT_EQ_INT(-ENOENT, iso_lookup(v, path, 2, &e)); // its records are ISO names, not Joliet
    const char *root[] = {"read me.txt"};
    ASSERT_EQ_INT(0, iso_lookup(v, root, 1, &e));
    ASSERT_EQ_INT(3000, (int)e.size);
    iso_close(v);
    gs_source_release(s);
}

// Rock Ridge NM entries name files when the root's "." says SUSP is in use.
TEST(test_iso_rock_ridge_names) {
    build(false, true);
    gs_source_t *s = src();
    iso_volume_t *v = iso_open_source(s, 0, sizeof(disc));
    ASSERT_TRUE(v != NULL);
    char list[256];
    names(v, 20, SEC, list, sizeof(list));
    ASSERT_TRUE(strcmp(list, "ReadMe.txt, DIR") == 0);
    iso_close(v);
    gs_source_release(s);
}

// Corrupt records are refused: an identifier running past its record, a
// file extent past the disc's end, a directory larger than the disc.
TEST(test_iso_corrupt_records_are_refused) {
    build(false, false);
    disc[20 * SEC + 34 + 34 + 32] = 200; // the third record's identifier runs past the record
    gs_source_t *s = src();
    iso_volume_t *v = iso_open_source(s, 0, sizeof(disc));
    ASSERT_TRUE(v != NULL);
    iso_dir_iter_t *it = iso_opendir(v, 20, SEC);
    iso_dirent_t e;
    int rc;
    while ((rc = iso_readdir_next(it, &e)) > 0)
        ;
    ASSERT_TRUE(rc < 0);
    iso_closedir(it);
    size_t got;
    uint8_t buf[16];
    ASSERT_EQ_INT(-EIO, iso_read(v, SECTORS + 5, 100, 0, buf, sizeof(buf), &got));
    ASSERT_TRUE(iso_opendir(v, 20, (uint64_t)SECTORS * SEC * 2) == NULL);
    iso_close(v);
    gs_source_release(s);

    build(false, false);
    disc[16 * SEC + 1] = 'X'; // no "CD001"
    s = src();
    ASSERT_TRUE(!iso_probe_source(s, 0, sizeof(disc)));
    ASSERT_TRUE(iso_open_source(s, 0, sizeof(disc)) == NULL);
    gs_source_release(s);
}

int main(void) {
    RUN(test_iso_plain_tree);
    RUN(test_iso_joliet_names_win);
    RUN(test_iso_rock_ridge_names);
    RUN(test_iso_corrupt_records_are_refused);
    fprintf(stderr, "All ISO 9660 tests passed\n");
    return 0;
}
