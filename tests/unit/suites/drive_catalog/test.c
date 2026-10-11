// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for drive_catalog.c: the size strings files.hd_create takes
// and the model lookups the SCSI attach path makes.

#include "drive_catalog.h"
#include "test_assert.h"

#include <stdio.h>

#define HD20SC_MINISCRIBE 21307392u
#define HD20SC_SEAGATE    21411840u

TEST(binary_suffixes_are_exact) {
    ASSERT_EQ_INT(drive_catalog_parse_size("20K"), 20 * 1024);
    ASSERT_EQ_INT(drive_catalog_parse_size("512k"), 512 * 1024);
    ASSERT_EQ_INT(drive_catalog_parse_size("20M"), 20 * 1024 * 1024);
    ASSERT_EQ_INT(drive_catalog_parse_size("100m"), 100 * 1024 * 1024);
    ASSERT_EQ_INT(drive_catalog_parse_size("21411840"), HD20SC_SEAGATE);
}

// Anything after the number and its one-letter suffix is refused, whether it
// follows a suffix ("20K extra") or the bare number ("20 extra").
TEST(trailing_garbage_is_refused) {
    ASSERT_EQ_INT(drive_catalog_parse_size("20K extra"), 0);
    ASSERT_EQ_INT(drive_catalog_parse_size("20Kx"), 0);
    ASSERT_EQ_INT(drive_catalog_parse_size("20MM"), 0);
    ASSERT_EQ_INT(drive_catalog_parse_size("20 extra"), 0);
    ASSERT_EQ_INT(drive_catalog_parse_size("20x"), 0);
}

TEST(empty_zero_and_non_numbers_are_refused) {
    ASSERT_EQ_INT(drive_catalog_parse_size(NULL), 0);
    ASSERT_EQ_INT(drive_catalog_parse_size(""), 0);
    ASSERT_EQ_INT(drive_catalog_parse_size("0"), 0);
    ASSERT_EQ_INT(drive_catalog_parse_size("0K"), 0);
    ASSERT_EQ_INT(drive_catalog_parse_size("K"), 0);
    ASSERT_EQ_INT(drive_catalog_parse_size("mb"), 0);
}

// "mb" / "gb" are decimal and snap up to the next catalog model.
TEST(decimal_sizes_snap_to_a_model) {
    ASSERT_EQ_INT(drive_catalog_parse_size("20mb"), HD20SC_MINISCRIBE);
    ASSERT_EQ_INT(drive_catalog_parse_size("21MB"), HD20SC_MINISCRIBE);
    ASSERT_EQ_INT(drive_catalog_parse_size("22mb"), 40061952);
    ASSERT_EQ_INT(drive_catalog_parse_size("40mb"), 40061952);
    ASSERT_EQ_INT(drive_catalog_parse_size("1gb"), 1084489728);
}

// A label names one model; the two HD20SC mechanisms share theirs, and the
// label is the first (the Miniscribe), as the catalog documents.
TEST(labels_are_case_insensitive_and_first_wins) {
    ASSERT_EQ_INT(drive_catalog_parse_size("HD40SC"), 40061952);
    ASSERT_EQ_INT(drive_catalog_parse_size("hd40sc"), 40061952);
    ASSERT_EQ_INT(drive_catalog_parse_size("HD20SC"), HD20SC_MINISCRIBE);
}

TEST(closest_model_is_the_smallest_that_fits) {
    ASSERT_EQ_INT(drive_catalog_find_closest(1)->size, HD20SC_MINISCRIBE);
    ASSERT_EQ_INT(drive_catalog_find_closest(HD20SC_MINISCRIBE + 1)->size, HD20SC_SEAGATE);
    ASSERT_EQ_INT(drive_catalog_find_closest(HD20SC_SEAGATE)->size, HD20SC_SEAGATE);
    // Past the largest model, the largest.
    const struct drive_model *last = drive_catalog_get(drive_catalog_count() - 1);
    ASSERT_TRUE(drive_catalog_find_closest(last->size + 1) == last);
}

TEST(the_catalog_is_sorted_and_block_aligned) {
    int n = drive_catalog_count();
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(drive_catalog_get(-1) == NULL);
    ASSERT_TRUE(drive_catalog_get(n) == NULL);
    for (int i = 0; i < n; i++) {
        const struct drive_model *m = drive_catalog_get(i);
        ASSERT_TRUE(m->size % 512 == 0);
        if (i > 0)
            ASSERT_TRUE(drive_catalog_get(i - 1)->size < m->size);
    }
}

int main(void) {
    RUN(binary_suffixes_are_exact);
    RUN(trailing_garbage_is_refused);
    RUN(empty_zero_and_non_numbers_are_refused);
    RUN(decimal_sizes_snap_to_a_model);
    RUN(labels_are_case_insensitive_and_first_wins);
    RUN(closest_model_is_the_smallest_that_fits);
    RUN(the_catalog_is_sorted_and_block_aligned);
    fprintf(stderr, "All drive_catalog tests passed\n");
    return 0;
}
