// Checkpoint stubs for unit tests
// Provides no-op implementations of checkpoint read/write functions.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pull in the checkpoint typedef from common.h
#include "../../../src/core/common.h"

// Minimal checkpoint kind enum (for tests not linking with system.h)
#ifndef CHECKPOINT_KIND_DEFINED
typedef enum {
    CHECKPOINT_KIND_QUICK = 0,
    CHECKPOINT_KIND_CONSOLIDATED = 1,
} checkpoint_kind_t;
#define CHECKPOINT_KIND_DEFINED
#endif

// Checkpoint read/write helpers referenced via x_system_* from cpu.c when
// checkpoint instrumentation is compiled in. Provide no-op versions.
void system_read_checkpoint_data_loc(checkpoint_t *checkpoint, void *data,
                                   size_t size, const char *file, int line) {
    (void)checkpoint; (void)data; (void)size; (void)file; (void)line;
}

void system_write_checkpoint_data_loc(checkpoint_t *checkpoint, const void *data,
                                    size_t size, const char *file, int line) {
    (void)checkpoint; (void)data; (void)size; (void)file; (void)line;
}

bool checkpoint_has_error(checkpoint_t *checkpoint) {
    (void)checkpoint;
    return false;
}

checkpoint_kind_t checkpoint_get_kind(checkpoint_t *checkpoint) {
    (void)checkpoint;
    return CHECKPOINT_KIND_QUICK;
}

// File checkpoint helper stubs (no-op for unit tests)
void checkpoint_write_file_loc(checkpoint_t *checkpoint, const char *path,
                       const char *file, int line) {
    (void)checkpoint; (void)path; (void)file; (void)line;
}

size_t checkpoint_read_file_loc(checkpoint_t *checkpoint, uint8_t *dest,
                                   size_t capacity, char **out_path,
                                   const char *file, int line) {
    (void)checkpoint; (void)dest; (void)capacity; (void)file; (void)line;
    if (out_path) { *out_path = NULL; }
    return 0;
}
