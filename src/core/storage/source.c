// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// source.c
// Host-file, view and memory byte sources, and path opening.  See source.h.

#include "source.h"

#include "appledouble.h"
#include "resource_fork.h"
#include "storage_util.h"

#include <ctype.h>
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
// Keys
// ============================================================================

bool source_key_within(const char *key, const char *parent) {
    if (!key || !parent || !*parent)
        return false;
    size_t n = strlen(parent);
    return strncmp(key, parent, n) == 0 && (key[n] == '\0' || key[n] == '/' || key[n] == '#');
}

// ============================================================================
// Host file
// ============================================================================

typedef struct {
    int fd;
    uint64_t size;
    char *key; // "<canonical path>@<size>:<mtime>"
} host_src_t;

static int64_t host_read(source_t *s, uint64_t off, void *buf, size_t len) {
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

static uint64_t host_size(source_t *s) {
    return ((host_src_t *)s->ctx)->size;
}

static const char *host_key(source_t *s) {
    return ((host_src_t *)s->ctx)->key;
}

static source_tier_t host_tier(source_t *s) {
    (void)s;
    return GS_TIER_RANDOM;
}

static void host_close(source_t *s) {
    host_src_t *h = s->ctx;
    if (!h)
        return;
    if (h->fd >= 0)
        close(h->fd);
    free(h->key);
    free(h);
}

static const source_ops_t host_ops = {host_read, host_size, host_key, host_tier, host_close};

source_t *source_host(const char *path, int *err) {
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
    char *key = str_printf("%s@%" PRIu64 ":%lld", canon ? canon : path, (uint64_t)st.st_size, (long long)st.st_mtime);
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
    source_t *s = peel_source_new(&host_ops, h, NULL);
    if (!s)
        *err = -ENOMEM;
    return s;
}

// ============================================================================
// View and memory
// ============================================================================

source_t *source_view(source_t *parent, uint64_t off, uint64_t len, const char *key) {
    return peel_source_view_keyed(parent, off, len, key);
}

source_t *source_memory(const void *buf, size_t len, bool own, const char *key) {
    return peel_source_memory_keyed(buf, len, own, key);
}

// A parent lengthened with a zero tail carrying one patch (source_pad).
typedef struct {
    uint64_t size, psize, patch_off;
    uint8_t *patch;
    size_t patch_len;
    char *key;
} pad_src_t;

static int64_t pad_read(source_t *s, uint64_t off, void *buf, size_t len) {
    pad_src_t *p = s->ctx;
    if (off >= p->size)
        return 0;
    if (len > p->size - off)
        len = (size_t)(p->size - off);
    if (off < p->psize) // the parent's own bytes (a short read is the caller's to resume)
        return peel_source_read(s->parent, off, buf, len < p->psize - off ? len : (size_t)(p->psize - off));
    memset(buf, 0, len);
    uint64_t lo = off > p->patch_off ? off : p->patch_off;
    uint64_t hi = off + len < p->patch_off + p->patch_len ? off + len : p->patch_off + p->patch_len;
    if (lo < hi)
        memcpy((uint8_t *)buf + (lo - off), p->patch + (lo - p->patch_off), (size_t)(hi - lo));
    return (int64_t)len;
}

static uint64_t pad_size(source_t *s) {
    return ((pad_src_t *)s->ctx)->size;
}

static const char *pad_key(source_t *s) {
    return ((pad_src_t *)s->ctx)->key;
}

static source_tier_t pad_tier(source_t *s) {
    return peel_source_tier(s->parent);
}

static void pad_close(source_t *s) {
    pad_src_t *p = s->ctx;
    if (p) {
        free(p->patch);
        free(p->key);
    }
    free(p);
}

static const source_ops_t pad_ops = {pad_read, pad_size, pad_key, pad_tier, pad_close, NULL};

source_t *source_pad(source_t *parent, uint64_t size, uint64_t patch_off, const void *patch, size_t patch_len) {
    if (!parent)
        return NULL;
    uint64_t psize = peel_source_size(parent);
    if (size <= psize ||
        (patch_len && (!patch || patch_off < psize || patch_off > size || patch_len > size - patch_off)))
        return NULL;
    pad_src_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->size = size;
    p->psize = psize;
    p->patch_off = patch_off;
    p->patch_len = patch_len;
    const char *pk = peel_source_key(parent);
    p->key = str_printf("%s#pad%" PRIu64, pk ? pk : "", size);
    p->patch = patch_len ? malloc(patch_len) : NULL;
    if (!p->key || (patch_len && !p->patch)) {
        pad_close(&(source_t){.ctx = p});
        return NULL;
    }
    if (patch_len)
        memcpy(p->patch, patch, patch_len);
    return peel_source_new(&pad_ops, p, parent);
}

// ============================================================================
// Reads
// ============================================================================

// The next character of `k` at *i, skipping a host time stamp (":<digits>"
// right after "@<digits>" and before the end, '/' or '#').  NUL at the end.
static char key_next(const char *k, size_t *i, bool skip_times) {
    if (skip_times && k[*i] == ':' && *i > 0 && isdigit((unsigned char)k[*i - 1])) {
        // Back over "<digits>" to the '@' that must start the size.
        size_t j = *i;
        while (j > 0 && isdigit((unsigned char)k[j - 1]))
            j--;
        size_t e = *i + 1;
        while (isdigit((unsigned char)k[e]))
            e++;
        if (j > 0 && k[j - 1] == '@' && e > *i + 1 && (k[e] == '\0' || k[e] == '/' || k[e] == '#'))
            *i = e;
    }
    return k[*i];
}

bool source_key_same(const char *a, const char *b, bool ignore_host_times) {
    if (!a || !b)
        return false;
    if (!ignore_host_times)
        return strcmp(a, b) == 0;
    size_t i = 0, j = 0;
    for (;;) {
        char ca = key_next(a, &i, true), cb = key_next(b, &j, true);
        if (ca != cb)
            return false;
        if (!ca)
            return true;
        i++;
        j++;
    }
}

bool source_key_same_source(const char *saved, const char *now) {
#ifdef __EMSCRIPTEN__
    return source_key_same(saved, now, true);
#else
    return source_key_same(saved, now, false);
#endif
}

int source_read_exact(source_t *s, uint64_t off, void *buf, size_t len) {
    int rc = peel_source_read_exact(s, off, buf, len);
    return rc == -5 ? -EIO : rc; // peeler's "short" is EIO's value; keep errno spelling
}

int source_read_all(source_t *s, size_t max, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    uint64_t size = source_size(s);
    if (size > max)
        return -EFBIG;
    if (size == 0)
        return 0;
    uint8_t *buf = malloc((size_t)size);
    if (!buf)
        return -ENOMEM;
    int rc = source_read_exact(s, 0, buf, (size_t)size);
    if (rc != 0) {
        free(buf);
        return rc;
    }
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

int source_read_path(const char *path, size_t max, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    int err = 0;
    source_t *s = source_open_path(path, GS_FORK_DATA, &err);
    if (!s)
        return err ? err : -ENOENT;
    int rc = source_read_all(s, max, out, out_len);
    source_release(s);
    return rc;
}

const char *source_tier_name(source_tier_t t) {
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

static source_path_opener_t g_opener;

void source_set_path_opener(source_path_opener_t opener) {
    g_opener = opener;
}

source_t *source_open_path(const char *path, source_fork_t fork, int *err) {
    return (g_opener ? g_opener : source_open_host_path)(path, fork, err);
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
static source_t *sidecar_fork(const char *path, source_fork_t fork) {
    static const char *const prefixes[] = {"._", "%"};
    char side[PATH_MAX];
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        sidecar_path(path, prefixes[i], side, sizeof(side));
        uint8_t *raw = NULL;
        size_t raw_len = 0;
        if (read_file(side, RFORK_MAX_FORK_LEN + 65536, &raw, &raw_len) != 0)
            continue;
        ad_file_t ad;
        source_t *s = NULL;
        if (ad_detect(raw, raw_len) && ad_parse(raw, raw_len, &ad) == 0) {
            const uint8_t *p = fork == GS_FORK_RSRC ? ad.rsrc : ad.finder;
            size_t n = fork == GS_FORK_RSRC ? ad.rsrc_len : ad.finder_len;
            if (p && n) {
                // Finder info is always 32 bytes; a shorter entry is padded.
                size_t keep = fork == GS_FORK_FINFO ? GS_FINDER_INFO_SIZE : n;
                uint8_t *copy = calloc(1, keep);
                if (copy) {
                    memcpy(copy, p, n < keep ? n : keep);
                    char *key = str_printf("%s/%s", side, fork == GS_FORK_RSRC ? "rsrc" : "finf");
                    s = source_memory(copy, keep, true, key);
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

source_t *source_open_host_path(const char *path, source_fork_t fork, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    *err = 0;
    if (fork == GS_FORK_DATA)
        return source_host(path, err);
    source_t *s = sidecar_fork(path, fork);
    if (s)
        return s;
    if (fork == GS_FORK_RSRC) {
        // A raw sibling "NAME.rsrc".
        char raw[PATH_MAX];
        snprintf(raw, sizeof(raw), "%s.rsrc", path);
        struct stat st;
        if (stat(raw, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
            return source_host(raw, err);
    }
    *err = -ENOENT;
    return NULL;
}
