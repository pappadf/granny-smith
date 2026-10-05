// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// namespace.c
// Namespace lifecycle, helpers, and registration of the namespace formats
// with the format registry.  See namespace.h.

#include "namespace.h"

#include "format_registry.h"
#include "image_apm.h"
#include "image_hfs.h"
#include "image_iso9660.h"
#include "image_mfs.h"
#include "image_ufs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Lifecycle
// ============================================================================

gs_namespace_t *gs_namespace_new(const gs_namespace_ops_t *ops, void *ctx, gs_source_t *src) {
    gs_namespace_t *ns = calloc(1, sizeof(*ns));
    if (!ns)
        return NULL;
    ns->ops = ops;
    ns->ctx = ctx;
    ns->src = gs_source_retain(src);
    ns->refs = 1;
    return ns;
}

gs_namespace_t *gs_namespace_retain(gs_namespace_t *ns) {
    if (ns)
        ns->refs++;
    return ns;
}

void gs_namespace_release(gs_namespace_t *ns) {
    if (!ns || --ns->refs > 0)
        return;
    if (ns->ops && ns->ops->close)
        ns->ops->close(ns);
    gs_source_release(ns->src);
    free(ns);
}

void gs_namespace_close(gs_namespace_t *ns) {
    gs_namespace_release(ns);
}

// ============================================================================
// Wrappers
// ============================================================================

int gs_ns_list(gs_namespace_t *ns, const char *path, gs_dirent_t **out, int *count) {
    *out = NULL;
    *count = 0;
    int cap = 64;
    for (;;) {
        gs_dirent_t *buf = malloc((size_t)cap * sizeof(*buf));
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

int gs_ns_stat(gs_namespace_t *ns, const char *path, gs_dirent_t *out) {
    memset(out, 0, sizeof(*out));
    return ns->ops->stat(ns, path ? path : "", out);
}

gs_source_t *gs_ns_open(gs_namespace_t *ns, const char *path, gs_fork_t fork, int *err) {
    int e = 0;
    if (!err)
        err = &e;
    *err = 0;
    return ns->ops->open(ns, path ? path : "", fork, err);
}

// ============================================================================
// Helpers
// ============================================================================

int gs_ns_split(const char *path, char *buf, size_t buf_cap, const char **comps, int max) {
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

void gs_ns_finder_info(uint32_t type, uint32_t creator, uint16_t flags, uint8_t out[GS_FINDER_INFO_SIZE]) {
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
// 32 KiB is inside it).
static bool disk_detect(const gs_probe_t *p) {
    const uint8_t *h = p->p.head;
    size_t n = p->p.head_len;
    if (n >= 1024 && image_apm_probe_magic(h + 512))
        return true;
    if (n >= 1024 + 2) {
        uint16_t sig = (uint16_t)(h[1024] << 8 | h[1025]);
        if (sig == HFS_SIG_BD || sig == HFS_SIG_HP || sig == HFS_SIG_HX || sig == MFS_SIG)
            return true;
    }
    // ISO 9660: a primary volume descriptor at sector 16 (of 2048 bytes).
    if (n >= ISO9660_VD_OFF + 7) {
        const uint8_t *vd = h + ISO9660_VD_OFF;
        if (vd[0] == 1 && memcmp(vd + 1, ISO9660_ID, 5) == 0 && vd[6] == 1)
            return true;
    }
    if (n >= UFS_SBOFF + 1372 + 4) {
        const uint8_t *m = h + UFS_SBOFF + 1372;
        uint32_t be = (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3];
        uint32_t le = (uint32_t)m[3] << 24 | (uint32_t)m[2] << 16 | (uint32_t)m[1] << 8 | m[0];
        if (be == UFS_FS_MAGIC || le == UFS_FS_MAGIC)
            return true;
    }
    return false;
}

static struct gs_namespace *disk_open(gs_source_t *data, gs_source_t *rsrc) {
    (void)rsrc;
    return gs_ns_open_disk(data);
}

// One registry row per peeler archive format.
#define ARCHIVE_ROW(fmt, what)                                                                                         \
    static bool fmt##_detect(const gs_probe_t *p) {                                                                    \
        int n = 0;                                                                                                     \
        const peel_format_desc_t *d = peel_formats(&n);                                                                \
        for (int i = 0; i < n; i++)                                                                                    \
            if (strcmp(d[i].name, #fmt) == 0)                                                                          \
                return d[i].detect(&p->p);                                                                             \
        return false;                                                                                                  \
    }                                                                                                                  \
    static struct gs_namespace *fmt##_open(gs_source_t *data, gs_source_t *rsrc) {                                     \
        (void)rsrc;                                                                                                    \
        return gs_ns_open_archive(data, #fmt);                                                                         \
    }                                                                                                                  \
    static const gs_format_t fmt##_format = {#fmt, GS_FMT_NAMESPACE, what, fmt##_detect, NULL, fmt##_open};

ARCHIVE_ROW(sit, "StuffIt archive")
ARCHIVE_ROW(cpt, "Compact Pro archive")
ARCHIVE_ROW(zip, "Zip archive")
ARCHIVE_ROW(tar, "tar archive")

static const gs_format_t disk_format = {
    "disk", GS_FMT_NAMESPACE, "disk image (partition map or bare volume)", disk_detect, NULL, disk_open};

void gs_ns_register_formats(void) {
    gs_format_register(&disk_format);
    gs_format_register(&sit_format);
    gs_format_register(&cpt_format);
    gs_format_register(&zip_format);
    gs_format_register(&tar_format);
    gs_format_set_wrapper_namespace(gs_ns_open_archive);
}
