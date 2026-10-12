// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ns_archive.c
// A peeler archive as a namespace: its member tree, listed from the
// archive's headers and directory alone; a member opens as a source (a
// view of the archive when stored, a decode-through fork when compressed),
// with its resource fork and Finder info when it has them.  A one-file
// wrapper (MacBinary, BinHex, gzip) is the same with one member.
// See namespace.h.

#include "namespace.h"

#include "storage_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    peel_archive_t *a;
    const char *format; // peeler's name for it (static)
} arc_ns_t;

// `path` without leading or trailing slashes, into `buf`.
static const char *trim(const char *path, char *buf, size_t cap) {
    while (*path == '/')
        path++;
    snprintf(buf, cap, "%s", path);
    size_t n = strlen(buf);
    while (n > 0 && buf[n - 1] == '/')
        buf[--n] = '\0';
    return buf;
}

// Fill a dirent from entry `e`, named `name`.
static void entry_dirent(const peel_entry_t *e, const char *name, ns_dirent_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->is_dir = e->is_dir;
    if (e->is_dir)
        return;
    out->data_size = e->data_len;
    out->rsrc_size = e->rsrc_len;
    out->mtime = e->mtime;
    out->type = e->mac_type;
    out->creator = e->mac_creator;
    out->finder_flags = e->finder_flags;
    out->has_finder_info = e->mac_type || e->mac_creator || e->finder_flags;
    out->tier = e->data_tier;
}

// A folder with no entry of its own (a zip that lists only files).
static void dir_dirent(const char *name, ns_dirent_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->is_dir = true;
}

static int arc_stat(ns_t *ns, const char *path, ns_dirent_t *out) {
    arc_ns_t *an = ns->ctx;
    char p[512];
    trim(path, p, sizeof(p));
    if (!p[0]) {
        dir_dirent("", out);
        return 0;
    }
    const char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    int i = peel_lookup(an->a, p);
    if (i >= 0) {
        entry_dirent(peel_entry(an->a, i), base, out);
        return 0;
    }
    // A folder only implied by the paths under it.
    size_t pl = strlen(p);
    for (int k = 0; k < peel_count(an->a); k++) {
        const char *ep = peel_entry(an->a, k)->path;
        if (strncmp(ep, p, pl) == 0 && ep[pl] == '/') {
            dir_dirent(base, out);
            return 0;
        }
    }
    return -ENOENT;
}

static int arc_list(ns_t *ns, const char *path, ns_dirent_t *out, int cap, int *count) {
    arc_ns_t *an = ns->ctx;
    char p[512];
    trim(path, p, sizeof(p));
    size_t pl = strlen(p);
    *count = 0;
    if (pl) {
        ns_dirent_t self;
        int rc = arc_stat(ns, p, &self);
        if (rc < 0)
            return rc;
        if (!self.is_dir)
            return -ENOTDIR;
    }
    // Each child once: an entry directly inside, or the first component of
    // a deeper one (a folder the archive does not list).
    for (int k = 0; k < peel_count(an->a); k++) {
        const peel_entry_t *e = peel_entry(an->a, k);
        const char *ep = e->path;
        if (pl) {
            if (strncmp(ep, p, pl) != 0 || ep[pl] != '/')
                continue;
            ep += pl + 1;
        }
        const char *slash = strchr(ep, '/');
        char name[256];
        snprintf(name, sizeof(name), "%.*s", (int)(slash ? (size_t)(slash - ep) : strlen(ep)), ep);
        if (!name[0])
            continue;
        // Seen already?
        bool seen = false;
        for (int j = 0; j < *count && j < cap && !seen; j++)
            seen = strcmp(out[j].name, name) == 0;
        if (seen)
            continue;
        if (*count < cap) {
            if (slash)
                dir_dirent(name, &out[*count]);
            else
                entry_dirent(e, name, &out[*count]);
        }
        (*count)++;
    }
    return 0;
}

static source_t *arc_open(ns_t *ns, const char *path, source_fork_t fork, int *err) {
    arc_ns_t *an = ns->ctx;
    char p[512];
    trim(path, p, sizeof(p));
    int i = peel_lookup(an->a, p);
    const peel_entry_t *e = peel_entry(an->a, i);
    if (!e || e->is_dir) {
        *err = e ? -EISDIR : -ENOENT;
        return NULL;
    }
    if (fork == GS_FORK_FINFO) {
        if (!e->mac_type && !e->mac_creator && !e->finder_flags) {
            *err = -ENOENT;
            return NULL;
        }
        uint8_t *fi = malloc(GS_FINDER_INFO_SIZE);
        char *key = str_printf("%s/%s/finf", source_key(ns->src), p);
        if (!fi || !key) {
            free(fi);
            free(key);
            *err = -ENOMEM;
            return NULL;
        }
        ns_finder_info(e->mac_type, e->mac_creator, e->finder_flags, fi);
        source_t *s = source_memory(fi, GS_FINDER_INFO_SIZE, true, key);
        free(key);
        *err = s ? 0 : -ENOMEM;
        return s;
    }
    if (fork == GS_FORK_RSRC && e->rsrc_len == 0) {
        *err = -ENOENT;
        return NULL;
    }
    peel_err_t *pe = NULL;
    source_t *raw = peel_open_fork(an->a, i, fork == GS_FORK_RSRC ? PEEL_FORK_RSRC : PEEL_FORK_DATA, &pe);
    if (!raw) {
        peel_err_free(pe);
        *err = -EIO;
        return NULL;
    }
    // Peeler's sources are not thread-safe; this one may be a disk the guest
    // and the I/O worker read at once.
    source_t *s = source_locked(raw);
    source_release(raw);
    *err = s ? 0 : -ENOMEM;
    return s;
}

static void arc_close(ns_t *ns) {
    arc_ns_t *an = ns->ctx;
    if (!an)
        return;
    peel_close(an->a);
    free(an);
}

static const ns_ops_t arc_ops = {"archive", arc_list, arc_stat, arc_open, arc_close};

ns_t *ns_open_archive(source_t *src, const char *format) {
    peel_err_t *err = NULL;
    peel_archive_t *a = peel_open_as(format, src, source_scratch_sink(), NULL, &err);
    if (!a) {
        peel_err_free(err);
        return NULL;
    }
    arc_ns_t *an = calloc(1, sizeof(*an));
    if (!an) {
        peel_close(a);
        return NULL;
    }
    an->a = a;
    an->format = peel_format(a);
    ns_t *ns = ns_new(&arc_ops, an, src);
    if (!ns)
        arc_close(&(ns_t){.ctx = an});
    return ns;
}

// The archive's peeler format name, or NULL when `ns` is no archive.
const char *ns_archive_format(ns_t *ns) {
    return (ns && ns->ops == &arc_ops) ? ((arc_ns_t *)ns->ctx)->format : NULL;
}
