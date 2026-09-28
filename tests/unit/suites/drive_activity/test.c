// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for drive_activity.c: counter sums in, light edges out.

#include "drive_activity.h"
#include "test_assert.h"

static uint64_t R[DRIVE_KIND_COUNT], W[DRIVE_KIND_COUNT];

static unsigned step(drive_activity_t *a, double t) {
    return drive_activity_update(a, R, W, t);
}

static void reset(drive_activity_t *a) {
    *a = (drive_activity_t){0};
    for (int k = 0; k < DRIVE_KIND_COUNT; k++)
        R[k] = W[k] = 0;
}

TEST(the_first_sample_is_a_baseline) {
    drive_activity_t a;
    reset(&a);
    R[DRIVE_KIND_HD] = 5000; // a machine that has been running
    ASSERT_EQ_INT(step(&a, 0), 0);
    ASSERT_EQ_INT(a.light[DRIVE_KIND_HD], DRIVE_LIGHT_IDLE);
}

TEST(a_read_lights_and_stays_lit_its_minimum) {
    drive_activity_t a;
    reset(&a);
    step(&a, 0);
    R[DRIVE_KIND_FD] = 1;
    ASSERT_EQ_INT(step(&a, 10), 1u << DRIVE_KIND_FD);
    ASSERT_EQ_INT(a.light[DRIVE_KIND_FD], DRIVE_LIGHT_READ);
    ASSERT_EQ_INT(step(&a, 50), 0); // still within 100 ms: no edge
    ASSERT_EQ_INT(step(&a, 109), 0);
    ASSERT_EQ_INT(step(&a, 110), 1u << DRIVE_KIND_FD); // off
    ASSERT_EQ_INT(a.light[DRIVE_KIND_FD], DRIVE_LIGHT_IDLE);
}

TEST(steady_io_is_one_steady_light) {
    drive_activity_t a;
    reset(&a);
    step(&a, 0);
    unsigned edges = 0;
    for (int t = 16; t < 1000; t += 16) {
        R[DRIVE_KIND_HD] += 8;
        if (step(&a, t))
            edges++;
    }
    ASSERT_EQ_INT(edges, 1);
    ASSERT_EQ_INT(a.light[DRIVE_KIND_HD], DRIVE_LIGHT_READ);
}

TEST(a_write_outranks_reads_for_its_minimum) {
    drive_activity_t a;
    reset(&a);
    step(&a, 0);
    W[DRIVE_KIND_HD] = 1;
    step(&a, 10);
    ASSERT_EQ_INT(a.light[DRIVE_KIND_HD], DRIVE_LIGHT_WRITE);
    R[DRIVE_KIND_HD] = 1;
    ASSERT_EQ_INT(step(&a, 50), 0); // a read inside the write's window keeps "write"
    ASSERT_EQ_INT(a.light[DRIVE_KIND_HD], DRIVE_LIGHT_WRITE);
    R[DRIVE_KIND_HD] = 2;
    ASSERT_EQ_INT(step(&a, 160), 1u << DRIVE_KIND_HD); // the window passed: reads show
    ASSERT_EQ_INT(a.light[DRIVE_KIND_HD], DRIVE_LIGHT_READ);
}

TEST(a_write_gives_way_to_steady_reads) {
    drive_activity_t a;
    reset(&a);
    step(&a, 0);
    W[DRIVE_KIND_HD] = 1;
    step(&a, 16);
    for (int t = 32; t < 400; t += 16) {
        R[DRIVE_KIND_HD] += 4;
        step(&a, t);
    }
    ASSERT_EQ_INT(a.light[DRIVE_KIND_HD], DRIVE_LIGHT_READ);
}

TEST(a_closed_image_is_not_activity) {
    drive_activity_t a;
    reset(&a);
    R[DRIVE_KIND_CD] = 900;
    step(&a, 0);
    R[DRIVE_KIND_CD] = 0; // the image was closed: the sum drops
    ASSERT_EQ_INT(step(&a, 16), 0);
    ASSERT_EQ_INT(a.light[DRIVE_KIND_CD], DRIVE_LIGHT_IDLE);
    R[DRIVE_KIND_CD] = 3; // and the next image's reads count from there
    ASSERT_EQ_INT(step(&a, 32), 1u << DRIVE_KIND_CD);
}

TEST(kinds_are_independent) {
    drive_activity_t a;
    reset(&a);
    step(&a, 0);
    R[DRIVE_KIND_HD] = 1;
    W[DRIVE_KIND_FD] = 1;
    ASSERT_EQ_INT(step(&a, 10), (1u << DRIVE_KIND_HD) | (1u << DRIVE_KIND_FD));
    ASSERT_EQ_INT(a.light[DRIVE_KIND_CD], DRIVE_LIGHT_IDLE);
}

int main(void) {
    RUN(the_first_sample_is_a_baseline);
    RUN(a_read_lights_and_stays_lit_its_minimum);
    RUN(steady_io_is_one_steady_light);
    RUN(a_write_outranks_reads_for_its_minimum);
    RUN(a_write_gives_way_to_steady_reads);
    RUN(a_closed_image_is_not_activity);
    RUN(kinds_are_independent);
    return 0;
}
