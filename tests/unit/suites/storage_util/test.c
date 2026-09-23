// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Shared storage helpers.  See Makefile.

#include "storage_util.h"
#include "test_assert.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[64];

static void fresh_root(void) {
    strcpy(g_root, "/tmp/storage_util_test_XXXXXX");
    ASSERT_TRUE(mkdtemp(g_root) != NULL);
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0;
}

static void write_file(const char *p, const char *text) {
    FILE *f = fopen(p, "wb");
    ASSERT_TRUE(f != NULL);
    fputs(text, f);
    fclose(f);
}

TEST(test_mkdir_p_and_parents) {
    fresh_root();
    char *d = gs_str_printf("%s/a/b/c/", g_root);
    ASSERT_EQ_INT(0, gs_mkdir_p(d));
    ASSERT_EQ_INT(0, gs_mkdir_p(d)); // already there
    ASSERT_TRUE(is_dir(d));
    char *f = gs_str_printf("%s/x/y/file.bin", g_root);
    ASSERT_EQ_INT(0, gs_mkdir_parents(f));
    char *parent = gs_str_printf("%s/x/y", g_root);
    ASSERT_TRUE(is_dir(parent));
    ASSERT_TRUE(!exists(f)); // the file itself is not created
    ASSERT_EQ_INT(0, gs_rm_tree(g_root));
    ASSERT_TRUE(!exists(g_root));
    free(d);
    free(f);
    free(parent);
}

// rm -r removes a whole tree, and a symlink -- at the top or inside the tree
// -- is removed, never followed: storage.rm of a link to a directory used to
// open the link and empty the directory it pointed at (F-36).
TEST(test_rm_tree_does_not_follow_symlinks) {
    fresh_root();
    char *target = gs_str_printf("%s/target", g_root);
    char *keep = gs_str_printf("%s/target/keep.txt", g_root);
    char *tree = gs_str_printf("%s/tree/sub", g_root);
    char *inner = gs_str_printf("%s/tree/sub/link", g_root);
    char *top = gs_str_printf("%s/toplink", g_root);
    ASSERT_EQ_INT(0, gs_mkdir_p(target));
    write_file(keep, "keep");
    ASSERT_EQ_INT(0, gs_mkdir_p(tree));
    ASSERT_EQ_INT(0, symlink(target, inner));
    ASSERT_EQ_INT(0, symlink(target, top));

    ASSERT_EQ_INT(0, gs_rm_tree(top));
    ASSERT_TRUE(!exists(top));
    ASSERT_TRUE(exists(keep));

    char *tree_root = gs_str_printf("%s/tree", g_root);
    ASSERT_EQ_INT(0, gs_rm_tree(tree_root));
    ASSERT_TRUE(!exists(tree_root));
    ASSERT_TRUE(exists(keep));

    ASSERT_EQ_INT(0, gs_rm_tree("/tmp/storage_util_test_no_such_path"));
    ASSERT_EQ_INT(0, gs_rm_tree(g_root));
    free(target);
    free(keep);
    free(tree);
    free(inner);
    free(top);
    free(tree_root);
}

// Every read states its cap, and a larger file is refused before anything
// is allocated for it (F-57).  An empty file reads as a non-NULL buffer.
TEST(test_read_file_is_capped) {
    fresh_root();
    char *p = gs_str_printf("%s/f", g_root);
    write_file(p, "0123456789");
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(-EFBIG, gs_read_file(p, 9, &buf, &len));
    ASSERT_TRUE(buf == NULL);
    ASSERT_EQ_INT(0, gs_read_file(p, 10, &buf, &len));
    ASSERT_EQ_INT(10, (int)len);
    ASSERT_TRUE(memcmp(buf, "0123456789", 10) == 0);
    free(buf);
    write_file(p, "");
    ASSERT_EQ_INT(0, gs_read_file(p, 10, &buf, &len));
    ASSERT_TRUE(buf != NULL && len == 0);
    free(buf);
    ASSERT_EQ_INT(-ENOENT, gs_read_file("/tmp/storage_util_test_missing", 10, &buf, &len));
    free(p);
    ASSERT_EQ_INT(0, gs_rm_tree(g_root));
}

// One escaper for both former copies' escape sets, and it refuses rather
// than truncates.
TEST(test_json_escape) {
    char out[64];
    ASSERT_EQ_INT(21, gs_json_escape("a\"b\\c\n\t\b\f\x01", out, sizeof(out)));
    ASSERT_TRUE(strcmp(out, "a\\\"b\\\\c\\n\\t\\b\\f\\u0001") == 0);
    ASSERT_EQ_INT(-EINVAL, gs_json_escape("abcd", out, 4)); // no room for the NUL
    ASSERT_EQ_INT(3, gs_json_escape("abc", out, 4));
    ASSERT_EQ_INT(-EINVAL, gs_json_escape("\x01", out, 6)); // \u0001 needs 7 with NUL
    char *dup = gs_json_escape_dup("x\"y");
    ASSERT_TRUE(dup && strcmp(dup, "x\\\"y") == 0);
    free(dup);
    dup = gs_json_escape_dup(NULL);
    ASSERT_TRUE(dup && strcmp(dup, "") == 0);
    free(dup);
}

int main(void) {
    RUN(test_mkdir_p_and_parents);
    RUN(test_rm_tree_does_not_follow_symlinks);
    RUN(test_read_file_is_capped);
    RUN(test_json_escape);
    fprintf(stderr, "All storage_util tests passed\n");
    return 0;
}
