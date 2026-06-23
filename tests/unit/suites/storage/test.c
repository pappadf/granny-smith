// Storage engine unit tests (delta-file model)

#include "storage.h"
#include "test_assert.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SANDBOX_DIR   "_test_sandbox"
#define BASE_FILE     SANDBOX_DIR "/disk.img"
#define DELTA_FILE    SANDBOX_DIR "/disk.img.delta"
#define JOURNAL_FILE  SANDBOX_DIR "/disk.img.journal"
#define BASE2_FILE    SANDBOX_DIR "/disk2.img"
#define DELTA2_FILE   SANDBOX_DIR "/disk2.img.delta"
#define JOURNAL2_FILE SANDBOX_DIR "/disk2.img.journal"
#define STATE_FILE    SANDBOX_DIR "/state.bin"
#define TEST_BLOCKS   128

#define ASSERT_OK(expr)        ASSERT_EQ_INT(GS_SUCCESS, (expr))
#define ASSERT_ERR(expr, code) ASSERT_EQ_INT((code), (expr))

// ============================================================================
// Helpers
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
        /* Tolerate EEXIST: a stale sandbox left by an interrupted prior run
         * (where cleanup_dir couldn't remove the directory) is fine as long
         * as it's a directory we can write into.  Per-test files use fixed
         * names and get overwritten unconditionally, so stale contents at
         * this point don't affect subsequent test logic. */
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

// Create a base image file filled with a pattern.
// Each block: byte[i] = (base_salt + lba + i) % 256
static void create_base_image(const char *path, uint64_t blocks, uint8_t base_salt) {
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    uint8_t buf[STORAGE_BLOCK_SIZE];
    for (uint64_t lba = 0; lba < blocks; lba++) {
        for (size_t i = 0; i < STORAGE_BLOCK_SIZE; i++)
            buf[i] = (uint8_t)(base_salt + lba + i);
        ASSERT_TRUE(fwrite(buf, STORAGE_BLOCK_SIZE, 1, f) == 1);
    }
    fclose(f);
}

static storage_config_t make_config(const char *base, const char *delta, const char *journal, uint64_t blocks) {
    storage_config_t config = {0};
    config.base_path = base;
    config.delta_path = delta;
    config.journal_path = journal;
    config.block_count = blocks;
    config.block_size = STORAGE_BLOCK_SIZE;
    config.base_data_offset = 0;
    return config;
}

static void fill_block(size_t lba, uint8_t salt, uint8_t *buffer) {
    for (size_t i = 0; i < STORAGE_BLOCK_SIZE; i++)
        buffer[i] = (uint8_t)(salt + lba + i);
}

static void expect_block(size_t lba, uint8_t salt, const uint8_t *buffer) {
    for (size_t i = 0; i < STORAGE_BLOCK_SIZE; i++)
        ASSERT_TRUE(buffer[i] == (uint8_t)(salt + lba + i));
}

static int file_write_cb(void *ctx, const void *data, size_t size) {
    FILE *f = (FILE *)ctx;
    return (fwrite(data, 1, size, f) == size) ? 0 : -1;
}

static int file_read_cb(void *ctx, void *data, size_t size) {
    FILE *f = (FILE *)ctx;
    size_t r = fread(data, 1, size, f);
    return (int)r;
}

// ============================================================================
// Tests
// ============================================================================

TEST(storage_invalid_arguments) {
    setup_sandbox();
    create_base_image(BASE_FILE, TEST_BLOCKS, 0x00);
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, TEST_BLOCKS);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    uint8_t buffer[STORAGE_BLOCK_SIZE];
    memset(buffer, 0xAA, sizeof(buffer));

    ASSERT_ERR(storage_read_block(NULL, 0, buffer), GS_ERROR);
    ASSERT_ERR(storage_write_block(NULL, 0, buffer), GS_ERROR);
    ASSERT_ERR(storage_read_block(storage, 1, buffer), GS_ERROR); // unaligned
    ASSERT_ERR(storage_write_block(storage, STORAGE_BLOCK_SIZE / 2, buffer), GS_ERROR);
    ASSERT_ERR(storage_checkpoint(NULL, NULL), GS_ERROR);
    ASSERT_ERR(storage_save_state(NULL, NULL, file_write_cb), GS_ERROR);
    ASSERT_ERR(storage_load_state(NULL, NULL, file_read_cb), GS_ERROR);

    ASSERT_OK(storage_delete(storage));

    storage_t *dummy = NULL;
    storage_config_t bad = make_config(NULL, DELTA_FILE, JOURNAL_FILE, TEST_BLOCKS);
    // NULL base_path is allowed (new image with no base), but NULL delta is not
    bad.delta_path = NULL;
    ASSERT_ERR(storage_new(&bad, &dummy), GS_ERROR);
    bad = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, 0);
    ASSERT_ERR(storage_new(&bad, &dummy), GS_ERROR);

    teardown_sandbox();
}

TEST(storage_basic_read_write) {
    setup_sandbox();
    uint8_t base_salt = 0x55;
    create_base_image(BASE_FILE, TEST_BLOCKS, base_salt);
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, TEST_BLOCKS);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    // Read unmodified block — should come from base image
    uint8_t buffer[STORAGE_BLOCK_SIZE];
    ASSERT_OK(storage_read_block(storage, 0, buffer));
    expect_block(0, base_salt, buffer);

    // Write a block — should be readable with new data
    fill_block(3, 0x11, buffer);
    ASSERT_OK(storage_write_block(storage, 3 * STORAGE_BLOCK_SIZE, buffer));

    uint8_t verify[STORAGE_BLOCK_SIZE];
    ASSERT_OK(storage_read_block(storage, 3 * STORAGE_BLOCK_SIZE, verify));
    expect_block(3, 0x11, verify);

    // Unmodified block still reads from base
    ASSERT_OK(storage_read_block(storage, 5 * STORAGE_BLOCK_SIZE, verify));
    expect_block(5, base_salt, verify);

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

TEST(storage_state_roundtrip) {
    setup_sandbox();
    create_base_image(BASE_FILE, TEST_BLOCKS, 0x00);
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, TEST_BLOCKS);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    // Write some blocks
    uint8_t buffer[STORAGE_BLOCK_SIZE];
    for (size_t lba = 0; lba < 8; lba++) {
        fill_block(lba, 0x20, buffer);
        ASSERT_OK(storage_write_block(storage, lba * STORAGE_BLOCK_SIZE, buffer));
    }

    // Save state to file
    FILE *state = fopen(STATE_FILE, "wb+");
    ASSERT_TRUE(state != NULL);
    ASSERT_OK(storage_save_state(storage, state, file_write_cb));
    fclose(state);
    ASSERT_OK(storage_delete(storage));

    // Load state into a fresh storage instance
    create_base_image(BASE2_FILE, TEST_BLOCKS, 0x00);
    storage_config_t config2 = make_config(BASE2_FILE, DELTA2_FILE, JOURNAL2_FILE, TEST_BLOCKS);
    storage_t *reloaded = NULL;
    ASSERT_OK(storage_new(&config2, &reloaded));
    state = fopen(STATE_FILE, "rb");
    ASSERT_TRUE(state != NULL);
    ASSERT_OK(storage_load_state(reloaded, state, file_read_cb));
    fclose(state);

    // Verify written blocks
    for (size_t lba = 0; lba < 8; lba++) {
        ASSERT_OK(storage_read_block(reloaded, lba * STORAGE_BLOCK_SIZE, buffer));
        expect_block(lba, 0x20, buffer);
    }

    ASSERT_OK(storage_delete(reloaded));
    teardown_sandbox();
}

TEST(storage_delta_persistence) {
    // Verify that closing and reopening preserves modified blocks
    setup_sandbox();
    create_base_image(BASE_FILE, TEST_BLOCKS, 0xAA);
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, TEST_BLOCKS);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    // Write a block and commit
    uint8_t buffer[STORAGE_BLOCK_SIZE];
    fill_block(7, 0x30, buffer);
    ASSERT_OK(storage_write_block(storage, 7 * STORAGE_BLOCK_SIZE, buffer));
    ASSERT_OK(storage_clear_rollback(storage)); // commit

    ASSERT_OK(storage_delete(storage));

    // Reopen — modified block should persist from delta
    storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    uint8_t verify[STORAGE_BLOCK_SIZE];
    ASSERT_OK(storage_read_block(storage, 7 * STORAGE_BLOCK_SIZE, verify));
    expect_block(7, 0x30, verify);

    // Unmodified block still reads from base
    ASSERT_OK(storage_read_block(storage, 0, verify));
    expect_block(0, 0xAA, verify);

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

TEST(storage_rollback) {
    setup_sandbox();
    create_base_image(BASE_FILE, TEST_BLOCKS, 0xBB);
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, TEST_BLOCKS);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    // Write block 5, commit
    uint8_t block[STORAGE_BLOCK_SIZE];
    fill_block(5, 0x10, block);
    ASSERT_OK(storage_write_block(storage, 5 * STORAGE_BLOCK_SIZE, block));
    ASSERT_OK(storage_clear_rollback(storage)); // commit

    // Overwrite block 5 (uncommitted — preimage captured in journal)
    fill_block(5, 0x40, block);
    ASSERT_OK(storage_write_block(storage, 5 * STORAGE_BLOCK_SIZE, block));

    // Current read should show new data
    uint8_t verify[STORAGE_BLOCK_SIZE];
    ASSERT_OK(storage_read_block(storage, 5 * STORAGE_BLOCK_SIZE, verify));
    expect_block(5, 0x40, verify);

    // Rollback should restore committed data
    ASSERT_OK(storage_apply_rollback(storage));
    ASSERT_OK(storage_read_block(storage, 5 * STORAGE_BLOCK_SIZE, verify));
    expect_block(5, 0x10, verify);

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

// --- Variable block size -------------------------------------------------
// The engine is block-size-agnostic: 512 (flat disks), 532 (Lisa ProFile:
// 512 data + 20 inline tag), or any multiple of 4 in [512, STORAGE_MAX_BLOCK_SIZE].
// The journal stride and delta data area follow the runtime block_size, and the
// size is recorded in the delta header so a reopen self-describes its geometry.

// Create a base image of `blocks` blocks of `bsize` bytes; byte[i] = salt+lba+i.
static void create_base_image_bs(const char *path, uint64_t blocks, uint32_t bsize, uint8_t salt) {
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    uint8_t buf[STORAGE_MAX_BLOCK_SIZE];
    for (uint64_t lba = 0; lba < blocks; lba++) {
        for (uint32_t i = 0; i < bsize; i++)
            buf[i] = (uint8_t)(salt + lba + i);
        ASSERT_TRUE(fwrite(buf, bsize, 1, f) == 1);
    }
    fclose(f);
}

static void fill_block_bs(size_t lba, uint32_t bsize, uint8_t salt, uint8_t *buffer) {
    for (uint32_t i = 0; i < bsize; i++)
        buffer[i] = (uint8_t)(salt + lba + i);
}

static void expect_block_bs(size_t lba, uint32_t bsize, uint8_t salt, const uint8_t *buffer) {
    for (uint32_t i = 0; i < bsize; i++)
        ASSERT_TRUE(buffer[i] == (uint8_t)(salt + lba + i));
}

// Round-trip read/write + commit/reopen at a given non-default block size.
static void run_block_size_roundtrip(uint32_t bsize) {
    const uint64_t blocks = 16;
    const uint8_t base_salt = 0x55;
    setup_sandbox();
    create_base_image_bs(BASE_FILE, blocks, bsize, base_salt);

    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    config.block_size = bsize;
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    uint8_t buffer[STORAGE_MAX_BLOCK_SIZE];
    uint8_t verify[STORAGE_MAX_BLOCK_SIZE];

    // Unmodified block comes from the base image.
    ASSERT_OK(storage_read_block(storage, 0, buffer));
    expect_block_bs(0, bsize, base_salt, buffer);

    // Write block 4 (offset = 4 * bsize), read it back.
    fill_block_bs(4, bsize, 0x11, buffer);
    ASSERT_OK(storage_write_block(storage, (size_t)4 * bsize, buffer));
    ASSERT_OK(storage_read_block(storage, (size_t)4 * bsize, verify));
    expect_block_bs(4, bsize, 0x11, verify);

    // Commit, overwrite (journals a preimage), then roll back to the committed
    // value — exercises the variable-stride journal path.
    ASSERT_OK(storage_clear_rollback(storage));
    fill_block_bs(4, bsize, 0x99, buffer);
    ASSERT_OK(storage_write_block(storage, (size_t)4 * bsize, buffer));
    ASSERT_OK(storage_read_block(storage, (size_t)4 * bsize, verify));
    expect_block_bs(4, bsize, 0x99, verify);
    ASSERT_OK(storage_apply_rollback(storage));
    ASSERT_OK(storage_read_block(storage, (size_t)4 * bsize, verify));
    expect_block_bs(4, bsize, 0x11, verify);

    ASSERT_OK(storage_delete(storage));

    // Reopen — the delta header records block_size, so the modified block
    // persists and unmodified blocks still read from the base.
    storage = NULL;
    config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    config.block_size = bsize;
    ASSERT_OK(storage_new(&config, &storage));
    ASSERT_OK(storage_read_block(storage, (size_t)4 * bsize, verify));
    expect_block_bs(4, bsize, 0x11, verify);
    ASSERT_OK(storage_read_block(storage, 0, verify));
    expect_block_bs(0, bsize, base_salt, verify);

    // A delta written at one block size must be rejected when reopened with a
    // different one (the header validates block_size).
    ASSERT_OK(storage_delete(storage));
    storage = NULL;
    config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    config.block_size = (bsize == STORAGE_BLOCK_SIZE) ? 532 : STORAGE_BLOCK_SIZE;
    ASSERT_ERR(storage_new(&config, &storage), GS_ERROR);

    teardown_sandbox();
}

TEST(storage_block_size_532) {
    // The Lisa ProFile geometry: 512 data + 20 inline tag.
    run_block_size_roundtrip(532);
}

TEST(storage_block_size_other) {
    // A non-512, non-532 size proves the engine is not special-cased to either.
    run_block_size_roundtrip(1024);
}

TEST(storage_block_size_validation) {
    setup_sandbox();
    create_base_image_bs(BASE_FILE, 4, 512, 0x00);
    storage_t *storage = NULL;

    // Too small, too large, or not a multiple of 4 are all rejected.
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, 4);
    config.block_size = 256; // < 512
    ASSERT_ERR(storage_new(&config, &storage), GS_ERROR);
    config.block_size = STORAGE_MAX_BLOCK_SIZE + 4; // > max
    ASSERT_ERR(storage_new(&config, &storage), GS_ERROR);
    config.block_size = 530; // not a multiple of 4
    ASSERT_ERR(storage_new(&config, &storage), GS_ERROR);

    teardown_sandbox();
}

int main(void) {
    RUN(storage_invalid_arguments);
    RUN(storage_basic_read_write);
    RUN(storage_state_roundtrip);
    RUN(storage_delta_persistence);
    RUN(storage_rollback);
    RUN(storage_block_size_532);
    RUN(storage_block_size_other);
    RUN(storage_block_size_validation);
    return 0;
}
