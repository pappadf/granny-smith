// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The decoded-image scratch cache.  See Makefile.

#include "image_scratch.h"
#include "test_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[64];

static void use_fresh_root(void) {
    strcpy(g_root, "/tmp/image_scratch_test_XXXXXX");
    ASSERT_TRUE(mkdtemp(g_root) != NULL);
    setenv("GS_STORAGE_CACHE", g_root, 1);
}

static void write_bytes(const char *path, size_t n) {
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    for (size_t i = 0; i < n; i++)
        fputc(0, f);
    fclose(f);
}

static void remove_tree(const char *scratch) {
    char side[512];
    snprintf(side, sizeof(side), "%s.id", scratch);
    remove(side);
    remove(scratch);
}

// Names live under GS_STORAGE_CACHE, are the same for the same identity and
// differ for different ones.
TEST(test_path_is_under_the_cache_root_and_keyed_on_identity) {
    use_fresh_root();
    char a[512], a2[512], b[512];
    ASSERT_TRUE(image_scratch_path("udif",
                                   "udif\x1f/x.dmg\x1f"
                                   "100:5",
                                   a, sizeof(a)));
    ASSERT_TRUE(image_scratch_path("udif",
                                   "udif\x1f/x.dmg\x1f"
                                   "100:5",
                                   a2, sizeof(a2)));
    ASSERT_TRUE(image_scratch_path("udif",
                                   "udif\x1f/x.dmg\x1f"
                                   "100:6",
                                   b, sizeof(b)));
    ASSERT_TRUE(strncmp(a, g_root, strlen(g_root)) == 0);
    ASSERT_TRUE(strcmp(a, a2) == 0);
    ASSERT_TRUE(strcmp(a, b) != 0);
    ASSERT_TRUE(strcmp(image_scratch_dir(), g_root) == 0);
    rmdir(g_root);
}

// A file is trusted only once sealed with the same identity and at the
// expected size.  In particular a full-size file with no seal -- what an
// interrupted decode leaves, since decoders pre-extend the file -- is not
// reused; reuse used to be decided by size alone.
TEST(test_only_a_sealed_matching_file_is_valid) {
    use_fresh_root();
    const char *id = "ndif\x1f/disk.img\x1f"
                     "4096:77";
    char p[512];
    ASSERT_TRUE(image_scratch_path("ndif", id, p, sizeof(p)));
    ASSERT_TRUE(!image_scratch_valid(p, id, 4096)); // nothing there

    ASSERT_EQ_INT(0, image_scratch_prepare(p));
    write_bytes(p, 4096);
    ASSERT_TRUE(!image_scratch_valid(p, id, 4096)); // complete size, not sealed

    ASSERT_EQ_INT(0, image_scratch_seal(p, id));
    ASSERT_TRUE(image_scratch_valid(p, id, 4096));
    ASSERT_TRUE(image_scratch_valid(p, id, 0)); // 0: any size
    ASSERT_TRUE(!image_scratch_valid(p, id, 8192)); // wrong size
    ASSERT_TRUE(!image_scratch_valid(p,
                                     "ndif\x1f/disk.img\x1f"
                                     "4096:78",
                                     4096)); // another source
    ASSERT_TRUE(!image_scratch_valid(p,
                                     "ndif\x1f/disk.img\x1f"
                                     "4096:7",
                                     4096)); // a prefix of the seal

    ASSERT_EQ_INT(0, image_scratch_prepare(p)); // about to rewrite: no longer trusted
    ASSERT_TRUE(!image_scratch_valid(p, id, 4096));
    remove_tree(p);
    rmdir(g_root);
}

// prepare creates the directories a tag with a '/' asks for.
TEST(test_prepare_creates_subdirectories) {
    use_fresh_root();
    char p[512];
    ASSERT_TRUE(image_scratch_path("nested/img", "nested\x1fid", p, sizeof(p)));
    ASSERT_EQ_INT(0, image_scratch_prepare(p));
    write_bytes(p, 1);
    ASSERT_EQ_INT(0, image_scratch_seal(p, "nested\x1fid"));
    ASSERT_TRUE(image_scratch_valid(p, "nested\x1fid", 1));
    remove_tree(p);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/nested", g_root);
    rmdir(dir);
    rmdir(g_root);
}

int main(void) {
    RUN(test_path_is_under_the_cache_root_and_keyed_on_identity);
    RUN(test_only_a_sealed_matching_file_is_valid);
    RUN(test_prepare_creates_subdirectories);
    fprintf(stderr, "All image_scratch tests passed\n");
    return 0;
}
