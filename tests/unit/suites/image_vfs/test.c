// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The image VFS backend over in-memory HFS+ volumes.  See Makefile.

#include "hfsplus_builder.h"
#include "image.h"
#include "image_vfs.h"
#include "namespace.h"
#include "source.h"
#include "test_assert.h"
#include "vfs.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- realpath(), failing on demand (native builds; see Makefile) ----------

#ifdef TEST_WRAP_REALPATH
// 0, or the errno every realpath() call fails with.
static int g_realpath_errno;
char *__real_realpath(const char *path, char *resolved);
char *__wrap_realpath(const char *path, char *resolved) {
    if (g_realpath_errno) {
        errno = g_realpath_errno;
        return NULL;
    }
    return __real_realpath(path, resolved);
}
#endif

// ---- The volume on disk, and the image.c entry point image_vfs uses ------

#define IMG_CAP (64u * HFSB_BLOCK)
static uint8_t g_img[IMG_CAP];
static size_t g_img_size;

// The source key image.c reports open writable, or "" for none.
static char g_writable[PATH_MAX + 64];

// With image.c's own rule: the key itself, or anything inside it.
bool image_key_is_open_writable(const char *key) {
    return g_writable[0] && source_key_within(key, g_writable);
}

// The volume lives in a real host file, mounted like any other.
static char g_host[64];
static char g_host_canon[PATH_MAX]; // the path image_vfs mounts it under
static char g_host_key[PATH_MAX + 64]; // its source's key: what "attached writable" names

// Write `len` bytes of `img` to a fresh temp file named into g_host.
static void write_volume_file(const uint8_t *img, size_t len) {
    strcpy(g_host, "/tmp/image_vfs_test_XXXXXX");
    int fd = mkstemp(g_host);
    ASSERT_TRUE(fd >= 0);
    ASSERT_TRUE(write(fd, img, len) == (ssize_t)len);
    close(fd);
    ASSERT_TRUE(realpath(g_host, g_host_canon) != NULL);
    source_t *s = source_host(g_host, NULL);
    ASSERT_TRUE(s != NULL);
    snprintf(g_host_key, sizeof(g_host_key), "%s", source_key(s));
    source_release(s);
}

// Build the volume and its file, without mounting it.
static void make_volume(const hfsb_file_t *files, int n) {
    g_writable[0] = 0;
    ns_register_formats();
    g_img_size = hfsb_build(g_img, sizeof(g_img), "Vol", files, n);
    ASSERT_TRUE(g_img_size > 0);
    write_volume_file(g_img, g_img_size);
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

// An open resource file borrows bytes from the resource-fork cache, and the
// cache (8 entries) evicted without asking.  Open file 0's resource, touch
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

// The resource fork's size comes from the catalog and was malloc'd as given.  A
// catalog claiming 3 GiB (the volume holds a few hundred bytes of fork) must be
// refused before anything that size is allocated; under this suite's 256 MB
// ASan ceiling the unfixed backend's malloc aborts the run.
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

// ---- The emulator holding the file writable --------------------------------

static const hfsb_file_t one_file[] = {
    {.name = "A", .data = (const uint8_t *)"data", .data_len = 4}
};

// While image.c reports the file open writable -- an attached disk, whose
// writes land in a delta this mount cannot see -- every call refuses with
// -EBUSY, a handle opened earlier included.  Once it is no longer open, the
// same mount serves again.  The flag this replaced was set on attach and
// never cleared: the detach notification had no caller.
TEST(test_busy_exactly_while_open_writable) {
    image_mount_t *m = mount_volume(one_file, 1);
    const vfs_backend_t *be = vfs_image_backend();
    vfs_file_t *f = NULL;
    ASSERT_EQ_INT(0, be->open(m, "/partition1/A", &f));

    strcpy(g_writable, g_host_key);
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
    strcpy(g_writable, g_host_key);
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

// ---- Path length ------------------------------------------------------------

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

// ---- Not an image -----------------------------------------------------------

// A file that is no disk and no archive is refused as not an image.
TEST(test_non_image_is_refused_cleanly) {
    ns_register_formats();
    static uint8_t junk[4096];
    memset(junk, 0x5A, sizeof(junk));
    write_volume_file(junk, sizeof(junk));
    image_mount_t *m = NULL;
    ASSERT_EQ_INT(-ENOTDIR, image_vfs_acquire_mount(g_host, &m));
    unlink(g_host);
}

#ifdef TEST_WRAP_REALPATH
// A realpath() that fails for a reason other than a missing file (a backend,
// such as one under WasmFS, that cannot answer it) does not stop the mount:
// the path is used as given, as image.c and source.c use it, and the mount
// is found under that spelling.  A missing file is still refused.
TEST(test_realpath_failure_falls_back_to_the_raw_path) {
    make_volume(one_file, 1);
    image_mount_t *m = NULL;
    g_realpath_errno = EINVAL;
    int rc = image_vfs_acquire_mount(g_host, &m);
    ASSERT_EQ_INT(0, rc);
    ASSERT_TRUE(m != NULL);
    vfs_stat_t st;
    rc = vfs_image_backend()->stat(m, "/partition1/A", &st);
    ASSERT_EQ_INT(0, rc);
    rc = image_vfs_unmount(g_host);
    ASSERT_EQ_INT(0, rc);

    g_realpath_errno = ENOENT;
    m = NULL;
    rc = image_vfs_acquire_mount(g_host, &m);
    ASSERT_EQ_INT(-ENOENT, rc);
    g_realpath_errno = 0;
    unlink(g_host);
}
#endif

// ---- Nesting ------------------------------------------------------------------

// A volume inside a file of another volume mounts straight from the outer
// file's source: nothing is copied out, and the inner mount's key names the
// file inside the outer one.  Attaching the outer volume writable makes the
// inner mount busy too.
TEST(test_nested_volume_mounts_from_its_source) {
    static uint8_t inner[IMG_CAP / 2];
    hfsb_file_t inner_files[] = {
        {.name = "Deep", .data = (const uint8_t *)"inner!", .data_len = 6}
    };
    size_t inner_len = hfsb_build(inner, sizeof(inner), "Inner", inner_files, 1);
    ASSERT_TRUE(inner_len > 0);
    hfsb_file_t outer_files[] = {
        {.name = "Inner.img", .data = inner, .data_len = inner_len}
    };
    image_mount_t *m = mount_volume(outer_files, 1);

    int err = 0;
    source_t *data = image_vfs_open_source(m, "/partition1/Inner.img", GS_FORK_DATA, &err);
    ASSERT_TRUE(data != NULL);
    ASSERT_EQ_INT((int)inner_len, (int)source_size(data));
    char want_key[PATH_MAX + 128];
    snprintf(want_key, sizeof(want_key), "%s/partition1/Inner.img", g_host_key);
    ASSERT_TRUE(strcmp(source_key(data), want_key) == 0);

    image_mount_t *nm = NULL;
    ASSERT_EQ_INT(0, image_vfs_acquire_mount_source("/x/Inner.img", data, NULL, &nm));
    source_release(data);
    char buf[16] = {0};
    size_t got = 0;
    ASSERT_EQ_INT(0, read_all(vfs_image_backend(), nm, "/partition1/Deep", buf, sizeof(buf), &got));
    ASSERT_EQ_INT(6, (int)got);
    ASSERT_TRUE(memcmp(buf, "inner!", 6) == 0);

    // The outer volume attached writable: the inner mount refuses too.
    strcpy(g_writable, g_host_key);
    vfs_stat_t st;
    ASSERT_EQ_INT(-EBUSY, vfs_image_backend()->stat(nm, "/partition1/Deep", &st));
    g_writable[0] = 0;

    ASSERT_EQ_INT(0, image_vfs_unmount("/x/Inner.img"));
    unmount_volume();
}

// ---- The image.c entry points image_part.c links against -----------------
// (its image-backed helpers are not used here: every mount reads a source)

size_t disk_size(image_t *img) {
    (void)img;
    return 0;
}
uint32_t disk_block_size(image_t *img) {
    (void)img;
    return 512;
}
size_t disk_read_data(image_t *img, size_t offset, uint8_t *buf, size_t size) {
    (void)img;
    (void)offset;
    (void)buf;
    (void)size;
    return 0;
}

// ---- Names the VFS also synthesises ------------------------------------------

// A file literally named "rsrc" or "finf" resolves as itself: those names
// anchor a fork only after the name of an existing file.  At the volume root
// the anchor's prefix is the partition, a directory, and the lookup used to
// stop there with -ENOENT.  The partition name needs its digits: "partition 1"
// is no partition.
static const hfsb_file_t named_like_forks[] = {
    {.name = "rsrc", .data = (const uint8_t *)"literal", .data_len = 7},
    {.name = "finf", .data = (const uint8_t *)"also",    .data_len = 4},
};

TEST(test_fork_names_resolve_literally_where_no_file_precedes) {
    image_mount_t *m = mount_volume(named_like_forks, 2);
    const vfs_backend_t *be = vfs_image_backend();
    char buf[16];
    size_t got = 0;
    ASSERT_EQ_INT(0, read_all(be, m, "/partition1/rsrc", buf, sizeof(buf), &got));
    ASSERT_EQ_INT(7, (int)got);
    ASSERT_TRUE(memcmp(buf, "literal", 7) == 0);
    vfs_stat_t st;
    ASSERT_EQ_INT(0, be->stat(m, "/partition1/finf", &st));
    ASSERT_EQ_INT(VFS_MODE_FILE, (int)st.mode);
    ASSERT_EQ_INT(4, (int)st.size);
    ASSERT_EQ_INT(-ENOENT, be->stat(m, "/partition 1", &st));
    // Read-only by flag: the writers are vfs.c's to refuse.
    ASSERT_TRUE((be->flags & VFS_BE_RDONLY) != 0);
    ASSERT_TRUE(be->mkdir == NULL && be->unlink == NULL && be->rename == NULL);
    unmount_volume();
}

// ---- A file that changes under its mount ----------------------------------

static int g_listed, g_listed_stale;

static void count_mounts(const char *path, const char *format, uint32_t n_partitions, uint32_t refcount, bool busy,
                         bool stale, void *user) {
    (void)path;
    (void)format;
    (void)n_partitions;
    (void)refcount;
    (void)busy;
    (void)user;
    g_listed++;
    g_listed_stale += stale;
}

// The image file changes while a handle is open on its mount: the next
// descent gets a new mount, the old one is marked stale and keeps serving
// that handle, and the handle's close drops it.  It used to stay in the
// table, unmarked, until evicted.
TEST(test_changed_file_supersedes_a_held_mount) {
    image_mount_t *m = mount_volume(one_file, 1);
    const vfs_backend_t *be = vfs_image_backend();
    vfs_file_t *f = NULL;
    ASSERT_EQ_INT(0, be->open(m, "/partition1/A", &f));
    g_listed = g_listed_stale = 0;
    image_vfs_list(count_mounts, NULL); // whatever earlier tests left, plus m
    int before = g_listed, before_stale = g_listed_stale;

    // Grow the file: its source key (size, mtime) changes.
    FILE *fp = fopen(g_host, "ab");
    ASSERT_TRUE(fp != NULL);
    ASSERT_TRUE(fwrite("x", 1, 1, fp) == 1);
    fclose(fp);
    image_mount_t *fresh = NULL;
    ASSERT_EQ_INT(0, image_vfs_acquire_mount(g_host, &fresh));
    ASSERT_TRUE(fresh != m);

    g_listed = g_listed_stale = 0;
    image_vfs_list(count_mounts, NULL);
    ASSERT_EQ_INT(before + 1, g_listed);
    ASSERT_EQ_INT(before_stale + 1, g_listed_stale);
    char buf[8];
    size_t got = 0;
    ASSERT_EQ_INT(0, be->read(f, 0, buf, sizeof(buf), &got)); // the old mount still serves its handle
    ASSERT_EQ_INT(4, (int)got);
    be->close(f);

    g_listed = g_listed_stale = 0;
    image_vfs_list(count_mounts, NULL);
    ASSERT_EQ_INT(before, g_listed);
    ASSERT_EQ_INT(before_stale, g_listed_stale);
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
    RUN(test_non_image_is_refused_cleanly);
#ifdef TEST_WRAP_REALPATH
    RUN(test_realpath_failure_falls_back_to_the_raw_path);
#endif
    RUN(test_nested_volume_mounts_from_its_source);
    RUN(test_fork_names_resolve_literally_where_no_file_precedes);
    RUN(test_changed_file_supersedes_a_held_mount);
    fprintf(stderr, "All image_vfs tests passed\n");
    return 0;
}
