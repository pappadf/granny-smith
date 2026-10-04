// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// A writable mount with no delta directory keeps its sidecars off the media.
// See Makefile.

#include "image.h"
#include "image_scratch.h"
#include "test_assert.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLK    512u
#define BLOCKS 16u

static char g_media[64];
static char g_base[96];

// A private media directory holding one blank base image.
static void make_media(void) {
    strcpy(g_media, "/tmp/gs-image-sidecars-XXXXXX");
    ASSERT_TRUE(mkdtemp(g_media) != NULL);
    snprintf(g_base, sizeof(g_base), "%s/disk.img", g_media);
    FILE *f = fopen(g_base, "wb");
    ASSERT_TRUE(f != NULL);
    static uint8_t blk[BLK];
    for (unsigned i = 0; i < BLOCKS; i++)
        ASSERT_TRUE(fwrite(blk, 1, BLK, f) == BLK);
    fclose(f);
}

// Entries in `dir` other than "." and "..".
static int entries_in(const char *dir) {
    DIR *d = opendir(dir);
    ASSERT_TRUE(d != NULL);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            n++;
    closedir(d);
    return n;
}

// Mount the base writable with no delta directory, write a block, and check
// the instance landed under `want_dir` and nothing joined the base.
static void mount_and_check(const char *want_dir) {
    image_t *img = image_create(g_base, NULL);
    ASSERT_TRUE(img != NULL);
    uint8_t buf[BLK];
    memset(buf, 0x5A, sizeof(buf));
    ASSERT_EQ_INT((int)disk_write_data(img, 0, buf, sizeof(buf)), (int)sizeof(buf));

    char stem[512];
    snprintf(stem, sizeof(stem), "%s", image_path(img));
    size_t n = strlen(want_dir);
    ASSERT_TRUE(strncmp(stem, want_dir, n) == 0 && stem[n] == '/');
    ASSERT_EQ_INT(entries_in(g_media), 1); // the base alone

    image_close(img);
    char p[540];
    snprintf(p, sizeof(p), "%s.delta", stem);
    ASSERT_TRUE(access(p, F_OK) == 0); // a writable instance outlives the close
    unlink(p);
    snprintf(p, sizeof(p), "%s.journal", stem);
    unlink(p);
    ASSERT_EQ_INT(entries_in(g_media), 1);
}

TEST(test_cache_dir_takes_the_sidecars) {
    char cache[64];
    strcpy(cache, "/tmp/gs-image-sidecars-cache-XXXXXX");
    ASSERT_TRUE(mkdtemp(cache) != NULL);
    setenv("GS_STORAGE_CACHE", cache, 1);
    mount_and_check(cache);
    unsetenv("GS_STORAGE_CACHE");
    rmdir(cache);
}

// The case that used to drop them beside the base: no cache, no machine dir
// (a headless daemon or script run by hand).
TEST(test_no_cache_uses_the_scratch_root) {
    unsetenv("GS_STORAGE_CACHE");
    mount_and_check(image_scratch_dir());
}

int main(void) {
    make_media();
    RUN(test_cache_dir_takes_the_sidecars);
    RUN(test_no_cache_uses_the_scratch_root);
    unlink(g_base);
    rmdir(g_media);
    fprintf(stderr, "image_sidecars: all tests passed\n");
    return 0;
}
