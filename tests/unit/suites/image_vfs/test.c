// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The image VFS backend over in-memory HFS+ volumes.  See Makefile.

#include "hfsplus_builder.h"
#include "image.h"
#include "image_vfs.h"
#include "test_assert.h"
#include "vfs.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- The in-memory image, and the image.c entry points image_vfs uses -----

#define IMG_CAP (64u * HFSB_BLOCK)
static uint8_t g_img[IMG_CAP];
static size_t g_img_size;
static int g_token; // the image_t* handed out: an address, never dereferenced

image_t *image_open_readonly(const char *path) {
    (void)path;
    return (image_t *)&g_token;
}
void image_close(image_t *img) {
    (void)img;
}
size_t disk_size(image_t *img) {
    (void)img;
    return g_img_size;
}
size_t disk_read_data(image_t *img, size_t offset, uint8_t *buf, size_t size) {
    (void)img;
    if (offset > g_img_size || size > g_img_size - offset)
        return 0;
    memcpy(buf, g_img + offset, size);
    return size;
}

// image_vfs keys mounts on the host path, which it canonicalises and stats,
// so a real (empty) file stands in for the image; its bytes are never read.
static char g_host[64];

static image_mount_t *mount_volume(const hfsb_file_t *files, int n) {
    image_vfs_reset();
    g_img_size = hfsb_build(g_img, sizeof(g_img), "Vol", files, n);
    ASSERT_TRUE(g_img_size > 0);
    strcpy(g_host, "/tmp/image_vfs_test_XXXXXX");
    int fd = mkstemp(g_host);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    image_mount_t *m = NULL;
    int rc = image_vfs_acquire_mount(g_host, &m);
    if (rc != 0)
        fprintf(stderr, "  acquire_mount: %d\n", rc);
    ASSERT_EQ_INT(0, rc);
    return m;
}

static void unmount_volume(void) {
    image_vfs_reset();
    unlink(g_host);
}

// A resource fork holding one 'TEXT' 128 resource with `text`.
static size_t text_rsrc(uint8_t *out, size_t cap, const char *text) {
    size_t n = rforkb_build(out, cap, 0x54455854 /* 'TEXT' */, 128, (const uint8_t *)text, strlen(text));
    ASSERT_TRUE(n > 0);
    return n;
}

static int read_all(const vfs_backend_t *be, image_mount_t *m, const char *path, char *buf, size_t cap, size_t *got) {
    vfs_file_t *f = NULL;
    int rc = be->open(m, path, &f);
    if (rc != 0)
        return rc;
    rc = be->read(f, 0, buf, cap, got);
    be->close(f);
    return rc;
}

// ---- Tests -----------------------------------------------------------------

// The fixture and the harness are right: a file's data fork and one of its
// resources read back through the backend.
TEST(test_reads_a_data_fork_and_a_resource) {
    static uint8_t rsrc[1024];
    size_t rlen = text_rsrc(rsrc, sizeof(rsrc), "resource!");
    hfsb_file_t files[] = {
        {.name = "A", .data = (const uint8_t *)"data", .data_len = 4, .rsrc = rsrc, .rsrc_len = rlen}
    };
    image_mount_t *m = mount_volume(files, 1);
    const vfs_backend_t *be = vfs_image_backend();

    char buf[64] = {0};
    size_t got = 0;
    ASSERT_EQ_INT(0, read_all(be, m, "/partition1/A", buf, sizeof(buf), &got));
    ASSERT_EQ_INT(4, (int)got);
    ASSERT_TRUE(memcmp(buf, "data", 4) == 0);

    memset(buf, 0, sizeof(buf));
    ASSERT_EQ_INT(0, read_all(be, m, "/partition1/A/rsrc/TEXT/128", buf, sizeof(buf), &got));
    ASSERT_EQ_INT(9, (int)got);
    ASSERT_TRUE(memcmp(buf, "resource!", 9) == 0);
    unmount_volume();
}

// F-42: an open resource file borrows bytes from the resource-fork cache, and
// the cache (8 entries) evicted without asking.  Open file 0's resource, touch
// eight other files' forks so the ninth acquire evicts file 0's entry, then
// read the handle: the unfixed backend reads freed memory (ASan:
// heap-use-after-free).  A borrowed entry must stay alive while borrowed.
TEST(test_open_resource_survives_cache_pressure) {
    static uint8_t rsrc[9][1024];
    static char names[9][4];
    hfsb_file_t files[9];
    for (int i = 0; i < 9; i++) {
        char text[16];
        snprintf(text, sizeof(text), "res-%d", i);
        snprintf(names[i], sizeof(names[i]), "F%d", i);
        files[i] = (hfsb_file_t){.name = names[i],
                                 .data = (const uint8_t *)"d",
                                 .data_len = 1,
                                 .rsrc = rsrc[i],
                                 .rsrc_len = text_rsrc(rsrc[i], sizeof(rsrc[i]), text)};
    }
    image_mount_t *m = mount_volume(files, 9);
    const vfs_backend_t *be = vfs_image_backend();

    vfs_file_t *h0 = NULL;
    ASSERT_EQ_INT(0, be->open(m, "/partition1/F0/rsrc/TEXT/128", &h0));
    for (int i = 1; i < 9; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/partition1/F%d/rsrc/TEXT/128", i);
        vfs_stat_t st;
        ASSERT_EQ_INT(0, be->stat(m, path, &st));
    }
    char buf[16] = {0};
    size_t got = 0;
    ASSERT_EQ_INT(0, be->read(h0, 0, buf, sizeof(buf), &got));
    ASSERT_EQ_INT(5, (int)got);
    ASSERT_TRUE(memcmp(buf, "res-0", 5) == 0);
    be->close(h0);
    unmount_volume();
}

// Nine files with one 'TEXT' 128 resource each ("res-0".."res-8"), mounted.
static image_mount_t *mount_nine(void) {
    static uint8_t rsrc[9][1024];
    static char names[9][4];
    static hfsb_file_t files[9];
    for (int i = 0; i < 9; i++) {
        char text[16];
        snprintf(text, sizeof(text), "res-%d", i);
        snprintf(names[i], sizeof(names[i]), "F%d", i);
        files[i] = (hfsb_file_t){.name = names[i],
                                 .data = (const uint8_t *)"d",
                                 .data_len = 1,
                                 .rsrc = rsrc[i],
                                 .rsrc_len = text_rsrc(rsrc[i], sizeof(rsrc[i]), text)};
    }
    return mount_volume(files, 9);
}

// The same, for the other borrower: a resource directory walks the cached
// fork's parsed map.  Open F0's /rsrc as a directory, put eight other forks
// through the cache, then read the directory.
TEST(test_open_resource_directory_survives_cache_pressure) {
    image_mount_t *m = mount_nine();
    const vfs_backend_t *be = vfs_image_backend();
    vfs_dir_t *d = NULL;
    ASSERT_EQ_INT(0, be->opendir(m, "/partition1/F0/rsrc", &d));
    for (int i = 1; i < 9; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/partition1/F%d/rsrc/TEXT/128", i);
        vfs_stat_t st;
        ASSERT_EQ_INT(0, be->stat(m, path, &st));
    }
    vfs_dirent_t ent;
    ASSERT_EQ_INT(1, be->readdir(d, &ent));
    ASSERT_TRUE(strcmp(ent.name, "TEXT") == 0);
    be->closedir(d);
    unmount_volume();
}

// Eight open handles pin all eight entries: a ninth fork cannot be cached,
// and must be refused -- never made room for by freeing a borrowed one.  Once
// a handle closes, the ninth fork opens.
TEST(test_all_entries_pinned_refuses_a_ninth_fork) {
    image_mount_t *m = mount_nine();
    const vfs_backend_t *be = vfs_image_backend();
    vfs_file_t *h[8];
    for (int i = 0; i < 8; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/partition1/F%d/rsrc/TEXT/128", i);
        ASSERT_EQ_INT(0, be->open(m, path, &h[i]));
    }
    vfs_file_t *h8 = NULL;
    ASSERT_TRUE(be->open(m, "/partition1/F8/rsrc/TEXT/128", &h8) != 0);
    be->close(h[0]);
    ASSERT_EQ_INT(0, be->open(m, "/partition1/F8/rsrc/TEXT/128", &h8));
    char buf[8] = {0};
    size_t got = 0;
    ASSERT_EQ_INT(0, be->read(h8, 0, buf, sizeof(buf), &got));
    ASSERT_TRUE(memcmp(buf, "res-8", 5) == 0);
    for (int i = 1; i < 8; i++) {
        ASSERT_EQ_INT(0, be->read(h[i], 0, buf, sizeof(buf), &got)); // still intact
        ASSERT_TRUE(buf[4] == '0' + i);
        be->close(h[i]);
    }
    be->close(h8);
    unmount_volume();
}

// F-26: the resource fork's size comes from the catalog and was malloc'd as
// given.  A catalog claiming 3 GiB (the volume holds a few hundred bytes of
// fork) must be refused before anything that size is allocated; under this
// suite's 256 MB ASan ceiling the unfixed backend's malloc aborts the run.
TEST(test_huge_resource_fork_is_refused_before_allocating) {
    static uint8_t rsrc[1024];
    size_t rlen = text_rsrc(rsrc, sizeof(rsrc), "x");
    hfsb_file_t files[] = {
        {.name = "Big",
         .data = (const uint8_t *)"d",
         .data_len = 1,
         .rsrc = rsrc,
         .rsrc_len = rlen,
         .rsrc_logical_override = 3ull << 30}
    };
    image_mount_t *m = mount_volume(files, 1);
    const vfs_backend_t *be = vfs_image_backend();
    vfs_stat_t st;
    ASSERT_TRUE(be->stat(m, "/partition1/Big/rsrc/TEXT/128", &st) != 0);
    unmount_volume();
}

int main(void) {
    RUN(test_reads_a_data_fork_and_a_resource);
    RUN(test_open_resource_survives_cache_pressure);
    RUN(test_open_resource_directory_survives_cache_pressure);
    RUN(test_all_entries_pinned_refuses_a_ninth_fork);
    RUN(test_huge_resource_fork_is_refused_before_allocating);
    fprintf(stderr, "All image_vfs tests passed\n");
    return 0;
}
