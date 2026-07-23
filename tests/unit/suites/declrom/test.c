// Declaration-ROM builder unit tests (runtime-vrom proposal stages B/C).
//
// gsvrom_generate builds every personality's image host-side from a
// monitors[] table mirroring the card kinds'; each image must pass the
// §5 structural validator, carry the expected identity (BoardId, vendor
// string, sResource names), splice the assembled fragments verbatim,
// and regenerate bit-identically (checkpoint-restore determinism).
// Plus the validation guards: zero offsets, non-ascending ids, and CRC
// corruption must all be caught.

#include "card.h"
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

// --- Monitor tables mirroring the card kinds' --------------------------------

static const int depths4[] = {1, 2, 4, 8, 0};
static const int depths5[] = {1, 4, 8, 16, 32, 0};
static const int depths1[] = {1, 0};

static const nubus_monitor_t jmfb_mons[] = {
    {.id = "13in_rgb", .width = 640, .height = 480, .depths = depths4, .srsrc_sister = 0xA6},
    {.id = "12in_rgb", .width = 512, .height = 384, .depths = depths4, .srsrc_sister = 0xA2},
    {.id = "15in_bw", .width = 640, .height = 870, .depths = depths4, .srsrc_sister = 0xA1},
    {.id = "21in_rgb", .width = 1152, .height = 870, .depths = depths4, .srsrc_sister = 0xA7},
    {0},
};
static const nubus_monitor_t boogie_mons[] = {
    {.id = "rgb_640x480", .width = 640, .height = 480, .depths = depths5, .srsrc_sister = 0x6B},
    {.id = "rgb_800x600", .width = 800, .height = 600, .depths = depths5, .srsrc_sister = 0x6C},
    {.id = "rgb_832x624", .width = 832, .height = 624, .depths = depths5, .srsrc_sister = 0x6D},
    {0},
};
static const nubus_monitor_t gc_mons[] = {
    {.id = "gc_640x480", .width = 640, .height = 480, .depths = depths4, .srsrc_sister = 0x80},
    {0},
};
static const nubus_monitor_t se30_mons[] = {
    {.id = "se30_internal", .width = 512, .height = 342, .depths = depths1, .srsrc_sister = 0x80},
    {0},
};

static const struct {
    gsvrom_personality_t p;
    const nubus_monitor_t *mons;
    uint16_t board_id;
    const char *vid_name;
    size_t n_vidsrsrc; // directory video entries (GC adds the $A0 family)
} PERSONALITIES[] = {
    {GSVROM_JMFB,   jmfb_mons,   0x0027, "Display_Video_Apple_MDC",    4},
    {GSVROM_BOOGIE, boogie_mons, 0x05FA, "Display_Video_Apple_Boogie", 3},
    {GSVROM_MDCGC,  gc_mons,     0x002C, "Display_Video_Apple_MDCGC",  2},
    {GSVROM_SE30,   se30_mons,   0x000C, "Display_Video_Apple_SE30",   1},
};

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

// Find a board-sResource entry's target block (e.g. PrimaryInit = 34);
// SIZE_MAX if absent (offset 0 is a legitimate block position — the
// first spliced fragment starts the image).
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
    return (size_t)-1;
}

// Count directory entries with id != 1 (the functional sResources).
static size_t count_vid_srsrcs(const uint8_t *img, size_t size) {
    size_t fb = size - 20;
    size_t dir = target_of(img, fb);
    size_t n = 0;
    for (size_t e = dir; (be32(img + e) >> 24) != 0xFF; e += 4)
        if ((uint8_t)(be32(img + e) >> 24) != 1)
            n++;
    return n;
}

// Find the DRVR sBlock via the first video sResource's sDriver dir;
// SIZE_MAX if absent.
static size_t find_drvr_block(const uint8_t *img, size_t size) {
    size_t fb = size - 20;
    size_t dir = target_of(img, fb);
    for (size_t e = dir; (be32(img + e) >> 24) != 0xFF; e += 4) {
        uint8_t id = (uint8_t)(be32(img + e) >> 24);
        if (id == 1)
            continue;
        size_t sr = target_of(img, e);
        for (size_t ve = sr; (be32(img + ve) >> 24) != 0xFF; ve += 4)
            if ((uint8_t)(be32(img + ve) >> 24) == 4)
                return target_of(img, target_of(img, ve)); // dir's first entry
    }
    return (size_t)-1;
}

// The first video sResource's name cString, or NULL.
static const char *find_vid_name(const uint8_t *img, size_t size) {
    size_t fb = size - 20;
    size_t dir = target_of(img, fb);
    for (size_t e = dir; (be32(img + e) >> 24) != 0xFF; e += 4) {
        uint8_t id = (uint8_t)(be32(img + e) >> 24);
        if (id == 1)
            continue;
        size_t sr = target_of(img, e);
        for (size_t ve = sr; (be32(img + ve) >> 24) != 0xFF; ve += 4)
            if ((uint8_t)(be32(img + ve) >> 24) == 2)
                return (const char *)img + target_of(img, ve);
    }
    return NULL;
}

// --- A minimal JMFB-shaped build used by the guard tests ---------------------

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

// --- Tests -------------------------------------------------------------------

TEST(test_generate_all_personalities) {
    for (size_t i = 0; i < sizeof(PERSONALITIES) / sizeof(PERSONALITIES[0]); i++) {
        declrom_builder_t *b = gsvrom_generate(PERSONALITIES[i].p, PERSONALITIES[i].mons);
        ASSERT_TRUE(b != NULL);
        size_t n = 0;
        const uint8_t *img = declrom_builder_bytes(b, &n);
        ASSERT_TRUE(img != NULL && n > 20);
        // §5 structural validation.
        ASSERT_TRUE(declrom_image_validate(img, n));
        // Identity: vendor string + BoardId (the structural-recognition
        // pair vrom.identify keys on).
        uint16_t board_id = 0;
        ASSERT_TRUE(declrom_identify_vendor(img, n, "granny-smith", &board_id));
        ASSERT_EQ_INT(PERSONALITIES[i].board_id, board_id);
        // The functional sResources: count + name.
        ASSERT_EQ_INT((int)PERSONALITIES[i].n_vidsrsrc, (int)count_vid_srsrcs(img, n));
        const char *vname = find_vid_name(img, n);
        ASSERT_TRUE(vname && strcmp(vname, PERSONALITIES[i].vid_name) == 0);
        // The spliced code blocks are the assembled fragments, verbatim.
        size_t fn = 0;
        const uint8_t *frag = gsvrom_frag(PERSONALITIES[i].p, GSVROM_FRAG_INIT, &fn);
        size_t pi = find_board_block(img, n, 34);
        ASSERT_TRUE(frag && pi != (size_t)-1 && memcmp(img + pi, frag, fn) == 0);
        frag = gsvrom_frag(PERSONALITIES[i].p, GSVROM_FRAG_DRVR, &fn);
        size_t dr = find_drvr_block(img, n);
        ASSERT_TRUE(frag && dr != (size_t)-1 && memcmp(img + dr, frag, fn) == 0);
        frag = gsvrom_frag(PERSONALITIES[i].p, GSVROM_FRAG_SINIT, &fn);
        size_t si = find_board_block(img, n, 38);
        if (PERSONALITIES[i].p == GSVROM_MDCGC)
            ASSERT_TRUE(frag && si != (size_t)-1 && memcmp(img + si, frag, fn) == 0);
        else
            ASSERT_TRUE(!frag && si == (size_t)-1);
        declrom_builder_free(b);
    }
}

TEST(test_generate_determinism) {
    // Regeneration must be bit-identical (checkpoint restore rebuilds the
    // image from the recorded configuration; proposal §4).
    for (size_t i = 0; i < sizeof(PERSONALITIES) / sizeof(PERSONALITIES[0]); i++) {
        declrom_builder_t *b1 = gsvrom_generate(PERSONALITIES[i].p, PERSONALITIES[i].mons);
        declrom_builder_t *b2 = gsvrom_generate(PERSONALITIES[i].p, PERSONALITIES[i].mons);
        ASSERT_TRUE(b1 && b2);
        size_t n1 = 0, n2 = 0;
        const uint8_t *i1 = declrom_builder_bytes(b1, &n1);
        const uint8_t *i2 = declrom_builder_bytes(b2, &n2);
        ASSERT_EQ_INT((int)n1, (int)n2);
        ASSERT_TRUE(memcmp(i1, i2, n1) == 0);
        declrom_builder_free(b1);
        declrom_builder_free(b2);
    }
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
    // spid colliding with the board id / terminator.
    declrom_vidmode_t m = {
        .row_bytes = 64, .width = 512, .height = 342, .pixel_size = 1, .cmp_count = 1, .cmp_size = 1, .page_count = 1};
    declrom_vidsrsrc_t v = {.name = "n", .modes = &m, .mode_count = 1, .length = 64 * 342};
    ASSERT_TRUE(!declrom_add_video_srsrc(b, 0x01, &v));
    ASSERT_TRUE(!declrom_add_video_srsrc(b, 0xFF, &v));
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
    RUN(test_generate_all_personalities);
    RUN(test_generate_determinism);
    RUN(test_validation_guards);
    RUN(test_builder_input_guards);
    fprintf(stderr, "declrom: all tests passed\n");
    return 0;
}
