#include "storage.h"
#include "test_assert.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SANDBOX_DIR         "_test_sandbox"
#define STORAGE1_DIR        SANDBOX_DIR "/disk1.blocks"
#define STORAGE2_DIR        SANDBOX_DIR "/disk2.blocks"
#define ROLLBACK1_DIR       STORAGE1_DIR "/rollback"
#define ROLLBACK2_DIR       STORAGE2_DIR "/rollback"
#define STATE_FILE          SANDBOX_DIR "/state.bin"
#define TEST_BLOCKS         128

#define ASSERT_OK(expr) ASSERT_EQ_INT(GS_SUCCESS, (expr))
#define ASSERT_ERR(expr, code) ASSERT_EQ_INT((code), (expr))

static void cleanup_dir(const char* path);
static void setup_sandbox(void);
static void teardown_sandbox(void);
static storage_config_t make_config(const char* dir, uint64_t blocks);
static void fill_block(size_t lba, uint8_t salt, uint8_t* buffer);
static void expect_block(size_t lba, uint8_t salt, const uint8_t* buffer);
static void expect_zero(const uint8_t* buffer);
static size_t count_dat_files(const char* dir);
static size_t count_pre_files(const char* dir);
static void read_pre_file(const char* dir, uint32_t lba, uint8_t* buffer);

static void cleanup_dir(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) {
        remove(path);
        return;
    }
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        cleanup_dir(child);
    }
    closedir(dir);
    rmdir(path);
}

static void setup_sandbox(void) {
    cleanup_dir(SANDBOX_DIR);
    ASSERT_TRUE(mkdir(SANDBOX_DIR, 0777) == 0);
}

static void teardown_sandbox(void) {
    cleanup_dir(SANDBOX_DIR);
}

static storage_config_t make_config(const char* dir, uint64_t blocks) {
    storage_config_t config = {0};
    config.path_dir = dir;
    config.block_count = blocks;
    config.block_size = STORAGE_BLOCK_SIZE;
    config.consolidations_per_tick = 4;
    return config;
}

static void fill_block(size_t lba, uint8_t salt, uint8_t* buffer) {
    for (size_t i = 0; i < STORAGE_BLOCK_SIZE; ++i) {
        buffer[i] = (uint8_t)(salt + lba + i);
    }
}

static void expect_block(size_t lba, uint8_t salt, const uint8_t* buffer) {
    for (size_t i = 0; i < STORAGE_BLOCK_SIZE; ++i) {
        ASSERT_TRUE(buffer[i] == (uint8_t)(salt + lba + i));
    }
}

static void expect_zero(const uint8_t* buffer) {
    for (size_t i = 0; i < STORAGE_BLOCK_SIZE; ++i) {
        ASSERT_TRUE(buffer[i] == 0);
    }
}

static size_t count_dat_files(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) {
        return 0;
    }
    size_t count = 0;
    struct dirent* entry = NULL;
    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len >= 4 && strcmp(entry->d_name + (len - 4), ".dat") == 0) {
            count++;
        }
    }
    closedir(d);
    return count;
}

static size_t count_pre_files(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) {
        return 0;
    }
    size_t count = 0;
    struct dirent* entry = NULL;
    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len >= 4 && strcmp(entry->d_name + (len - 4), ".pre") == 0) {
            count++;
        }
    }
    closedir(d);
    return count;
}

static void read_pre_file(const char* dir, uint32_t lba, uint8_t* buffer) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%08X.pre", dir, lba);
    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    size_t read = fread(buffer, 1, STORAGE_BLOCK_SIZE, f);
    fclose(f);
    ASSERT_TRUE(read == STORAGE_BLOCK_SIZE);
}

static int file_write_cb(void* ctx, const void* data, size_t size) {
    FILE* f = (FILE*)ctx;
    return (fwrite(data, 1, size, f) == size) ? 0 : -1;
}

static int file_read_cb(void* ctx, void* data, size_t size) {
    FILE* f = (FILE*)ctx;
    size_t read = fread(data, 1, size, f);
    return (int)read;
}

TEST(storage_invalid_arguments) {
    setup_sandbox();
    storage_config_t config = make_config(STORAGE1_DIR, TEST_BLOCKS);
    storage_t* storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    uint8_t buffer[STORAGE_BLOCK_SIZE];
    memset(buffer, 0xAA, sizeof(buffer));

    ASSERT_ERR(storage_read_block(NULL, 0, buffer), GS_ERROR);
    ASSERT_ERR(storage_write_block(NULL, 0, buffer), GS_ERROR);
    ASSERT_ERR(storage_read_block(storage, 1, buffer), GS_ERROR);
    ASSERT_ERR(storage_write_block(storage, STORAGE_BLOCK_SIZE / 2, buffer), GS_ERROR);
    ASSERT_ERR(storage_tick(NULL), GS_ERROR);
    ASSERT_ERR(storage_checkpoint(NULL, NULL), GS_ERROR);
    ASSERT_ERR(storage_save_state(NULL, NULL, file_write_cb), GS_ERROR);
    ASSERT_ERR(storage_load_state(NULL, NULL, file_read_cb), GS_ERROR);

    ASSERT_OK(storage_delete(storage));

    storage_t* dummy = NULL;
    storage_config_t bad = make_config(NULL, TEST_BLOCKS);
    ASSERT_ERR(storage_new(&bad, &dummy), GS_ERROR);
    bad = make_config(STORAGE1_DIR, 0);
    ASSERT_ERR(storage_new(&bad, &dummy), GS_ERROR);
    teardown_sandbox();
}

TEST(storage_basic_read_write) {
    setup_sandbox();
    storage_config_t config = make_config(STORAGE1_DIR, TEST_BLOCKS);
    storage_t* storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    uint8_t buffer[STORAGE_BLOCK_SIZE];
    ASSERT_OK(storage_read_block(storage, 0, buffer));
    expect_zero(buffer);

    fill_block(3, 0x11, buffer);
    ASSERT_OK(storage_write_block(storage, 3 * STORAGE_BLOCK_SIZE, buffer));

    uint8_t verify[STORAGE_BLOCK_SIZE];
    ASSERT_OK(storage_read_block(storage, 3 * STORAGE_BLOCK_SIZE, verify));
    expect_block(3, 0x11, verify);

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

TEST(storage_state_roundtrip) {
    setup_sandbox();
    storage_config_t config = make_config(STORAGE1_DIR, TEST_BLOCKS);
    storage_t* storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    uint8_t buffer[STORAGE_BLOCK_SIZE];
    for (size_t lba = 0; lba < 8; ++lba) {
        fill_block(lba, 0x20, buffer);
        ASSERT_OK(storage_write_block(storage, lba * STORAGE_BLOCK_SIZE, buffer));
    }

    FILE* state = fopen(STATE_FILE, "wb+");
    ASSERT_TRUE(state != NULL);
    ASSERT_OK(storage_save_state(storage, state, file_write_cb));
    fclose(state);
    ASSERT_OK(storage_delete(storage));

    storage_t* reloaded = NULL;
    storage_config_t config2 = make_config(STORAGE2_DIR, TEST_BLOCKS);
    ASSERT_OK(storage_new(&config2, &reloaded));
    state = fopen(STATE_FILE, "rb");
    ASSERT_TRUE(state != NULL);
    ASSERT_OK(storage_load_state(reloaded, state, file_read_cb));
    fclose(state);

    for (size_t lba = 0; lba < 8; ++lba) {
        ASSERT_OK(storage_read_block(reloaded, lba * STORAGE_BLOCK_SIZE, buffer));
        expect_block(lba, 0x20, buffer);
    }

    ASSERT_OK(storage_delete(reloaded));
    teardown_sandbox();
}

TEST(storage_consolidation) {
    setup_sandbox();
    storage_config_t config = make_config(STORAGE1_DIR, TEST_BLOCKS);
    storage_t* storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    uint8_t buffer[STORAGE_BLOCK_SIZE];
    for (size_t lba = 0; lba < 16; ++lba) {
        fill_block(lba, 0x33, buffer);
        ASSERT_OK(storage_write_block(storage, lba * STORAGE_BLOCK_SIZE, buffer));
    }

    ASSERT_TRUE(count_dat_files(STORAGE1_DIR) == 16);
    ASSERT_OK(storage_tick(storage));
    ASSERT_TRUE(count_dat_files(STORAGE1_DIR) <= 2);

    for (size_t lba = 0; lba < 16; ++lba) {
        ASSERT_OK(storage_read_block(storage, lba * STORAGE_BLOCK_SIZE, buffer));
        expect_block(lba, 0x33, buffer);
    }

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

TEST(storage_rollback_overlay_lifecycle) {
    setup_sandbox();
    storage_config_t config = make_config(STORAGE1_DIR, TEST_BLOCKS);
    storage_t* storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    uint8_t block[STORAGE_BLOCK_SIZE];
    fill_block(5, 0x10, block);
    ASSERT_OK(storage_write_block(storage, 5 * STORAGE_BLOCK_SIZE, block));

    checkpoint_t* fake_checkpoint = (checkpoint_t*)0x1;
    ASSERT_OK(storage_checkpoint(storage, fake_checkpoint));
    ASSERT_TRUE(count_pre_files(ROLLBACK1_DIR) == 0);

    fill_block(5, 0x40, block);
    ASSERT_OK(storage_write_block(storage, 5 * STORAGE_BLOCK_SIZE, block));
    ASSERT_TRUE(count_pre_files(ROLLBACK1_DIR) == 1);

    uint8_t pre[STORAGE_BLOCK_SIZE];
    read_pre_file(ROLLBACK1_DIR, 5, pre);
    expect_block(5, 0x10, pre);

    ASSERT_OK(storage_delete(storage));
    storage = NULL;
    ASSERT_OK(storage_new(&config, &storage));

    uint8_t verify[STORAGE_BLOCK_SIZE];
    ASSERT_OK(storage_read_block(storage, 5 * STORAGE_BLOCK_SIZE, verify));
    expect_block(5, 0x40, verify);

    ASSERT_OK(storage_apply_rollback(storage));
    ASSERT_TRUE(count_pre_files(ROLLBACK1_DIR) == 0);
    ASSERT_OK(storage_read_block(storage, 5 * STORAGE_BLOCK_SIZE, verify));
    expect_block(5, 0x10, verify);

    ASSERT_OK(storage_delete(storage));
    teardown_sandbox();
}

int main(void) {
    RUN(storage_invalid_arguments);
    RUN(storage_basic_read_write);
    RUN(storage_state_roundtrip);
    RUN(storage_consolidation);
    RUN(storage_rollback_overlay_lifecycle);
    return 0;
}
