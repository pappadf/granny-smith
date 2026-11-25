// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// namespace.c
// Namespace lifecycle, helpers, and registration of the namespace formats
// with the format registry.  See namespace.h.

#include "namespace.h"

#include "format_registry.h"
#include "image_volume.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Lifecycle
// ============================================================================

ns_t *ns_new(const ns_ops_t *ops, void *ctx, source_t *src) {
    ns_t *ns = calloc(1, sizeof(*ns));
    if (!ns)
        return NULL;
    ns->ops = ops;
    ns->ctx = ctx;
    ns->src = source_retain(src);
    ns->refs = 1;
    return ns;
}

ns_t *ns_retain(ns_t *ns) {
    if (ns)
        ns->refs++;
    return ns;
}

void ns_release(ns_t *ns) {
    if (!ns || --ns->refs > 0)
        return;
    if (ns->ops && ns->ops->close)
        ns->ops->close(ns);
    source_release(ns->src);
    free(ns);
}

void ns_close(ns_t *ns) {
    ns_release(ns);
}

// ============================================================================
// Wrappers
// ============================================================================

int ns_list(ns_t *ns, const char *path, ns_dirent_t **out, int *count) {
    *out = NULL;
    *count = 0;
    int cap = 64;
    for (;;) {
        ns_dirent_t *buf = malloc((size_t)cap * sizeof(*buf));
        if (!buf)
            return -ENOMEM;
        int n = 0;
        int rc = ns->ops->list(ns, path ? path : "", buf, cap, &n);
        if (rc < 0) {
            free(buf);
            return rc;
        }
        if (n <= cap) {
            *out = buf;
            *count = n;
            return 0;
        }
        free(buf);
        cap = n; // the whole listing did not fit: again, at its size
    }
}

int ns_stat(ns_t *ns, const char *path, ns_dirent_t *out) {
    memset(out, 0, sizeof(*out));
    return ns->ops->stat(ns, path ? path : "", out);
}

source_t *ns_open(ns_t *ns, const char *path, source_fork_t fork, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    *err = 0;
    return ns->ops->open(ns, path ? path : "", fork, err);
}

// ============================================================================
// Helpers
// ============================================================================

int ns_split(const char *path, char *buf, size_t buf_cap, const char **comps, int max) {
    size_t n = strlen(path);
    if (n >= buf_cap)
        return -ENAMETOOLONG;
    memcpy(buf, path, n + 1);
    int count = 0;
    char *p = buf;
    while (*p) {
        while (*p == '/')
            *p++ = '\0';
        if (!*p)
            break;
        if (count == max)
            return -ENAMETOOLONG;
        comps[count++] = p;
        while (*p && *p != '/')
            p++;
    }
    return count;
}

void ns_finder_info(uint32_t type, uint32_t creator, uint16_t flags, uint8_t out[GS_FINDER_INFO_SIZE]) {
    memset(out, 0, GS_FINDER_INFO_SIZE);
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)(type >> (24 - 8 * i));
        out[4 + i] = (uint8_t)(creator >> (24 - 8 * i));
    }
    out[8] = (uint8_t)(flags >> 8);
    out[9] = (uint8_t)flags;
}

// ============================================================================
// Registration
// ============================================================================

// A disk: an Apple Partition Map at block 1, or a bare HFS/HFS+, MFS, UFS
// or ISO 9660 volume.  From the probe's head alone (the ISO descriptor at
// 32 KiB is inside it), by the classifier ns_open_disk opens with, so a disk
// claimed here is one it accepts.
static bool disk_detect(const format_probe_t *p) {
    return volume_sniff(p->p.head, p->p.head_len, p->p.size).kind != VOL_NONE;
}

static struct ns *disk_open(source_t *data, source_t *rsrc) {
    (void)rsrc;
    return ns_open_disk(data);
}

// One registry row per peeler archive format.
#define ARCHIVE_ROW(fmt, what)                                                                                         \
    static bool fmt##_detect(const format_probe_t *p) {                                                                \
        int n = 0;                                                                                                     \
        const peel_format_desc_t *d = peel_formats(&n);                                                                \
        for (int i = 0; i < n; i++)                                                                                    \
            if (strcmp(d[i].name, #fmt) == 0)                                                                          \
                return d[i].detect(&p->p);                                                                             \
        return false;                                                                                                  \
    }                                                                                                                  \
    static struct ns *fmt##_open(source_t *data, source_t *rsrc) {                                                     \
        (void)rsrc;                                                                                                    \
        return ns_open_archive(data, #fmt);                                                                            \
    }                                                                                                                  \
    static const format_t fmt##_format = {#fmt, GS_FMT_NAMESPACE, what, fmt##_detect, NULL, fmt##_open};

ARCHIVE_ROW(sit, "StuffIt archive")
ARCHIVE_ROW(cpt, "Compact Pro archive")
ARCHIVE_ROW(zip, "Zip archive")
ARCHIVE_ROW(tar, "tar archive")

static const format_t disk_format = {"disk", GS_FMT_NAMESPACE, "disk image (partition map or bare volume)", disk_detect,
                                     NULL,   disk_open};

void ns_register_formats(void) {
    format_register(&disk_format);
    format_register(&sit_format);
    format_register(&cpt_format);
    format_register(&zip_format);
    format_register(&tar_format);
    format_set_wrapper_namespace(ns_open_archive);
}
