// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// display_ramdac.h: the shared RAMDAC palette-port sequencer.
//
// Drives ramdac_seq_t against a plain 256-entry store, the way every chip's
// wrapper does: commit on the last component, read the component it names.

#include "display_ramdac.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The palette a wrapper would keep.
static uint8_t s_pal[256][4];

// A data write through the sequencer, storing the entry when it completes.
static void wr(ramdac_seq_t *s, uint8_t v) {
    uint8_t idx;
    if (ramdac_seq_write(s, v, &idx))
        memcpy(s_pal[idx], s->pending, ramdac_seq_ncomp(s));
}

// A data read (or peek) through the sequencer.
static int rd(ramdac_seq_t *s, bool peek) {
    uint8_t comp;
    uint8_t idx = ramdac_seq_read(s, peek, &comp);
    return s_pal[idx][comp];
}

// Index then a run of components loads consecutive entries.
TEST(run_write_commits_on_last_component) {
    memset(s_pal, 0, sizeof s_pal);
    ramdac_seq_t s;
    ramdac_seq_init(&s, 0);
    ramdac_seq_set_waddr(&s, 0xFE);
    wr(&s, 1);
    wr(&s, 2);
    ASSERT_EQ_INT(0, s_pal[0xFE][0]); // nothing stored before blue
    wr(&s, 3);
    ASSERT_EQ_INT(1, s_pal[0xFE][0]);
    ASSERT_EQ_INT(3, s_pal[0xFE][2]);
    wr(&s, 4);
    wr(&s, 5);
    wr(&s, 6); // entry $FF
    wr(&s, 7);
    wr(&s, 8);
    wr(&s, 9); // wraps to entry 0
    ASSERT_EQ_INT(4, s_pal[0xFF][0]);
    ASSERT_EQ_INT(7, s_pal[0][0]);
    ASSERT_EQ_INT(1, ramdac_seq_addr(&s));
}

// A run-read walks entries, advancing after the last component.
TEST(run_read_advances) {
    memset(s_pal, 0, sizeof s_pal);
    ramdac_seq_t s;
    ramdac_seq_init(&s, 0);
    ramdac_seq_set_waddr(&s, 10);
    for (int i = 1; i <= 6; i++)
        wr(&s, (uint8_t)i);
    ramdac_seq_set_raddr(&s, 10);
    for (int i = 1; i <= 6; i++)
        ASSERT_EQ_INT(i, rd(&s, false));
    ASSERT_EQ_INT(12, ramdac_seq_addr(&s)); // shared: the write index followed
}

// A peek names the next read's component and moves nothing.
TEST(peek_moves_nothing) {
    memset(s_pal, 0, sizeof s_pal);
    ramdac_seq_t s;
    ramdac_seq_init(&s, 0);
    ramdac_seq_set_waddr(&s, 0);
    wr(&s, 0x11);
    wr(&s, 0x22);
    wr(&s, 0x33);
    ramdac_seq_set_raddr(&s, 0);
    ASSERT_EQ_INT(0x11, rd(&s, true));
    ASSERT_EQ_INT(0x11, rd(&s, true));
    ASSERT_EQ_INT(0x11, rd(&s, false));
    ASSERT_EQ_INT(0x22, rd(&s, false));
    ASSERT_EQ_INT(0x33, rd(&s, true));
    ASSERT_EQ_INT(0x33, rd(&s, false));
    ASSERT_EQ_INT(1, ramdac_seq_addr(&s));
}

// An address write mid-entry drops the partial entry and restarts at R.
TEST(readdress_mid_entry_drops_partial) {
    memset(s_pal, 0, sizeof s_pal);
    ramdac_seq_t s;
    ramdac_seq_init(&s, 0);
    ramdac_seq_set_waddr(&s, 9);
    wr(&s, 0x77); // R only
    ramdac_seq_set_waddr(&s, 9);
    wr(&s, 0xAA);
    wr(&s, 0xBB);
    ASSERT_EQ_INT(0, s_pal[9][0]); // still pending
    wr(&s, 0xCC);
    ASSERT_EQ_INT(0xAA, s_pal[9][0]);
    ASSERT_EQ_INT(0xCC, s_pal[9][2]);
}

// The VGA DAC: separate write and read indices over one phase.
TEST(split_indices) {
    memset(s_pal, 0, sizeof s_pal);
    ramdac_seq_t s;
    ramdac_seq_init(&s, RAMDAC_SPLIT_INDEX);
    ramdac_seq_set_waddr(&s, 4);
    for (int i = 1; i <= 3; i++)
        wr(&s, (uint8_t)i);
    ramdac_seq_set_raddr(&s, 4);
    ramdac_seq_set_waddr(&s, 50); // does not move the read index
    for (int i = 1; i <= 3; i++)
        ASSERT_EQ_INT(i, rd(&s, false));
    ASSERT_EQ_INT(5, s.ridx);
    ASSERT_EQ_INT(50, s.widx);
}

// Sebastian: four components per entry.
TEST(four_components) {
    memset(s_pal, 0, sizeof s_pal);
    ramdac_seq_t s;
    ramdac_seq_init(&s, RAMDAC_RGBA);
    ramdac_seq_set_waddr(&s, 2);
    for (int i = 1; i <= 8; i++)
        wr(&s, (uint8_t)i);
    ASSERT_EQ_INT(4, s_pal[2][3]);
    ASSERT_EQ_INT(5, s_pal[3][0]);
    ramdac_seq_set_raddr(&s, 2);
    for (int i = 1; i <= 8; i++)
        ASSERT_EQ_INT(i, rd(&s, false));
    ASSERT_EQ_INT(4, ramdac_seq_addr(&s));
}

// An all-zero sequencer is the common shape: three components, one index.
TEST(zero_is_rgb_shared) {
    ramdac_seq_t s;
    memset(&s, 0, sizeof s);
    ASSERT_EQ_INT(3, ramdac_seq_ncomp(&s));
    ASSERT_TRUE(ramdac_seq_shared(&s));
}

// A phase out of range (a damaged checkpoint) neither overruns nor wedges.
TEST(bad_phase_is_contained) {
    ramdac_seq_t s;
    ramdac_seq_init(&s, 0);
    s.phase = 200;
    uint8_t comp;
    (void)ramdac_seq_read(&s, true, &comp);
    ASSERT_EQ_INT(0, comp);
    uint8_t idx = 0xAA;
    ASSERT_TRUE(ramdac_seq_write(&s, 1, &idx)); // completes at once
    ASSERT_EQ_INT(0, idx);
    ASSERT_EQ_INT(0, s.phase);
}

int main(void) {
    RUN(run_write_commits_on_last_component);
    RUN(run_read_advances);
    RUN(peek_moves_nothing);
    RUN(readdress_mid_entry_drops_partial);
    RUN(split_indices);
    RUN(four_components);
    RUN(zero_is_rgb_shared);
    RUN(bad_phase_is_contained);
    fprintf(stderr, "display_ramdac: all tests passed\n");
    return 0;
}
