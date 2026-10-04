// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// The checkpoint parts list (machine_parts.h) over the real stream reader.
//
// A machine's checkpoint is its parts in construction order, each part's name
// ahead of its block.  What the list owes a restore: a checkpoint that does
// not match the machine fails at the first part that differs, BEFORE that
// part's constructor reads anything -- no constructor is handed another
// part's bytes -- and a checkpoint cut short fails rather than leaving a part
// half-read.  Each test writes a valid stream through the list itself, then
// restores it into a machine that differs, or from a file that was cut.

#include "build_id.h"
#include "checkpoint.h"
#include "machine_parts.h"
#include "system_config.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CP_PATH "_machine_parts_test.checkpoint"

// === Stubs for what checkpoint.c reaches outside its module ==================

const char *get_build_id(void) {
    return "unit-test-build-0001";
}
int system_checkpoint_load(const char *filename) {
    (void)filename;
    return 1;
}
int system_checkpoint(const char *filename, checkpoint_kind_t kind) {
    (void)filename;
    (void)kind;
    return 1;
}
bool system_checkpoint_probe(void) {
    return false;
}
const char *find_valid_checkpoint_path(void) {
    return NULL;
}
int gs_background_checkpoint(const char *label) {
    (void)label;
    return -1;
}
bool gs_checkpoint_auto_get(void) {
    return false;
}
int gs_checkpoint_auto_set(bool on) {
    (void)on;
    return -2;
}
int gs_checkpoint_clear(void) {
    return 0;
}

// === A toy machine ===========================================================
//
// Each "device" is one uint32_t; its constructor reads its block on a restore,
// the way every real constructor does.

static void save_u32(void *obj, checkpoint_t *cp) {
    system_write_checkpoint_data(cp, obj, sizeof(uint32_t));
}

static config_t *machine_new(void) {
    return (config_t *)calloc(1, sizeof(config_t));
}

static void machine_free(config_t *cfg) {
    machine_parts_free(cfg);
    free(cfg);
}

// Build one device as the part `name`: open the part, construct (reading the
// block on a restore), register.
static void build_device(config_t *cfg, checkpoint_t *cp, const char *name, uint32_t *dev, uint32_t power_on) {
    machine_part_begin(cfg, cp, name);
    if (cp)
        system_read_checkpoint_data(cp, dev, sizeof *dev);
    else
        *dev = power_on;
    machine_part(cfg, cp, name, save_u32, dev);
}

// Save a machine of parts `names` holding `values`.
static void save_machine(const char *const *names, const uint32_t *values, int n) {
    remove(CP_PATH);
    config_t *cfg = machine_new();
    uint32_t devs[8];
    for (int i = 0; i < n; i++)
        build_device(cfg, NULL, names[i], &devs[i], values[i]);
    checkpoint_t *cp = checkpoint_open_write(CP_PATH, CHECKPOINT_KIND_CONSOLIDATED);
    ASSERT_TRUE(cp != NULL);
    machine_parts_save(cfg, cp);
    ASSERT_EQ_INT(checkpoint_has_error(cp), 0);
    checkpoint_close(cp);
    machine_free(cfg);
}

// Restore into a machine of parts `names`; returns whether the checkpoint
// ended without error, and what each device read.
static bool restore_machine(const char *const *names, uint32_t *out, int n) {
    checkpoint_t *cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    config_t *cfg = machine_new();
    for (int i = 0; i < n; i++) {
        out[i] = 0xDEADBEEFu;
        build_device(cfg, cp, names[i], &out[i], 0);
    }
    bool ok = !checkpoint_has_error(cp);
    checkpoint_close(cp);
    machine_free(cfg);
    return ok;
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void truncate_to(const char *path, long bytes) {
    long n = file_size(path);
    if (n < 0 || bytes >= n)
        return;
    uint8_t *buf = (uint8_t *)malloc((size_t)bytes);
    FILE *f = fopen(path, "rb");
    size_t got = fread(buf, 1, (size_t)bytes, f);
    fclose(f);
    f = fopen(path, "wb");
    fwrite(buf, 1, got, f);
    fclose(f);
    free(buf);
}

static const char *const ABC[] = {"a", "b", "c"};
static const uint32_t VALUES[] = {0x11111111u, 0x22222222u, 0x33333333u};

// === Tests ===================================================================

// The machine that saved restores exactly; so the rejections below cannot
// pass vacuously.
TEST(same_machine_round_trips) {
    save_machine(ABC, VALUES, 3);
    uint32_t got[3];
    ASSERT_TRUE(restore_machine(ABC, got, 3));
    for (int i = 0; i < 3; i++)
        ASSERT_TRUE(got[i] == VALUES[i]);
    remove(CP_PATH);
}

// A machine whose second part differs fails there, and that part's constructor
// is not handed the saved second part's bytes: the name is checked first, and
// after the error every read returns zeros.
TEST(mismatched_part_fails_before_its_block_is_read) {
    save_machine(ABC, VALUES, 3);
    static const char *const AXC[] = {"a", "x", "c"};
    uint32_t got[3];
    ASSERT_TRUE(!restore_machine(AXC, got, 3));
    ASSERT_TRUE(got[0] == VALUES[0]); // the matching part before it restored
    ASSERT_TRUE(got[1] == 0); // not 0x22222222: "x" never saw b's block
    ASSERT_TRUE(got[2] == 0);
    remove(CP_PATH);
}

// Two equal-sized parts in the other order: the sizes cannot tell them apart,
// the names do.
TEST(reordered_parts_fail_by_name) {
    save_machine(ABC, VALUES, 3);
    static const char *const ACB[] = {"a", "c", "b"};
    uint32_t got[3];
    ASSERT_TRUE(!restore_machine(ACB, got, 3));
    ASSERT_TRUE(got[1] != VALUES[2]); // "c" did not take b's state
    remove(CP_PATH);
}

// A machine with a part the file does not have (the file ends early from the
// machine's point of view).
TEST(extra_part_fails) {
    save_machine(ABC, VALUES, 2);
    uint32_t got[3];
    ASSERT_TRUE(!restore_machine(ABC, got, 3));
    remove(CP_PATH);
}

// A checkpoint cut anywhere fails: inside the last part's block, or inside a
// part's name.
TEST(truncated_checkpoint_fails) {
    save_machine(ABC, VALUES, 3);
    long full = file_size(CP_PATH);
    ASSERT_TRUE(full > 0);
    for (long cut = 1; cut <= 48; cut += 1) {
        save_machine(ABC, VALUES, 3);
        truncate_to(CP_PATH, full - cut);
        uint32_t got[3];
        ASSERT_TRUE(!restore_machine(ABC, got, 3));
    }
    remove(CP_PATH);
}

// machine_part_expect is the same check for the reader that runs before
// there is a machine (system_restore's board part).
TEST(expect_checks_one_name) {
    save_machine(ABC, VALUES, 3);
    checkpoint_t *cp = checkpoint_open_read(CP_PATH);
    ASSERT_TRUE(cp != NULL);
    ASSERT_TRUE(machine_part_expect(cp, "a", 0));
    uint32_t v = 0;
    system_read_checkpoint_data(cp, &v, sizeof v);
    ASSERT_TRUE(v == VALUES[0]);
    ASSERT_TRUE(!machine_part_expect(cp, "c", 1));
    ASSERT_TRUE(checkpoint_has_error(cp));
    checkpoint_close(cp);
    remove(CP_PATH);
}

int main(void) {
    remove(CP_PATH);
    RUN(same_machine_round_trips);
    RUN(mismatched_part_fails_before_its_block_is_read);
    RUN(reordered_parts_fail_by_name);
    RUN(extra_part_fails);
    RUN(truncated_checkpoint_fails);
    RUN(expect_checks_one_name);
    remove(CP_PATH);
    return 0;
}
