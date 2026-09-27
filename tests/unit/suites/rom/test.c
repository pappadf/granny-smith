// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// ROM content identity unit tests: the per-kind id and self-check rules, the
// table of known ROMs, and the Lisa / Macintosh XL two-chip interleave.
// Hermetic: synthesises images that carry each kind's checksum fields rather
// than depending on proprietary ROM files.

#include "rom.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HALF     (8 * 1024) // one Lisa byte-slice chip
#define FULL     (16 * 1024) // interleaved Lisa image
#define PPC_SIZE (4u * 1024 * 1024) // Old World PowerPC image
#define PPC_HALF 0x300000u // end of the 68k half
#define CI_OFF   0xD000u // ConfigInfo offset planted at 0x300080
#define M68_SIZE (128 * 1024) // a Plus-sized 68k image

// Big-endian stores used to plant checksum fields.
static void wr_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void wr_be32(uint8_t *p, uint32_t v) {
    wr_be16(p, (uint16_t)(v >> 16));
    wr_be16(p + 2, (uint16_t)v);
}
static void wr_be64(uint8_t *p, uint64_t v) {
    wr_be32(p, (uint32_t)(v >> 32));
    wr_be32(p + 4, (uint32_t)v);
}
static uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

// Sum of big-endian words over [from, to): the classic Mac header sum.
static uint32_t word_sum(const uint8_t *d, size_t from, size_t to) {
    uint32_t s = 0;
    for (size_t i = from; i + 1 < to; i += 2)
        s += rd_be16(d + i);
    return s;
}

// A Lisa image: reset SSP $00000480, version word at $3FFC, filler, and the
// check word at $3FFE chosen so the boot ROM's rotating sum comes out zero.
static void make_lisa(uint8_t *out, uint16_t version) {
    for (int i = 0; i < FULL; i++)
        out[i] = (uint8_t)(i * 7 + 1);
    wr_be32(out, 0x00000480u);
    wr_be16(out + 0x3FFC, version);
    uint16_t sum = 0;
    for (int i = 0; i < 0x3FFE; i += 2) {
        sum = (uint16_t)(sum + rd_be16(out + i));
        sum = (uint16_t)((sum << 1) | (sum >> 15));
    }
    wr_be16(out + 0x3FFE, (uint16_t)(0x10000u - sum)); // makes sum + check == 0
}

// A 68k image whose header sum verifies over the whole image.
static void make_68k(uint8_t *out) {
    for (int i = 0; i < M68_SIZE; i++)
        out[i] = (uint8_t)(i * 13 + 5);
    wr_be32(out, word_sum(out, 4, M68_SIZE));
}

// A 4 MiB image with a ConfigInfo block whose lane sums and 64-bit sum, and
// whose header sum over the 68k half, all verify.
static uint8_t *make_ppc(void) {
    uint8_t *d = malloc(PPC_SIZE);
    for (uint32_t i = 0; i < PPC_SIZE; i++)
        d[i] = (uint8_t)(i * 31 + (i >> 11));
    wr_be32(d + 0x300080, CI_OFF); // ConfigInfo pointer
    uint8_t *ci = d + PPC_HALF + CI_OFF;
    memset(ci, 0, 0x28);
    wr_be32(d, word_sum(d, 4, PPC_HALF)); // header first: the 64-bit sum covers it
    uint64_t sum64 = 0;
    uint32_t lanes[8] = {0};
    for (uint32_t i = 0; i < PPC_SIZE; i += 8) {
        if (i >= PPC_HALF + CI_OFF && i < PPC_HALF + CI_OFF + 0x28)
            continue;
        uint64_t q = 0;
        for (int b = 0; b < 8; b++) {
            q = (q << 8) | d[i + b];
            lanes[b] += d[i + b];
        }
        sum64 += q;
    }
    for (int b = 0; b < 8; b++)
        wr_be32(ci + 4 * b, lanes[b]);
    wr_be64(ci + 0x20, sum64);
    return d;
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
    make_lisa(combined, 0x0248);
    uint8_t hi[HALF], lo[HALF];
    split(combined, hi, lo);
    uint8_t out[FULL];
    memset(out, 0xAA, sizeof out);
    rom_interleave_pair(hi, HALF, lo, HALF, out);
    ASSERT_TRUE(memcmp(out, combined, FULL) == 0);
}

// A Lisa image is identified by its check word and verified by ROMTST's sum.
TEST(test_lisa_identity) {
    uint8_t c[FULL];
    make_lisa(c, 0x0248);
    rom_identity_t id;
    rom_identity_compute(c, FULL, &id);
    ASSERT_TRUE(id.kind == ROM_KIND_LISA);
    ASSERT_TRUE(id.intact);
    char want[8];
    snprintf(want, sizeof want, "%04x", rd_be16(c + 0x3FFE));
    ASSERT_TRUE(strcmp(id.id, want) == 0);

    c[100] ^= 0x5A; // damage one byte: same id, no longer intact
    rom_identity_compute(c, FULL, &id);
    ASSERT_TRUE(id.kind == ROM_KIND_LISA);
    ASSERT_TRUE(strcmp(id.id, want) == 0);
    ASSERT_TRUE(!id.intact);
    ASSERT_TRUE(id.reason[0] != 0);
}

// The two Lisa fixtures' ids are in the table for the right models.
TEST(test_lisa_rows) {
    const rom_info_t *h = rom_lookup("3f7b", FULL);
    const rom_info_t *xl = rom_lookup("d905", FULL);
    ASSERT_TRUE(h && strcmp(h->compatible[0], "lisa") == 0);
    ASSERT_TRUE(xl && strcmp(xl->compatible[0], "macxl") == 0);
}

// A 68k image's id is its header sum; damage changes the verdict, not the id.
TEST(test_68k_identity) {
    static uint8_t d[M68_SIZE];
    make_68k(d);
    rom_identity_t id;
    rom_identity_compute(d, M68_SIZE, &id);
    ASSERT_TRUE(id.kind == ROM_KIND_MAC68K);
    ASSERT_TRUE(id.intact);
    char want[16];
    snprintf(want, sizeof want, "%02x%02x%02x%02x", d[0], d[1], d[2], d[3]);
    ASSERT_TRUE(strcmp(id.id, want) == 0);
    d[M68_SIZE - 3] ^= 0x01;
    rom_identity_compute(d, M68_SIZE, &id);
    ASSERT_TRUE(strcmp(id.id, want) == 0);
    ASSERT_TRUE(!id.intact);
}

// A PPC image's id is header sum + ConfigInfo 64-bit sum; damage in the last
// megabyte is caught by the 64-bit sum and the lane that differs is named.
TEST(test_ppc_identity) {
    uint8_t *d = make_ppc();
    rom_identity_t id;
    rom_identity_compute(d, PPC_SIZE, &id);
    ASSERT_TRUE(id.kind == ROM_KIND_PPC);
    ASSERT_TRUE(id.intact);
    ASSERT_EQ_INT((int)strlen(id.id), 25);
    char before[ROM_ID_MAX];
    snprintf(before, sizeof before, "%s", id.id);

    d[0x3F0002] ^= 0x40; // PowerPC megabyte, byte lane 2
    rom_identity_compute(d, PPC_SIZE, &id);
    ASSERT_TRUE(strcmp(id.id, before) == 0); // the label is stored, so unchanged
    ASSERT_TRUE(!id.intact);
    ASSERT_TRUE(strstr(id.reason, "PowerPC section") != NULL);
    ASSERT_TRUE(strstr(id.reason, "2") != NULL);
    d[0x3F0002] ^= 0x40;

    d[0x100001] ^= 0x40; // 68k half
    rom_identity_compute(d, PPC_SIZE, &id);
    ASSERT_TRUE(!id.intact);
    ASSERT_TRUE(strstr(id.reason, "68k section") != NULL);
    d[0x100001] ^= 0x40;

    wr_be32(d + 0x300080, 0x00FFFFF9); // pointer outside the PowerPC megabyte
    rom_identity_compute(d, PPC_SIZE, &id);
    ASSERT_TRUE(!id.intact);
    ASSERT_EQ_INT((int)strlen(id.id), 8);
    free(d);
}

// A known 68k half with a foreign PowerPC half is not the known ROM: the
// 64-bit part of the id differs, so lookup finds no row.
TEST(test_ppc_foreign_half_not_recognised) {
    uint8_t *d = make_ppc();
    wr_be32(d, 0x96CD923Du); // the TNT v1 header sum
    rom_identity_t id;
    ASSERT_TRUE(rom_identify_data(d, PPC_SIZE, &id) == NULL);
    ASSERT_TRUE(strncmp(id.id, "96cd923d-", 9) == 0);
    ASSERT_TRUE(rom_lookup("96cd923d-c241cd82bf90797a", PPC_SIZE) != NULL);
    ASSERT_TRUE(rom_lookup("96cd923d-c241cd82bf90797a", PPC_SIZE / 2) == NULL); // size is part of the key
    free(d);
}

// Non-ROM inputs are kind NONE with no id.
TEST(test_not_a_rom) {
    uint8_t b[7] = {0};
    rom_identity_t id;
    rom_identity_compute(b, sizeof b, &id);
    ASSERT_TRUE(id.kind == ROM_KIND_NONE);
    ASSERT_TRUE(id.id[0] == 0);
    ASSERT_TRUE(!id.intact);
}

// Is `s` exactly `n` lowercase hex digits?
static bool is_hex(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}

// Every row's id is well-formed for its size; no two rows share (id, size);
// supported rows have distinct names; flags and spans are where they belong.
TEST(test_table_integrity) {
    ASSERT_TRUE(rom_table_count > 100);
    int supported = 0;
    for (size_t i = 0; i < rom_table_count; i++) {
        const rom_info_t *r = &rom_table[i];
        size_t n = strlen(r->id);
        ASSERT_TRUE(r->family_name && r->family_name[0]);
        ASSERT_TRUE(r->compatible != NULL);
        if (r->rom_size == FULL)
            ASSERT_TRUE(n == 4 && is_hex(r->id, 4));
        else if (r->rom_size == PPC_SIZE)
            ASSERT_TRUE(n == 25 && is_hex(r->id, 8) && r->id[8] == '-' && is_hex(r->id + 9, 16));
        else
            ASSERT_TRUE(n == 8 && is_hex(r->id, 8));
        if (r->checksum_span)
            ASSERT_TRUE(r->checksum_span < r->rom_size && r->rom_size != PPC_SIZE);
        if (r->flags & ROM_F_NO_SUM64)
            ASSERT_TRUE(r->rom_size == PPC_SIZE);
        if (rom_is_supported(r))
            supported++;
        for (size_t j = i + 1; j < rom_table_count; j++) {
            const rom_info_t *o = &rom_table[j];
            ASSERT_TRUE(!(o->rom_size == r->rom_size && strcmp(o->id, r->id) == 0));
            if (rom_is_supported(r) && rom_is_supported(o))
                ASSERT_TRUE(strcmp(o->family_name, r->family_name) != 0);
        }
    }
    ASSERT_TRUE(supported >= 21);
}

// rom_load_lisa_pair produces the same correct image whichever file comes
// first: only the right orientation passes the boot ROM's self-check.
TEST(test_load_pair_order_independent) {
    uint8_t combined[FULL];
    make_lisa(combined, 0x0248);
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
    RUN(test_lisa_identity);
    RUN(test_lisa_rows);
    RUN(test_68k_identity);
    RUN(test_ppc_identity);
    RUN(test_ppc_foreign_half_not_recognised);
    RUN(test_not_a_rom);
    RUN(test_table_integrity);
    RUN(test_load_pair_order_independent);
    printf("[PASS] All ROM identity tests passed\n");
    return 0;
}
