// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// stub_checkpoint_strings.c
// The bounded count and length-prefixed string readers of checkpoint.c,
// rebuilt over whatever block I/O the suite links (stub_checkpoint.c's no-op
// pair, or a suite's own in-memory stream), so a module that writes strings
// or image references round-trips in the real format.

#include "checkpoint.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// A count the stream claims, refused (and the stream flagged) over `max`
bool checkpoint_read_count(checkpoint_t *checkpoint, uint32_t *out, uint32_t max, const char *what) {
    (void)what;
    *out = 0;
    uint32_t v = 0;
    system_read_checkpoint_data_loc(checkpoint, &v, sizeof v, NULL, __FILE__, __LINE__);
    if (v > max) {
        checkpoint_set_error(checkpoint);
        return false;
    }
    *out = v;
    return true;
}

// uint32 length (with the NUL) + bytes; NULL and "" write length 0
void checkpoint_write_string(checkpoint_t *checkpoint, const char *s) {
    uint32_t len = (s && *s) ? (uint32_t)strlen(s) + 1 : 0;
    system_write_checkpoint_data_loc(checkpoint, &len, sizeof len, NULL, __FILE__, __LINE__);
    if (len)
        system_write_checkpoint_data_loc(checkpoint, s, len, NULL, __FILE__, __LINE__);
}

// What checkpoint_write_string wrote, malloc'd and terminated; NULL for length 0
char *checkpoint_read_string(checkpoint_t *checkpoint, uint32_t max, const char *what) {
    uint32_t len = 0;
    if (!checkpoint_read_count(checkpoint, &len, max, what) || len == 0)
        return NULL;
    char *s = (char *)malloc((size_t)len + 1);
    if (!s) {
        checkpoint_set_error(checkpoint);
        return NULL;
    }
    system_read_checkpoint_data_loc(checkpoint, s, len, NULL, __FILE__, __LINE__);
    s[len] = '\0';
    return s;
}
