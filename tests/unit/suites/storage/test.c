// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Storage engine unit tests (delta-file model)

#include "source.h"
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

// Base sources opened by make_config: storage takes its own reference, so
// the test's are dropped when the sandbox goes.
static gs_source_t *g_bases[128];
static int g_n_bases;

static void teardown_sandbox(void) {
    while (g_n_bases > 0)
        gs_source_release(g_bases[--g_n_bases]);
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
    // The base is a byte source; a path with no file (a brand-new image)
    // is no base, as a missing base file always was.
    config.base = base ? gs_source_host(base, NULL) : NULL;
    if (config.base && g_n_bases < (int)(sizeof(g_bases) / sizeof(g_bases[0])))
        g_bases[g_n_bases++] = config.base;
    config.delta_path = delta;
    config.journal_path = journal;
    config.block_count = blocks;
    config.block_size = STORAGE_BLOCK_SIZE;
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
    // A NULL base is allowed (new image with no base), but NULL delta is not
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

// ============================================================================
// storage_save_state run coalescing
// ============================================================================
//
// storage_save_state batches contiguous same-source blocks into one read
// instead of doing a seek+read per block.  storage_read_block still walks a
// block at a time and is untouched by that change, so it serves as an
// independent oracle: the streamed bytes must equal the per-block reads
// concatenated, for every arrangement of base/delta/zero runs.

// Stream `storage` to STATE_FILE, then assert the result is byte-identical to
// block-by-block storage_read_block output.  Also asserts the stream is
// emitted as one record per block, which the checkpoint format requires.
static uint64_t g_stream_records;

static int counting_write_cb(void *ctx, const void *data, size_t size) {
    g_stream_records++;
    return file_write_cb(ctx, data, size);
}

static void assert_stream_matches_per_block(storage_t *storage, uint64_t blocks, uint32_t bsize) {
    FILE *state = fopen(STATE_FILE, "wb");
    ASSERT_TRUE(state != NULL);
    g_stream_records = 0;
    ASSERT_OK(storage_save_state(storage, state, counting_write_cb));
    fclose(state);

    // One callback per block: storage_load_state reads the checkpoint stream a
    // block at a time and its reader asserts each record's exact size.
    ASSERT_EQ_INT((int)blocks, (int)g_stream_records);

    state = fopen(STATE_FILE, "rb");
    ASSERT_TRUE(state != NULL);
    uint8_t *want = malloc(bsize);
    uint8_t *got = malloc(bsize);
    ASSERT_TRUE(want != NULL && got != NULL);
    for (uint64_t lba = 0; lba < blocks; lba++) {
        ASSERT_OK(storage_read_block(storage, (size_t)lba * bsize, want));
        ASSERT_TRUE(fread(got, 1, bsize, state) == bsize);
        ASSERT_TRUE(memcmp(want, got, bsize) == 0);
    }
    // Nothing beyond the last block.
    ASSERT_TRUE(fread(got, 1, 1, state) == 0);
    free(want);
    free(got);
    fclose(state);
}

// Write a spread of blocks chosen to exercise every run shape: a leading
// unmodified run, isolated modified blocks, a modified run longer than one
// staging chunk, an alternating stretch, and a modified final block.
static void write_run_pattern(storage_t *storage, uint64_t blocks, uint32_t bsize) {
    uint8_t *buf = malloc(bsize);
    ASSERT_TRUE(buf != NULL);

    // Isolated singles surrounded by base-sourced blocks.
    const uint64_t singles[] = {1, 7, 100, 4095, 4096, 8191, 8192, 8193};
    for (size_t i = 0; i < sizeof(singles) / sizeof(singles[0]); i++) {
        if (singles[i] >= blocks)
            continue;
        fill_block_bs((size_t)singles[i], bsize, 0x30, buf);
        ASSERT_OK(storage_write_block(storage, (size_t)singles[i] * bsize, buf));
    }

    // A modified run longer than one staging chunk (4 MB / bsize blocks), so
    // the run must be split across chunks and rejoined without a gap.
    uint64_t run_start = 9000;
    uint64_t run_end = run_start + 10000;
    if (run_end > blocks)
        run_end = blocks;
    for (uint64_t lba = run_start; lba < run_end; lba++) {
        fill_block_bs((size_t)lba, bsize, 0x40, buf);
        ASSERT_OK(storage_write_block(storage, (size_t)lba * bsize, buf));
    }

    // Alternating modified/unmodified — every run is length 1.
    for (uint64_t lba = 200; lba < 400 && lba < blocks; lba += 2) {
        fill_block_bs((size_t)lba, bsize, 0x50, buf);
        ASSERT_OK(storage_write_block(storage, (size_t)lba * bsize, buf));
    }

    // First and last block modified — the boundaries of the walk.
    fill_block_bs(0, bsize, 0x60, buf);
    ASSERT_OK(storage_write_block(storage, 0, buf));
    fill_block_bs((size_t)(blocks - 1), bsize, 0x60, buf);
    ASSERT_OK(storage_write_block(storage, (size_t)(blocks - 1) * bsize, buf));

    free(buf);
}

TEST(storage_save_state_run_patterns) {
    setup_sandbox();
    // 20000 blocks x 512 B is ~10 MB, spanning several 4 MB staging chunks.
    const uint64_t blocks = 20000;
    create_base_image_bs(BASE_FILE, blocks, STORAGE_BLOCK_SIZE, 0x11);

    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    // Fully unmodified: one enormous base-sourced run.
    assert_stream_matches_per_block(storage, blocks, STORAGE_BLOCK_SIZE);

    write_run_pattern(storage, blocks, STORAGE_BLOCK_SIZE);
    assert_stream_matches_per_block(storage, blocks, STORAGE_BLOCK_SIZE);

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

TEST(storage_save_state_runs_532) {
    // 532 does not divide the staging chunk evenly, so the chunk holds a
    // partial-block remainder that must never be streamed.
    setup_sandbox();
    const uint64_t blocks = 20000;
    create_base_image_bs(BASE_FILE, blocks, 532, 0x22);

    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    config.block_size = 532;
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    write_run_pattern(storage, blocks, 532);
    assert_stream_matches_per_block(storage, blocks, 532);

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

TEST(storage_save_state_no_base) {
    // No base file: unmodified blocks are the zero source, so runs alternate
    // between zeros and the delta with no file behind the zeros.
    setup_sandbox();
    const uint64_t blocks = 12000;
    storage_config_t config = make_config(NULL, DELTA_FILE, JOURNAL_FILE, blocks);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    write_run_pattern(storage, blocks, STORAGE_BLOCK_SIZE);
    assert_stream_matches_per_block(storage, blocks, STORAGE_BLOCK_SIZE);

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

TEST(storage_save_state_short_base) {
    // A base shorter than the declared geometry, ending mid-block.  Blocks
    // past EOF read as zeros, and the trailing partial block must read as all
    // zeros too rather than leaking the bytes that are present.
    setup_sandbox();
    const uint64_t blocks = 12000;
    const uint64_t base_blocks = 5000;
    create_base_image_bs(BASE_FILE, base_blocks, STORAGE_BLOCK_SIZE, 0x33);
    // Chop the last block in half so the base ends mid-block.
    ASSERT_TRUE(truncate(BASE_FILE, (off_t)((base_blocks - 1) * STORAGE_BLOCK_SIZE + 200)) == 0);

    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    assert_stream_matches_per_block(storage, blocks, STORAGE_BLOCK_SIZE);
    write_run_pattern(storage, blocks, STORAGE_BLOCK_SIZE);
    assert_stream_matches_per_block(storage, blocks, STORAGE_BLOCK_SIZE);

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

// A consolidated restore writes the delta a run at a time.  Into a storage
// whose delta already holds clusters in scattered slots (the last cluster
// given the first slot), the target positions jump backwards and forwards,
// and the disk spans several staging chunks: every block must still land
// where it belongs.
TEST(storage_load_state_runs) {
    setup_sandbox();
    const uint64_t blocks = 20000;
    create_base_image_bs(BASE_FILE, blocks, STORAGE_BLOCK_SIZE, 0x11);
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    write_run_pattern(storage, blocks, STORAGE_BLOCK_SIZE);
    FILE *state = fopen(STATE_FILE, "wb");
    ASSERT_TRUE(state != NULL);
    ASSERT_OK(storage_save_state(storage, state, file_write_cb));
    fclose(state);

    create_base_image_bs(BASE2_FILE, blocks, STORAGE_BLOCK_SIZE, 0x77);
    storage_config_t config2 = make_config(BASE2_FILE, DELTA2_FILE, JOURNAL2_FILE, blocks);
    storage_t *reloaded = NULL;
    ASSERT_OK(storage_new(&config2, &reloaded));
    uint8_t buf[STORAGE_BLOCK_SIZE];
    const uint64_t scattered[] = {blocks - 1, 5000, 12345, 3};
    for (size_t i = 0; i < sizeof(scattered) / sizeof(scattered[0]); i++) {
        fill_block_bs((size_t)scattered[i], STORAGE_BLOCK_SIZE, 0x99, buf);
        ASSERT_OK(storage_write_block(reloaded, (size_t)scattered[i] * STORAGE_BLOCK_SIZE, buf));
    }
    state = fopen(STATE_FILE, "rb");
    ASSERT_TRUE(state != NULL);
    ASSERT_OK(storage_load_state(reloaded, state, file_read_cb));
    fclose(state);

    uint8_t want[STORAGE_BLOCK_SIZE];
    for (uint64_t lba = 0; lba < blocks; lba++) {
        ASSERT_OK(storage_read_block(storage, (size_t)lba * STORAGE_BLOCK_SIZE, want));
        ASSERT_OK(storage_read_block(reloaded, (size_t)lba * STORAGE_BLOCK_SIZE, buf));
        ASSERT_TRUE(memcmp(want, buf, STORAGE_BLOCK_SIZE) == 0);
    }

    ASSERT_OK(storage_delete(storage));
    ASSERT_OK(storage_delete(reloaded));
    teardown_sandbox();
}

// A consolidated restore onto the base the state was saved from writes only
// the blocks that differ from it: the delta stays the size of the changes,
// not of the disk.  Every block used to be written, so restoring a Save
// State of a machine with a large disk cost the whole disk in the browser's
// storage, and a second restore ran out of quota.
TEST(storage_load_state_writes_only_changes) {
    setup_sandbox();
    const uint64_t blocks = 20000;
    create_base_image_bs(BASE_FILE, blocks, STORAGE_BLOCK_SIZE, 0x11);
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    uint8_t buf[STORAGE_BLOCK_SIZE];
    const uint64_t changed[] = {0, 2, 3, 9999, blocks - 1};
    for (size_t i = 0; i < sizeof(changed) / sizeof(changed[0]); i++) {
        fill_block_bs((size_t)changed[i], STORAGE_BLOCK_SIZE, 0x5A, buf);
        ASSERT_OK(storage_write_block(storage, (size_t)changed[i] * STORAGE_BLOCK_SIZE, buf));
    }
    FILE *state = fopen(STATE_FILE, "wb");
    ASSERT_TRUE(state != NULL);
    ASSERT_OK(storage_save_state(storage, state, file_write_cb));
    fclose(state);

    // Restore onto the same base, into a fresh delta.
    storage_config_t config2 = make_config(BASE_FILE, DELTA2_FILE, JOURNAL2_FILE, blocks);
    storage_t *reloaded = NULL;
    ASSERT_OK(storage_new(&config2, &reloaded));
    state = fopen(STATE_FILE, "rb");
    ASSERT_TRUE(state != NULL);
    ASSERT_OK(storage_load_state(reloaded, state, file_read_cb));
    fclose(state);

    uint8_t want[STORAGE_BLOCK_SIZE];
    for (uint64_t lba = 0; lba < blocks; lba++) {
        ASSERT_OK(storage_read_block(storage, (size_t)lba * STORAGE_BLOCK_SIZE, want));
        ASSERT_OK(storage_read_block(reloaded, (size_t)lba * STORAGE_BLOCK_SIZE, buf));
        ASSERT_TRUE(memcmp(want, buf, STORAGE_BLOCK_SIZE) == 0);
    }
    ASSERT_OK(storage_delete(reloaded));

    // A tenth of the disk is generous for five changed blocks (their
    // clusters plus the header and tables); the old behaviour wrote it all.
    struct stat st;
    ASSERT_TRUE(stat(DELTA2_FILE, &st) == 0);
    ASSERT_TRUE((uint64_t)st.st_size < blocks * STORAGE_BLOCK_SIZE / 10);

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

// ---- 64-bit offsets and the journal ---------------------------------------

// A block past 2 GiB of the delta is written and read back where it
// belongs.  On wasm32 -- the shipping build -- the (long) seek wrapped at
// 2 GiB and the block landed elsewhere with no error; natively long is 64
// bits and this passes either way, so the wasm32 tier is what tests it.
TEST(storage_block_past_2gib) {
    setup_sandbox();
    const uint64_t blocks = 6u * 1024u * 1024u; // 3 GiB, blank: no base file
    storage_config_t config = make_config(NULL, DELTA_FILE, JOURNAL_FILE, blocks);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    const size_t far = 5u * 1000u * 1000u; // 2.56 GB in: past 2^31, inside 2^32
    uint8_t block[STORAGE_BLOCK_SIZE], verify[STORAGE_BLOCK_SIZE];
    fill_block(far, 0x5A, block);
    ASSERT_OK(storage_write_block(storage, far * STORAGE_BLOCK_SIZE, block));
    fill_block(0, 0x33, block);
    ASSERT_OK(storage_write_block(storage, 0, block));

    ASSERT_OK(storage_read_block(storage, far * STORAGE_BLOCK_SIZE, verify));
    expect_block(far, 0x5A, verify);
    ASSERT_OK(storage_read_block(storage, 0, verify));
    expect_block(0, 0x33, verify);

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

// Leave a journal holding `good` valid preimage entries (for blocks 5, 6, ...)
// and the storage closed.  Block 20 is committed too but never overwritten,
// so a later write to it appends a fresh preimage.  Returns the size of one
// journal entry.
static long make_journal(int good) {
    create_base_image(BASE_FILE, TEST_BLOCKS, 0xBB);
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, TEST_BLOCKS);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    uint8_t block[STORAGE_BLOCK_SIZE];
    for (int i = 0; i < good; i++) {
        fill_block(5 + i, 0x10, block);
        ASSERT_OK(storage_write_block(storage, (5 + i) * STORAGE_BLOCK_SIZE, block));
    }
    fill_block(20, 0x10, block);
    ASSERT_OK(storage_write_block(storage, 20 * STORAGE_BLOCK_SIZE, block));
    ASSERT_OK(storage_clear_rollback(storage)); // commit
    for (int i = 0; i < good; i++) {
        fill_block(5 + i, 0x40, block); // overwrite: preimages go to the journal
        ASSERT_OK(storage_write_block(storage, (5 + i) * STORAGE_BLOCK_SIZE, block));
    }
    ASSERT_OK(storage_delete(storage));
    return 4 + STORAGE_BLOCK_SIZE;
}

static long file_size(const char *path) {
    struct stat st;
    ASSERT_TRUE(stat(path, &st) == 0);
    return (long)st.st_size;
}

// A journal entry naming a block the device does not have -- a damaged
// journal, the crash-recovery case the file exists for -- ends the journal:
// the entries before it still roll back, and it is cut off rather than
// replayed at data_offset + lba * block_size, wherever that lands.
TEST(storage_journal_entry_out_of_range_is_dropped) {
    setup_sandbox();
    long entry = make_journal(2);
    ASSERT_EQ_INT(2 * entry, file_size(JOURNAL_FILE));
    FILE *j = fopen(JOURNAL_FILE, "ab");
    ASSERT_TRUE(j != NULL);
    uint32_t lba = TEST_BLOCKS + 1000;
    uint8_t data[STORAGE_BLOCK_SIZE] = {0};
    ASSERT_TRUE(fwrite(&lba, sizeof(lba), 1, j) == 1 && fwrite(data, sizeof(data), 1, j) == 1);
    fclose(j);
    long delta_before = file_size(DELTA_FILE);

    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, TEST_BLOCKS);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    ASSERT_EQ_INT(2 * entry, file_size(JOURNAL_FILE)); // cut at the bad entry
    ASSERT_OK(storage_apply_rollback(storage));
    ASSERT_EQ_INT(delta_before, file_size(DELTA_FILE)); // nothing written past the data
    uint8_t verify[STORAGE_BLOCK_SIZE];
    for (int i = 0; i < 2; i++) {
        ASSERT_OK(storage_read_block(storage, (5 + i) * STORAGE_BLOCK_SIZE, verify));
        expect_block(5 + i, 0x10, verify); // the good entries rolled back
    }
    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

// A journal cut off mid-entry (a crash during an append) keeps its whole
// entries, and loses the fragment: the file is opened for appending, so a
// fragment left in place would misalign every entry written after it.
TEST(storage_journal_partial_tail_is_dropped) {
    setup_sandbox();
    long entry = make_journal(1);
    FILE *j = fopen(JOURNAL_FILE, "ab");
    ASSERT_TRUE(j != NULL);
    uint32_t lba = 7;
    ASSERT_TRUE(fwrite(&lba, sizeof(lba), 1, j) == 1 && fwrite("frag", 4, 1, j) == 1);
    fclose(j);

    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, TEST_BLOCKS);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    ASSERT_EQ_INT(entry, file_size(JOURNAL_FILE));

    // A new preimage lands on an entry boundary and rolls back with the rest.
    uint8_t block[STORAGE_BLOCK_SIZE];
    fill_block(20, 0x40, block);
    ASSERT_OK(storage_write_block(storage, 20 * STORAGE_BLOCK_SIZE, block));
    ASSERT_EQ_INT(2 * entry, file_size(JOURNAL_FILE));
    ASSERT_OK(storage_apply_rollback(storage));
    uint8_t verify[STORAGE_BLOCK_SIZE];
    ASSERT_OK(storage_read_block(storage, 5 * STORAGE_BLOCK_SIZE, verify));
    expect_block(5, 0x10, verify);
    ASSERT_OK(storage_read_block(storage, 20 * STORAGE_BLOCK_SIZE, verify));
    expect_block(20, 0x10, verify);
    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

// ---- Delta v2: cluster-indexed, compact ------------------------------------

// The browser charges a file's logical length, so the delta must grow with
// what was written, not with the highest block written: the last sector of
// a 2 GiB disk costs the metadata plus one 32 KB slot.
TEST(storage_v2_last_block_is_one_slot) {
    setup_sandbox();
    const uint64_t blocks = 4u * 1024u * 1024u; // 2 GiB of 512 B
    storage_config_t config = make_config(NULL, DELTA_FILE, JOURNAL_FILE, blocks);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    long meta = file_size(DELTA_FILE);
    // Header + two 512 KiB bitmaps + two 256 KiB tables, sector-rounded.
    ASSERT_TRUE(meta < 2 * 1024 * 1024);

    uint8_t block[STORAGE_BLOCK_SIZE], verify[STORAGE_BLOCK_SIZE];
    fill_block(blocks - 2, 0x71, block);
    ASSERT_OK(storage_write_block(storage, (size_t)(blocks - 2) * STORAGE_BLOCK_SIZE, block));
    ASSERT_OK(storage_clear_rollback(storage));
    ASSERT_TRUE(file_size(DELTA_FILE) <= meta + 32 * 1024);
    ASSERT_OK(storage_read_block(storage, (size_t)(blocks - 2) * STORAGE_BLOCK_SIZE, verify));
    expect_block(blocks - 2, 0x71, verify);
    // A neighbour in the same cluster was never written: it still reads as
    // the (absent) base, not as whatever the slot holds.
    ASSERT_OK(storage_read_block(storage, (size_t)(blocks - 3) * STORAGE_BLOCK_SIZE, verify));
    for (size_t i = 0; i < STORAGE_BLOCK_SIZE; i++)
        ASSERT_TRUE(verify[i] == 0);
    ASSERT_OK(storage_delete(storage));

    // Reopened, it is still there.
    storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    ASSERT_OK(storage_read_block(storage, (size_t)(blocks - 2) * STORAGE_BLOCK_SIZE, verify));
    expect_block(blocks - 2, 0x71, verify);
    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

// Slots allocated after the last commit are discarded wholesale by a
// rollback: the file is truncated back, and those blocks read the base again.
TEST(storage_v2_rollback_truncates_new_slots) {
    setup_sandbox();
    const uint64_t blocks = 64 * 1024;
    create_base_image(BASE_FILE, blocks, 0x21);
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    uint8_t block[STORAGE_BLOCK_SIZE], verify[STORAGE_BLOCK_SIZE];
    fill_block(10, 0x10, block);
    ASSERT_OK(storage_write_block(storage, 10 * STORAGE_BLOCK_SIZE, block));
    ASSERT_OK(storage_clear_rollback(storage));
    long committed = file_size(DELTA_FILE);

    // Post-commit: a new cluster far away, and an overwrite of block 10.
    for (uint64_t lba = 50000; lba < 50200; lba++) {
        fill_block(lba, 0x55, block);
        ASSERT_OK(storage_write_block(storage, lba * STORAGE_BLOCK_SIZE, block));
    }
    fill_block(10, 0x66, block);
    ASSERT_OK(storage_write_block(storage, 10 * STORAGE_BLOCK_SIZE, block));
    long grown = file_size(DELTA_FILE);
    ASSERT_TRUE(grown > committed + 32 * 1024);

    // Cut back to the end of the committed slot (the committed file may end
    // inside it, where its last written block ends).
    ASSERT_OK(storage_apply_rollback(storage));
    ASSERT_TRUE(file_size(DELTA_FILE) <= committed + 32 * 1024);
    ASSERT_TRUE(file_size(DELTA_FILE) < grown);
    ASSERT_OK(storage_read_block(storage, 10 * STORAGE_BLOCK_SIZE, verify));
    expect_block(10, 0x10, verify); // journal replayed into the committed slot
    ASSERT_OK(storage_read_block(storage, 50100 * STORAGE_BLOCK_SIZE, verify));
    expect_block(50100, 0x21, verify); // the base again

    // Writing after the rollback reuses the freed slot space.
    fill_block(50100, 0x77, block);
    ASSERT_OK(storage_write_block(storage, 50100 * STORAGE_BLOCK_SIZE, block));
    ASSERT_OK(storage_read_block(storage, 50100 * STORAGE_BLOCK_SIZE, verify));
    expect_block(50100, 0x77, verify);
    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

// A crash (no rollback, no commit) reopens at the last commit: the slots
// allocated after it are cut away, as a rollback would cut them.
TEST(storage_v2_reopen_after_crash_is_committed_state) {
    setup_sandbox();
    const uint64_t blocks = 64 * 1024;
    create_base_image(BASE_FILE, blocks, 0x31);
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    uint8_t block[STORAGE_BLOCK_SIZE], verify[STORAGE_BLOCK_SIZE];
    fill_block(3, 0x10, block);
    ASSERT_OK(storage_write_block(storage, 3 * STORAGE_BLOCK_SIZE, block));
    ASSERT_OK(storage_clear_rollback(storage));
    long committed = file_size(DELTA_FILE);
    fill_block(40000, 0x44, block);
    ASSERT_OK(storage_write_block(storage, 40000 * STORAGE_BLOCK_SIZE, block));
    ASSERT_OK(storage_delete(storage)); // no commit: the crash

    storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    ASSERT_TRUE(file_size(DELTA_FILE) <= committed + 32 * 1024);
    ASSERT_OK(storage_read_block(storage, 3 * STORAGE_BLOCK_SIZE, verify));
    expect_block(3, 0x10, verify);
    ASSERT_OK(storage_read_block(storage, 40000 * STORAGE_BLOCK_SIZE, verify));
    expect_block(40000, 0x31, verify);
    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

// A version-1 delta (LBA-positioned, written by earlier builds) still opens,
// reads and takes writes in place.
TEST(storage_v1_delta_still_opens) {
    setup_sandbox();
    const uint64_t blocks = TEST_BLOCKS;
    create_base_image(BASE_FILE, blocks, 0x41);
    // Hand-build a v1 delta: 24-byte header, two bitmaps, block area.
    size_t bm = (size_t)((blocks + 7) / 8);
    FILE *f = fopen(DELTA_FILE, "wb");
    ASSERT_TRUE(f != NULL);
    uint8_t hdr[24] = {'G', 'S', 'D', 'L', 1, 0, 0, 0};
    uint64_t bc = blocks;
    uint32_t bs = STORAGE_BLOCK_SIZE;
    memcpy(hdr + 8, &bc, 8);
    memcpy(hdr + 16, &bs, 4);
    ASSERT_TRUE(fwrite(hdr, sizeof(hdr), 1, f) == 1);
    uint8_t *bits = calloc(1, bm);
    bits[9 / 8] |= 1u << (9 % 8);
    ASSERT_TRUE(fwrite(bits, bm, 1, f) == 1);
    ASSERT_TRUE(fwrite(bits, bm, 1, f) == 1);
    free(bits);
    uint8_t block[STORAGE_BLOCK_SIZE], verify[STORAGE_BLOCK_SIZE];
    fill_block(9, 0x99, block);
    ASSERT_TRUE(fseek(f, (long)(24 + 2 * bm + 9 * STORAGE_BLOCK_SIZE), SEEK_SET) == 0);
    ASSERT_TRUE(fwrite(block, sizeof(block), 1, f) == 1);
    fclose(f);

    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    ASSERT_OK(storage_read_block(storage, 9 * STORAGE_BLOCK_SIZE, verify));
    expect_block(9, 0x99, verify);
    ASSERT_OK(storage_read_block(storage, 8 * STORAGE_BLOCK_SIZE, verify));
    expect_block(8, 0x41, verify);
    fill_block(9, 0x12, block); // committed: journaled, then rolled back
    ASSERT_OK(storage_write_block(storage, 9 * STORAGE_BLOCK_SIZE, block));
    fill_block(100, 0x13, block);
    ASSERT_OK(storage_write_block(storage, 100 * STORAGE_BLOCK_SIZE, block));
    ASSERT_OK(storage_read_block(storage, 100 * STORAGE_BLOCK_SIZE, verify));
    expect_block(100, 0x13, verify);
    ASSERT_OK(storage_apply_rollback(storage));
    ASSERT_OK(storage_read_block(storage, 9 * STORAGE_BLOCK_SIZE, verify));
    expect_block(9, 0x99, verify);
    ASSERT_OK(storage_read_block(storage, 100 * STORAGE_BLOCK_SIZE, verify));
    expect_block(100, 0x41, verify);
    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

// A source that passes reads through to its parent and counts them.
static int g_base_reads;
static int64_t counting_base_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    g_base_reads++;
    return gs_source_read(s->ctx, off, buf, len);
}
static uint64_t counting_base_size(gs_source_t *s) {
    return gs_source_size(s->ctx);
}
static const char *counting_base_key(gs_source_t *s) {
    return gs_source_key(s->ctx);
}
static gs_tier_t counting_base_tier(gs_source_t *s) {
    return gs_source_tier(s->ctx);
}
static void counting_base_close(gs_source_t *s) {
    (void)s; // the parent reference is the source's own
}
static const gs_source_ops_t counting_base_ops = {counting_base_read, counting_base_size, counting_base_key,
                                                  counting_base_tier, counting_base_close};

// storage_read_blocks over the whole disk, and over windows that start and
// end inside modified and unmodified runs, returns what storage_read_block
// returns block by block.
static void assert_read_blocks_matches_per_block(storage_t *storage, uint64_t blocks) {
    size_t size = (size_t)blocks * STORAGE_BLOCK_SIZE;
    uint8_t *want = malloc(size), *got = malloc(size);
    ASSERT_TRUE(want != NULL && got != NULL);
    for (uint64_t lba = 0; lba < blocks; lba++)
        ASSERT_OK(storage_read_block(storage, (size_t)lba * STORAGE_BLOCK_SIZE, want + lba * STORAGE_BLOCK_SIZE));
    ASSERT_OK(storage_read_blocks(storage, 0, got, (size_t)blocks));
    ASSERT_TRUE(memcmp(want, got, size) == 0);
    static const uint64_t starts[] = {0, 1, 6, 199, 250, 4095, 8190, 8999, 18999};
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        if (starts[i] >= blocks)
            continue;
        uint64_t n = blocks - starts[i] < 300 ? blocks - starts[i] : 300;
        memset(got, 0xEE, size);
        ASSERT_OK(storage_read_blocks(storage, (size_t)starts[i] * STORAGE_BLOCK_SIZE, got, (size_t)n));
        ASSERT_TRUE(memcmp(want + starts[i] * STORAGE_BLOCK_SIZE, got, (size_t)n * STORAGE_BLOCK_SIZE) == 0);
    }
    // Past the end is refused, as storage_read_block refuses it.
    ASSERT_ERR(storage_read_blocks(storage, (size_t)(blocks - 1) * STORAGE_BLOCK_SIZE, got, 2), GS_ERROR);
    free(want);
    free(got);
}

TEST(storage_read_blocks_matches_per_block) {
    const uint64_t blocks = 20000;
    // A full base, modified in runs, singles and alternating blocks.
    setup_sandbox();
    create_base_image_bs(BASE_FILE, blocks, STORAGE_BLOCK_SIZE, 0x11);
    storage_config_t config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    assert_read_blocks_matches_per_block(storage, blocks);
    write_run_pattern(storage, blocks, STORAGE_BLOCK_SIZE);
    assert_read_blocks_matches_per_block(storage, blocks);
    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();

    // A base that ends mid-block: the partial block and everything past it
    // read as zeros.
    setup_sandbox();
    create_base_image_bs(BASE_FILE, 5000, STORAGE_BLOCK_SIZE, 0x33);
    ASSERT_TRUE(truncate(BASE_FILE, (off_t)(4999 * STORAGE_BLOCK_SIZE + 200)) == 0);
    config = make_config(BASE_FILE, DELTA_FILE, JOURNAL_FILE, blocks);
    ASSERT_OK(storage_new(&config, &storage));
    assert_read_blocks_matches_per_block(storage, blocks);
    write_run_pattern(storage, blocks, STORAGE_BLOCK_SIZE);
    assert_read_blocks_matches_per_block(storage, blocks);
    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();

    // No base at all.
    setup_sandbox();
    config = make_config(NULL, DELTA_FILE, JOURNAL_FILE, blocks);
    ASSERT_OK(storage_new(&config, &storage));
    write_run_pattern(storage, blocks, STORAGE_BLOCK_SIZE);
    assert_read_blocks_matches_per_block(storage, blocks);
    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

// An unmodified run is one read of the base, however many blocks it spans
// (a disk image converted or exported reads its file in large pieces).
TEST(storage_read_blocks_one_base_read_per_run) {
    setup_sandbox();
    const uint64_t blocks = 2048;
    create_base_image_bs(BASE_FILE, blocks, STORAGE_BLOCK_SIZE, 0x21);
    gs_source_t *host = gs_source_host(BASE_FILE, NULL);
    ASSERT_TRUE(host != NULL);
    storage_config_t config = make_config(NULL, DELTA_FILE, JOURNAL_FILE, blocks);
    config.base = peel_source_new(&counting_base_ops, host, host);
    gs_source_release(host);
    storage_t *storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));
    gs_source_release(config.base); // storage holds its own reference
    uint8_t *buf = malloc(blocks * STORAGE_BLOCK_SIZE);
    ASSERT_TRUE(buf != NULL);
    g_base_reads = 0;
    ASSERT_OK(storage_read_blocks(storage, 0, buf, (size_t)blocks));
    ASSERT_EQ_INT(1, g_base_reads);
    expect_block(1000, 0x21, buf + 1000 * STORAGE_BLOCK_SIZE);
    // One modified block in the middle splits the run in two.
    fill_block(1000, 0x55, buf);
    ASSERT_OK(storage_write_block(storage, 1000 * STORAGE_BLOCK_SIZE, buf));
    g_base_reads = 0;
    ASSERT_OK(storage_read_blocks(storage, 0, buf, (size_t)blocks));
    ASSERT_EQ_INT(2, g_base_reads);
    expect_block(1000, 0x55, buf + 1000 * STORAGE_BLOCK_SIZE);
    expect_block(1001, 0x21, buf + 1001 * STORAGE_BLOCK_SIZE);
    free(buf);
    ASSERT_OK(storage_delete(storage));
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
    RUN(storage_save_state_run_patterns);
    RUN(storage_save_state_runs_532);
    RUN(storage_save_state_no_base);
    RUN(storage_save_state_short_base);
    RUN(storage_load_state_runs);
    RUN(storage_load_state_writes_only_changes);
    RUN(storage_block_past_2gib);
    RUN(storage_journal_entry_out_of_range_is_dropped);
    RUN(storage_journal_partial_tail_is_dropped);
    RUN(storage_v2_last_block_is_one_slot);
    RUN(storage_v2_rollback_truncates_new_slots);
    RUN(storage_v2_reopen_after_crash_is_committed_state);
    RUN(storage_v1_delta_still_opens);
    RUN(storage_read_blocks_matches_per_block);
    RUN(storage_read_blocks_one_base_read_per_run);
    return 0;
}
