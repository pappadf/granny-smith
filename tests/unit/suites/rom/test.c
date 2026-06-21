// Apple Lisa 2 / Macintosh XL boot-ROM interleave + identification unit tests
// (Step 1 of proposal-machine-lisa-xl.md).  Hermetic: synthesises chip images
// carrying the Lisa reset SSP ($00000480) and version word rather than
// depending on the proprietary Lisa ROM files.

#include "rom.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HALF (8 * 1024) // one byte-slice chip
#define FULL (16 * 1024) // interleaved image

// Build a synthetic combined image: reset SSP $00000480 at offset 0, version
// word at $3FFC, deterministic filler elsewhere.
static void make_combined(uint8_t *out, uint16_t version) {
    for (int i = 0; i < FULL; i++)
        out[i] = (uint8_t)(i * 7 + 1);
    out[0] = 0x00; // reset SSP $00000480
    out[1] = 0x00;
    out[2] = 0x04;
    out[3] = 0x80;
    out[0x3FFC] = (uint8_t)(version >> 8);
    out[0x3FFD] = (uint8_t)version;
}

// De-interleave a combined image into its even (hi) and odd (lo) byte-slices.
static void split(const uint8_t *combined, uint8_t *hi, uint8_t *lo) {
    for (int i = 0; i < HALF; i++) {
        hi[i] = combined[2 * i];
        lo[i] = combined[2 * i + 1];
    }
}

// Write a buffer to a fresh temp file in the CWD; returns malloc'd path.
static char *write_temp(const uint8_t *buf, size_t n) {
    char *path = strdup("lisa_chip_XXXXXX");
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    ssize_t w = write(fd, buf, n);
    ASSERT_TRUE(w == (ssize_t)n);
    close(fd);
    return path;
}

// Interleaving high/low chips reconstructs the combined image exactly.
TEST(test_interleave_roundtrip) {
    uint8_t combined[FULL];
    make_combined(combined, 0x0248);
    uint8_t hi[HALF], lo[HALF];
    split(combined, hi, lo);
    uint8_t out[FULL];
    memset(out, 0xAA, sizeof out);
    rom_interleave_pair(hi, HALF, lo, HALF, out);
    ASSERT_TRUE(memcmp(out, combined, FULL) == 0);
}

// Version $0248 → Lisa 2 (rev H), compatible with "lisa".
TEST(test_identify_lisa_h) {
    uint8_t c[FULL];
    make_combined(c, 0x0248);
    const rom_info_t *info = rom_identify_lisa(c, FULL);
    ASSERT_TRUE(info != NULL);
    ASSERT_TRUE(strcmp(info->compatible[0], "lisa") == 0);
}

// Version $0341 → Macintosh XL ("3A"), compatible with "macxl".
TEST(test_identify_macxl) {
    uint8_t c[FULL];
    make_combined(c, 0x0341);
    const rom_info_t *info = rom_identify_lisa(c, FULL);
    ASSERT_TRUE(info != NULL);
    ASSERT_TRUE(strcmp(info->compatible[0], "macxl") == 0);
}

// Wrong size, wrong reset SSP, or unknown version word must all be rejected.
TEST(test_identify_rejects_bad) {
    uint8_t c[FULL];
    make_combined(c, 0x0248);
    ASSERT_TRUE(rom_identify_lisa(c, FULL - 2) == NULL); // wrong size

    uint8_t c2[FULL];
    make_combined(c2, 0x0248);
    c2[3] = 0x00; // SSP now $00000400
    ASSERT_TRUE(rom_identify_lisa(c2, FULL) == NULL);

    uint8_t c3[FULL];
    make_combined(c3, 0x9999); // unknown version
    ASSERT_TRUE(rom_identify_lisa(c3, FULL) == NULL);
}

// rom_identify_data falls through to the Lisa path and reports the computed
// checksum (a unique content id), not the meaningless reset-SSP first longword.
TEST(test_identify_data_fallback) {
    uint8_t c[FULL];
    make_combined(c, 0x0341);
    uint32_t cks = 0;
    const rom_info_t *info = rom_identify_data(c, FULL, &cks);
    ASSERT_TRUE(info != NULL);
    ASSERT_TRUE(strcmp(info->compatible[0], "macxl") == 0);
    ASSERT_TRUE(cks != 0x00000480u);
    ASSERT_EQ_INT((int)cks, (int)rom_compute_checksum(c, FULL));
}

// rom_load_lisa_pair must produce the same correct image regardless of which
// file is passed first (it auto-detects the high/low byte orientation).
TEST(test_load_pair_order_independent) {
    uint8_t combined[FULL];
    make_combined(combined, 0x0248);
    uint8_t hi[HALF], lo[HALF];
    split(combined, hi, lo);
    char *pa = write_temp(hi, HALF);
    char *pb = write_temp(lo, HALF);

    size_t sz = 0;
    uint8_t *r1 = rom_load_lisa_pair(pa, pb, &sz); // hi, lo
    ASSERT_TRUE(r1 != NULL);
    ASSERT_EQ_INT((int)sz, FULL);
    ASSERT_TRUE(memcmp(r1, combined, FULL) == 0);
    free(r1);

    sz = 0;
    uint8_t *r2 = rom_load_lisa_pair(pb, pa, &sz); // lo, hi (swapped)
    ASSERT_TRUE(r2 != NULL);
    ASSERT_TRUE(memcmp(r2, combined, FULL) == 0);
    free(r2);

    unlink(pa);
    unlink(pb);
    free(pa);
    free(pb);
}

int main(void) {
    RUN(test_interleave_roundtrip);
    RUN(test_identify_lisa_h);
    RUN(test_identify_macxl);
    RUN(test_identify_rejects_bad);
    RUN(test_identify_data_fallback);
    RUN(test_load_pair_order_independent);
    printf("[PASS] All Lisa ROM tests passed\n");
    return 0;
}
