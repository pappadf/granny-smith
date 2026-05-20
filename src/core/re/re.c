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

#include "annotate_disasm.h"
#include "code_segment.h"
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

// Forward declaration: dump_disasm lives below re_dump for readability
// (the disassembly helpers are conceptually a self-contained block) but
// re_dump calls it as the final step.  The forward decl keeps that
// ordering legal in C without cross-referencing surprises.
static int dump_disasm(const struct rfork *rf, const char *dst_dir);

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

    // Resource fork — parse, extract every resource, then disassemble
    // any CODE segments.  rf stays alive across both passes so we don't
    // pay the parse cost twice.
    size_t rsrc_len = 0;
    uint8_t *rsrc_bytes = read_suffix(vfs_path, "rsrc/_raw", &rsrc_len);
    int total_resources = 0;
    int disasm_written = 0;
    if (rsrc_bytes && rsrc_len > 0) {
        const char *errmsg = NULL;
        rfork_t *rf = rfork_parse(rsrc_bytes, rsrc_len, &errmsg);
        if (!rf) {
            fprintf(stderr, "re: corrupt resource fork in '%s': %s\n", vfs_path, errmsg ? errmsg : "unknown");
            free(rsrc_bytes);
            return -EIO;
        }
        total_resources = dump_resources(rf, dst_dir);
        if (total_resources >= 0)
            disasm_written = dump_disasm(rf, dst_dir);
        rfork_free(rf);
    }
    free(rsrc_bytes);

    if (total_resources < 0 || disasm_written < 0)
        return -EIO;

    printf("re.dump: %s -> %s (%d resource%s, %d code segment%s)\n", vfs_path, dst_dir, total_resources,
           total_resources == 1 ? "" : "s", disasm_written, disasm_written == 1 ? "" : "s");
    return 0;
}

// ============================================================================
// Disassembly helpers — CODE 0 jump table + CODE N segment listings
// ============================================================================

static FILE *open_out_file(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp)
        fprintf(stderr, "re: cannot create '%s': %s\n", path, strerror(errno));
    return fp;
}

static void write_jt_listing(FILE *fp, const re_jt_table_t *jt) {
    fprintf(fp, "; ============================================================\n");
    fprintf(fp, "; CODE 0 — application jump table\n");
    fprintf(fp, "; above_a5 = %u (0x%X)\n", jt->above_a5, jt->above_a5);
    fprintf(fp, "; below_a5 = %u (0x%X)\n", jt->below_a5, jt->below_a5);
    fprintf(fp, "; jt_size  = %u (0x%X)\n", jt->jt_size, jt->jt_size);
    fprintf(fp, "; jt_offset_from_A5 = 0x%X\n", jt->jt_offset);
    fprintf(fp, "; entries  = %zu\n", jt->n_entries);
    fprintf(fp, "; ============================================================\n\n");
    for (size_t i = 0; i < jt->n_entries; i++)
        fprintf(fp, "JT[%zu]\tsegment=%u\toffset=0x%04X\n", i, jt->entries[i].segment, jt->entries[i].offset);
}

static void write_segment_listing(FILE *fp, int16_t code_id, const re_code_segment_t *seg) {
    const char *model_str = (seg->model == RE_CODE_MODEL_FAR) ? "far" : "near";
    fprintf(fp, "; ============================================================\n");
    fprintf(fp, "; CODE %d\n", code_id);
    fprintf(fp, "; model=%s  header_bytes=%zu  jt_offset=0x%04X  jt_count=%u  code_bytes=%zu\n", model_str,
            seg->header_bytes, seg->jt_offset, seg->jt_count, seg->insts_len);
    fprintf(fp, "; ============================================================\n\n");
    // Addresses start at header_bytes so a JSR -X(A5) destination resolves
    // to a label inside the listing, matching the runtime loader's view.
    re_annotate_disasm_write(fp, seg->insts, seg->insts_len, (uint32_t)seg->header_bytes, RE_DISASM_ALL);
}

// Write disasm/CODE-NNNN.s for every CODE id != 0 (and disasm/jump-table.s
// for CODE 0).  Returns the number of files written, or -1 on failure.
static int dump_disasm(const rfork_t *rf, const char *dst_dir) {
    static const uint8_t code_cc[4] = {'C', 'O', 'D', 'E'};
    size_t n_code = rfork_num_resources(rf, code_cc);
    if (n_code == 0)
        return 0;
    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/disasm", dst_dir);
    if (n < 0 || (size_t)n >= sizeof(dir))
        return -1;
    if (re_mkdir_p(dir) != 0) {
        fprintf(stderr, "re: cannot create '%s': %s\n", dir, strerror(errno));
        return -1;
    }

    int written = 0;
    for (size_t i = 0; i < n_code; i++) {
        int16_t id = rfork_id_at(rf, code_cc, i);
        const uint8_t *bytes = NULL;
        size_t sz = 0;
        if (rfork_lookup(rf, code_cc, id, &bytes, &sz, NULL, NULL) < 0)
            continue;
        char path[PATH_MAX];
        if (id == 0)
            n = snprintf(path, sizeof(path), "%s/jump-table.s", dir);
        else
            n = snprintf(path, sizeof(path), "%s/CODE-%04d.s", dir, id);
        if (n < 0 || (size_t)n >= sizeof(path))
            return -1;
        FILE *fp = open_out_file(path);
        if (!fp)
            return -1;
        if (id == 0) {
            re_jt_table_t jt;
            if (re_parse_code0(bytes, sz, &jt) == 0) {
                write_jt_listing(fp, &jt);
                re_jt_free(&jt);
            } else {
                fprintf(fp, "; CODE 0 malformed (%zu bytes)\n", sz);
            }
        } else {
            re_code_segment_t seg;
            if (re_parse_code_n(bytes, sz, &seg) == 0)
                write_segment_listing(fp, id, &seg);
            else
                fprintf(fp, "; CODE %d malformed (%zu bytes)\n", id, sz);
        }
        fclose(fp);
        written++;
    }
    return written;
}

// ============================================================================
// re_disasm_code — disassemble one CODE resource to dst_file (or stdout)
// ============================================================================

int re_disasm_code(const char *vfs_path, int code_id, const char *dst_file) {
    if (!vfs_path || !*vfs_path)
        return -EINVAL;
    if (code_id < INT16_MIN || code_id > INT16_MAX)
        return -EINVAL;
    char rsrc_path[PATH_MAX];
    int n = snprintf(rsrc_path, sizeof(rsrc_path), "%s/rsrc/CODE/%d", vfs_path, code_id);
    if (n < 0 || (size_t)n >= sizeof(rsrc_path))
        return -ENAMETOOLONG;
    size_t sz = 0;
    uint8_t *bytes = re_read_vfs_file(rsrc_path, &sz);
    if (!bytes) {
        fprintf(stderr, "re: cannot read '%s'\n", rsrc_path);
        return -ENOENT;
    }
    FILE *fp = (dst_file && *dst_file) ? open_out_file(dst_file) : stdout;
    if (!fp) {
        free(bytes);
        return -EIO;
    }
    if (code_id == 0) {
        re_jt_table_t jt;
        if (re_parse_code0(bytes, sz, &jt) == 0) {
            write_jt_listing(fp, &jt);
            re_jt_free(&jt);
        } else {
            fprintf(fp, "; CODE 0 malformed (%zu bytes)\n", sz);
        }
    } else {
        re_code_segment_t seg;
        if (re_parse_code_n(bytes, sz, &seg) == 0)
            write_segment_listing(fp, (int16_t)code_id, &seg);
        else
            fprintf(fp, "; CODE %d malformed (%zu bytes)\n", code_id, sz);
    }
    if (fp != stdout)
        fclose(fp);
    free(bytes);
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

static value_t re_method_disasm_code(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = argv[0].s;
    int code_id = (int)argv[1].i;
    const char *dst = (argc >= 3 && argv[2].s && *argv[2].s) ? argv[2].s : NULL;
    return val_bool(re_disasm_code(path, code_id, dst) == 0);
}

static const arg_decl_t re_identify_args[] = {
    {.name = "vfs_path", .kind = V_STRING, .doc = "VFS path to a forked Mac file"},
};
static const arg_decl_t re_dump_args[] = {
    {.name = "vfs_path", .kind = V_STRING, .doc = "VFS path to a forked Mac file"        },
    {.name = "dst_dir",  .kind = V_STRING, .doc = "Output directory (created if missing)"},
};
static const arg_decl_t re_disasm_code_args[] = {
    {.name = "vfs_path", .kind = V_STRING, .doc = "VFS path to a forked Mac file"},
    {.name = "code_id", .kind = V_INT, .width = 2, .doc = "CODE resource ID (0 = jump table)"},
    {.name = "dst_file",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Output file (defaults to stdout)"},
};

static const member_t re_members[] = {
    {.kind = M_METHOD,
     .name = "identify",
     .doc = "Print a one-line summary of a forked Mac file",
     .method = {.args = re_identify_args, .nargs = 1, .result = V_BOOL, .fn = re_method_identify}      },
    {.kind = M_METHOD,
     .name = "dump",
     .doc = "Extract data fork, finder info, resources, and per-CODE disasm into dst_dir",
     .method = {.args = re_dump_args, .nargs = 2, .result = V_BOOL, .fn = re_method_dump}              },
    {.kind = M_METHOD,
     .name = "disasm_code",
     .doc = "Disassemble one CODE resource to dst_file (or stdout)",
     .method = {.args = re_disasm_code_args, .nargs = 3, .result = V_BOOL, .fn = re_method_disasm_code}},
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
