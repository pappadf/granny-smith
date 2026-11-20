// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Checkpoint stubs for unit tests
// Provides no-op implementations of checkpoint read/write functions.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The API stubbed here (checkpoint_t, checkpoint_kind_t and the prototypes)
#include "checkpoint.h"
#include "machine_parts.h"

// Checkpoint read/write helpers referenced via x_system_* from cpu.c when
// checkpoint instrumentation is compiled in. Provide no-op versions.
void system_read_checkpoint_data_loc(checkpoint_t *checkpoint, void *data, size_t size, const char *tag,
                                     const char *file, int line) {
    (void)checkpoint;
    (void)tag;
    (void)data;
    (void)size;
    (void)file;
    (void)line;
}

// What a suite asked to see of the writes (stub_checkpoint_capture): the
// bytes of every block, concatenated, up to the buffer's size.
static uint8_t *s_capture;
static size_t s_capture_cap, s_capture_len;

void stub_checkpoint_capture(uint8_t *buf, size_t cap) {
    s_capture = buf;
    s_capture_cap = cap;
    s_capture_len = 0;
}

size_t stub_checkpoint_captured(void) {
    return s_capture_len;
}

void system_write_checkpoint_data_loc(checkpoint_t *checkpoint, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)checkpoint;
    (void)tag;
    (void)file;
    (void)line;
    // Capture, when a suite asked to
    for (size_t i = 0; s_capture && i < size && s_capture_len < s_capture_cap; i++)
        s_capture[s_capture_len++] = ((const uint8_t *)data)[i];
}

bool checkpoint_has_error(checkpoint_t *checkpoint) {
    (void)checkpoint;
    return false;
}

// Restore paths flag a checkpoint they cannot trust rather than asserting on
// it, so any suite that links a module with a restore path needs this symbol.
void checkpoint_set_error(checkpoint_t *checkpoint) {
    (void)checkpoint;
}

checkpoint_kind_t checkpoint_get_kind(checkpoint_t *checkpoint) {
    (void)checkpoint;
    return CHECKPOINT_KIND_QUICK;
}

// File checkpoint helper stubs (no-op for unit tests)
void checkpoint_write_file_loc(checkpoint_t *checkpoint, const char *path, const char *file, int line) {
    (void)checkpoint;
    (void)path;
    (void)file;
    (void)line;
}

size_t checkpoint_read_file_loc(checkpoint_t *checkpoint, uint8_t *dest, size_t capacity, char **out_path,
                                const char *file, int line) {
    (void)checkpoint;
    (void)dest;
    (void)capacity;
    (void)file;
    (void)line;
    if (out_path) {
        *out_path = NULL;
    }
    return 0;
}

// A unit test builds no machine checkpoint: parts register into nothing.
void machine_part_begin(struct config *cfg, checkpoint_t *cp, const char *name) {
    (void)cfg;
    (void)cp;
    (void)name;
}

void machine_part_cancel(struct config *cfg) {
    (void)cfg;
}

void machine_part(struct config *cfg, checkpoint_t *cp, const char *name, machine_part_save_fn save, void *obj) {
    (void)cfg;
    (void)cp;
    (void)name;
    (void)save;
    (void)obj;
}
