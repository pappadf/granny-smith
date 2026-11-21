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

// uint32 length + bytes, both under `tag`; no data writes length 0
void checkpoint_write_blob(checkpoint_t *checkpoint, const void *data, uint32_t len, const char *tag) {
    if (!data)
        len = 0;
    system_write_checkpoint_data_loc(checkpoint, &len, sizeof len, tag, __FILE__, __LINE__);
    if (len)
        system_write_checkpoint_data_loc(checkpoint, data, len, tag, __FILE__, __LINE__);
}

// What checkpoint_write_blob wrote, malloc'd; NULL (and *len 0) when empty or refused
uint8_t *checkpoint_read_blob(checkpoint_t *checkpoint, uint32_t max, uint32_t *len, const char *what) {
    uint32_t n = 0;
    *len = 0;
    system_read_checkpoint_data_loc(checkpoint, &n, sizeof n, what, __FILE__, __LINE__);
    if (checkpoint_has_error(checkpoint) || n == 0)
        return NULL;
    if (n > max) {
        checkpoint_set_error(checkpoint);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) {
        checkpoint_set_error(checkpoint);
        return NULL;
    }
    system_read_checkpoint_data_loc(checkpoint, buf, n, what, __FILE__, __LINE__);
    if (checkpoint_has_error(checkpoint)) {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

// True when the `page` bytes at `pg` are all zero
static bool page_is_zero(const uint8_t *pg, uint32_t page) {
    for (uint32_t b = 0; b < page; b++)
        if (pg[b])
            return false;
    return true;
}

// The non-zero `page`-byte pages of `buf`: a count, then index + bytes each
void checkpoint_write_sparse(checkpoint_t *checkpoint, const uint8_t *buf, size_t size, uint32_t page) {
    uint32_t pages = (uint32_t)(size / page), live = 0;
    for (uint32_t i = 0; i < pages; i++)
        live += !page_is_zero(buf + (size_t)i * page, page);
    system_write_checkpoint_data_loc(checkpoint, &live, sizeof live, NULL, __FILE__, __LINE__);
    for (uint32_t i = 0; i < pages; i++) {
        if (page_is_zero(buf + (size_t)i * page, page))
            continue;
        system_write_checkpoint_data_loc(checkpoint, &i, sizeof i, NULL, __FILE__, __LINE__);
        system_write_checkpoint_data_loc(checkpoint, buf + (size_t)i * page, page, NULL, __FILE__, __LINE__);
    }
}

// What checkpoint_write_sparse wrote; a bad count or index flags the stream
void checkpoint_read_sparse(checkpoint_t *checkpoint, uint8_t *buf, size_t size, uint32_t page) {
    memset(buf, 0, size);
    uint32_t pages = (uint32_t)(size / page), live = 0;
    if (!checkpoint_read_count(checkpoint, &live, pages, "sparse pages"))
        return;
    for (uint32_t n = 0; n < live; n++) {
        uint32_t i = 0;
        system_read_checkpoint_data_loc(checkpoint, &i, sizeof i, NULL, __FILE__, __LINE__);
        if (checkpoint_has_error(checkpoint))
            return;
        if (i >= pages) {
            checkpoint_set_error(checkpoint);
            return;
        }
        system_read_checkpoint_data_loc(checkpoint, buf + (size_t)i * page, page, NULL, __FILE__, __LINE__);
    }
}
