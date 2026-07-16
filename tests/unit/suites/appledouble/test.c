// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the AppleSingle / AppleDouble (v2) container codec
// (src/core/storage/appledouble.c): detection, parse, build, the copy-out
// sidecar convenience, unknown-entry preservation, and bounds rejection.
// Header byte layout is asserted by hand so the tests are self-contained.

#include "appledouble.h"
#include "test_assert.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

// ---- build / parse round-trip ---------------------------------------------

TEST(sidecar_roundtrip_rsrc_and_finder) {
    uint8_t rsrc[300];
    for (size_t i = 0; i < sizeof(rsrc); i++)
        rsrc[i] = (uint8_t)(i * 7 + 1);
    uint8_t finder[AD_FINDER_INFO_SIZE];
    for (size_t i = 0; i < sizeof(finder); i++)
        finder[i] = (uint8_t)(0xF0 - i);

    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, ad_build_sidecar(rsrc, sizeof(rsrc), finder, &buf, &len));

    // Header shape: AppleDouble magic, v2, 2 entries.
    ASSERT_TRUE(rd32(buf) == APPLEDOUBLE_MAGIC);
    ASSERT_TRUE(rd32(buf + 4) == APPLE_FORK_VERSION);
    ASSERT_EQ_INT(2, (int)rd16(buf + 24));
    // total = 26 hdr + 2*12 desc + 32 finder + 300 rsrc.
    ASSERT_EQ_INT(26 + 24 + 32 + 300, (int)len);

    ASSERT_TRUE(ad_detect(buf, len));
    ad_file_t ad;
    ASSERT_EQ_INT(0, ad_parse(buf, len, &ad));
    ASSERT_EQ_INT(2, (int)ad.n_entries);
    ASSERT_TRUE(ad.magic == APPLEDOUBLE_MAGIC);
    ASSERT_TRUE(ad.data == NULL); // AppleDouble header never carries the data fork
    ASSERT_EQ_INT((int)sizeof(finder), (int)ad.finder_len);
    ASSERT_EQ_INT(0, memcmp(ad.finder, finder, sizeof(finder)));
    ASSERT_EQ_INT((int)sizeof(rsrc), (int)ad.rsrc_len);
    ASSERT_EQ_INT(0, memcmp(ad.rsrc, rsrc, sizeof(rsrc)));
    free(buf);
}

TEST(sidecar_rsrc_only_when_finder_null) {
    uint8_t rsrc[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, ad_build_sidecar(rsrc, sizeof(rsrc), NULL, &buf, &len));
    ASSERT_EQ_INT(1, (int)rd16(buf + 24)); // resource fork only
    ad_file_t ad;
    ASSERT_EQ_INT(0, ad_parse(buf, len, &ad));
    ASSERT_TRUE(ad.finder == NULL);
    ASSERT_EQ_INT((int)sizeof(rsrc), (int)ad.rsrc_len);
    ASSERT_EQ_INT(0, memcmp(ad.rsrc, rsrc, sizeof(rsrc)));
    free(buf);
}

TEST(sidecar_needs_at_least_one_part) {
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(-EINVAL, ad_build_sidecar(NULL, 0, NULL, &buf, &len));
}

TEST(applesingle_roundtrip_with_data_fork) {
    uint8_t data[64];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)i;
    uint8_t rsrc[8] = {9, 8, 7, 6, 5, 4, 3, 2};
    ad_entry_t entries[] = {
        {.id = AD_ENTRY_RSRC, .bytes = rsrc, .len = sizeof(rsrc)},
        {.id = AD_ENTRY_DATA, .bytes = data, .len = sizeof(data)},
    };
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, ad_build(true, entries, 2, &buf, &len));
    ASSERT_TRUE(rd32(buf) == APPLESINGLE_MAGIC);
    ad_file_t ad;
    ASSERT_EQ_INT(0, ad_parse(buf, len, &ad));
    ASSERT_EQ_INT((int)sizeof(data), (int)ad.data_len);
    ASSERT_EQ_INT(0, memcmp(ad.data, data, sizeof(data)));
    ASSERT_EQ_INT(0, memcmp(ad.rsrc, rsrc, sizeof(rsrc)));
    free(buf);
}

TEST(appledouble_rejects_data_entry) {
    uint8_t data[4] = {0};
    ad_entry_t entries[] = {
        {.id = AD_ENTRY_DATA, .bytes = data, .len = sizeof(data)}
    };
    uint8_t *buf = NULL;
    size_t len = 0;
    // Building an AppleDouble *header* with a data-fork entry is illegal.
    ASSERT_EQ_INT(-EINVAL, ad_build(false, entries, 1, &buf, &len));
}

TEST(preserves_unknown_entries) {
    // An entry id we don't special-case (e.g. AFP File Info, id 14) must
    // round-trip through build->parse in the generic entries[] table.
    uint8_t afp[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t finder[AD_FINDER_INFO_SIZE] = {0};
    finder[0] = 'A';
    ad_entry_t entries[] = {
        {.id = AD_ENTRY_FINDER,  .bytes = finder, .len = sizeof(finder)},
        {.id = AD_ENTRY_AFPINFO, .bytes = afp,    .len = sizeof(afp)   },
    };
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, ad_build(false, entries, 2, &buf, &len));
    ad_file_t ad;
    ASSERT_EQ_INT(0, ad_parse(buf, len, &ad));
    ASSERT_EQ_INT(2, (int)ad.n_entries);
    // Locate the AFP entry in the generic table.
    const uint8_t *afp_out = NULL;
    size_t afp_len = 0;
    for (size_t i = 0; i < ad.n_entries; i++)
        if (ad.entries[i].id == AD_ENTRY_AFPINFO) {
            afp_out = ad.entries[i].bytes;
            afp_len = ad.entries[i].len;
        }
    ASSERT_EQ_INT((int)sizeof(afp), (int)afp_len);
    ASSERT_EQ_INT(0, memcmp(afp_out, afp, sizeof(afp)));
    free(buf);
}

// ---- detection / bounds rejection -----------------------------------------

TEST(detect_false_on_garbage) {
    uint8_t junk[64];
    memset(junk, 0xAB, sizeof(junk));
    ASSERT_TRUE(!ad_detect(junk, sizeof(junk)));
    ASSERT_TRUE(!ad_detect(NULL, 0));
    ASSERT_TRUE(!ad_detect((const uint8_t *)"", 0));
}

TEST(detect_false_on_wrong_version) {
    uint8_t rsrc[4] = {1, 2, 3, 4};
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, ad_build_sidecar(rsrc, sizeof(rsrc), NULL, &buf, &len));
    buf[4] = 0x00;
    buf[5] = 0x01; // version 0x00010000 (v1) — we only accept v2
    ASSERT_TRUE(!ad_detect(buf, len));
    ASSERT_EQ_INT(-EINVAL, ad_parse(buf, len, (ad_file_t[]){0}));
    free(buf);
}

TEST(parse_rejects_out_of_bounds_entry) {
    uint8_t rsrc[32] = {0};
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, ad_build_sidecar(rsrc, sizeof(rsrc), NULL, &buf, &len));
    // Corrupt the single descriptor's length to exceed the buffer.
    // desc[0] starts at offset 26: id(4) off(4) len(4).
    buf[26 + 8 + 0] = 0xFF;
    buf[26 + 8 + 1] = 0xFF;
    ASSERT_TRUE(!ad_detect(buf, len));
    ad_file_t ad;
    ASSERT_EQ_INT(-EINVAL, ad_parse(buf, len, &ad));
    free(buf);
}

TEST(parse_rejects_truncated_header) {
    uint8_t rsrc[4] = {1, 2, 3, 4};
    uint8_t *buf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, ad_build_sidecar(rsrc, sizeof(rsrc), NULL, &buf, &len));
    // Truncate below the fixed header.
    ASSERT_TRUE(!ad_detect(buf, 10));
    free(buf);
}

int main(void) {
    RUN(sidecar_roundtrip_rsrc_and_finder);
    RUN(sidecar_rsrc_only_when_finder_null);
    RUN(sidecar_needs_at_least_one_part);
    RUN(applesingle_roundtrip_with_data_fork);
    RUN(appledouble_rejects_data_entry);
    RUN(preserves_unknown_entries);
    RUN(detect_false_on_garbage);
    RUN(detect_false_on_wrong_version);
    RUN(parse_rejects_out_of_bounds_entry);
    RUN(parse_rejects_truncated_header);
    fprintf(stderr, "All appledouble tests passed.\n");
    return 0;
}
