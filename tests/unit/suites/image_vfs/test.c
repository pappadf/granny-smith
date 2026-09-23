// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The image VFS backend over in-memory HFS+ volumes.  See Makefile.

#include "hfsplus_builder.h"
#include "image.h"
#include "image_vfs.h"
#include "test_assert.h"
#include "vfs.h"

#include <errno.h>
#include <limits.h>
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
static uint32_t g_block_size = 512; // the geometry the "image" was opened with
static int g_misaligned_reads; // reads the real disk_read_data would assert on

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
uint32_t disk_block_size(image_t *img) {
    (void)img;
    return g_block_size;
}
size_t disk_read_data(image_t *img, size_t offset, uint8_t *buf, size_t size) {
    (void)img;
    // The real one works in whole blocks of the image's own size and asserts
    // on anything else -- an assert that returns, then reads on.
    if (offset % g_block_size || size % g_block_size) {
        g_misaligned_reads++;
        return 0;
    }
    if (offset > g_img_size || size > g_img_size - offset)
        return 0;
    memcpy(buf, g_img + offset, size);
    return size;
}

// The canonical path image.c reports open writable, or "" for none.
static char g_writable[PATH_MAX];

bool image_path_is_open_writable(const char *canonical_path) {
    return g_writable[0] && strcmp(canonical_path, g_writable) == 0;
}

// image_vfs keys mounts on the host path, which it canonicalises and stats,
// so a real (empty) file stands in for the image; its bytes are never read.
static char g_host[64];
static char g_host_canon[PATH_MAX]; // the key image_vfs holds it under

// Build the volume and its stand-in file, without mounting it.
static void make_volume(const hfsb_file_t *files, int n) {
    g_writable[0] = 0;
    g_img_size = hfsb_build(g_img, sizeof(g_img), "Vol", files, n);
    ASSERT_TRUE(g_img_size > 0);
    strcpy(g_host, "/tmp/image_vfs_test_XXXXXX");
    int fd = mkstemp(g_host);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    ASSERT_TRUE(realpath(g_host, g_host_canon) != NULL);
}

static image_mount_t *mount_volume(const hfsb_file_t *files, int n) {
    make_volume(files, n);
    image_mount_t *m = NULL;
    int rc = image_vfs_acquire_mount(g_host, &m);
    if (rc != 0)
        fprintf(stderr, "  acquire_mount: %d\n", rc);
    ASSERT_EQ_INT(0, rc);
    return m;
}

// Every test closes its handles first, so the unmount is immediate.
static void unmount_volume(void) {
    g_writable[0] = 0;
    ASSERT_EQ_INT(0, image_vfs_unmount(g_host_canon));
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

// ---- The emulator holding the file writable (F-39, F-40, F-41) -------------

static const hfsb_file_t one_file[] = {
    {.name = "A", .data = (const uint8_t *)"data", .data_len = 4}
};

// While image.c reports the file open writable -- an attached disk, whose
// writes land in a delta this mount cannot see -- every call refuses with
// -EBUSY, a handle opened earlier included.  Once it is no longer open, the
// same mount serves again.  The flag this replaced was set on attach and
// never cleared: the detach notification had no caller (F-39).
TEST(test_busy_exactly_while_open_writable) {
    image_mount_t *m = mount_volume(one_file, 1);
    const vfs_backend_t *be = vfs_image_backend();
    vfs_file_t *f = NULL;
    ASSERT_EQ_INT(0, be->open(m, "/partition1/A", &f));

    strcpy(g_writable, g_host_canon);
    vfs_stat_t st;
    char buf[8];
    size_t got = 0;
    vfs_dir_t *d = NULL;
    image_mount_t *again = NULL;
    ASSERT_EQ_INT(-EBUSY, be->stat(m, "/partition1/A", &st));
    ASSERT_EQ_INT(-EBUSY, be->opendir(m, "/partition1", &d));
    ASSERT_EQ_INT(-EBUSY, be->read(f, 0, buf, sizeof(buf), &got));
    ASSERT_EQ_INT(-EBUSY, image_vfs_acquire_mount(g_host, &again));

    g_writable[0] = 0;
    ASSERT_EQ_INT(0, be->stat(m, "/partition1/A", &st));
    ASSERT_EQ_INT(0, be->read(f, 0, buf, sizeof(buf), &got));
    ASSERT_EQ_INT(4, (int)got);
    ASSERT_EQ_INT(0, image_vfs_acquire_mount(g_host, &again));
    ASSERT_TRUE(again == m);
    be->close(f);
    unmount_volume();
}

// A file attached before the VFS ever mounted it is refused on the first
// acquire.  A notification sent at attach time found no mount to mark, so
// the first mount after it was made -- and served -- as if nothing were open.
TEST(test_open_writable_before_first_mount_refuses) {
    make_volume(one_file, 1);
    strcpy(g_writable, g_host_canon);
    image_mount_t *m = NULL;
    ASSERT_EQ_INT(-EBUSY, image_vfs_acquire_mount(g_host, &m));
    g_writable[0] = 0;
    ASSERT_EQ_INT(0, image_vfs_acquire_mount(g_host, &m));
    unmount_volume();
}

// An unmount asked for while a handle is open refuses new calls, and the
// last handle's close completes it.  It used to leave the mount in the table,
// refusing everything, for the rest of the process.
TEST(test_pending_unmount_completes_on_last_close) {
    image_mount_t *m = mount_volume(one_file, 1);
    const vfs_backend_t *be = vfs_image_backend();
    vfs_file_t *f = NULL;
    ASSERT_EQ_INT(0, be->open(m, "/partition1/A", &f));
    ASSERT_EQ_INT(-EBUSY, image_vfs_unmount(g_host_canon));
    image_mount_t *again = NULL;
    ASSERT_EQ_INT(-EBUSY, image_vfs_acquire_mount(g_host, &again));
    be->close(f);
    int rc = image_vfs_unmount(g_host_canon);
    ASSERT_EQ_INT(-ENOENT, rc); // already gone
    ASSERT_EQ_INT(0, image_vfs_acquire_mount(g_host, &again));
    unmount_volume();
}

// ---- Path length (F-44) -----------------------------------------------------

// An in-image path longer than the resolver's buffer was truncated and
// resolved anyway.  "/partition1/A", then slashes to past the buffer, then
// "B" asks for B under A -- which, A being a file, does not exist -- but
// truncated to its first 1023 bytes it is "A" and a run of trailing slashes,
// which trim to A: the stat succeeded, for a file nobody asked for.
TEST(test_overlong_path_is_refused_not_truncated) {
    image_mount_t *m = mount_volume(one_file, 1);
    const vfs_backend_t *be = vfs_image_backend();
    static char path[1200];
    strcpy(path, "/partition1/A");
    size_t len = strlen(path);
    memset(path + len, '/', sizeof(path) - len - 2);
    strcpy(path + sizeof(path) - 2, "B");
    vfs_stat_t st;
    ASSERT_EQ_INT(-ENAMETOOLONG, be->stat(m, path, &st));
    unmount_volume();
}

// ---- Geometry (F-49) --------------------------------------------------------

// An image opened with 532-byte blocks (a Lisa ProFile) has no partition map
// or HFS volume in 512-byte terms.  It is refused as not an image, without
// a single read disk_read_data would reject as misaligned.
TEST(test_non_512_geometry_is_refused_cleanly) {
    make_volume(one_file, 1);
    g_block_size = 532;
    g_misaligned_reads = 0;
    image_mount_t *m = NULL;
    ASSERT_EQ_INT(-ENOTDIR, image_vfs_acquire_mount(g_host, &m));
    ASSERT_EQ_INT(0, g_misaligned_reads);
    g_block_size = 512;
    unlink(g_host);
}

// ---- Nested-image scratch copies (F-33, F-64) -------------------------------

static void read_file(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
}

// A file inside a mounted volume is copied out under the scratch root --
// GS_STORAGE_CACHE when set, which the nested cache used to ignore (a fixed
// /tmp/gs-image-ro/nested) -- and reused on the next call.  A copy that was
// never sealed complete is not reused: reuse used to need only a non-empty
// file, so an interrupted copy was served as the image.
TEST(test_nested_copy_honours_the_cache_root_and_its_seal) {
    char root[64] = "/tmp/image_vfs_cache_XXXXXX";
    ASSERT_TRUE(mkdtemp(root) != NULL);
    setenv("GS_STORAGE_CACHE", root, 1);
    image_mount_t *m = mount_volume(one_file, 1);

    char *p1 = image_vfs_materialize_nested(m, "/partition1/A");
    ASSERT_TRUE(p1 != NULL);
    ASSERT_TRUE(strncmp(p1, root, strlen(root)) == 0);
    char buf[16];
    read_file(p1, buf, sizeof(buf));
    ASSERT_TRUE(strcmp(buf, "data") == 0);

    char *p2 = image_vfs_materialize_nested(m, "/partition1/A");
    ASSERT_TRUE(p2 != NULL && strcmp(p1, p2) == 0);

    // What an interrupted copy leaves: the file, but no seal.
    char side[PATH_MAX];
    snprintf(side, sizeof(side), "%s.id", p1);
    ASSERT_EQ_INT(0, remove(side));
    FILE *f = fopen(p1, "wb");
    ASSERT_TRUE(f != NULL);
    fputs("junk", f);
    fclose(f);
    char *p3 = image_vfs_materialize_nested(m, "/partition1/A");
    ASSERT_TRUE(p3 != NULL);
    read_file(p3, buf, sizeof(buf));
    ASSERT_TRUE(strcmp(buf, "data") == 0);

    remove(side);
    remove(p1);
    free(p1);
    free(p2);
    free(p3);
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/nested", root);
    rmdir(dir);
    rmdir(root);
    unsetenv("GS_STORAGE_CACHE");
    unmount_volume();
}

int main(void) {
    RUN(test_reads_a_data_fork_and_a_resource);
    RUN(test_open_resource_survives_cache_pressure);
    RUN(test_open_resource_directory_survives_cache_pressure);
    RUN(test_all_entries_pinned_refuses_a_ninth_fork);
    RUN(test_huge_resource_fork_is_refused_before_allocating);
    RUN(test_busy_exactly_while_open_writable);
    RUN(test_open_writable_before_first_mount_refuses);
    RUN(test_pending_unmount_completes_on_last_close);
    RUN(test_overlong_path_is_refused_not_truncated);
    RUN(test_non_512_geometry_is_refused_cleanly);
    RUN(test_nested_copy_honours_the_cache_root_and_its_seal);
    fprintf(stderr, "All image_vfs tests passed\n");
    return 0;
}
