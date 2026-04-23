// Unit tests for the VFS layer and APM parser.
// The APM tests drive image_apm_parse_buffer with synthetic bytes to avoid
// pulling in the full image/storage stack.  The VFS tests exercise the
// host backend against a temporary sandbox directory.

#include "image_apm.h"
#include "test_assert.h"
#include "vfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SANDBOX_DIR "_test_sandbox_vfs"

// ============================================================================
// Sandbox helpers (mirrored from tests/unit/suites/storage/test.c)
// ============================================================================

static void cleanup_dir(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        remove(path);
        return;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        cleanup_dir(child);
    }
    closedir(dir);
    rmdir(path);
}

static void setup_sandbox(void) {
    cleanup_dir(SANDBOX_DIR);
    if (mkdir(SANDBOX_DIR, 0777) != 0) {
        int saved_errno = errno;
        struct stat st;
        if (saved_errno != EEXIST || stat(SANDBOX_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "setup_sandbox: mkdir(%s) failed: %s\n", SANDBOX_DIR, strerror(saved_errno));
            ASSERT_TRUE(0);
        }
    }
}

static void teardown_sandbox(void) {
    cleanup_dir(SANDBOX_DIR);
}

// Write an arbitrary byte buffer to a sandbox file.
static void write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    ASSERT_TRUE(fwrite(data, 1, size, f) == size);
    fclose(f);
}

// ============================================================================
// APM builders
// ============================================================================

// Write a big-endian uint16 into a byte buffer.
static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

// Write a big-endian uint32 into a byte buffer.
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Fill in one APM entry at buf (512 bytes expected).
static void build_apm_entry(uint8_t *buf, uint32_t map_block_count, uint32_t start_block, uint32_t size_blocks,
                            const char *name, const char *type) {
    memset(buf, 0, 512);
    put_be16(buf + 0x00, 0x504D); // "PM"
    put_be16(buf + 0x02, 0x0000); // pmSigPad
    put_be32(buf + 0x04, map_block_count); // pmMapBlkCnt
    put_be32(buf + 0x08, start_block); // pmPyPartStart
    put_be32(buf + 0x0C, size_blocks); // pmPartBlkCnt
    if (name)
        strncpy((char *)(buf + 0x10), name, 32);
    if (type)
        strncpy((char *)(buf + 0x30), type, 32);
    put_be32(buf + 0x58, 0); // pmPartStatus
}

// Build a synthetic 3-partition APM image in `out` (must hold >= 4*512 bytes).
// Partitions: (Apple_partition_map, Apple_HFS, Apple_UNIX_SVR2).
static void build_apm_three_partitions(uint8_t *out, size_t out_size) {
    ASSERT_TRUE(out_size >= 4 * 512);
    memset(out, 0, out_size);
    // Block 0 = driver descriptor, leave as zeros for this test.
    build_apm_entry(out + 1 * 512, 3, 1, 3, "Apple", "Apple_partition_map");
    build_apm_entry(out + 2 * 512, 3, 100, 4000, "MacOS", "Apple_HFS");
    build_apm_entry(out + 3 * 512, 3, 4100, 20000, "A/UX Root", "Apple_UNIX_SVR2");
}

// ============================================================================
// APM parser tests
// ============================================================================

TEST(apm_parse_valid) {
    uint8_t buf[4 * 512];
    build_apm_three_partitions(buf, sizeof(buf));

    const char *err = NULL;
    apm_table_t *table = image_apm_parse_buffer(buf, sizeof(buf), &err);
    ASSERT_TRUE(table != NULL);
    ASSERT_EQ_INT(3, (int)table->n_partitions);
    ASSERT_EQ_INT(3, (int)table->map_block_count);

    // Partition 1 — the partition map itself.
    ASSERT_EQ_INT(1, (int)table->partitions[0].index);
    ASSERT_EQ_INT(1, (int)table->partitions[0].start_block);
    ASSERT_EQ_INT(3, (int)table->partitions[0].size_blocks);
    ASSERT_TRUE(strcmp(table->partitions[0].name, "Apple") == 0);
    ASSERT_TRUE(strcmp(table->partitions[0].type, "Apple_partition_map") == 0);
    ASSERT_EQ_INT(APM_FS_PARTITION_MAP, (int)table->partitions[0].fs_kind);

    // Partition 2 — HFS.
    ASSERT_EQ_INT(100, (int)table->partitions[1].start_block);
    ASSERT_EQ_INT(4000, (int)table->partitions[1].size_blocks);
    ASSERT_TRUE(strcmp(table->partitions[1].name, "MacOS") == 0);
    ASSERT_EQ_INT(APM_FS_HFS, (int)table->partitions[1].fs_kind);

    // Partition 3 — UFS.
    ASSERT_EQ_INT(4100, (int)table->partitions[2].start_block);
    ASSERT_EQ_INT(20000, (int)table->partitions[2].size_blocks);
    ASSERT_TRUE(strcmp(table->partitions[2].type, "Apple_UNIX_SVR2") == 0);
    ASSERT_EQ_INT(APM_FS_UFS, (int)table->partitions[2].fs_kind);

    image_apm_free(table);
}

TEST(apm_bad_signature) {
    uint8_t buf[4 * 512];
    build_apm_three_partitions(buf, sizeof(buf));
    // Corrupt the signature on entry 1.
    buf[1 * 512 + 0] = 'X';
    buf[1 * 512 + 1] = 'Y';

    const char *err = NULL;
    apm_table_t *table = image_apm_parse_buffer(buf, sizeof(buf), &err);
    ASSERT_TRUE(table == NULL);
    ASSERT_TRUE(err != NULL);
}

TEST(apm_probe_magic) {
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    // All zeros: no signature.
    ASSERT_TRUE(!image_apm_probe_magic(buf));
    // Signature but zero map count: rejected.
    put_be16(buf + 0x00, 0x504D);
    ASSERT_TRUE(!image_apm_probe_magic(buf));
    // Valid.
    put_be32(buf + 0x04, 9);
    ASSERT_TRUE(image_apm_probe_magic(buf));
    // Absurd map count: rejected.
    put_be32(buf + 0x04, 1000);
    ASSERT_TRUE(!image_apm_probe_magic(buf));
}

TEST(apm_truncated_trailing_entry) {
    // Claim 3 partitions but only fill 2.  Parser should return the two
    // valid entries rather than refuse the map entirely.
    uint8_t buf[4 * 512];
    memset(buf, 0, sizeof(buf));
    build_apm_entry(buf + 1 * 512, 3, 1, 3, "Apple", "Apple_partition_map");
    build_apm_entry(buf + 2 * 512, 3, 100, 4000, "MacOS", "Apple_HFS");
    // Entry 3 is left all-zeros (bad signature).

    const char *err = NULL;
    apm_table_t *table = image_apm_parse_buffer(buf, sizeof(buf), &err);
    ASSERT_TRUE(table != NULL);
    ASSERT_EQ_INT(2, (int)table->n_partitions);
    image_apm_free(table);
}

TEST(apm_classify_type) {
    ASSERT_EQ_INT(APM_FS_HFS, (int)image_apm_classify_type("Apple_HFS"));
    ASSERT_EQ_INT(APM_FS_HFS, (int)image_apm_classify_type("Apple_HFSX"));
    ASSERT_EQ_INT(APM_FS_UFS, (int)image_apm_classify_type("Apple_UNIX_SVR2"));
    ASSERT_EQ_INT(APM_FS_DRIVER, (int)image_apm_classify_type("Apple_Driver43"));
    ASSERT_EQ_INT(APM_FS_DRIVER, (int)image_apm_classify_type("Apple_Driver_ATA"));
    ASSERT_EQ_INT(APM_FS_FREE, (int)image_apm_classify_type("Apple_Free"));
    ASSERT_EQ_INT(APM_FS_PARTITION_MAP, (int)image_apm_classify_type("Apple_partition_map"));
    ASSERT_EQ_INT(APM_FS_UNKNOWN, (int)image_apm_classify_type("Random_Garbage"));
    ASSERT_EQ_INT(APM_FS_UNKNOWN, (int)image_apm_classify_type(NULL));
}

// ============================================================================
// VFS host-backend tests
// ============================================================================

TEST(vfs_stat_dir) {
    setup_sandbox();

    // Force the VFS cwd to the repo root equivalent via an absolute path.
    vfs_set_cwd("/");

    // Build a sandbox-relative absolute path based on the test's cwd.
    char cwd[PATH_MAX];
    ASSERT_TRUE(getcwd(cwd, sizeof(cwd)) != NULL);
    char dir_path[PATH_MAX + 64];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", cwd, SANDBOX_DIR);

    vfs_stat_t st = {0};
    int rc = vfs_stat(dir_path, &st);
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(VFS_MODE_DIR, (int)st.mode);

    teardown_sandbox();
}

TEST(vfs_readdir_lists_entries) {
    setup_sandbox();
    vfs_set_cwd("/");

    char cwd[PATH_MAX];
    ASSERT_TRUE(getcwd(cwd, sizeof(cwd)) != NULL);

    // Seed two files.
    char a[128], b[128];
    snprintf(a, sizeof(a), "%s/alpha.txt", SANDBOX_DIR);
    snprintf(b, sizeof(b), "%s/beta.txt", SANDBOX_DIR);
    write_file(a, "hello", 5);
    write_file(b, "world", 5);

    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", cwd, SANDBOX_DIR);
    vfs_dir_t *dir = NULL;
    const vfs_backend_t *be = NULL;
    ASSERT_EQ_INT(0, vfs_opendir(dir_path, &dir, &be));
    ASSERT_TRUE(dir != NULL);
    ASSERT_TRUE(be != NULL);

    int saw_alpha = 0, saw_beta = 0;
    vfs_dirent_t entry;
    int rc;
    while ((rc = be->readdir(dir, &entry)) > 0) {
        if (strcmp(entry.name, "alpha.txt") == 0)
            saw_alpha = 1;
        else if (strcmp(entry.name, "beta.txt") == 0)
            saw_beta = 1;
    }
    ASSERT_EQ_INT(0, rc);
    be->closedir(dir);
    ASSERT_TRUE(saw_alpha);
    ASSERT_TRUE(saw_beta);

    teardown_sandbox();
}

TEST(vfs_read_at_offset) {
    setup_sandbox();
    vfs_set_cwd("/");

    char cwd[PATH_MAX];
    ASSERT_TRUE(getcwd(cwd, sizeof(cwd)) != NULL);

    // Seed a file with a byte-index pattern so offset reads are easy to check.
    char rel[128];
    snprintf(rel, sizeof(rel), "%s/pattern.bin", SANDBOX_DIR);
    uint8_t data[256];
    for (int i = 0; i < 256; i++)
        data[i] = (uint8_t)i;
    write_file(rel, data, sizeof(data));

    char abs_path[PATH_MAX + 128];
    snprintf(abs_path, sizeof(abs_path), "%s/%s", cwd, rel);

    vfs_file_t *f = NULL;
    const vfs_backend_t *be = NULL;
    ASSERT_EQ_INT(0, vfs_open(abs_path, &f, &be));
    ASSERT_TRUE(f != NULL);

    uint8_t buf[50];
    size_t nread = 0;
    int rc = be->read(f, 100, buf, sizeof(buf), &nread);
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_INT(50, (int)nread);
    for (int i = 0; i < 50; i++)
        ASSERT_EQ_INT(100 + i, (int)buf[i]);

    be->close(f);
    teardown_sandbox();
}

TEST(vfs_resolve_descent_not_image) {
    // A plain (non-image) file with segments remaining must return
    // -ENOTDIR from the resolver rather than silently downcast to host
    // stat on the leaf.  The stub backend in this binary reports every
    // file as "not an image", so this exercises the probe-failed branch.
    setup_sandbox();
    vfs_set_cwd("/");
    char cwd[PATH_MAX];
    ASSERT_TRUE(getcwd(cwd, sizeof(cwd)) != NULL);

    char rel[128];
    snprintf(rel, sizeof(rel), "%s/plain.bin", SANDBOX_DIR);
    write_file(rel, "notanimage", 10);

    char abs_path[PATH_MAX + 256];
    snprintf(abs_path, sizeof(abs_path), "%s/%s/plain.bin/more/path", cwd, SANDBOX_DIR);

    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    int rc = vfs_resolve(abs_path, resolved, sizeof(resolved), &be, &ctx, &tail);
    ASSERT_EQ_INT(-ENOTDIR, rc);

    teardown_sandbox();
}

TEST(vfs_resolve_bare_file_strict) {
    // Strict resolve on a bare file path stays on the host backend — no
    // descent, no ENOTDIR.  This is the /cat foo.img/ rule from §2.9.
    setup_sandbox();
    vfs_set_cwd("/");
    char cwd[PATH_MAX];
    ASSERT_TRUE(getcwd(cwd, sizeof(cwd)) != NULL);

    char rel[128];
    snprintf(rel, sizeof(rel), "%s/bare.bin", SANDBOX_DIR);
    write_file(rel, "x", 1);

    char abs_path[PATH_MAX + 128];
    snprintf(abs_path, sizeof(abs_path), "%s/%s", cwd, rel);

    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    int rc = vfs_resolve(abs_path, resolved, sizeof(resolved), &be, &ctx, &tail);
    ASSERT_EQ_INT(0, rc);
    ASSERT_TRUE(be == vfs_host_backend());

    teardown_sandbox();
}

TEST(vfs_resolve_normalises_relative) {
    // Verify resolve strips ./ and .. segments consistently.
    vfs_set_cwd("/tmp/sub");
    char resolved[VFS_PATH_MAX];
    const vfs_backend_t *be = NULL;
    void *ctx = NULL;
    const char *tail = NULL;
    int rc = vfs_resolve("../other/./file", resolved, sizeof(resolved), &be, &ctx, &tail);
    ASSERT_EQ_INT(0, rc);
    ASSERT_TRUE(strcmp(resolved, "/tmp/other/file") == 0);
    ASSERT_TRUE(be == vfs_host_backend());
    ASSERT_TRUE(ctx == NULL);
    ASSERT_TRUE(tail == resolved);
}

int main(void) {
    RUN(apm_parse_valid);
    RUN(apm_bad_signature);
    RUN(apm_probe_magic);
    RUN(apm_truncated_trailing_entry);
    RUN(apm_classify_type);
    RUN(vfs_stat_dir);
    RUN(vfs_readdir_lists_entries);
    RUN(vfs_read_at_offset);
    RUN(vfs_resolve_descent_not_image);
    RUN(vfs_resolve_bare_file_strict);
    RUN(vfs_resolve_normalises_relative);
    return 0;
}
