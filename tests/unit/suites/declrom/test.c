// Declaration-ROM builder unit tests (runtime-vrom proposal stage B).
//
// The heart of the suite: generate a JMFB-shaped image with the builder
// — splicing the PrimaryInit / DRVR blocks extracted from the assembled
// gsvrom blob — and assert its structural fingerprint (directory walk,
// record content, spliced code bytes) is IDENTICAL to the assembled
// blob's.  Offsets and record placement may differ; everything the Slot
// Manager consumes must not.  Plus the §5 validation guards: zero
// offsets, non-ascending ids, and CRC corruption must all be caught.

#include "declrom.h"
#include "gsvrom.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Stubs for declrom.c's loader-side dependencies (unused here) -----------
void machine_config_note_vrom(const char *card_id, const char *path, uint32_t crc, bool explicit_pick) {
    (void)card_id;
    (void)path;
    (void)crc;
    (void)explicit_pick;
}
const char *vrom_offer_find(const char *card_id, int idx, size_t *out_chip_size) {
    (void)card_id;
    (void)idx;
    (void)out_chip_size;
    return NULL;
}
bool vrom_offer_info(const char *path, uint32_t *out_crc, bool *out_explicit) {
    (void)path;
    (void)out_crc;
    (void)out_explicit;
    return false;
}

// --- Image walking helpers ---------------------------------------------------

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Resolve a list entry at `at` to its target offset (signed 24-bit
// self-relative).
static size_t target_of(const uint8_t *img, size_t at) {
    int32_t rel = (int32_t)(be32(img + at) & 0x00FFFFFF);
    if (rel & 0x800000)
        rel -= 0x1000000;
    return at + (size_t)(intptr_t)rel;
}

// Fingerprint sink: canonical text lines appended to a growing buffer.
typedef struct {
    char *buf;
    size_t len, cap;
} fp_t;

static void fp_addf(fp_t *f, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    if (f->len + n + 2 > f->cap) {
        f->cap = (f->cap ? f->cap * 2 : 65536) + n;
        f->buf = realloc(f->buf, f->cap);
        ASSERT_TRUE(f->buf != NULL);
    }
    memcpy(f->buf + f->len, line, n);
    f->len += n;
    f->buf[f->len++] = '\n';
    f->buf[f->len] = '\0';
}

static void fp_hex(fp_t *f, const char *tag, const uint8_t *p, size_t n) {
    char *line = malloc(n * 2 + strlen(tag) + 2);
    ASSERT_TRUE(line != NULL);
    char *w = line + sprintf(line, "%s ", tag);
    for (size_t i = 0; i < n; i++)
        w += sprintf(w, "%02x", p[i]);
    fp_addf(f, "%s", line);
    free(line);
}

// Fingerprint one size-prefixed block (sBlock / sExecBlock) verbatim.
static void fp_block(fp_t *f, const char *tag, const uint8_t *img, size_t at) {
    uint32_t size = be32(img + at);
    fp_hex(f, tag, img + at, size);
}

// Fingerprint a mode list: data entries + the VPBlock bytes.
static void fp_mode_list(fp_t *f, const uint8_t *img, size_t at) {
    for (size_t e = at; (be32(img + e) >> 24) != 0xFF; e += 4) {
        uint8_t id = (uint8_t)(be32(img + e) >> 24);
        if (id == 1) // mVidParams
            fp_block(f, "    vp", img, target_of(img, e));
        else
            fp_addf(f, "    m e%u dat %#x", id, be32(img + e) & 0xFFFFFF);
    }
}

// Fingerprint one sResource list; `board` selects the id namespace.
static void fp_srsrc(fp_t *f, const uint8_t *img, size_t at, int board) {
    for (size_t e = at; (be32(img + e) >> 24) != 0xFF; e += 4) {
        uint8_t id = (uint8_t)(be32(img + e) >> 24);
        uint32_t low = be32(img + e) & 0xFFFFFF;
        size_t tgt = target_of(img, e);
        if (id == 1) { // sRsrcType: 8-byte record
            fp_hex(f, "  type", img + tgt, 8);
        } else if (id == 2) { // sRsrcName cString
            fp_addf(f, "  name %s", (const char *)img + tgt);
        } else if (board && id == 32) {
            fp_addf(f, "  boardid %#x", low);
        } else if (board && id == 33) {
            fp_block(f, "  pram", img, tgt);
        } else if (board && (id == 34 || id == 38)) {
            fp_addf(f, "  exec e%u:", id);
            fp_block(f, "  code", img, tgt);
        } else if (board && id == 36) { // VendorInfo sub-list of cStrings
            for (size_t v = tgt; (be32(img + v) >> 24) != 0xFF; v += 4)
                fp_addf(f, "  vendor%u %s", (uint8_t)(be32(img + v) >> 24), (const char *)img + target_of(img, v));
        } else if (!board && id == 4) { // sDriver directory
            for (size_t d = tgt; (be32(img + d) >> 24) != 0xFF; d += 4) {
                fp_addf(f, "  drvr cpu%u:", (uint8_t)(be32(img + d) >> 24));
                fp_block(f, "  drvrblk", img, target_of(img, d));
            }
        } else if (!board && (id == 7 || id == 8)) { // data entries
            fp_addf(f, "  e%u dat %#x", id, low);
        } else if (!board && (id == 10 || id == 11 || id == 12 || id == 13)) {
            fp_addf(f, "  e%u long %#x", id, be32(img + tgt));
        } else if (!board && id >= 128) { // mode list
            fp_addf(f, "  mode %#x:", id);
            fp_mode_list(f, img, tgt);
        } else {
            fp_addf(f, "  e%u raw %#x", id, low);
        }
    }
}

// Fingerprint a whole image from its Format Block down.
static char *fingerprint(const uint8_t *img, size_t size) {
    fp_t f = {0};
    size_t fb = size - 20;
    fp_addf(&f, "format rev=%u fmt=%u lanes=%#04x", img[fb + 12], img[fb + 13], img[size - 1]);
    size_t dir = target_of(img, fb);
    for (size_t e = dir; (be32(img + e) >> 24) != 0xFF; e += 4) {
        uint8_t id = (uint8_t)(be32(img + e) >> 24);
        fp_addf(&f, "srsrc %#x", id);
        fp_srsrc(&f, img, target_of(img, e), id == 1);
    }
    return f.buf;
}

// Find the offset of a board-sResource entry's block in an image
// (e.g. PrimaryInit = 34).  Returns 0 if absent.
static size_t find_board_block(const uint8_t *img, size_t size, uint8_t want_id) {
    size_t fb = size - 20;
    size_t dir = target_of(img, fb);
    for (size_t e = dir; (be32(img + e) >> 24) != 0xFF; e += 4) {
        if ((uint8_t)(be32(img + e) >> 24) != 1)
            continue;
        size_t board = target_of(img, e);
        for (size_t be = board; (be32(img + be) >> 24) != 0xFF; be += 4)
            if ((uint8_t)(be32(img + be) >> 24) == want_id)
                return target_of(img, be);
    }
    return 0;
}

// Find the DRVR sBlock via the first video sResource's sDriver dir.
static size_t find_drvr_block(const uint8_t *img, size_t size) {
    size_t fb = size - 20;
    size_t dir = target_of(img, fb);
    for (size_t e = dir; (be32(img + e) >> 24) != 0xFF; e += 4) {
        uint8_t id = (uint8_t)(be32(img + e) >> 24);
        if (id < 128)
            continue;
        size_t sr = target_of(img, e);
        for (size_t ve = sr; (be32(img + ve) >> 24) != 0xFF; ve += 4)
            if ((uint8_t)(be32(img + ve) >> 24) == 4)
                return target_of(img, target_of(img, ve)); // dir's first entry
    }
    return 0;
}

// --- The JMFB-shaped build ---------------------------------------------------

// One VideoSRsrc4-shaped descriptor: four indexed depths, tight stride.
static void add_jmfb_monitor(declrom_builder_t *b, uint8_t spid, uint16_t w, uint16_t h) {
    declrom_vidmode_t modes[4];
    for (int i = 0; i < 4; i++) {
        uint16_t bpp = (uint16_t)(1u << i);
        modes[i] = (declrom_vidmode_t){
            .base_offset = 0,
            .row_bytes = (uint16_t)(w * bpp / 8),
            .width = w,
            .height = h,
            .pixel_type = 0,
            .pixel_size = bpp,
            .cmp_count = 1,
            .cmp_size = bpp,
            .dev_type = 0, // clutType
            .page_count = 1,
        };
    }
    declrom_vidsrsrc_t v = {
        .name = "Display_Video_Apple_MDC",
        .drhw = 0x0019,
        .flags = 2, // fOpenAtStart
        .use_major = false,
        .base = 0xA00,
        .length = (uint32_t)w * h,
        .modes = modes,
        .mode_count = 4,
    };
    ASSERT_TRUE(declrom_add_video_srsrc(b, spid, &v));
}

TEST(test_blobs_validate) {
    // All four embedded blobs must pass the structural validator.
    for (int p = 0; p < 4; p++) {
        size_t n = 0;
        const uint8_t *img = gsvrom_blob((gsvrom_personality_t)p, &n);
        ASSERT_TRUE(img != NULL);
        ASSERT_TRUE(declrom_image_validate(img, n));
    }
}

TEST(test_generated_matches_blob) {
    size_t blob_size = 0;
    const uint8_t *blob = gsvrom_blob(GSVROM_JMFB, &blob_size);
    ASSERT_TRUE(blob != NULL);

    // Extract the blob's own exec + driver blocks for splicing.
    size_t pi_at = find_board_block(blob, blob_size, 34);
    size_t drvr_at = find_drvr_block(blob, blob_size);
    ASSERT_TRUE(pi_at != 0 && drvr_at != 0);
    uint32_t pi_size = be32(blob + pi_at);
    uint32_t drvr_size = be32(blob + drvr_at);

    declrom_builder_t *b = declrom_builder_new(0x8000);
    ASSERT_TRUE(b != NULL);
    ASSERT_TRUE(declrom_set_board(b, "GS Generic Display (8*24)", 0x0027));
    declrom_set_vendor(b, "granny-smith", "1.0", "gsvrom");
    static const uint8_t pram[6] = {0x80, 0, 0, 0, 0, 0}; // b1 = savedMode
    declrom_set_pram_init(b, pram);
    ASSERT_TRUE(declrom_add_exec(b, DECLROM_PRIMARY_INIT, blob + pi_at, pi_size));
    ASSERT_TRUE(declrom_add_drvr(b, blob + drvr_at, drvr_size));
    // Deliberately added out of spID order — finalise must sort.
    add_jmfb_monitor(b, 0xA6, 640, 480);
    add_jmfb_monitor(b, 0xA1, 640, 870);
    add_jmfb_monitor(b, 0xA7, 1152, 870);
    add_jmfb_monitor(b, 0xA2, 512, 384);
    ASSERT_TRUE(declrom_finalise(b, 0x0F));

    size_t gen_size = 0;
    const uint8_t *gen = declrom_builder_bytes(b, &gen_size);
    ASSERT_TRUE(gen != NULL && gen_size > 20);
    ASSERT_TRUE(declrom_image_validate(gen, gen_size));

    // The structural fingerprints must be byte-for-byte identical even
    // though offsets and record placement differ.
    char *fp_blob = fingerprint(blob, blob_size);
    char *fp_gen = fingerprint(gen, gen_size);
    if (strcmp(fp_blob, fp_gen) != 0) {
        fprintf(stderr, "--- blob fingerprint ---\n%s\n--- generated fingerprint ---\n%s\n", fp_blob, fp_gen);
        ASSERT_TRUE(!"fingerprints differ");
    }
    free(fp_blob);
    free(fp_gen);
    declrom_builder_free(b);
}

TEST(test_determinism) {
    // Two builds from identical inputs must be byte-identical.
    uint8_t *first = NULL;
    size_t first_size = 0;
    for (int round = 0; round < 2; round++) {
        declrom_builder_t *b = declrom_builder_new(0x8000);
        ASSERT_TRUE(declrom_set_board(b, "Determinism", 0x1234));
        add_jmfb_monitor(b, 0xA6, 640, 480);
        add_jmfb_monitor(b, 0xA1, 640, 870);
        ASSERT_TRUE(declrom_finalise(b, 0x0F));
        size_t n = 0;
        const uint8_t *img = declrom_builder_bytes(b, &n);
        if (round == 0) {
            first = malloc(n);
            memcpy(first, img, n);
            first_size = n;
        } else {
            ASSERT_EQ_INT((int)first_size, (int)n);
            ASSERT_TRUE(memcmp(first, img, n) == 0);
        }
        declrom_builder_free(b);
    }
    free(first);
}

TEST(test_validation_guards) {
    declrom_builder_t *b = declrom_builder_new(0x8000);
    ASSERT_TRUE(declrom_set_board(b, "Guard", 0x0001));
    add_jmfb_monitor(b, 0x80, 640, 480);
    ASSERT_TRUE(declrom_finalise(b, 0x0F));
    size_t n = 0;
    const uint8_t *img = declrom_builder_bytes(b, &n);
    uint8_t *copy = malloc(n);
    ASSERT_TRUE(copy != NULL);

    // 1. A zero offset in a directory entry (the silent-|-fold class).
    memcpy(copy, img, n);
    size_t dir = target_of(copy, n - 20);
    copy[dir + 1] = copy[dir + 2] = copy[dir + 3] = 0;
    ASSERT_TRUE(!declrom_image_validate(copy, n));

    // 2. Non-ascending directory ids.
    memcpy(copy, img, n);
    copy[dir] = 0x90;
    copy[dir + 4] = 0x80; // second entry now lower than first
    ASSERT_TRUE(!declrom_image_validate(copy, n));

    // 3. A flipped content byte breaks the CRC.
    memcpy(copy, img, n);
    copy[dir] ^= 0; // keep structure; corrupt a name byte instead
    copy[target_of(copy, dir)] ^= 0xFF;
    ASSERT_TRUE(!declrom_image_validate(copy, n));

    // 4. The pristine image still validates.
    ASSERT_TRUE(declrom_image_validate(img, n));

    free(copy);
    declrom_builder_free(b);
}

TEST(test_builder_input_guards) {
    declrom_builder_t *b = declrom_builder_new(0x8000);
    // finalise without a board must fail.
    ASSERT_TRUE(!declrom_finalise(b, 0x0F));
    ASSERT_TRUE(declrom_set_board(b, "X", 1));
    // Only dense 4-lane images are generated.
    ASSERT_TRUE(!declrom_finalise(b, 0x78));
    // spid outside the designer range.
    declrom_vidmode_t m = {
        .row_bytes = 64, .width = 512, .height = 342, .pixel_size = 1, .cmp_count = 1, .cmp_size = 1, .page_count = 1};
    declrom_vidsrsrc_t v = {.name = "n", .modes = &m, .mode_count = 1, .length = 64 * 342};
    ASSERT_TRUE(!declrom_add_video_srsrc(b, 0x01, &v));
    // duplicate spid.
    ASSERT_TRUE(declrom_add_video_srsrc(b, 0x80, &v));
    ASSERT_TRUE(!declrom_add_video_srsrc(b, 0x80, &v));
    // bad exec fragment: undersized / wrong revision.
    static const uint8_t tiny[4] = {0, 0, 0, 4};
    ASSERT_TRUE(!declrom_add_exec(b, DECLROM_PRIMARY_INIT, tiny, sizeof tiny));
    static const uint8_t badrev[12] = {0, 0, 0, 12, 0x03, 0x02, 0, 0, 0, 0, 0, 4};
    ASSERT_TRUE(!declrom_add_exec(b, DECLROM_PRIMARY_INIT, badrev, sizeof badrev));
    declrom_builder_free(b);

    // Overflow: a builder too small for its content fails finalise.
    declrom_builder_t *small = declrom_builder_new(64);
    ASSERT_TRUE(declrom_set_board(small, "Too big for 64 bytes", 1));
    ASSERT_TRUE(!declrom_finalise(small, 0x0F));
    declrom_builder_free(small);
}

int main(void) {
    RUN(test_blobs_validate);
    RUN(test_generated_matches_blob);
    RUN(test_determinism);
    RUN(test_validation_guards);
    RUN(test_builder_input_guards);
    fprintf(stderr, "declrom: all tests passed\n");
    return 0;
}
