// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// The sector tags a guest writes survive a checkpoint.  See Makefile.
//
// Tags live only in memory: a guest's disk_write_tag lands in image->tags,
// not in the delta, and the base file is never written.  So a restore that
// reopened the base and replayed the delta used to hand the guest the new
// data under the OLD tags -- on a Lisa diskette, page labels that no longer
// describe the pages.  image_checkpoint now ends with the tags and
// mac_checkpoint_restore_one_image reads them back; this checks both, for a
// quick and a consolidated checkpoint, from a raw and a DiskCopy source.

#include "checkpoint.h"
#include "checkpoint_images.h"
#include "checkpoint_machine.h"
#include "format_registry.h"
#include "image.h"
#include "image_internal.h" // the tag fields under test
#include "image_wrap.h"
#include "test_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECTOR    512u
#define SECTORS   800u // a 400K GCR diskette
#define TAG_BYTES 12u

// ============================================================
// An in-memory checkpoint (replaces support/stub_checkpoint.c)
// ============================================================

struct checkpoint {
    uint8_t *buf;
    size_t len, cap, pos;
    bool error;
    checkpoint_kind_t kind;
};

static checkpoint_t *cp_new(checkpoint_kind_t kind) {
    checkpoint_t *cp = calloc(1, sizeof *cp);
    ASSERT_TRUE(cp != NULL);
    cp->kind = kind;
    return cp;
}

static void cp_free(checkpoint_t *cp) {
    free(cp->buf);
    free(cp);
}

void system_write_checkpoint_data_loc(checkpoint_t *cp, const void *data, size_t size, const char *tag,
                                      const char *file, int line) {
    (void)tag, (void)file, (void)line;
    if (cp->len + size > cp->cap) {
        size_t cap = (cp->len + size) * 2;
        uint8_t *b = realloc(cp->buf, cap);
        ASSERT_TRUE(b != NULL);
        cp->buf = b;
        cp->cap = cap;
    }
    memcpy(cp->buf + cp->len, data, size);
    cp->len += size;
}

// A read past the end marks the stream failed and yields zeros, as the
// real reader does.
void system_read_checkpoint_data_loc(checkpoint_t *cp, void *data, size_t size, const char *tag, const char *file,
                                     int line) {
    (void)tag, (void)file, (void)line;
    if (cp->error || cp->pos + size > cp->len) {
        cp->error = true;
        memset(data, 0, size);
        return;
    }
    memcpy(data, cp->buf + cp->pos, size);
    cp->pos += size;
}

bool checkpoint_has_error(checkpoint_t *cp) {
    return cp->error;
}

void checkpoint_set_error(checkpoint_t *cp) {
    cp->error = true;
}

checkpoint_kind_t checkpoint_get_kind(checkpoint_t *cp) {
    return cp->kind;
}

bool checkpoint_read_count(checkpoint_t *cp, uint32_t *out, uint32_t max, const char *what) {
    (void)what;
    system_read_checkpoint_data_loc(cp, out, sizeof *out, NULL, __FILE__, __LINE__);
    if (*out > max) {
        cp->error = true;
        *out = 0;
        return false;
    }
    return true;
}

char *checkpoint_read_string(checkpoint_t *cp, uint32_t max, const char *what) {
    uint32_t len = 0;
    if (!checkpoint_read_count(cp, &len, max, what) || len == 0)
        return NULL;
    char *s = malloc((size_t)len + 1);
    ASSERT_TRUE(s != NULL);
    system_read_checkpoint_data_loc(cp, s, len, NULL, __FILE__, __LINE__);
    s[len] = '\0';
    return s;
}

static char g_dir[64]; // the media and the deltas

const char *checkpoint_machine_dir(void) {
    return g_dir;
}

// Never reached: no image here is attached through the volume wrapper.
int image_wrap_volume(image_t *img) {
    (void)img;
    return IMAGE_WRAP_NONE;
}

// ============================================================
// Media
// ============================================================

// A private directory for this run.
static void make_dir(void) {
    if (g_dir[0])
        return;
    strcpy(g_dir, "/tmp/gs-ckpt-tags-XXXXXX");
    ASSERT_TRUE(mkdtemp(g_dir) != NULL);
}

// The tag the base file holds for `sector`, and the one the guest writes.
static void base_tag(unsigned sector, uint8_t t[TAG_BYTES]) {
    for (unsigned i = 0; i < TAG_BYTES; i++)
        t[i] = (uint8_t)(0x40 + sector + i);
}
static void guest_tag(unsigned sector, uint8_t t[TAG_BYTES]) {
    for (unsigned i = 0; i < TAG_BYTES; i++)
        t[i] = (uint8_t)(0xA0 ^ (sector * 7 + i));
}

// Write a zero-filled 400K diskette at `path`: a raw image, or a DiskCopy
// 4.2 file whose tag section carries base_tag for every sector.
static void make_disk(const char *path, bool dc42) {
    make_dir();
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    static uint8_t data[SECTORS * SECTOR];
    static uint8_t tags[SECTORS * TAG_BYTES];
    if (dc42) {
        for (unsigned s = 0; s < SECTORS; s++)
            base_tag(s, tags + s * TAG_BYTES);
        uint8_t name[DC42_NAME_FIELD], hdr[DISKCOPY_HEADER_SIZE];
        dc42_name_field(name, "-not a Macintosh disk-");
        dc42_build_header(hdr, name, sizeof data, sizeof tags, dc42_checksum(0, data, sizeof data),
                          dc42_checksum(0, tags + TAG_BYTES, sizeof tags - TAG_BYTES), 0, 0x02);
        ASSERT_TRUE(fwrite(hdr, sizeof hdr, 1, f) == 1);
    }
    ASSERT_TRUE(fwrite(data, sizeof data, 1, f) == 1);
    if (dc42)
        ASSERT_TRUE(fwrite(tags, sizeof tags, 1, f) == 1);
    fclose(f);
}

// The sectors the guest writes in these tests: the boot block, one in the
// middle and the last.
static const unsigned k_written[] = {0, 400, SECTORS - 1};
#define N_WRITTEN (sizeof k_written / sizeof k_written[0])

// The guest writes each sector's data and tag together, as the Sony and
// IWM controllers do.
static void guest_writes(image_t *img) {
    for (unsigned i = 0; i < N_WRITTEN; i++) {
        unsigned s = k_written[i];
        uint8_t data[SECTOR], tag[TAG_BYTES];
        memset(data, (int)(0x11 + i), sizeof data);
        guest_tag(s, tag);
        ASSERT_EQ_INT((int)disk_write_data(img, (size_t)s * SECTOR, data, SECTOR), (int)SECTOR);
        ASSERT_EQ_INT((int)disk_write_tag(img, s, tag, TAG_BYTES), (int)TAG_BYTES);
    }
}

// True when `s` is one of the sectors guest_writes wrote.
static bool written(unsigned s) {
    for (unsigned i = 0; i < N_WRITTEN; i++)
        if (k_written[i] == s)
            return true;
    return false;
}

// Every sector reads back the guest's tag where it wrote one, else the
// base's (zero for a raw source); the written data is there too.
static void check_disk(image_t *img, bool dc42) {
    ASSERT_TRUE(img->tags != NULL);
    ASSERT_EQ_INT((int)img->tag_bytes, (int)TAG_BYTES);
    ASSERT_EQ_INT((int)img->tag_count, (int)SECTORS);
    for (unsigned s = 0; s < SECTORS; s++) {
        uint8_t got[TAG_BYTES], want[TAG_BYTES];
        ASSERT_EQ_INT((int)disk_read_tag(img, s, got, TAG_BYTES), (int)TAG_BYTES);
        if (written(s))
            guest_tag(s, want);
        else if (dc42)
            base_tag(s, want);
        else
            memset(want, 0, sizeof want);
        ASSERT_TRUE(memcmp(got, want, TAG_BYTES) == 0);
    }
    for (unsigned i = 0; i < N_WRITTEN; i++) {
        uint8_t data[SECTOR];
        ASSERT_EQ_INT((int)disk_read_data(img, (size_t)k_written[i] * SECTOR, data, SECTOR), (int)SECTOR);
        for (unsigned b = 0; b < SECTOR; b++)
            ASSERT_EQ_INT(data[b], 0x11 + i);
    }
}

// The marker written after the image: the restore must leave the stream
// exactly at it.
#define SENTINEL 0x5EC7A65u

// Mount `path` writable, let the guest write, checkpoint it, close it, then
// restore it from the checkpoint and check the tags and data came back.
static void round_trip(const char *path, bool dc42, checkpoint_kind_t kind) {
    image_t *img = image_create(path, g_dir);
    ASSERT_TRUE(img != NULL);
    guest_writes(img);

    checkpoint_t *cp = cp_new(kind);
    image_checkpoint(img, cp);
    uint32_t sentinel = SENTINEL;
    system_write_checkpoint_data_loc(cp, &sentinel, sizeof sentinel, NULL, __FILE__, __LINE__);
    ASSERT_TRUE(!cp->error);
    image_close(img);

    image_t *back = mac_checkpoint_restore_one_image(cp, (image_geometry_t){0});
    ASSERT_TRUE(back != NULL);
    ASSERT_TRUE(!cp->error);
    uint32_t after = 0;
    system_read_checkpoint_data_loc(cp, &after, sizeof after, NULL, __FILE__, __LINE__);
    ASSERT_EQ_INT((int)after, (int)SENTINEL);
    check_disk(back, dc42);

    image_close(back);
    cp_free(cp);
}

// ============================================================
// Tests
// ============================================================

// A DiskCopy source: the restored tags are the guest's, not the file's.
// This is the case that lost data: reopening the base reloads the file's
// tags, and only the checkpoint knows better.
TEST(test_dc42_tags_survive_a_quick_checkpoint) {
    char path[96];
    make_dir();
    snprintf(path, sizeof path, "%s/lisa.dc42", g_dir);
    make_disk(path, true);
    round_trip(path, true, CHECKPOINT_KIND_QUICK);
}

TEST(test_dc42_tags_survive_a_consolidated_checkpoint) {
    char path[96];
    make_dir();
    snprintf(path, sizeof path, "%s/lisa-c.dc42", g_dir);
    make_disk(path, true);
    round_trip(path, true, CHECKPOINT_KIND_CONSOLIDATED);
}

// A raw source has no tags in its file at all; the zeroed area it is given
// carries the guest's writes through a checkpoint the same way.
TEST(test_raw_tags_survive_a_quick_checkpoint) {
    char path[96];
    make_dir();
    snprintf(path, sizeof path, "%s/mac.dsk", g_dir);
    make_disk(path, false);
    round_trip(path, false, CHECKPOINT_KIND_QUICK);
}

TEST(test_raw_tags_survive_a_consolidated_checkpoint) {
    char path[96];
    make_dir();
    snprintf(path, sizeof path, "%s/mac-c.dsk", g_dir);
    make_disk(path, false);
    round_trip(path, false, CHECKPOINT_KIND_CONSOLIDATED);
}

// A disk with no tags (a hard disk) writes an empty tag record, and the
// restore still ends exactly where the image did.
TEST(test_an_untagged_disk_keeps_the_stream_aligned) {
    char path[96];
    make_dir();
    snprintf(path, sizeof path, "%s/hd.img", g_dir);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    ASSERT_TRUE(ftruncate(fileno(f), 1024 * 1024) == 0); // 1 MB: no floppy size
    fclose(f);

    image_t *img = image_create(path, g_dir);
    ASSERT_TRUE(img != NULL);
    ASSERT_TRUE(img->tags == NULL);
    checkpoint_t *cp = cp_new(CHECKPOINT_KIND_QUICK);
    image_checkpoint(img, cp);
    uint32_t sentinel = SENTINEL;
    system_write_checkpoint_data_loc(cp, &sentinel, sizeof sentinel, NULL, __FILE__, __LINE__);
    image_close(img);

    image_t *back = mac_checkpoint_restore_one_image(cp, (image_geometry_t){0});
    ASSERT_TRUE(back != NULL);
    ASSERT_TRUE(back->tags == NULL);
    uint32_t after = 0;
    system_read_checkpoint_data_loc(cp, &after, sizeof after, NULL, __FILE__, __LINE__);
    ASSERT_TRUE(!cp->error);
    ASSERT_EQ_INT((int)after, (int)SENTINEL);
    image_close(back);
    cp_free(cp);
}

// A corrupt tag record (an absurd tag size) fails the restore loudly
// instead of allocating from it.
TEST(test_a_corrupt_tag_record_fails_the_restore) {
    char path[96];
    make_dir();
    snprintf(path, sizeof path, "%s/bad.dsk", g_dir);
    make_disk(path, false);
    image_t *img = image_create(path, g_dir);
    ASSERT_TRUE(img != NULL);
    checkpoint_t *cp = cp_new(CHECKPOINT_KIND_QUICK);
    image_checkpoint(img, cp);
    image_close(img);

    // The tag record is the stream's last 8 + 9600 bytes; make its tag size
    // 4096.
    size_t rec = cp->len - (SECTORS * TAG_BYTES) - 8;
    uint32_t huge = 4096;
    memcpy(cp->buf + rec, &huge, sizeof huge);

    image_t *back = mac_checkpoint_restore_one_image(cp, (image_geometry_t){0});
    ASSERT_TRUE(cp->error);
    image_close(back);
    cp_free(cp);
}

int main(void) {
    RUN(test_dc42_tags_survive_a_quick_checkpoint);
    RUN(test_dc42_tags_survive_a_consolidated_checkpoint);
    RUN(test_raw_tags_survive_a_quick_checkpoint);
    RUN(test_raw_tags_survive_a_consolidated_checkpoint);
    RUN(test_an_untagged_disk_keeps_the_stream_aligned);
    RUN(test_a_corrupt_tag_record_fails_the_restore);
    printf("[PASS] All image checkpoint tag tests passed\n");
    return 0;
}
