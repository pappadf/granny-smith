// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// source.c
// Host-file, view and memory byte sources, and path opening.  See source.h.

#include "source.h"

#include "appledouble.h"
#include "resource_fork.h"
#include "storage_util.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// Host file
// ============================================================================

typedef struct {
    int fd;
    uint64_t size;
    char *key; // "<canonical path>@<size>:<mtime>"
} host_src_t;

static int64_t host_read(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    host_src_t *h = s->ctx;
    if (off >= h->size)
        return 0;
    if (len > h->size - off)
        len = (size_t)(h->size - off);
    for (;;) {
        ssize_t n = pread(h->fd, buf, len, (off_t)off);
        if (n >= 0)
            return (int64_t)n;
        if (errno != EINTR)
            return -errno;
    }
}

static uint64_t host_size(gs_source_t *s) {
    return ((host_src_t *)s->ctx)->size;
}

static const char *host_key(gs_source_t *s) {
    return ((host_src_t *)s->ctx)->key;
}

static gs_tier_t host_tier(gs_source_t *s) {
    (void)s;
    return GS_TIER_RANDOM;
}

static void host_close(gs_source_t *s) {
    host_src_t *h = s->ctx;
    if (!h)
        return;
    if (h->fd >= 0)
        close(h->fd);
    free(h->key);
    free(h);
}

static const gs_source_ops_t host_ops = {host_read, host_size, host_key, host_tier, host_close};

gs_source_t *gs_source_host(const char *path, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    *err = 0;
    if (!path || !*path) {
        *err = -EINVAL;
        return NULL;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        *err = -errno;
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        *err = -errno;
        close(fd);
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        *err = S_ISDIR(st.st_mode) ? -EISDIR : -EINVAL;
        close(fd);
        return NULL;
    }
    host_src_t *h = calloc(1, sizeof(*h));
    // The key names the file's content: the canonical path (one spelling for
    // every relative, absolute or symlinked route to it) plus size and mtime.
    char *canon = realpath(path, NULL);
    char *key =
        gs_str_printf("%s@%" PRIu64 ":%lld", canon ? canon : path, (uint64_t)st.st_size, (long long)st.st_mtime);
    free(canon);
    if (!h || !key) {
        free(h);
        free(key);
        close(fd);
        *err = -ENOMEM;
        return NULL;
    }
    h->fd = fd;
    h->size = (uint64_t)st.st_size;
    h->key = key;
    gs_source_t *s = peel_source_new(&host_ops, h, NULL);
    if (!s)
        *err = -ENOMEM;
    return s;
}

// ============================================================================
// View and memory
// ============================================================================

gs_source_t *gs_source_view(gs_source_t *parent, uint64_t off, uint64_t len, const char *key) {
    return peel_source_view_keyed(parent, off, len, key);
}

gs_source_t *gs_source_memory(const void *buf, size_t len, bool own, const char *key) {
    return peel_source_memory_keyed(buf, len, own, key);
}

// ============================================================================
// Reads
// ============================================================================

int gs_source_read_exact(gs_source_t *s, uint64_t off, void *buf, size_t len) {
    int rc = peel_source_read_exact(s, off, buf, len);
    return rc == -5 ? -EIO : rc; // peeler's "short" is EIO's value; keep errno spelling
}

int gs_source_read_all(gs_source_t *s, size_t max, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    uint64_t size = gs_source_size(s);
    if (size > max)
        return -EFBIG;
    if (size == 0)
        return 0;
    uint8_t *buf = malloc((size_t)size);
    if (!buf)
        return -ENOMEM;
    int rc = gs_source_read_exact(s, 0, buf, (size_t)size);
    if (rc != 0) {
        free(buf);
        return rc;
    }
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

const char *gs_tier_name(gs_tier_t t) {
    switch (t) {
    case GS_TIER_RANDOM:
        return "random";
    case GS_TIER_INDEXED:
        return "indexed";
    case GS_TIER_EARNED:
        return "earned";
    case GS_TIER_STREAM:
        return "stream";
    case GS_TIER_WHOLE:
        return "whole";
    }
    return "?";
}

// ============================================================================
// Opening a path
// ============================================================================

static gs_path_opener_t g_opener;

void gs_source_set_path_opener(gs_path_opener_t opener) {
    g_opener = opener;
}

gs_source_t *gs_source_open_path(const char *path, gs_fork_t fork, int *err) {
    return (g_opener ? g_opener : gs_source_open_host_path)(path, fork, err);
}

// "<dir>/<prefix><name>" for `path`.
static void sidecar_path(const char *path, const char *prefix, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    if (slash)
        snprintf(out, cap, "%.*s%s%s", (int)(slash - path + 1), path, prefix, slash + 1);
    else
        snprintf(out, cap, "%s%s", prefix, path);
}

// A fork out of the AppleDouble / AppleSingle companion of `path`: "._NAME",
// then the legacy "%NAME".  NULL when there is none (or it lacks the fork).
static gs_source_t *sidecar_fork(const char *path, gs_fork_t fork) {
    static const char *const prefixes[] = {"._", "%"};
    char side[PATH_MAX];
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        sidecar_path(path, prefixes[i], side, sizeof(side));
        uint8_t *raw = NULL;
        size_t raw_len = 0;
        if (gs_read_file(side, RFORK_MAX_FORK_LEN + 65536, &raw, &raw_len) != 0)
            continue;
        ad_file_t ad;
        gs_source_t *s = NULL;
        if (ad_detect(raw, raw_len) && ad_parse(raw, raw_len, &ad) == 0) {
            const uint8_t *p = fork == GS_FORK_RSRC ? ad.rsrc : ad.finder;
            size_t n = fork == GS_FORK_RSRC ? ad.rsrc_len : ad.finder_len;
            if (p && n) {
                // Finder info is always 32 bytes; a shorter entry is padded.
                size_t keep = fork == GS_FORK_FINFO ? GS_FINDER_INFO_SIZE : n;
                uint8_t *copy = calloc(1, keep);
                if (copy) {
                    memcpy(copy, p, n < keep ? n : keep);
                    char *key = gs_str_printf("%s/%s", side, fork == GS_FORK_RSRC ? "rsrc" : "finf");
                    s = gs_source_memory(copy, keep, true, key);
                    free(key);
                }
            }
        }
        free(raw);
        if (s)
            return s;
    }
    return NULL;
}

gs_source_t *gs_source_open_host_path(const char *path, gs_fork_t fork, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    *err = 0;
    if (fork == GS_FORK_DATA)
        return gs_source_host(path, err);
    gs_source_t *s = sidecar_fork(path, fork);
    if (s)
        return s;
    if (fork == GS_FORK_RSRC) {
        // A raw sibling "NAME.rsrc".
        char raw[PATH_MAX];
        snprintf(raw, sizeof(raw), "%s.rsrc", path);
        struct stat st;
        if (stat(raw, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
            return gs_source_host(raw, err);
    }
    *err = -ENOENT;
    return NULL;
}
