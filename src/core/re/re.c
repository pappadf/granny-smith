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
#include "symbols.h"
#include "value.h"
#include "vfs.h"
#include "decoders/decoders.h"

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

// Forward decls for the new PR 5 helpers (decoded/ pass + manifest writer).
// Both live below this function for readability.
static int dump_decoded(const struct rfork *rf, const char *dst_dir);
static int dump_manifest(const struct rfork *rf, const char *vfs_path, const char *dst_dir, size_t data_len,
                         size_t rsrc_len, const uint8_t *finder_info, size_t finder_info_len);
static void dump_readme(const struct rfork *rf, const char *vfs_path, const char *dst_dir, size_t data_len,
                        size_t rsrc_len, const uint8_t *finder_info, size_t finder_info_len, int total_resources,
                        int disasm_written, int decoded_written);

int re_dump(const char *vfs_path, const char *dst_dir) {
    return re_dump_with_flags(vfs_path, dst_dir, 0);
}

int re_dump_with_flags(const char *vfs_path, const char *dst_dir, uint32_t flags) {
    if (!vfs_path || !dst_dir || !*vfs_path || !*dst_dir)
        return -EINVAL;
    if (re_mkdir_p(dst_dir) != 0) {
        fprintf(stderr, "re: cannot create output directory '%s': %s\n", dst_dir, strerror(errno));
        return -EIO;
    }
    // Refuse to overwrite a non-empty existing dir unless --force is set.
    // Empty dirs (including freshly mkdir'd ones) are fine.
    if (!(flags & RE_DUMP_FORCE)) {
        // A quick non-empty check via stat on the data.bin we're about to
        // create — if it already exists, treat the dir as non-empty.
        char probe[PATH_MAX];
        snprintf(probe, sizeof(probe), "%s/data.bin", dst_dir);
        struct stat st;
        if (stat(probe, &st) == 0) {
            fprintf(stderr,
                    "re: refusing to overwrite existing dump at '%s' (pass --force "
                    "to override)\n",
                    dst_dir);
            return -EEXIST;
        }
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
    }

    // Resource fork — parse, extract every resource, then disassemble
    // any CODE segments.  rf stays alive across the three passes so we
    // don't pay the parse cost three times.
    size_t rsrc_len = 0;
    uint8_t *rsrc_bytes = read_suffix(vfs_path, "rsrc/_raw", &rsrc_len);
    int total_resources = 0;
    int disasm_written = 0;
    int decoded_written = 0;
    if (rsrc_bytes && rsrc_len > 0) {
        const char *errmsg = NULL;
        rfork_t *rf = rfork_parse(rsrc_bytes, rsrc_len, &errmsg);
        if (!rf) {
            fprintf(stderr, "re: corrupt resource fork in '%s': %s\n", vfs_path, errmsg ? errmsg : "unknown");
            free(rsrc_bytes);
            free(finf_bytes);
            return -EIO;
        }
        total_resources = dump_resources(rf, dst_dir);
        if (total_resources >= 0 && !(flags & RE_DUMP_NO_DISASM))
            disasm_written = dump_disasm(rf, dst_dir);
        if (total_resources >= 0 && !(flags & RE_DUMP_NO_DECODE))
            decoded_written = dump_decoded(rf, dst_dir);
        // Always write the manifest — it consolidates every layer we did
        // actually run.  README.md mirrors the same data in human-readable
        // form so a reader landing in the directory cold has a clear
        // table of contents.
        dump_manifest(rf, vfs_path, dst_dir, data_len, rsrc_len, finf_bytes, finf_len);
        dump_readme(rf, vfs_path, dst_dir, data_len, rsrc_len, finf_bytes, finf_len, total_resources, disasm_written,
                    decoded_written);
        rfork_free(rf);
    }
    free(rsrc_bytes);
    free(finf_bytes);

    if (total_resources < 0 || disasm_written < 0 || decoded_written < 0)
        return -EIO;

    printf("re.dump: %s -> %s (%d resource%s, %d code segment%s, %d decoded)\n", vfs_path, dst_dir, total_resources,
           total_resources == 1 ? "" : "s", disasm_written, disasm_written == 1 ? "" : "s", decoded_written);
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

static void write_segment_listing(FILE *fp, int16_t code_id, const re_code_segment_t *seg, const re_jt_table_t *jt,
                                  re_symbols_t *symbols) {
    const char *model_str = (seg->model == RE_CODE_MODEL_FAR) ? "far" : "near";
    fprintf(fp, "; ============================================================\n");
    fprintf(fp, "; CODE %d\n", code_id);
    fprintf(fp, "; model=%s  header_bytes=%zu  jt_offset=0x%04X  jt_count=%u  code_bytes=%zu\n", model_str,
            seg->header_bytes, seg->jt_offset, seg->jt_count, seg->insts_len);
    fprintf(fp, "; ============================================================\n\n");
    // Addresses start at header_bytes so a JSR -X(A5) destination resolves
    // to a label inside the listing, matching the runtime loader's view.
    re_annotate_ctx_t ctx = {.jt = jt, .code_id = code_id, .symbols = symbols};
    re_annotate_disasm_write(fp, seg->insts, seg->insts_len, (uint32_t)seg->header_bytes, RE_DISASM_ALL, &ctx);
}

// Write disasm/CODE-NNNN.s for every CODE id != 0 (and disasm/jump-table.s
// for CODE 0), plus a consolidated symbols.txt at dst_dir's root.
// Returns the number of disasm files written, or -1 on failure.
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

    // Parse CODE 0 up front (best effort).  Both the JT listing and the
    // per-segment listings consume it for xref + label labelling.  When
    // CODE 0 is missing or malformed we keep going with NULL — the
    // listings still emit, just without the JT annotations.
    re_jt_table_t jt;
    bool jt_valid = false;
    {
        const uint8_t *c0_bytes = NULL;
        size_t c0_sz = 0;
        if (rfork_lookup(rf, code_cc, 0, &c0_bytes, &c0_sz, NULL, NULL) == 0) {
            if (re_parse_code0(c0_bytes, c0_sz, &jt) == 0)
                jt_valid = true;
        }
    }
    re_symbols_t symbols;
    re_symbols_init(&symbols);

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
            goto fail;
        FILE *fp = open_out_file(path);
        if (!fp)
            goto fail;
        if (id == 0) {
            if (jt_valid)
                write_jt_listing(fp, &jt);
            else
                fprintf(fp, "; CODE 0 malformed (%zu bytes)\n", sz);
        } else {
            re_code_segment_t seg;
            if (re_parse_code_n(bytes, sz, &seg) == 0)
                write_segment_listing(fp, id, &seg, jt_valid ? &jt : NULL, &symbols);
            else
                fprintf(fp, "; CODE %d malformed (%zu bytes)\n", id, sz);
        }
        fclose(fp);
        written++;
    }

    // Consolidated symbol listing at the top of the dump.
    char sym_path[PATH_MAX];
    n = snprintf(sym_path, sizeof(sym_path), "%s/symbols.txt", dst_dir);
    if (n > 0 && (size_t)n < sizeof(sym_path)) {
        FILE *sfp = open_out_file(sym_path);
        if (sfp) {
            re_symbols_write_txt(&symbols, sfp);
            fclose(sfp);
        }
    }

    re_symbols_free(&symbols);
    if (jt_valid)
        re_jt_free(&jt);
    return written;

fail:
    re_symbols_free(&symbols);
    if (jt_valid)
        re_jt_free(&jt);
    return -1;
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
    // For a single-segment call we still want JT-xref annotation, so
    // try to fetch and parse CODE 0 from the same resource fork.  Symbol
    // recovery runs locally for this segment only (no shared symbols.txt).
    re_jt_table_t jt;
    bool jt_valid = false;
    if (code_id != 0) {
        char c0_path[PATH_MAX];
        int cn = snprintf(c0_path, sizeof(c0_path), "%s/rsrc/CODE/0", vfs_path);
        if (cn > 0 && (size_t)cn < sizeof(c0_path)) {
            size_t c0_sz = 0;
            uint8_t *c0_bytes = re_read_vfs_file(c0_path, &c0_sz);
            if (c0_bytes) {
                if (re_parse_code0(c0_bytes, c0_sz, &jt) == 0)
                    jt_valid = true;
                free(c0_bytes);
            }
        }
    }
    re_symbols_t symbols;
    re_symbols_init(&symbols);

    if (code_id == 0) {
        re_jt_table_t local_jt;
        if (re_parse_code0(bytes, sz, &local_jt) == 0) {
            write_jt_listing(fp, &local_jt);
            re_jt_free(&local_jt);
        } else {
            fprintf(fp, "; CODE 0 malformed (%zu bytes)\n", sz);
        }
    } else {
        re_code_segment_t seg;
        if (re_parse_code_n(bytes, sz, &seg) == 0)
            write_segment_listing(fp, (int16_t)code_id, &seg, jt_valid ? &jt : NULL, &symbols);
        else
            fprintf(fp, "; CODE %d malformed (%zu bytes)\n", code_id, sz);
    }

    re_symbols_free(&symbols);
    if (jt_valid)
        re_jt_free(&jt);
    if (fp != stdout)
        fclose(fp);
    free(bytes);
    return 0;
}

// ============================================================================
// Decoded/ output and manifest.json (PR 5 helpers)
// ============================================================================

// Run re_decode_dispatch on every resource of every type.  Output:
//   decoded/<TYPE>/<id>.json (always; the JSON object the decoder
//                              produced, or a minimal stub when no
//                              decoder is registered)
//   decoded/<TYPE>/<id>.txt  (when the decoder also writes a plain-text
//                              summary)
// Returns the number of files written, or -1 on failure.
static int dump_decoded(const rfork_t *rf, const char *dst_dir) {
    char base[PATH_MAX];
    int n = snprintf(base, sizeof(base), "%s/decoded", dst_dir);
    if (n < 0 || (size_t)n >= sizeof(base))
        return -1;
    if (re_mkdir_p(base) != 0)
        return -1;

    int written = 0;
    size_t n_types = rfork_num_types(rf);
    for (size_t t = 0; t < n_types; t++) {
        const uint8_t *type = rfork_type_at(rf, t);
        if (!re_decode_dispatch_name(type))
            continue; // skip types with no decoder
        char type_path[16];
        rfork_type_to_path(type, type_path, sizeof(type_path));
        char type_dir[PATH_MAX];
        n = snprintf(type_dir, sizeof(type_dir), "%s/%s", base, type_path);
        if (n < 0 || (size_t)n >= sizeof(type_dir))
            return -1;
        if (re_mkdir_p(type_dir) != 0)
            return -1;

        size_t n_res = rfork_num_resources(rf, type);
        for (size_t r = 0; r < n_res; r++) {
            int16_t id = rfork_id_at(rf, type, r);
            const uint8_t *bytes = NULL;
            size_t sz = 0;
            if (rfork_lookup(rf, type, id, &bytes, &sz, NULL, NULL) < 0)
                continue;
            char id_str[16];
            rfork_id_to_path(id, id_str, sizeof(id_str));
            char json_path[PATH_MAX];
            char txt_path[PATH_MAX];
            snprintf(json_path, sizeof(json_path), "%s/%s.json", type_dir, id_str);
            snprintf(txt_path, sizeof(txt_path), "%s/%s.txt", type_dir, id_str);
            FILE *jfp = fopen(json_path, "wb");
            if (!jfp)
                continue;
            FILE *tfp = fopen(txt_path, "wb");
            int rc = re_decode_dispatch(type, bytes, sz, jfp, tfp);
            fclose(jfp);
            if (tfp)
                fclose(tfp);
            if (rc > 0) {
                written++;
            } else if (rc == 0) {
                // No decoder — shouldn't happen since we filtered above.
                // Remove the empty json file.
                remove(json_path);
                remove(txt_path);
            }
            // On decoder error the JSON already carries an "error" object.
        }
    }
    return written;
}

// Manifest writer.  Captures the source identity, fork sizes, resource
// type/id list, and the recovered CODE-segment structure.  Resources
// carry the relative paths of their bin / info / disasm / decoded files
// so downstream tooling can navigate without re-walking the dir tree.
static int dump_manifest(const rfork_t *rf, const char *vfs_path, const char *dst_dir, size_t data_len, size_t rsrc_len,
                         const uint8_t *finder_info, size_t finder_info_len) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/manifest.json", dst_dir);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "re: cannot create '%s': %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "{\n  \"schema_version\": 1,\n  \"generator\": \"granny-smith re v1\",\n");
    fprintf(fp, "  \"source\": {\n    \"vfs_path\": ");
    extern void re_json_write_string(FILE *, const char *);
    re_json_write_string(fp, vfs_path);
    fprintf(fp, ",\n    \"data_fork\": {\"size\": %zu},\n", data_len);
    fprintf(fp, "    \"rsrc_fork\": {\"size\": %zu}", rsrc_len);
    if (finder_info && finder_info_len == 32) {
        fprintf(fp, ",\n    \"finder\": {\"type\":\"%c%c%c%c\",\"creator\":\"%c%c%c%c\",\"flags_hex\":\"0x%02x%02x\"}",
                finder_info[0], finder_info[1], finder_info[2], finder_info[3], finder_info[4], finder_info[5],
                finder_info[6], finder_info[7], finder_info[8], finder_info[9]);
    }
    fprintf(fp, "\n  },\n");

    // Resource summary by type.
    fprintf(fp, "  \"resource_fork\": {\n    \"attrs\": %u,\n    \"types\": [\n", rfork_attrs(rf));
    size_t n_types = rfork_num_types(rf);
    for (size_t t = 0; t < n_types; t++) {
        const uint8_t *type = rfork_type_at(rf, t);
        char type_path[16];
        rfork_type_to_path(type, type_path, sizeof(type_path));
        fprintf(fp, "      {\"type\": ");
        re_json_write_string(fp, type_path);
        fprintf(fp, ", \"ids\": [");
        size_t nr = rfork_num_resources(rf, type);
        for (size_t r = 0; r < nr; r++)
            fprintf(fp, "%s%d", r ? "," : "", rfork_id_at(rf, type, r));
        fprintf(fp, "]}%s\n", t + 1 < n_types ? "," : "");
    }
    fprintf(fp, "    ]\n  },\n");

    // CODE segment summary (model + a5_world + per-segment metadata).
    static const uint8_t code_cc[4] = {'C', 'O', 'D', 'E'};
    size_t n_code = rfork_num_resources(rf, code_cc);
    if (n_code > 0) {
        fprintf(fp, "  \"code\": {\n");
        const uint8_t *c0_bytes = NULL;
        size_t c0_sz = 0;
        rfork_lookup(rf, code_cc, 0, &c0_bytes, &c0_sz, NULL, NULL);
        re_jt_table_t jt;
        bool jt_valid = (c0_bytes && re_parse_code0(c0_bytes, c0_sz, &jt) == 0);
        if (jt_valid) {
            fprintf(fp, "    \"model\": \"near\",\n");
            fprintf(fp, "    \"a5_world\": {\"above_a5\": %u, \"below_a5\": %u, \"jt_size\": %u, \"jt_offset\": %u},\n",
                    jt.above_a5, jt.below_a5, jt.jt_size, jt.jt_offset);
        } else {
            fprintf(fp, "    \"model\": \"unknown\",\n");
        }
        fprintf(fp, "    \"segments\": [\n");
        for (size_t r = 0; r < n_code; r++) {
            int16_t id = rfork_id_at(rf, code_cc, r);
            const uint8_t *bytes = NULL;
            size_t sz = 0;
            if (rfork_lookup(rf, code_cc, id, &bytes, &sz, NULL, NULL) < 0)
                continue;
            const char *kind = (id == 0) ? "jumptable" : "code";
            uint16_t jt_off = 0, jt_count = 0;
            const char *model = "near";
            if (id != 0) {
                re_code_segment_t seg;
                if (re_parse_code_n(bytes, sz, &seg) == 0) {
                    jt_off = seg.jt_offset;
                    jt_count = seg.jt_count;
                    model = (seg.model == RE_CODE_MODEL_FAR) ? "far" : "near";
                }
            }
            fprintf(fp,
                    "      {\"id\": %d, \"kind\": \"%s\", \"size\": %zu, \"model\": \"%s\", \"jt_offset\": "
                    "%u, \"jt_count\": %u}%s\n",
                    id, kind, sz, model, jt_off, jt_count, r + 1 < n_code ? "," : "");
        }
        fprintf(fp, "    ]\n  },\n");
        if (jt_valid)
            re_jt_free(&jt);
    }

    // Resources roll-up with per-file paths.
    fprintf(fp, "  \"resources\": [\n");
    bool first = true;
    for (size_t t = 0; t < n_types; t++) {
        const uint8_t *type = rfork_type_at(rf, t);
        char type_path[16];
        rfork_type_to_path(type, type_path, sizeof(type_path));
        bool is_code = memcmp(type, code_cc, 4) == 0;
        bool has_decoder = re_decode_dispatch_name(type) != NULL;
        size_t nr = rfork_num_resources(rf, type);
        for (size_t r = 0; r < nr; r++) {
            int16_t id = rfork_id_at(rf, type, r);
            const uint8_t *bytes = NULL;
            size_t sz = 0;
            const char *name = NULL;
            uint8_t attrs = 0;
            if (rfork_lookup(rf, type, id, &bytes, &sz, &name, &attrs) < 0)
                continue;
            if (!first)
                fputs(",\n", fp);
            first = false;
            char id_str[16];
            rfork_id_to_path(id, id_str, sizeof(id_str));
            fprintf(fp, "    {\"type\": ");
            re_json_write_string(fp, type_path);
            fprintf(fp, ", \"id\": %d, \"name\": ", id);
            re_json_write_string(fp, name ? name : "");
            fprintf(fp, ", \"attrs\": %u, \"size\": %zu, \"files\": {\"bin\": \"resources/%s/%s\", \"info\": ", attrs,
                    sz, type_path, id_str);
            fprintf(fp, "\"resources/%s/%s.info\", \"disasm\": ", type_path, id_str);
            if (is_code) {
                if (id == 0)
                    fprintf(fp, "\"disasm/jump-table.s\"");
                else
                    fprintf(fp, "\"disasm/CODE-%04d.s\"", (int)id);
            } else {
                fprintf(fp, "null");
            }
            fprintf(fp, ", \"decoded\": ");
            if (has_decoder)
                fprintf(fp, "\"decoded/%s/%s.json\"", type_path, id_str);
            else
                fprintf(fp, "null");
            fprintf(fp, "}}");
        }
    }
    fprintf(fp, "\n  ]\n}\n");
    fclose(fp);
    return 0;
}

// ============================================================================
// README.md writer — human-readable table of contents
// ============================================================================
//
// `manifest.json` is the machine-readable index; this file is the
// hand-readable companion.  A reader who opens the dump directory cold
// gets a one-page tour: source identity, what's in each subdirectory,
// where to start, and what conventions to expect (raw bytes vs decoded
// JSON, MacRoman in path components, etc.).

static void dump_readme(const struct rfork *rf, const char *vfs_path, const char *dst_dir, size_t data_len,
                        size_t rsrc_len, const uint8_t *finder_info, size_t finder_info_len, int total_resources,
                        int disasm_written, int decoded_written) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/README.md", dst_dir);
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return;

    // Header + identity table.  Finder OSTypes are rendered as the
    // literal 4 ASCII bytes when printable, hex otherwise — same shape
    // as finder.json.
    fprintf(fp, "# re.dump — exploded classic-Mac binary\n\n");
    fprintf(fp, "Self-contained dump of a classic-Mac forked file produced by `re.dump`\n");
    fprintf(fp, "(see [docs/storage.md](../docs/storage.md) §11 and the\n");
    fprintf(fp, "[proposal](../local/gs-docs/proposals/proposal-re-binary-explode.md) for the\n");
    fprintf(fp, "convention).  Every directory here is a layer of progressively-decoded\n");
    fprintf(fp, "data — raw bytes at the bottom, JSON / `.s` listings on top.\n\n");

    fprintf(fp, "## Source\n\n");
    fprintf(fp, "| Field | Value |\n|---|---|\n");
    fprintf(fp, "| VFS path | `%s` |\n", vfs_path ? vfs_path : "?");
    fprintf(fp, "| Data fork | %zu bytes |\n", data_len);
    fprintf(fp, "| Resource fork | %zu bytes |\n", rsrc_len);
    if (finder_info && finder_info_len == 32) {
        bool t_ok = true, c_ok = true;
        for (int i = 0; i < 4; i++) {
            if (finder_info[i] < 0x20 || finder_info[i] > 0x7E)
                t_ok = false;
            if (finder_info[4 + i] < 0x20 || finder_info[4 + i] > 0x7E)
                c_ok = false;
        }
        if (t_ok)
            fprintf(fp, "| Finder type | `%c%c%c%c` |\n", finder_info[0], finder_info[1], finder_info[2],
                    finder_info[3]);
        else
            fprintf(fp, "| Finder type | `0x%02x%02x%02x%02x` |\n", finder_info[0], finder_info[1], finder_info[2],
                    finder_info[3]);
        if (c_ok)
            fprintf(fp, "| Finder creator | `%c%c%c%c` |\n", finder_info[4], finder_info[5], finder_info[6],
                    finder_info[7]);
        else
            fprintf(fp, "| Finder creator | `0x%02x%02x%02x%02x` |\n", finder_info[4], finder_info[5], finder_info[6],
                    finder_info[7]);
        fprintf(fp, "| Finder flags | `0x%02x%02x` |\n", finder_info[8], finder_info[9]);
    }
    size_t n_types = rfork_num_types(rf);
    fprintf(fp, "| Resource types | %zu |\n", n_types);
    fprintf(fp, "| Resources extracted | %d |\n", total_resources);
    fprintf(fp, "| CODE segments disassembled | %d |\n", disasm_written);
    fprintf(fp, "| Resources decoded | %d |\n\n", decoded_written);

    // Directory layout table.  This mirrors the on-disk shape but with
    // intent annotations so the reader knows where to look for what.
    fprintf(fp, "## Directory layout\n\n");
    fprintf(fp, "| Path | What it contains |\n|---|---|\n");
    fprintf(fp, "| `data.bin` | Data fork, verbatim (often empty on Mac apps). |\n");
    if (finder_info && finder_info_len == 32)
        fprintf(fp, "| `finder.json` | 32-byte Finder info decoded into type / creator / flags / folder. |\n");
    fprintf(fp, "| `manifest.json` | Machine-readable index.  Schema v1; carries the full\n");
    fprintf(fp, "  resource list with per-file paths and the parsed CODE-segment table. |\n");
    fprintf(fp, "| `README.md` | This file — human-readable table of contents. |\n");
    fprintf(fp, "| `resources/<TYPE>/<id>` | Raw resource bytes, verbatim. `<TYPE>` is the\n");
    fprintf(fp, "  4-byte resource type (MacRoman → UTF-8 in the path component). `<id>` is\n");
    fprintf(fp, "  the signed-16-bit ID as base-10 (with `-` for negatives). |\n");
    fprintf(fp, "| `resources/<TYPE>/<id>.info` | JSON sidecar with the resource name (if\n");
    fprintf(fp, "  any), attribute flag list (`preload`, `locked`, …), and size. |\n");
    if (disasm_written > 0) {
        fprintf(fp, "| `disasm/jump-table.s` | Decoded CODE 0 — A5 world dimensions plus the\n");
        fprintf(fp, "  per-segment-and-offset jump table. |\n");
        fprintf(fp, "| `disasm/CODE-NNNN.s` | Per-segment 68k disassembly with PC-relative\n");
        fprintf(fp, "  branch resolution, trap-name annotations (`; trap _NewHandle`),\n");
        fprintf(fp, "  JT cross-references (`; xref JT[i] = CODE s:offset`),\n");
        fprintf(fp, "  low-memory global labels (`; global Ticks`), and recovered\n");
        fprintf(fp, "  MacsBug / boundary labels (`MainEventLoop:` / `sub_01AE:`). |\n");
        fprintf(fp, "| `symbols.txt` | Consolidated symbol table: `<code_id> <addr> <source> <name>`. |\n");
    }
    if (decoded_written > 0) {
        fprintf(fp, "| `decoded/<TYPE>/<id>.json` | Per-type decoded form (vers, MENU, DLOG,\n");
        fprintf(fp, "  STR#, SIZE, BNDL, …).  Hand-readable JSON; consumers should\n");
        fprintf(fp, "  prefer this over `resources/<TYPE>/<id>` for any structured\n");
        fprintf(fp, "  field access. |\n");
        fprintf(fp, "| `decoded/<TYPE>/<id>.txt` | Plain-text summary where the decoder\n");
        fprintf(fp, "  produces one (`vers`, `STR`/`STR#`, `MENU`, dialog templates). |\n");
    }
    fputc('\n', fp);

    // Per-type resource counts so a reader can see the shape of the
    // binary at a glance without opening manifest.json.
    fprintf(fp, "## Resource types\n\n");
    fprintf(fp, "| Type | Count | Decoded? |\n|---|---|---|\n");
    for (size_t t = 0; t < n_types; t++) {
        const uint8_t *cc = rfork_type_at(rf, t);
        char type_path[16];
        rfork_type_to_path(cc, type_path, sizeof(type_path));
        size_t nr = rfork_num_resources(rf, cc);
        const char *dec = re_decode_dispatch_name(cc);
        bool is_code = memcmp(cc, "CODE", 4) == 0;
        fprintf(fp, "| `%s` | %zu | %s |\n", type_path, nr,
                dec       ? "yes (decoded/)"
                : is_code ? "n/a (see disasm/)"
                          : "no — raw bytes only");
    }
    fputc('\n', fp);

    // Practical entry points.
    fprintf(fp, "## Where to start\n\n");
    fprintf(fp, "- `manifest.json` — start here for tooling.\n");
    if (decoded_written > 0)
        fprintf(fp, "- `decoded/vers/1.txt` — human-readable version string (when present).\n");
    if (disasm_written > 0) {
        fprintf(fp, "- `disasm/jump-table.s` — the application's main jump table, the index\n");
        fprintf(fp, "  into the rest of the disassembly.\n");
        fprintf(fp, "- `symbols.txt` — every label the recovery passes were able to name.\n");
    }
    fprintf(fp, "- `resources/<TYPE>/<id>.info` — quick metadata peek for any resource\n");
    fprintf(fp, "  without opening the binary blob.\n\n");

    fprintf(fp, "## Conventions\n\n");
    fprintf(fp, "- Resource type `<TYPE>` is the literal four MacRoman bytes from the on-disk\n");
    fprintf(fp, "  type list, transcoded to UTF-8.  Types with a trailing space (e.g. `STR `,\n");
    fprintf(fp, "  `SND `) preserve it.\n");
    fprintf(fp, "- Resource `<id>` is the signed 16-bit ID as a base-10 integer (`128`, `-16`,\n");
    fprintf(fp, "  `0`, `32767`).\n");
    fprintf(fp, "- `data.bin` is the *data fork*; resources live in the *resource fork* and\n");
    fprintf(fp, "  appear under `resources/`.  Most classic-Mac applications have an empty\n");
    fprintf(fp, "  data fork.\n");
    fprintf(fp, "- Disassembly addresses start at the segment's first instruction (offset 4\n");
    fprintf(fp, "  for near-model CODE), matching the runtime loader's view of the segment.\n");
    fprintf(fp, "\n");
    fprintf(fp, "Re-run any time with `re.dump <vfs_path> <dir>` (pass `--force` to overwrite,\n");
    fprintf(fp, "`--no-decode` / `--no-disasm` to skip those layers).\n");

    fclose(fp);
}

// ============================================================================
// re_decode_one — single-resource decode to dst_file (or stdout)
// ============================================================================

int re_decode_one(const char *vfs_path, const char *type_str, int id, const char *dst_file) {
    if (!vfs_path || !type_str || !*vfs_path || !*type_str)
        return -EINVAL;
    uint8_t type[4];
    if (rfork_type_from_path(type_str, type) != 0)
        return -EINVAL;
    if (!re_decode_dispatch_name(type))
        return -ENOENT;
    if (id < INT16_MIN || id > INT16_MAX)
        return -EINVAL;

    char rsrc_path[PATH_MAX];
    int n = snprintf(rsrc_path, sizeof(rsrc_path), "%s/rsrc/%s/%d", vfs_path, type_str, id);
    if (n < 0 || (size_t)n >= sizeof(rsrc_path))
        return -ENAMETOOLONG;
    size_t sz = 0;
    uint8_t *bytes = re_read_vfs_file(rsrc_path, &sz);
    if (!bytes)
        return -ENOENT;
    FILE *fp = (dst_file && *dst_file) ? fopen(dst_file, "wb") : stdout;
    if (!fp) {
        free(bytes);
        return -EIO;
    }
    int rc = re_decode_dispatch(type, bytes, sz, fp, NULL);
    if (fp != stdout)
        fclose(fp);
    free(bytes);
    return (rc > 0) ? 0 : -EIO;
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

// Iterate argv[start..argc) and OR in the matching RE_DUMP_* bits.
// Each rest-arg item is its own argv slot (the dispatcher does NOT pack
// them into a V_LIST — see storage.cp for the precedent).  Unknown
// flags log a warning but don't fail the call.
static uint32_t parse_dump_flags(int argc, const value_t *argv, int start) {
    uint32_t flags = 0;
    for (int i = start; i < argc; i++) {
        if (argv[i].kind != V_STRING || !argv[i].s)
            continue;
        const char *s = argv[i].s;
        if (strcmp(s, "--no-decode") == 0 || strcmp(s, "-D") == 0)
            flags |= RE_DUMP_NO_DECODE;
        else if (strcmp(s, "--no-disasm") == 0 || strcmp(s, "-S") == 0)
            flags |= RE_DUMP_NO_DISASM;
        else if (strcmp(s, "--force") == 0 || strcmp(s, "-f") == 0)
            flags |= RE_DUMP_FORCE;
        else
            fprintf(stderr, "re.dump: unknown flag '%s' (ignoring)\n", s);
    }
    return flags;
}

static value_t re_method_dump(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = argv[0].s;
    const char *dst = argv[1].s;
    uint32_t flags = parse_dump_flags(argc, argv, 2);
    return val_bool(re_dump_with_flags(path, dst, flags) == 0);
}

static value_t re_method_disasm_code(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = argv[0].s;
    int code_id = (int)argv[1].i;
    const char *dst = (argc >= 3 && argv[2].s && *argv[2].s) ? argv[2].s : NULL;
    return val_bool(re_disasm_code(path, code_id, dst) == 0);
}

static value_t re_method_decode(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = argv[0].s;
    const char *type = argv[1].s;
    int id = (int)argv[2].i;
    const char *dst = (argc >= 4 && argv[3].s && *argv[3].s) ? argv[3].s : NULL;
    return val_bool(re_decode_one(path, type, id, dst) == 0);
}

static const arg_decl_t re_identify_args[] = {
    {.name = "vfs_path", .kind = V_STRING, .doc = "VFS path to a forked Mac file"},
};
static const arg_decl_t re_dump_args[] = {
    {.name = "vfs_path", .kind = V_STRING, .doc = "VFS path to a forked Mac file"},
    {.name = "dst_dir", .kind = V_STRING, .doc = "Output directory (created if missing)"},
    {.name = "flags",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_REST,
     .doc = "Optional flags: --no-decode, --no-disasm, --force"},
};
static const arg_decl_t re_disasm_code_args[] = {
    {.name = "vfs_path", .kind = V_STRING, .doc = "VFS path to a forked Mac file"},
    {.name = "code_id", .kind = V_INT, .width = 2, .doc = "CODE resource ID (0 = jump table)"},
    {.name = "dst_file",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Output file (defaults to stdout)"},
};
static const arg_decl_t re_decode_args[] = {
    {.name = "vfs_path", .kind = V_STRING, .doc = "VFS path to a forked Mac file"},
    {.name = "type", .kind = V_STRING, .doc = "Resource type 4-CC (e.g. \"vers\")"},
    {.name = "id", .kind = V_INT, .width = 2, .doc = "Resource ID"},
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
     .doc = "Extract data/resources/disasm/decoded + manifest.json into dst_dir",
     .method = {.args = re_dump_args, .nargs = 3, .result = V_BOOL, .fn = re_method_dump}              },
    {.kind = M_METHOD,
     .name = "disasm_code",
     .doc = "Disassemble one CODE resource to dst_file (or stdout)",
     .method = {.args = re_disasm_code_args, .nargs = 3, .result = V_BOOL, .fn = re_method_disasm_code}},
    {.kind = M_METHOD,
     .name = "decode",
     .doc = "Run the per-type decoder for one resource (vers/MENU/STR#/...)",
     .method = {.args = re_decode_args, .nargs = 4, .result = V_BOOL, .fn = re_method_decode}          },
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
