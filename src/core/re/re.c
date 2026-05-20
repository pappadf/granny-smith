// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// re.c
// Reverse-engineering orchestrator: identify + dump for forked Mac files.
// PR 2 scope is the skeleton — extract data fork, finder info, and every
// raw resource (with .info sidecar).  Disassembly, per-type decoding, and
// manifest consolidation land in subsequent PRs.
//
// The path argument flows through the VFS so host files, image-vfs HFS
// paths, and any future backend route uniformly.  Resource-fork parsing
// uses the C API in resource_fork.h directly (rfork_parse +
// rfork_lookup); we do NOT round-trip through `storage.cp` on the
// per-resource extract path — that would push every byte through the
// shell dispatcher and defeat the point.

#include "re.h"

#include "object.h"
#include "resource_fork.h"
#include "value.h"
#include "vfs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

// ============================================================================
// Small filesystem helpers (mkdir -p + path joining)
// ============================================================================

// mkdir -p: create `path` and any missing parents.  Returns 0 on success
// (or when the leaf already exists), -1 otherwise.
static int re_mkdir_p(const char *path) {
    if (!path || !*path)
        return -1;
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

// ============================================================================
// Reading a VFS file into a malloc'd buffer
// ============================================================================

uint8_t *re_read_vfs_file(const char *vfs_path, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!vfs_path)
        return NULL;
    vfs_stat_t st = {0};
    if (vfs_stat(vfs_path, &st) < 0)
        return NULL;
    if (!(st.mode & VFS_MODE_FILE))
        return NULL;
    size_t sz = (size_t)st.size;
    uint8_t *buf = malloc(sz > 0 ? sz : 1);
    if (!buf)
        return NULL;
    if (sz == 0) {
        if (out_len)
            *out_len = 0;
        return buf;
    }
    vfs_file_t *vf = NULL;
    const vfs_backend_t *be = NULL;
    if (vfs_open(vfs_path, &vf, &be) < 0) {
        free(buf);
        return NULL;
    }
    size_t got_total = 0;
    while (got_total < sz) {
        size_t got = 0;
        int rc = be->read(vf, got_total, buf + got_total, sz - got_total, &got);
        if (rc < 0 || got == 0) {
            if (rc < 0)
                got_total = 0; // signal failure below
            break;
        }
        got_total += got;
    }
    be->close(vf);
    if (got_total != sz) {
        free(buf);
        return NULL;
    }
    if (out_len)
        *out_len = sz;
    return buf;
}

// Same shape, but for the optional `<file>/<suffix>` companion paths
// (`/rsrc/_raw`, `/finf`).  Returns NULL silently when the suffix
// doesn't resolve — empty fork / non-HFS backing — so callers can
// treat "no resource fork" as a normal outcome rather than an error.
static uint8_t *read_suffix(const char *base_path, const char *suffix, size_t *out_len) {
    char full[PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s/%s", base_path, suffix);
    if (n < 0 || (size_t)n >= sizeof(full))
        return NULL;
    return re_read_vfs_file(full, out_len);
}

// ============================================================================
// finder.json formatter — turns the 32-byte finder info blob into a small
// machine-readable JSON document.  Layout:
//   +0  type 4-CC
//   +4  creator 4-CC
//   +8  flags (uint16 BE)
//   +10 location (point, uint32 BE)
//   +14 fldr (uint16 BE)
//   +16..31 FXInfo (icon id, script, comment id, home dir)
// ============================================================================

static int finder_json_write(const uint8_t *finf, size_t finf_len, FILE *fp) {
    if (finf_len != 32) {
        fprintf(fp, "{\"error\":\"finder info wrong size: %zu\"}\n", finf_len);
        return -1;
    }
    uint16_t flags = (uint16_t)((finf[8] << 8) | finf[9]);
    uint16_t fldr = (uint16_t)((finf[14] << 8) | finf[15]);
    // Type and creator are 4-byte OSTypes; render as raw ASCII when
    // printable, else as a 4-byte hex string.  No MacRoman transcoding
    // here — finder.json is meant to round-trip the *literal* bytes.
    char type_buf[16] = {0};
    char creator_buf[16] = {0};
    bool type_ascii = true, creator_ascii = true;
    for (int i = 0; i < 4; i++) {
        if (finf[i] < 0x20 || finf[i] > 0x7E)
            type_ascii = false;
        if (finf[4 + i] < 0x20 || finf[4 + i] > 0x7E)
            creator_ascii = false;
    }
    if (type_ascii)
        snprintf(type_buf, sizeof(type_buf), "\"%c%c%c%c\"", finf[0], finf[1], finf[2], finf[3]);
    else
        snprintf(type_buf, sizeof(type_buf), "\"0x%02x%02x%02x%02x\"", finf[0], finf[1], finf[2], finf[3]);
    if (creator_ascii)
        snprintf(creator_buf, sizeof(creator_buf), "\"%c%c%c%c\"", finf[4], finf[5], finf[6], finf[7]);
    else
        snprintf(creator_buf, sizeof(creator_buf), "\"0x%02x%02x%02x%02x\"", finf[4], finf[5], finf[6], finf[7]);
    fprintf(fp, "{\"type\":%s,\"creator\":%s,\"flags\":\"0x%04x\",\"fldr\":%u}\n", type_buf, creator_buf, flags, fldr);
    return 0;
}

// ============================================================================
// re_identify — one-line summary
// ============================================================================

bool re_identify(const char *vfs_path) {
    if (!vfs_path || !*vfs_path)
        return false;
    vfs_stat_t st = {0};
    if (vfs_stat(vfs_path, &st) < 0)
        return false;
    if (!(st.mode & VFS_MODE_FILE)) {
        printf("not-a-file\n");
        return false;
    }
    size_t data_len = (size_t)st.size;

    size_t rsrc_len = 0;
    uint8_t *rsrc_bytes = read_suffix(vfs_path, "rsrc/_raw", &rsrc_len);

    size_t finf_len = 0;
    uint8_t *finf_bytes = read_suffix(vfs_path, "finf", &finf_len);

    if (rsrc_bytes && rsrc_len > 0) {
        const char *errmsg = NULL;
        rfork_t *rf = rfork_parse(rsrc_bytes, rsrc_len, &errmsg);
        size_t n_types = rf ? rfork_num_types(rf) : 0;
        size_t n_res = 0;
        for (size_t t = 0; t < n_types; t++)
            n_res += rfork_num_resources(rf, rfork_type_at(rf, t));
        printf("mac-forked-file: data_fork=%zu rsrc_fork=%zu types=%zu resources=%zu\n", data_len, rsrc_len, n_types,
               n_res);
        rfork_free(rf);
    } else {
        printf("data-only: data_fork=%zu\n", data_len);
    }
    free(rsrc_bytes);
    free(finf_bytes);
    return true;
}

// ============================================================================
// re_dump — full extract of data fork, finder info, and every resource
// ============================================================================

// Write `len` bytes from `bytes` to `out_path` (host filesystem).  Returns
// 0 on success, -1 on failure with a message on stderr.
static int write_blob(const char *out_path, const void *bytes, size_t len) {
    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        fprintf(stderr, "re: cannot create '%s': %s\n", out_path, strerror(errno));
        return -1;
    }
    if (len > 0 && fwrite(bytes, 1, len, fp) != len) {
        fprintf(stderr, "re: write error on '%s': %s\n", out_path, strerror(errno));
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

// Dump every resource of every type into <dst>/resources/<TYPE>/<id> and
// <id>.info.  Returns the number of resources written, or -1 on any
// per-resource failure (the partial output is left on disk for debugging).
static int dump_resources(const rfork_t *rf, const char *dst_dir) {
    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/resources", dst_dir);
    if (n < 0 || (size_t)n >= sizeof(dir))
        return -1;
    if (re_mkdir_p(dir) != 0) {
        fprintf(stderr, "re: cannot create '%s': %s\n", dir, strerror(errno));
        return -1;
    }

    int total = 0;
    size_t n_types = rfork_num_types(rf);
    for (size_t t = 0; t < n_types; t++) {
        const uint8_t *type_cc = rfork_type_at(rf, t);
        char type_path[16];
        rfork_type_to_path(type_cc, type_path, sizeof(type_path));
        char type_dir[PATH_MAX];
        n = snprintf(type_dir, sizeof(type_dir), "%s/%s", dir, type_path);
        if (n < 0 || (size_t)n >= sizeof(type_dir))
            return -1;
        if (re_mkdir_p(type_dir) != 0) {
            fprintf(stderr, "re: cannot create '%s': %s\n", type_dir, strerror(errno));
            return -1;
        }
        size_t n_res = rfork_num_resources(rf, type_cc);
        for (size_t r = 0; r < n_res; r++) {
            int16_t id = rfork_id_at(rf, type_cc, r);
            const uint8_t *bytes = NULL;
            size_t sz = 0;
            const char *name = NULL;
            uint8_t attrs = 0;
            if (rfork_lookup(rf, type_cc, id, &bytes, &sz, &name, &attrs) < 0)
                continue;
            char id_str[16];
            rfork_id_to_path(id, id_str, sizeof(id_str));
            char res_path[PATH_MAX];
            n = snprintf(res_path, sizeof(res_path), "%s/%s", type_dir, id_str);
            if (n < 0 || (size_t)n >= sizeof(res_path))
                return -1;
            if (write_blob(res_path, bytes, sz) != 0)
                return -1;
            char info_path[PATH_MAX];
            n = snprintf(info_path, sizeof(info_path), "%s/%s.info", type_dir, id_str);
            if (n < 0 || (size_t)n >= sizeof(info_path))
                return -1;
            char info_buf[512];
            int w = rfork_info_format(name, attrs, sz, info_buf, sizeof(info_buf));
            if (w < 0)
                return -1;
            if (write_blob(info_path, info_buf, (size_t)w) != 0)
                return -1;
            total++;
        }
    }
    return total;
}

int re_dump(const char *vfs_path, const char *dst_dir) {
    if (!vfs_path || !dst_dir || !*vfs_path || !*dst_dir)
        return -EINVAL;
    if (re_mkdir_p(dst_dir) != 0) {
        fprintf(stderr, "re: cannot create output directory '%s': %s\n", dst_dir, strerror(errno));
        return -EIO;
    }

    // Data fork (always present, even when zero bytes).
    size_t data_len = 0;
    uint8_t *data_bytes = re_read_vfs_file(vfs_path, &data_len);
    if (!data_bytes) {
        fprintf(stderr, "re: cannot read data fork from '%s'\n", vfs_path);
        return -EIO;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/data.bin", dst_dir);
    int rc = write_blob(path, data_bytes, data_len);
    free(data_bytes);
    if (rc != 0)
        return -EIO;

    // Finder info — present only on HFS-backed paths.  Absence is silent.
    size_t finf_len = 0;
    uint8_t *finf_bytes = read_suffix(vfs_path, "finf", &finf_len);
    if (finf_bytes) {
        snprintf(path, sizeof(path), "%s/finder.json", dst_dir);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            finder_json_write(finf_bytes, finf_len, fp);
            fclose(fp);
        }
        free(finf_bytes);
    }

    // Resource fork — parse + extract.  Absence is silent (data-only files).
    size_t rsrc_len = 0;
    uint8_t *rsrc_bytes = read_suffix(vfs_path, "rsrc/_raw", &rsrc_len);
    int total_resources = 0;
    if (rsrc_bytes && rsrc_len > 0) {
        const char *errmsg = NULL;
        rfork_t *rf = rfork_parse(rsrc_bytes, rsrc_len, &errmsg);
        if (!rf) {
            fprintf(stderr, "re: corrupt resource fork in '%s': %s\n", vfs_path, errmsg ? errmsg : "unknown");
            free(rsrc_bytes);
            return -EIO;
        }
        total_resources = dump_resources(rf, dst_dir);
        rfork_free(rf);
    }
    free(rsrc_bytes);

    if (total_resources < 0)
        return -EIO;
    printf("re.dump: %s -> %s (%d resource%s)\n", vfs_path, dst_dir, total_resources, total_resources == 1 ? "" : "s");
    return 0;
}

// ============================================================================
// Object-model method wrappers
// ============================================================================

static value_t re_method_identify(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    return val_bool(re_identify(argv[0].s));
}

static value_t re_method_dump(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    return val_bool(re_dump(argv[0].s, argv[1].s) == 0);
}

static const arg_decl_t re_identify_args[] = {
    {.name = "vfs_path", .kind = V_STRING, .doc = "VFS path to a forked Mac file"},
};
static const arg_decl_t re_dump_args[] = {
    {.name = "vfs_path", .kind = V_STRING, .doc = "VFS path to a forked Mac file"        },
    {.name = "dst_dir",  .kind = V_STRING, .doc = "Output directory (created if missing)"},
};

static const member_t re_members[] = {
    {.kind = M_METHOD,
     .name = "identify",
     .doc = "Print a one-line summary of a forked Mac file",
     .method = {.args = re_identify_args, .nargs = 1, .result = V_BOOL, .fn = re_method_identify}},
    {.kind = M_METHOD,
     .name = "dump",
     .doc = "Extract data fork, finder info, and every resource into dst_dir",
     .method = {.args = re_dump_args, .nargs = 2, .result = V_BOOL, .fn = re_method_dump}        },
};

const class_desc_t re_class = {
    .name = "re",
    .members = re_members,
    .n_members = sizeof(re_members) / sizeof(re_members[0]),
};

// ============================================================================
// Lifecycle (process-singleton, idempotent)
// ============================================================================

static struct object *s_re_object = NULL;

void re_init(void) {
    if (s_re_object)
        return;
    s_re_object = object_new(&re_class, NULL, "re");
    if (s_re_object)
        object_attach(object_root(), s_re_object);
}

void re_delete(void) {
    if (s_re_object) {
        object_detach(s_re_object);
        object_delete(s_re_object);
        s_re_object = NULL;
    }
}
