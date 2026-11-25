// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_capture.c
// The capture store an image keeps beside its sector view -- see
// image_capture.h.

#include "image_capture.h"

#include "codec.h"
#include "common.h"
#include "image_internal.h"
#include "log.h"
#include "source.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("image")

#define NUM_TRACKS 80

// The parsed capture: immutable once built.
struct image_capture {
    atomic_int refs; // the image's, and an export's while one runs
    uint8_t *file; // the whole MOOF file
    size_t len;
    moof_index_t ix; // pointers into `file`
    uint8_t *flux_bits[MOOF_TRACK_SLOTS]; // a flux track's bits, converted at build (NULL otherwise)
    uint32_t flux_nbits[MOOF_TRACK_SLOTS];
    uint8_t *status; // a MOOF_SECTOR_* per sector, in image order
    uint32_t sectors;
    uint32_t crc;
    size_t max_track_bytes;
};

// Sectors on a GCR track (12 outermost, one fewer per 16-track zone).
static int zone_sectors(int track) {
    return 12 - track / 16;
}

bool image_has_capture(const image_t *img) {
    return img && img->capture_src && !img->capture_failed;
}

bool image_capture_bits(const image_capture_t *cap, int track, int side, const uint8_t **bits, uint32_t *nbits) {
    *bits = NULL;
    *nbits = 0;
    if (!cap || track < 0 || track >= NUM_TRACKS || side < 0 || side >= cap->ix.sides)
        return false;
    int slot = track * 2 + side;
    if (moof_track_is_flux(&cap->ix, track, side)) {
        *bits = cap->flux_bits[slot];
        *nbits = cap->flux_nbits[slot];
    } else {
        uint8_t *owned = NULL; // never set for a bitstream track
        *nbits = moof_track_bits(&cap->ix, track, side, bits, &owned);
        free(owned);
    }
    return *bits && *nbits;
}

size_t image_capture_track(const image_capture_t *cap, int track, int side, uint8_t **nib,
                           moof_sector_t sec[MOOF_MAX_SECTORS]) {
    *nib = NULL;
    const uint8_t *bits = NULL;
    uint32_t nbits = 0;
    size_t n = 0;
    if (image_capture_bits(cap, track, side, &bits, &nbits))
        n = moof_frame_track(bits, nbits, nib);
    moof_scan_track(*nib, n, track, side, sec, NULL);
    return n;
}

uint32_t image_capture_track_diff(const moof_sector_t sec[MOOF_MAX_SECTORS], int nsec, const uint8_t *data,
                                  const uint8_t *tags, uint32_t in_delta) {
    uint32_t diff = 0;
    for (int k = 0; k < nsec && k < MOOF_MAX_SECTORS; k++) {
        // What the importer put in the sector view for this sector.
        bool same = memcmp(data + (size_t)k * 512, sec[k].data, 512) == 0 &&
                    memcmp(tags + (size_t)k * MOOF_TAG_BYTES, sec[k].tag, MOOF_TAG_BYTES) == 0;
        // A sector the capture could not read changes the medium once
        // written, whatever is written (content alone cannot tell).
        if (sec[k].state != MOOF_SECTOR_OK && (in_delta >> k) & 1)
            same = false;
        if (!same)
            diff |= 1u << k;
    }
    return diff;
}

// Free a store's memory.
static void capture_free(image_capture_t *cap) {
    if (!cap)
        return;
    for (int i = 0; i < MOOF_TRACK_SLOTS; i++)
        free(cap->flux_bits[i]);
    free(cap->status);
    free(cap->file);
    free(cap);
}

// Read and index the capture, convert its flux tracks, and record what
// every sector held.  NULL (logged) on failure.
static image_capture_t *capture_build(source_t *src) {
    image_capture_t *cap = calloc(1, sizeof *cap);
    if (!cap)
        return NULL;
    atomic_init(&cap->refs, 1);
    int rc = source_read_all(src, MOOF_MAX_FILE, &cap->file, &cap->len);
    if (!rc)
        rc = moof_index(cap->file, cap->len, &cap->ix);
    if (rc) {
        LOG(0, "capture '%s': cannot be read again (%s); serving its sectors only", source_key(src), strerror(-rc));
        capture_free(cap);
        return NULL;
    }
    uint32_t crc = RD_LE32(cap->file + 8);
    cap->crc = crc ? crc : codec_crc32(0, cap->file + MOOF_HEADER_SIZE, cap->len - MOOF_HEADER_SIZE);
    for (int t = 0; t < NUM_TRACKS; t++)
        cap->sectors += (uint32_t)(zone_sectors(t) * cap->ix.sides);
    cap->status = malloc(cap->sectors);
    moof_sector_t *sec = malloc(sizeof(moof_sector_t) * MOOF_MAX_SECTORS);
    if (!cap->status || !sec) {
        free(sec);
        capture_free(cap);
        return NULL;
    }
    size_t longest = 0;
    uint32_t block = 0;
    for (int t = 0; t < NUM_TRACKS; t++) {
        int nsec = zone_sectors(t);
        for (int s = 0; s < cap->ix.sides; s++, block += (uint32_t)nsec) {
            int slot = t * 2 + s;
            const uint8_t *bits = NULL;
            uint8_t *owned = NULL;
            uint32_t nbits = moof_track_bits(&cap->ix, t, s, &bits, &owned);
            if (owned) {
                // Converted once, kept: the store is immutable after this.
                cap->flux_bits[slot] = owned;
                cap->flux_nbits[slot] = nbits;
            }
            if ((size_t)nbits / 8 + 2 > longest)
                longest = (size_t)nbits / 8 + 2;
            uint8_t *nib = NULL;
            size_t n = moof_frame_track(bits, nbits, &nib);
            moof_scan_track(nib, n, t, s, sec, NULL);
            free(nib);
            for (int k = 0; k < nsec; k++)
                cap->status[block + (uint32_t)k] = sec[k].state;
        }
    }
    free(sec);
    // INFO's figure bounds every track; the longest seen when it is absent.
    cap->max_track_bytes = (size_t)cap->ix.largest_track_blocks * 512u;
    if (cap->max_track_bytes < longest)
        cap->max_track_bytes = longest;
    return cap;
}

image_capture_t *image_capture(image_t *img) {
    if (!image_has_capture(img))
        return NULL;
    if (!img->capture) {
        img->capture = capture_build(img->capture_src);
        if (!img->capture)
            img->capture_failed = true; // tried once; a plain sector image from here on
    }
    return img->capture;
}

bool image_track_bits(image_t *img, int track, int side, const uint8_t **bits, uint32_t *nbits) {
    *bits = NULL;
    *nbits = 0;
    return image_capture_bits(image_capture(img), track, side, bits, nbits);
}

image_sector_status_t image_capture_sector_status(const image_capture_t *cap, uint64_t block) {
    if (!cap || block >= cap->sectors)
        return IMAGE_SECTOR_OK;
    return (image_sector_status_t)cap->status[block];
}

image_sector_status_t image_sector_status(image_t *img, uint64_t block) {
    return image_capture_sector_status(image_capture(img), block);
}

int image_capture_sides(const image_capture_t *cap) {
    return cap ? cap->ix.sides : 0;
}

const moof_index_t *image_capture_index(const image_capture_t *cap) {
    return cap ? &cap->ix : NULL;
}

uint32_t image_capture_crc(const image_capture_t *cap) {
    return cap ? cap->crc : 0;
}

size_t image_capture_max_track_bytes(const image_capture_t *cap) {
    return cap ? cap->max_track_bytes : 0;
}

image_capture_t *image_capture_retain(image_capture_t *cap) {
    if (cap)
        atomic_fetch_add(&cap->refs, 1);
    return cap;
}

void image_capture_release(image_capture_t *cap) {
    if (cap && atomic_fetch_sub(&cap->refs, 1) == 1)
        capture_free(cap);
}
