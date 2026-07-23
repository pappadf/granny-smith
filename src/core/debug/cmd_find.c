// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_find.c
// `find.*` memory search (shell v2 §6.1): find.str / find.bytes /
// find.word / find.long return the complete V_LIST of match addresses
// (empty list = not found); optional start/end arguments bound the
// scan, defaulting to the whole address space (g_address_mask).

#include "addr_format.h"
#include "debug.h"
#include "memory.h"
#include "object.h"
#include "shell.h"
#include "value.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default number of hits reported before "... (N more, use 'find … all')".
#define FIND_DEFAULT_LIMIT 16

// Cap on pattern length; protects against absurd inputs and bounds the scan buffer.
#define FIND_MAX_PATTERN_LEN 256

// Parse one hex byte token (exactly two hex digits); returns true on success.
static bool parse_hex_byte(const char *tok, uint8_t *byte_out) {
    if (!tok || !tok[0] || !tok[1] || tok[2] != '\0')
        return false;
    int hi = 0, lo = 0;
    // Decode high nibble
    if (!isxdigit((unsigned char)tok[0]) || !isxdigit((unsigned char)tok[1]))
        return false;
    hi = (tok[0] <= '9') ? (tok[0] - '0') : (tolower((unsigned char)tok[0]) - 'a' + 10);
    lo = (tok[1] <= '9') ? (tok[1] - '0') : (tolower((unsigned char)tok[1]) - 'a' + 10);
    *byte_out = (uint8_t)((hi << 4) | lo);
    return true;
}

// === Object-model class descriptor =========================================
//
// Shell v2 §6.1: the `find.*` methods return data — a V_LIST of match
// addresses (empty list = not found) — and the REPL formats it. The
// printed match report and the `all` hit cap are gone: the list is
// always complete (bounded by FIND_MAX_HITS as a runaway guard).
// Optional `start` / `end` arguments bound the scan; `end` is
// inclusive and defaults to the current address mask.

#define FIND_MAX_HITS 65536

// Linear scan of [start..end_incl]; returns a V_LIST of V_UINT hit
// addresses (hex-flagged), or V_ERROR on overflow/oom.
static value_t scan_memory_list(uint32_t start, uint32_t end_incl, const uint8_t *pattern, size_t plen) {
    if (plen == 0)
        return val_err("find: empty pattern");
    uint64_t last = (uint64_t)end_incl;
    if (last + 1 < plen)
        return val_list(NULL, 0);
    uint64_t stop = last - (plen - 1);

    size_t cap = 16, len = 0;
    value_t *items = (value_t *)malloc(cap * sizeof(value_t));
    if (!items)
        return val_err("find: out of memory");

    // Byte-by-byte rolling compare. Use the side-effect-free debug read
    // so a scan crossing unmapped pages can't latch a spurious guest bus
    // error.
    for (uint64_t a = start; a <= stop; a++) {
        if (memory_debug_read_uint8((uint32_t)a) != pattern[0])
            continue;
        bool match = true;
        for (size_t k = 1; k < plen; k++) {
            if (memory_debug_read_uint8((uint32_t)(a + k)) != pattern[k]) {
                match = false;
                break;
            }
        }
        if (!match)
            continue;
        if (len == FIND_MAX_HITS) {
            for (size_t i = 0; i < len; i++)
                value_free(&items[i]);
            free(items);
            return val_err("find: more than %d matches; narrow the range", FIND_MAX_HITS);
        }
        if (len == cap) {
            cap *= 2;
            value_t *t = (value_t *)realloc(items, cap * sizeof(value_t));
            if (!t) {
                free(items);
                return val_err("find: out of memory");
            }
            items = t;
        }
        value_t v = val_uint(4, (uint32_t)a);
        v.flags |= VAL_HEX;
        items[len++] = v;
    }
    return val_list(items, len);
}

// Decode the optional start/end argument pair shared by every method:
// argv[i0] = start (default 0), argv[i0+1] = end inclusive (default
// g_address_mask). A V_NONE hole means "not given".
static bool find_range_args(int argc, const value_t *argv, int i0, uint32_t *start_out, uint32_t *end_out) {
    *start_out = 0;
    *end_out = g_address_mask;
    if (argc > i0 && argv[i0].kind != V_NONE) {
        bool ok = false;
        uint64_t s = val_as_u64(&argv[i0], &ok);
        if (!ok)
            return false;
        *start_out = (uint32_t)s;
    }
    if (argc > i0 + 1 && argv[i0 + 1].kind != V_NONE) {
        bool ok = false;
        uint64_t e = val_as_u64(&argv[i0 + 1], &ok);
        if (!ok)
            return false;
        *end_out = (uint32_t)e;
    }
    return *end_out >= *start_out;
}

static value_t find_method_str(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *text = argv[0].s ? argv[0].s : "";
    size_t n = strlen(text);
    if (n == 0)
        return val_err("find.str: empty pattern");
    if (n > FIND_MAX_PATTERN_LEN)
        return val_err("find.str: pattern too long (max %d)", FIND_MAX_PATTERN_LEN);
    uint32_t start, end;
    if (!find_range_args(argc, argv, 1, &start, &end))
        return val_err("find.str: invalid range");
    return scan_memory_list(start, end, (const uint8_t *)text, n);
}

static value_t find_method_bytes(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    // Pattern arrives as a space-separated hex string ("4E 71").
    uint8_t pattern[FIND_MAX_PATTERN_LEN];
    size_t plen = 0;
    const char *p = argv[0].s ? argv[0].s : "";
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char tok[3] = {0};
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]))
            return val_err("find.bytes: expected 2-digit hex tokens (e.g. \"4E B9\")");
        tok[0] = p[0];
        tok[1] = p[1];
        p += 2;
        if (*p && *p != ' ' && *p != '\t')
            return val_err("find.bytes: expected 2-digit hex tokens (e.g. \"4E B9\")");
        uint8_t b;
        if (!parse_hex_byte(tok, &b))
            return val_err("find.bytes: bad hex byte '%s'", tok);
        if (plen >= FIND_MAX_PATTERN_LEN)
            return val_err("find.bytes: pattern too long (max %d)", FIND_MAX_PATTERN_LEN);
        pattern[plen++] = b;
    }
    if (plen == 0)
        return val_err("find.bytes: empty pattern");
    uint32_t start, end;
    if (!find_range_args(argc, argv, 1, &start, &end))
        return val_err("find.bytes: invalid range");
    return scan_memory_list(start, end, pattern, plen);
}

static value_t find_int_common(const char *label, size_t width, int argc, const value_t *argv) {
    bool ok = false;
    uint64_t value = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("%s: value must be an integer", label);
    if (width == 2 && value > 0xFFFFu)
        return val_err("%s: value 0x%llx exceeds 16 bits", label, (unsigned long long)value);
    if (width == 4 && value > 0xFFFFFFFFu)
        return val_err("%s: value 0x%llx exceeds 32 bits", label, (unsigned long long)value);
    uint8_t pattern[4];
    for (size_t i = 0; i < width; i++)
        pattern[i] = (uint8_t)(value >> (8 * (width - 1 - i))); // 68K big-endian
    uint32_t start, end;
    if (!find_range_args(argc, argv, 1, &start, &end))
        return val_err("%s: invalid range", label);
    return scan_memory_list(start, end, pattern, width);
}

static value_t find_method_word(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    return find_int_common("find.word", 2, argc, argv);
}

static value_t find_method_long(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    return find_int_common("find.long", 4, argc, argv);
}

static const arg_decl_t find_str_args[] = {
    {.name = "text", .kind = V_STRING, .doc = "Search text"},
    {.name = "start",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_HEX,
     .doc = "Scan start address (default 0)"},
    {.name = "end",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_HEX,
     .doc = "Scan end address, inclusive (default: address mask)"},
};
static const arg_decl_t find_bytes_args[] = {
    {.name = "hex", .kind = V_STRING, .doc = "Space-separated hex bytes (\"4E 71\")"},
    {.name = "start",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_HEX,
     .doc = "Scan start address (default 0)"},
    {.name = "end",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_HEX,
     .doc = "Scan end address, inclusive (default: address mask)"},
};
static const arg_decl_t find_int_args[] = {
    {.name = "value", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "Integer value to search for"},
    {.name = "start",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_HEX,
     .doc = "Scan start address (default 0)"},
    {.name = "end",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_HEX,
     .doc = "Scan end address, inclusive (default: address mask)"},
};

static const member_t find_members[] = {
    {.kind = M_METHOD,
     .name = "str",
     .doc = "Search memory for a UTF-8 string; returns the list of match addresses",
     .method = {.args = find_str_args, .nargs = 3, .result = V_LIST, .fn = find_method_str}    },
    {.kind = M_METHOD,
     .name = "bytes",
     .doc = "Search memory for a byte sequence (hex string); returns the match addresses",
     .method = {.args = find_bytes_args, .nargs = 3, .result = V_LIST, .fn = find_method_bytes}},
    {.kind = M_METHOD,
     .name = "long",
     .doc = "Search memory for a 32-bit big-endian value; returns the match addresses",
     .method = {.args = find_int_args, .nargs = 3, .result = V_LIST, .fn = find_method_long}   },
    {.kind = M_METHOD,
     .name = "word",
     .doc = "Search memory for a 16-bit big-endian value; returns the match addresses",
     .method = {.args = find_int_args, .nargs = 3, .result = V_LIST, .fn = find_method_word}   },
};

const class_desc_t find_class = {
    .name = "find",
    .members = find_members,
    .n_members = sizeof(find_members) / sizeof(find_members[0]),
};

// === Process-singleton lifecycle ============================================
//
// `find` is a stateless facade — its methods scan whichever memory map
// is currently active. Register once at shell_init.

static struct object *s_find_object = NULL;

void find_class_register(void) {
    if (s_find_object)
        return;
    s_find_object = object_new(&find_class, NULL, "find");
    if (s_find_object)
        object_attach(object_root(), s_find_object);
}

void find_class_unregister(void) {
    if (s_find_object) {
        object_detach(s_find_object);
        object_delete(s_find_object);
        s_find_object = NULL;
    }
}
