// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the resource-fork parser (src/core/storage/resource_fork.c).
// Exercises the documented format edge cases: num_types-1 and count-1 off-by-
// one fields, signed int16 IDs at the boundaries, type 4-CCs with a trailing
// space and with high-bit MacRoman bytes, empty resources, single-resource
// forks, missing-name (name_off == -1) records, and assorted corruption
// scenarios that must produce a defined error rather than a crash.

#include "resource_fork.h"
#include "test_assert.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Big-endian writer helpers --------------------------------------------

static void w_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}
static void w_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void w_i16(uint8_t *p, int16_t v) {
    w_u16(p, (uint16_t)v);
}

// ---- Fork builder ----------------------------------------------------------
//
// Constructs a tiny but format-faithful resource fork in a freshly-malloc'd
// buffer.  Each type takes 8 bytes in the type list and contributes 12
// bytes per resource in the ref list.  Names are appended at the end of
// the map area.

typedef struct {
    const uint8_t cc[4];
    int16_t id;
    int16_t name_off; // -1 = no name
    uint8_t attrs;
    size_t data_len;
    const uint8_t *data; // borrowed
} test_res_t;

// Build a fork from `resources` (already grouped by type — order matters
// because the on-disk format groups ref entries per type).  `n_types` and
// `type_counts` describe the grouping.  Returns malloc'd buffer + len.
static uint8_t *build_fork(const test_res_t *resources, size_t n_resources, const uint8_t (*type_ccs)[4],
                           const size_t *type_counts, size_t n_types, const char *const *names, size_t *out_len) {
    // Data area: 4 bytes length prefix + resource bytes per entry, no
    // alignment padding (we read at any offset, the alignment hint in the
    // format is a hint not a requirement).
    size_t data_area = 0;
    for (size_t r = 0; r < n_resources; r++)
        data_area += 4 + resources[r].data_len;
    size_t name_area = 0;
    for (size_t i = 0; names && names[i]; i++)
        name_area += 1 + strlen(names[i]); // Pascal pstring
    size_t type_table_bytes = 2 + 8 * n_types;
    size_t ref_table_bytes = 12 * n_resources;
    size_t map_area = 30 + (type_table_bytes - 2) + ref_table_bytes + name_area;
    // Layout: 16-byte header + data area + map area.
    size_t fork_len = 16 + data_area + map_area;
    uint8_t *buf = calloc(1, fork_len);
    uint32_t data_off = 16;
    uint32_t map_off = (uint32_t)(16 + data_area);
    uint32_t data_len = (uint32_t)data_area;
    uint32_t map_len = (uint32_t)map_area;
    w_u32(buf + 0, data_off);
    w_u32(buf + 4, map_off);
    w_u32(buf + 8, data_len);
    w_u32(buf + 12, map_len);

    // Lay out data records and record absolute offsets within the data area.
    uint32_t *res_data_off = calloc(n_resources, sizeof(uint32_t));
    uint32_t cur = 0;
    for (size_t r = 0; r < n_resources; r++) {
        res_data_off[r] = cur;
        w_u32(buf + data_off + cur, (uint32_t)resources[r].data_len);
        memcpy(buf + data_off + cur + 4, resources[r].data, resources[r].data_len);
        cur += 4 + (uint32_t)resources[r].data_len;
    }

    // Map area.  Header copy 0..15 + 4 reserved bytes (16..19) +
    // reserved handle (20..21) + fork attrs (22..23) + offsets (24..27) +
    // num types - 1 (28..29).  Type list begins at 28 in the map, so
    // type_list_off (relative to map start) is 28.
    uint8_t *map = buf + map_off;
    uint16_t type_list_off = 28;
    uint16_t name_list_off = (uint16_t)(type_list_off + 2 + 8 * n_types + 12 * n_resources);
    w_u16(map + 22, 0); // fork attrs
    w_u16(map + 24, type_list_off);
    w_u16(map + 26, name_list_off);
    if (n_types == 0)
        w_u16(map + type_list_off, 0xFFFF); // num_types - 1 sentinel for empty fork
    else
        w_u16(map + type_list_off, (uint16_t)(n_types - 1));

    // Type list entries start at type_list_off + 2; ref list entries follow
    // the type list contiguously.
    uint16_t type_entry_off = (uint16_t)(type_list_off + 2);
    uint16_t ref_table_start = (uint16_t)(type_entry_off + 8 * n_types);
    size_t resource_cursor = 0;
    for (size_t t = 0; t < n_types; t++) {
        uint8_t *te = map + type_entry_off + 8 * t;
        memcpy(te, type_ccs[t], 4);
        w_u16(te + 4, (uint16_t)(type_counts[t] - 1));
        // ref-list offset is relative to type_list_off (not entry start).
        uint16_t this_ref_off = (uint16_t)(ref_table_start + 12 * resource_cursor - type_list_off);
        w_u16(te + 6, this_ref_off);
        for (size_t r = 0; r < type_counts[t]; r++) {
            uint8_t *re = map + ref_table_start + 12 * (resource_cursor + r);
            w_i16(re + 0, resources[resource_cursor + r].id);
            w_i16(re + 2, resources[resource_cursor + r].name_off);
            re[4] = resources[resource_cursor + r].attrs;
            uint32_t doff = res_data_off[resource_cursor + r];
            re[5] = (uint8_t)((doff >> 16) & 0xFF);
            re[6] = (uint8_t)((doff >> 8) & 0xFF);
            re[7] = (uint8_t)(doff & 0xFF);
            // 4 reserved bytes follow (already zeroed by calloc).
        }
        resource_cursor += type_counts[t];
    }
    // Append names as Pascal pstrings.
    if (names) {
        uint8_t *np = map + name_list_off;
        for (size_t i = 0; names[i]; i++) {
            size_t l = strlen(names[i]);
            np[0] = (uint8_t)l;
            memcpy(np + 1, names[i], l);
            np += 1 + l;
        }
    }
    free(res_data_off);
    *out_len = fork_len;
    return buf;
}

// ---- Tests -----------------------------------------------------------------

TEST(test_parse_empty_fork) {
    // Fork with zero types: num_types-1 stores 0xFFFF.
    size_t fork_len;
    uint8_t *buf = build_fork(NULL, 0, NULL, NULL, 0, NULL, &fork_len);
    const char *err = NULL;
    rfork_t *rf = rfork_parse(buf, fork_len, &err);
    ASSERT_TRUE(rf != NULL);
    ASSERT_EQ_INT(0, (int)rfork_num_types(rf));
    rfork_free(rf);
    free(buf);
}

TEST(test_parse_two_types_multi_ids) {
    uint8_t cc[2][4] = {
        {'C', 'O', 'D', 'E'},
        {'v', 'e', 'r', 's'}
    };
    size_t counts[2] = {2, 1};
    const uint8_t code1[] = {0xDE, 0xAD};
    const uint8_t code2[] = {0xBE, 0xEF, 0xCA, 0xFE};
    const uint8_t vers1[] = {0x01, 0x00, 0x80, 0x01, 0x00, 0x00};
    const char *names[] = {"Main", "Init", NULL};
    test_res_t res[] = {
        {.cc = {'C', 'O', 'D', 'E'}, .id = 1, .name_off = 0,  .attrs = 0x04, .data_len = 2, .data = code1},
        {.cc = {'C', 'O', 'D', 'E'}, .id = 2, .name_off = 5,  .attrs = 0x00, .data_len = 4, .data = code2},
        {.cc = {'v', 'e', 'r', 's'}, .id = 1, .name_off = -1, .attrs = 0x00, .data_len = 6, .data = vers1},
    };
    size_t fork_len;
    uint8_t *buf = build_fork(res, 3, cc, counts, 2, names, &fork_len);
    const char *err = NULL;
    rfork_t *rf = rfork_parse(buf, fork_len, &err);
    ASSERT_TRUE(rf != NULL);
    ASSERT_EQ_INT(2, (int)rfork_num_types(rf));

    uint8_t qcc[4] = {'C', 'O', 'D', 'E'};
    ASSERT_EQ_INT(2, (int)rfork_num_resources(rf, qcc));
    ASSERT_EQ_INT(1, rfork_id_at(rf, qcc, 0));
    ASSERT_EQ_INT(2, rfork_id_at(rf, qcc, 1));

    const uint8_t *bytes = NULL;
    size_t sz = 0;
    const char *name = NULL;
    uint8_t attrs = 0;
    ASSERT_EQ_INT(0, rfork_lookup(rf, qcc, 1, &bytes, &sz, &name, &attrs));
    ASSERT_EQ_INT(2, (int)sz);
    ASSERT_EQ_INT(0x04, attrs);
    ASSERT_EQ_INT(0, memcmp(bytes, code1, 2));
    ASSERT_EQ_INT(0, strcmp(name, "Main"));

    uint8_t vcc[4] = {'v', 'e', 'r', 's'};
    ASSERT_EQ_INT(0, rfork_lookup(rf, vcc, 1, &bytes, &sz, &name, &attrs));
    ASSERT_EQ_INT(6, (int)sz);
    ASSERT_EQ_INT(0, strcmp(name, "")); // no name on this resource

    rfork_free(rf);
    free(buf);
}

TEST(test_id_boundaries) {
    // IDs at int16 extremes: -32768, -1, 0, 32767.
    uint8_t cc[1][4] = {
        {'B', 'O', 'U', 'N'}
    };
    size_t counts[1] = {4};
    const uint8_t payload[1] = {0x01};
    test_res_t res[4] = {
        {.cc = {'B', 'O', 'U', 'N'}, .id = -32768, .name_off = -1, .attrs = 0, .data_len = 1, .data = payload},
        {.cc = {'B', 'O', 'U', 'N'}, .id = -1,     .name_off = -1, .attrs = 0, .data_len = 1, .data = payload},
        {.cc = {'B', 'O', 'U', 'N'}, .id = 0,      .name_off = -1, .attrs = 0, .data_len = 1, .data = payload},
        {.cc = {'B', 'O', 'U', 'N'}, .id = 32767,  .name_off = -1, .attrs = 0, .data_len = 1, .data = payload},
    };
    size_t fork_len;
    uint8_t *buf = build_fork(res, 4, cc, counts, 1, NULL, &fork_len);
    const char *err = NULL;
    rfork_t *rf = rfork_parse(buf, fork_len, &err);
    ASSERT_TRUE(rf != NULL);
    uint8_t qcc[4] = {'B', 'O', 'U', 'N'};
    ASSERT_EQ_INT(0, rfork_lookup(rf, qcc, -32768, NULL, NULL, NULL, NULL));
    ASSERT_EQ_INT(0, rfork_lookup(rf, qcc, 32767, NULL, NULL, NULL, NULL));
    ASSERT_EQ_INT(-ENOENT, rfork_lookup(rf, qcc, 1, NULL, NULL, NULL, NULL));
    rfork_free(rf);
    free(buf);
}

TEST(test_type_trailing_space) {
    // The "STR " type's trailing space round-trips through path conversion.
    uint8_t cc[1][4] = {
        {'S', 'T', 'R', ' '}
    };
    size_t counts[1] = {1};
    const uint8_t payload[] = {0x03, 'a', 'b', 'c'};
    test_res_t res[1] = {
        {.cc = {'S', 'T', 'R', ' '}, .id = 128, .name_off = -1, .data_len = 4, .data = payload}
    };
    size_t fork_len;
    uint8_t *buf = build_fork(res, 1, cc, counts, 1, NULL, &fork_len);
    rfork_t *rf = rfork_parse(buf, fork_len, NULL);
    ASSERT_TRUE(rf != NULL);
    const uint8_t *type_cc = rfork_type_at(rf, 0);
    ASSERT_TRUE(type_cc != NULL);
    ASSERT_EQ_INT(0, memcmp(type_cc, "STR ", 4));

    char path[16];
    rfork_type_to_path(type_cc, path, sizeof(path));
    ASSERT_EQ_INT(0, strcmp(path, "STR "));
    uint8_t round[4];
    ASSERT_EQ_INT(0, rfork_type_from_path("STR ", round));
    ASSERT_EQ_INT(0, memcmp(round, "STR ", 4));

    rfork_free(rf);
    free(buf);
}

TEST(test_type_high_bit_macroman) {
    // Type 4-CC containing high-bit MacRoman bytes round-trips through
    // UTF-8 path conversion.  In MacRoman: 0xA9 → © (U+00A9), 0xA8 → ®
    // (U+00AE), 0xAA → ™ (U+2122), 0xAB → ´ (U+00B4).  All four map into
    // BMP codepoints with 2- or 3-byte UTF-8 encodings; we round-trip
    // them all the way through type_from_path back to raw MacRoman.
    uint8_t cc[1][4] = {
        {0xA9, 0xA8, 0xAA, 0xAB}
    };
    size_t counts[1] = {1};
    const uint8_t payload[1] = {0xFF};
    test_res_t res[1] = {
        {.cc = {0xA9, 0xA8, 0xAA, 0xAB}, .id = 1, .name_off = -1, .data_len = 1, .data = payload}
    };
    size_t fork_len;
    uint8_t *buf = build_fork(res, 1, cc, counts, 1, NULL, &fork_len);
    rfork_t *rf = rfork_parse(buf, fork_len, NULL);
    ASSERT_TRUE(rf != NULL);

    char path[16];
    rfork_type_to_path(cc[0], path, sizeof(path));
    // © U+00A9 → C2 A9; ® U+00AE → C2 AE; ™ U+2122 → E2 84 A2; ´ U+00B4 → C2 B4.
    ASSERT_EQ_INT((int)0xC2, (int)(uint8_t)path[0]);
    ASSERT_EQ_INT((int)0xA9, (int)(uint8_t)path[1]);
    ASSERT_EQ_INT((int)0xC2, (int)(uint8_t)path[2]);
    ASSERT_EQ_INT((int)0xAE, (int)(uint8_t)path[3]);
    ASSERT_EQ_INT((int)0xE2, (int)(uint8_t)path[4]);
    ASSERT_EQ_INT((int)0x84, (int)(uint8_t)path[5]);
    ASSERT_EQ_INT((int)0xA2, (int)(uint8_t)path[6]);
    ASSERT_EQ_INT((int)0xC2, (int)(uint8_t)path[7]);
    ASSERT_EQ_INT((int)0xB4, (int)(uint8_t)path[8]);

    uint8_t round[4];
    ASSERT_EQ_INT(0, rfork_type_from_path(path, round));
    ASSERT_EQ_INT(0, memcmp(round, cc[0], 4));
    rfork_free(rf);
    free(buf);
}

TEST(test_empty_resource) {
    uint8_t cc[1][4] = {
        {'E', 'M', 'P', 'T'}
    };
    size_t counts[1] = {1};
    test_res_t res[1] = {
        {.cc = {'E', 'M', 'P', 'T'}, .id = 0, .name_off = -1, .data_len = 0, .data = NULL}
    };
    size_t fork_len;
    uint8_t *buf = build_fork(res, 1, cc, counts, 1, NULL, &fork_len);
    rfork_t *rf = rfork_parse(buf, fork_len, NULL);
    ASSERT_TRUE(rf != NULL);
    const uint8_t *bytes = (const uint8_t *)1;
    size_t sz = 999;
    ASSERT_EQ_INT(0, rfork_lookup(rf, cc[0], 0, &bytes, &sz, NULL, NULL));
    ASSERT_EQ_INT(0, (int)sz);
    rfork_free(rf);
    free(buf);
}

TEST(test_single_resource_fork) {
    uint8_t cc[1][4] = {
        {'O', 'N', 'L', 'Y'}
    };
    size_t counts[1] = {1};
    const uint8_t payload[3] = {1, 2, 3};
    const char *names[] = {"Sole", NULL};
    test_res_t res[1] = {
        {.cc = {'O', 'N', 'L', 'Y'}, .id = 5, .name_off = 0, .data_len = 3, .data = payload}
    };
    size_t fork_len;
    uint8_t *buf = build_fork(res, 1, cc, counts, 1, names, &fork_len);
    rfork_t *rf = rfork_parse(buf, fork_len, NULL);
    ASSERT_TRUE(rf != NULL);
    const uint8_t *bytes = NULL;
    size_t sz = 0;
    const char *name = NULL;
    ASSERT_EQ_INT(0, rfork_lookup(rf, cc[0], 5, &bytes, &sz, &name, NULL));
    ASSERT_EQ_INT(3, (int)sz);
    ASSERT_EQ_INT(0, strcmp(name, "Sole"));
    rfork_free(rf);
    free(buf);
}

TEST(test_truncated_header) {
    uint8_t buf[8] = {0};
    const char *err = NULL;
    rfork_t *rf = rfork_parse(buf, sizeof(buf), &err);
    ASSERT_TRUE(rf == NULL);
    ASSERT_TRUE(err != NULL);
}

TEST(test_corrupt_offsets) {
    // Header fields claim data and map regions past the end of the buffer.
    uint8_t buf[64] = {0};
    w_u32(buf + 0, 16); // data_off
    w_u32(buf + 4, 4096); // map_off way past EOF
    w_u32(buf + 8, 0);
    w_u32(buf + 12, 30); // map_len
    const char *err = NULL;
    rfork_t *rf = rfork_parse(buf, sizeof(buf), &err);
    ASSERT_TRUE(rf == NULL);
    ASSERT_TRUE(err != NULL);
}

TEST(test_id_path_parse) {
    int16_t v = 0;
    ASSERT_EQ_INT(0, rfork_id_from_path("0", &v));
    ASSERT_EQ_INT(0, v);
    ASSERT_EQ_INT(0, rfork_id_from_path("-16", &v));
    ASSERT_EQ_INT(-16, v);
    ASSERT_EQ_INT(0, rfork_id_from_path("32767", &v));
    ASSERT_EQ_INT(32767, v);
    ASSERT_EQ_INT(0, rfork_id_from_path("-32768", &v));
    ASSERT_EQ_INT(-32768, v);
    // Out of range.
    ASSERT_EQ_INT(-EINVAL, rfork_id_from_path("32768", &v));
    ASSERT_EQ_INT(-EINVAL, rfork_id_from_path("-32769", &v));
    // Trailing junk.
    ASSERT_EQ_INT(-EINVAL, rfork_id_from_path("12x", &v));
    ASSERT_EQ_INT(-EINVAL, rfork_id_from_path("", &v));
}

TEST(test_type_path_parse) {
    uint8_t out[4];
    ASSERT_EQ_INT(0, rfork_type_from_path("CODE", out));
    ASSERT_EQ_INT(0, memcmp(out, "CODE", 4));
    ASSERT_EQ_INT(0, rfork_type_from_path("STR ", out));
    ASSERT_EQ_INT(0, memcmp(out, "STR ", 4));
    // 3-char input must fail.
    ASSERT_EQ_INT(-EINVAL, rfork_type_from_path("STR", out));
    // 5-char input must fail.
    ASSERT_EQ_INT(-EINVAL, rfork_type_from_path("CODES", out));
    // Empty fails.
    ASSERT_EQ_INT(-EINVAL, rfork_type_from_path("", out));
}

TEST(test_info_format) {
    char buf[256];
    int n = rfork_info_format("Hello", 0x04, 1576, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"name\":\"Hello\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"preload\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"size\":1576") != NULL);
    // Empty name + no attrs.
    n = rfork_info_format("", 0, 0, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\"name\":\"\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\"attrs\":[]") != NULL);
    // JSON escape: a name with a quote and backslash.
    n = rfork_info_format("ab\"c\\d", 0, 0, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "\\\"") != NULL);
    ASSERT_TRUE(strstr(buf, "\\\\") != NULL);
}

int main(void) {
    RUN(test_parse_empty_fork);
    RUN(test_parse_two_types_multi_ids);
    RUN(test_id_boundaries);
    RUN(test_type_trailing_space);
    RUN(test_type_high_bit_macroman);
    RUN(test_empty_resource);
    RUN(test_single_resource_fork);
    RUN(test_truncated_header);
    RUN(test_corrupt_offsets);
    RUN(test_id_path_parse);
    RUN(test_type_path_parse);
    RUN(test_info_format);
    fprintf(stderr, "All resfork tests passed.\n");
    return 0;
}
