// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ns_archive.c
// A peeler archive as a namespace: its member tree, listed from the
// archive's headers and directory alone; a member opens as a source (a
// view of the archive when stored, a decode-through fork when compressed),
// with its resource fork and Finder info when it has them.  A one-file
// wrapper (MacBinary, BinHex, gzip) is the same with one member.
// Peeler hands out names as the archive stores them; this is where a Mac
// format's MacRoman names become the VFS's UTF-8 ones.
// See namespace.h.

#include "namespace.h"

#include "macroman.h"
#include "storage_util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A member's VFS path: peeler's entry path (up to 512 bytes) with each
// MacRoman byte now up to three UTF-8 ones.
#define ARC_PATH_MAX (3 * 512)

typedef struct {
    peel_archive_t *a;
    const char *format; // peeler's name for it (static)
    bool mac; // member names are MacRoman: shown as UTF-8, matched as the Finder would
    char **paths; // each entry's VFS path (ns_archive_member_path)
} arc_ns_t;

bool ns_archive_names_are_macroman(const char *format) {
    // The Mac formats store names as the Finder does; zip names are UTF-8
    // (or CP437), tar's and gzip's whatever bytes the host had.
    static const char *const mac_formats[] = {"sit", "cpt", "hqx", "bin"};
    for (size_t i = 0; format && i < sizeof(mac_formats) / sizeof(mac_formats[0]); i++)
        if (strcmp(format, mac_formats[i]) == 0)
            return true;
    return false;
}

void ns_archive_member_path(const char *format, const char *raw, char *out, size_t cap) {
    if (!cap)
        return;
    if (!ns_archive_names_are_macroman(format)) {
        snprintf(out, cap, "%s", raw);
        return;
    }
    // One component at a time: peeler has already spelled a '/' inside a
    // name ':', so every '/' left is a separator.
    size_t o = 0;
    out[0] = '\0';
    for (const char *c = raw;;) {
        const char *slash = strchr(c, '/');
        size_t n = slash ? (size_t)(slash - c) : strlen(c);
        if (o && o + 1 < cap)
            out[o++] = '/';
        mac_name_to_vfs((const uint8_t *)c, n, out + o, cap - o);
        o += strlen(out + o);
        if (!slash || o + 1 >= cap)
            return;
        c = slash + 1;
    }
}

// Whether component `a` (`al` bytes) names component `b` (`bl` bytes).
static bool comp_eq(const char *a, size_t al, const char *b, size_t bl, bool mac) {
    if (!mac)
        return al == bl && memcmp(a, b, al) == 0;
    char x[ARC_PATH_MAX], y[ARC_PATH_MAX];
    if (al >= sizeof(x) || bl >= sizeof(y))
        return false;
    memcpy(x, a, al);
    x[al] = '\0';
    memcpy(y, b, bl);
    y[bl] = '\0';
    return mac_name_eq(x, y); // per component, so a ':' in a name is no separator
}

// `path` past the leading components that are `prefix` (non-empty): "" when
// `path` is `prefix`, "/rest" when it lies under it, NULL otherwise.
static const char *path_after(const char *path, const char *prefix, bool mac) {
    for (;;) {
        const char *ps = strchr(prefix, '/'), *es = strchr(path, '/');
        size_t pn = ps ? (size_t)(ps - prefix) : strlen(prefix);
        size_t en = es ? (size_t)(es - path) : strlen(path);
        if (!comp_eq(path, en, prefix, pn, mac))
            return NULL;
        if (!ps)
            return path + en; // "" or "/rest"
        if (!es)
            return NULL; // `prefix` goes deeper than `path`
        prefix = ps + 1;
        path = es + 1;
    }
}

bool ns_archive_member_eq(const char *format, const char *vpath, const char *want) {
    if (!want[0])
        return !vpath[0];
    const char *rest = path_after(vpath, want, ns_archive_names_are_macroman(format));
    return rest && !*rest;
}

// The entry whose VFS path is `p` (trimmed, non-empty), or -1.
static int arc_lookup(const arc_ns_t *an, const char *p) {
    for (int k = 0; k < peel_count(an->a); k++) {
        const char *rest = path_after(an->paths[k], p, an->mac);
        if (rest && !*rest)
            return k;
    }
    return -1;
}

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

// The last component of VFS path `p`.
static const char *leaf(const char *p) {
    const char *base = strrchr(p, '/');
    return base ? base + 1 : p;
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

// A folder with no entry of its own (a zip that lists only files), named
// by the `len` bytes at `name`.
static void dir_dirent(const char *name, size_t len, ns_dirent_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%.*s", (int)len, name);
    out->is_dir = true;
}

static int arc_stat(ns_t *ns, const char *path, ns_dirent_t *out) {
    arc_ns_t *an = ns->ctx;
    char p[ARC_PATH_MAX];
    trim(path, p, sizeof(p));
    if (!p[0]) {
        dir_dirent("", 0, out);
        return 0;
    }
    // Named as the archive names it, whatever case the path asked in.
    int i = arc_lookup(an, p);
    if (i >= 0) {
        entry_dirent(peel_entry(an->a, i), leaf(an->paths[i]), out);
        return 0;
    }
    // A folder only implied by the paths under it.
    size_t depth = 0; // components of `p` before its last
    for (const char *c = p; (c = strchr(c, '/')) != NULL; c++)
        depth++;
    for (int k = 0; k < peel_count(an->a); k++) {
        const char *ep = an->paths[k];
        const char *rest = path_after(ep, p, an->mac);
        if (rest && *rest == '/') {
            // Its name is the matching component of the entry's path.
            const char *c = ep;
            for (size_t d = 0; d < depth; d++)
                c = strchr(c, '/') + 1;
            dir_dirent(c, (size_t)(rest - c), out);
            return 0;
        }
    }
    return -ENOENT;
}

static int arc_list(ns_t *ns, const char *path, ns_dirent_t *out, int cap, int *count) {
    arc_ns_t *an = ns->ctx;
    char p[ARC_PATH_MAX];
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
        const char *ep = an->paths[k];
        if (pl) {
            const char *rest = path_after(ep, p, an->mac);
            if (!rest || *rest != '/')
                continue;
            ep = rest + 1;
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
                dir_dirent(name, strlen(name), &out[*count]);
            else
                entry_dirent(e, name, &out[*count]);
        }
        (*count)++;
    }
    return 0;
}

static source_t *arc_open(ns_t *ns, const char *path, source_fork_t fork, int *err) {
    arc_ns_t *an = ns->ctx;
    char p[ARC_PATH_MAX];
    trim(path, p, sizeof(p));
    int i = p[0] ? arc_lookup(an, p) : -1;
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
        // Keyed by the entry, as peeler keys its forks (peel_fork_key).
        char *key = str_printf("%s/%s/finf", source_key(ns->src), e->path);
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
    for (int k = 0; an->paths && k < peel_count(an->a); k++)
        free(an->paths[k]);
    free(an->paths);
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
    an->mac = ns_archive_names_are_macroman(an->format);
    // Every member's VFS path, once: listings and lookups use only these.
    an->paths = calloc((size_t)peel_count(a) + 1, sizeof(*an->paths));
    bool ok = an->paths != NULL;
    for (int k = 0; ok && k < peel_count(a); k++) {
        char vp[ARC_PATH_MAX];
        ns_archive_member_path(an->format, peel_entry(a, k)->path, vp, sizeof(vp));
        ok = (an->paths[k] = gs_strdup(vp)) != NULL;
    }
    if (!ok) {
        arc_close(&(ns_t){.ctx = an});
        return NULL;
    }
    ns_t *ns = ns_new(&arc_ops, an, src);
    if (!ns)
        arc_close(&(ns_t){.ctx = an});
    return ns;
}

// The archive's peeler format name, or NULL when `ns` is no archive.
const char *ns_archive_format(ns_t *ns) {
    return (ns && ns->ops == &arc_ops) ? ((arc_ns_t *)ns->ctx)->format : NULL;
}
