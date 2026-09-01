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
    return 0;
}
