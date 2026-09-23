// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_scratch.c
// Identity-checked cache of derived images.  See image_scratch.h.

#include "image_scratch.h"
#include "storage_util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define IMAGE_SCRATCH_DEFAULT_DIR "/tmp/gs-image-ro"

// An identity is a path plus a few numbers; anything longer is not one.
#define IDENTITY_MAX (PATH_MAX * 2 + 256)

const char *image_scratch_dir(void) {
    const char *cache = getenv("GS_STORAGE_CACHE");
    return (cache && *cache) ? cache : IMAGE_SCRATCH_DEFAULT_DIR;
}

// 64-bit FNV-1a.  A name only has to make collisions rare; the sidecar is
// what makes reuse correct.
static uint64_t fnv1a64(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}

bool image_scratch_path(const char *tag, const char *identity, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s-%016llx.img", image_scratch_dir(), tag, (unsigned long long)fnv1a64(identity));
    return n > 0 && (size_t)n < cap;
}

static bool sidecar_path(const char *scratch, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s.id", scratch);
    return n > 0 && (size_t)n < cap;
}

bool image_scratch_valid(const char *scratch, const char *identity, uint64_t size) {
    struct stat st;
    if (stat(scratch, &st) != 0 || (size && (uint64_t)st.st_size != size))
        return false;
    char side[PATH_MAX];
    if (!sidecar_path(scratch, side, sizeof(side)))
        return false;
    FILE *f = fopen(side, "rb");
    if (!f)
        return false;
    size_t want = strlen(identity);
    char *buf = malloc(want + 1);
    bool ok = false;
    if (buf) {
        // One byte more than the identity: a longer sidecar must not match.
        size_t got = fread(buf, 1, want + 1, f);
        ok = got == want && memcmp(buf, identity, want) == 0;
    }
    free(buf);
    fclose(f);
    return ok;
}

int image_scratch_prepare(const char *scratch) {
    int rc = gs_mkdir_parents(scratch);
    if (rc != 0)
        return rc;
    char side[PATH_MAX];
    if (!sidecar_path(scratch, side, sizeof(side)))
        return -ENAMETOOLONG;
    if (remove(side) != 0 && errno != ENOENT)
        return -errno;
    return 0;
}

int image_scratch_seal(const char *scratch, const char *identity) {
    if (strlen(identity) > IDENTITY_MAX)
        return -EINVAL;
    char side[PATH_MAX];
    if (!sidecar_path(scratch, side, sizeof(side)))
        return -ENAMETOOLONG;
    FILE *f = fopen(side, "wb");
    if (!f)
        return -errno;
    size_t n = strlen(identity);
    bool ok = fwrite(identity, 1, n, f) == n;
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        remove(side);
        return -EIO;
    }
    return 0;
}
