// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dump.c
// Standalone reverse-engineering tool for classic-Mac forked files and
// A/UX COFF binaries.  Reads buffers passed by main() (loaded from host
// files via plain stdio) and produces a self-contained dump directory:
// raw data fork, every resource under resources/<TYPE>/<id>, per-segment
// disassembly with symbol annotation, per-type decoded JSON/text, a
// manifest.json, and a top-level README.md.
//
// Resource-fork parsing uses the C API in src/core/storage/resource_fork.h
// (rfork_parse + rfork_lookup); compressed resources are auto-inflated
// via rsrc_dcmp.  COFF binaries are detected by their 0x0150 magic and
// dispatched into coff_dump.c.

#include "dump.h"

#include "annotate_disasm.h"
#include "code_segment.h"
#include "coff.h"
#include "coff_dump.h"
#include "resource_fork.h"
#include "symbols.h"
#include "decoders/decoders.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Forward declaration: dump_disasm lives below dump_run for readability
// but dump_run calls it as the final step.
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
// Reading a host file into a malloc'd buffer
// ============================================================================

// Read the entire contents of `path` into a freshly malloc'd buffer.
// Returns NULL on any error (with errno preserved); on success the byte
// count is written to *out_len.  Caller frees with free().
static uint8_t *read_host_file(const char *path, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!path || !*path)
        return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz_signed = ftell(fp);
    if (sz_signed < 0) {
        fclose(fp);
        return NULL;
    }
    size_t sz = (size_t)sz_signed;
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    uint8_t *buf = malloc(sz > 0 ? sz : 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (sz > 0 && fread(buf, 1, sz, fp) != sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    if (out_len)
        *out_len = sz;
    return buf;
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
// dump_identify — one-line summary
// ============================================================================

bool dump_identify(const uint8_t *data_bytes, size_t data_len, const uint8_t *rsrc_bytes, size_t rsrc_len,
                   const char *label) {
    (void)label;
    // A/UX COFF is auto-detected first; takes precedence over Mac-fork shape.
    if (coff_is_coff(data_bytes, data_len)) {
        printf("a-ux-coff: bytes=%zu\n", data_len);
        return true;
    }
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
        return true;
    }
    if (data_bytes && data_len > 0) {
        printf("data-only: data_fork=%zu\n", data_len);
        return true;
    }
    return false;
}

// ============================================================================
// dump_run — full extract of data fork, finder info, and every resource
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
static int dump_manifest(const struct rfork *rf, const char *src_label, const char *dst_dir, size_t data_len,
                         size_t rsrc_len, const uint8_t *finder_info, size_t finder_info_len);
static void dump_readme(const struct rfork *rf, const char *src_label, const char *dst_dir, size_t data_len,
                        size_t rsrc_len, const uint8_t *finder_info, size_t finder_info_len, int total_resources,
                        int disasm_written, int decoded_written);

int dump_run(const uint8_t *data_bytes, size_t data_len, const uint8_t *rsrc_bytes, size_t rsrc_len,
             const uint8_t *finf_bytes, size_t finf_len, const char *src_label, const char *dst_dir, uint32_t flags) {
    if (!dst_dir || !*dst_dir)
        return -EINVAL;
    if (!src_label)
        src_label = "(unnamed)";
    if (re_mkdir_p(dst_dir) != 0) {
        fprintf(stderr, "dump: cannot create output directory '%s': %s\n", dst_dir, strerror(errno));
        return -EIO;
    }
    // Refuse to overwrite a non-empty existing dir unless --force is set.
    // Empty dirs (including freshly mkdir'd ones) are fine.
    if (!(flags & DUMP_FORCE)) {
        // A quick non-empty check via stat on the data.bin we're about to
        // create — if it already exists, treat the dir as non-empty.
        char probe[PATH_MAX];
        snprintf(probe, sizeof(probe), "%s/data.bin", dst_dir);
        struct stat st;
        if (stat(probe, &st) == 0) {
            fprintf(stderr,
                    "dump: refusing to overwrite existing dump at '%s' (pass --force "
                    "to override)\n",
                    dst_dir);
            return -EEXIST;
        }
    }

    // Auto-detect A/UX COFF binaries.  These are plain files (no
    // resource fork), so the entire dump shape differs from the Mac
    // path — different sections list, no decoded/, no manifest fork
    // sizes.  Dispatch into coff_dump.c which produces the COFF dump
    // layout described in coff_dump.h.  The Mac path stays untouched
    // for any other magic.
    if (coff_is_coff(data_bytes, data_len)) {
        return re_coff_dump(data_bytes, data_len, src_label, dst_dir);
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/data.bin", dst_dir);
    if (write_blob(path, data_bytes ? data_bytes : (const uint8_t *)"", data_len) != 0)
        return -EIO;

    // Finder info sidecar — caller supplies the 32-byte blob (typically
    // read from the file's `finf` companion sidecar).  Absence is silent.
    if (finf_bytes && finf_len > 0) {
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
    int total_resources = 0;
    int disasm_written = 0;
    int decoded_written = 0;
    if (rsrc_bytes && rsrc_len > 0) {
        const char *errmsg = NULL;
        rfork_t *rf = rfork_parse(rsrc_bytes, rsrc_len, &errmsg);
        if (!rf) {
            fprintf(stderr, "dump: corrupt resource fork in '%s': %s\n", src_label, errmsg ? errmsg : "unknown");
            return -EIO;
        }
        total_resources = dump_resources(rf, dst_dir);
        if (total_resources >= 0 && !(flags & DUMP_NO_DISASM))
            disasm_written = dump_disasm(rf, dst_dir);
        if (total_resources >= 0 && !(flags & DUMP_NO_DECODE))
            decoded_written = dump_decoded(rf, dst_dir);
        // Always write the manifest — it consolidates every layer we did
        // actually run.  README.md mirrors the same data in human-readable
        // form so a reader landing in the directory cold has a clear
        // table of contents.
        dump_manifest(rf, src_label, dst_dir, data_len, rsrc_len, finf_bytes, finf_len);
        dump_readme(rf, src_label, dst_dir, data_len, rsrc_len, finf_bytes, finf_len, total_resources, disasm_written,
                    decoded_written);
        rfork_free(rf);
    }

    if (total_resources < 0 || disasm_written < 0 || decoded_written < 0)
        return -EIO;

    printf("dump: %s -> %s (%d resource%s, %d code segment%s, %d decoded)\n", src_label, dst_dir, total_resources,
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

// ---- Code-bearing types beyond CODE --------------------------------------
//
// Per proposal §2.11 — several resource types other than CODE carry 68k
// instructions, each with a type-specific header.  We disassemble those
// too so the dump covers patches, drivers, defprocs, and the System
// file's linked-patch (lpch) machinery.  The header skip per type is
// documented inline below.

// Linked-patch resource — see the System 7.1 source drop,
//   LinkedPatches/LinkedPatchLoader.a
// for the full format.  Two flavours:
//   single-ROM lpch: +0 u16 num_lpch, +2 u32 size_of_code, +6 code
//   universal lpch:  +0 u16 num_rom_addrs, +2 u16 num_jt_entries,
//                    +4 u32 size_of_code, +8 code
// We pick the layout by trying single-ROM first and seeing whether the
// declared code size fits inside the buffer; if not, we fall back to
// universal.  Either way the rest of the resource (ROM tables, packed
// jump table, patch table) follows the code — we ignore it here and
// just disassemble the code region.
static int parse_lpch(const uint8_t *bytes, size_t sz, size_t *header_bytes, size_t *code_len) {
    if (sz < 8)
        return -1;
    // Try single-ROM layout: code_size at +2.
    uint32_t code_size_a =
        ((uint32_t)bytes[2] << 24) | ((uint32_t)bytes[3] << 16) | ((uint32_t)bytes[4] << 8) | bytes[5];
    if (code_size_a > 0 && code_size_a <= sz - 6) {
        *header_bytes = 6;
        *code_len = code_size_a;
        return 0;
    }
    // Try universal layout: code_size at +4.
    uint32_t code_size_b =
        ((uint32_t)bytes[4] << 24) | ((uint32_t)bytes[5] << 16) | ((uint32_t)bytes[6] << 8) | bytes[7];
    if (code_size_b > 0 && code_size_b <= sz - 8) {
        *header_bytes = 8;
        *code_len = code_size_b;
        return 0;
    }
    return -1;
}

// Generic code-bearing-resource disassembler — used for all non-CODE
// types in g_code_bearing.  `header_label` is the file-header comment;
// `header_bytes` is the offset where actual instructions begin; `code_len`
// is the number of bytes to disassemble (defaults to the rest of the
// resource when set to (size_t)-1).
static void write_generic_code_listing(FILE *fp, const char *type_str, int16_t id, const char *header_label,
                                       const uint8_t *bytes, size_t sz, size_t header_bytes, size_t code_len,
                                       re_symbols_t *symbols) {
    fprintf(fp, "; ============================================================\n");
    fprintf(fp, "; %s %d\n", type_str, id);
    if (header_label)
        fprintf(fp, "; %s\n", header_label);
    fprintf(fp, "; header_bytes=%zu  code_bytes=%zu  resource_bytes=%zu\n", header_bytes, code_len, sz);
    fprintf(fp, "; ============================================================\n\n");
    if (header_bytes > sz) {
        fprintf(fp, "; resource shorter than its declared header — bailing out\n");
        return;
    }
    if (code_len == (size_t)-1 || header_bytes + code_len > sz)
        code_len = sz - header_bytes;
    re_annotate_ctx_t ctx = {.jt = NULL, .code_id = id, .symbols = symbols};
    re_annotate_disasm_write(fp, bytes + header_bytes, code_len, (uint32_t)header_bytes, RE_DISASM_ALL, &ctx);
}

// Table of code-bearing types we treat as disassembly fodder.  Per
// proposal §2.11 — `CODE` gets full near/far-model parsing + jump-table
// xref handling (the existing path); `lpch` parses the linked-patches
// header to find the code region; `DRVR` skips an 18-byte fixed header
// plus the driver-name pstring before disassembling.  Everything else
// disassembles from offset 0 with a single `entry` label — that's the
// generic fallback the proposal endorses for unrecognised code-bearing
// types.
//
// Lowercase variants (`dcmp`, `boot`, `lmem`, `lmgr`, `mcky`, `proc`,
// `scod`, `snth`, `ptch`) cover the System-file's wide spread of
// patch / dispatch / boot code.
typedef enum {
    CBKIND_CODE,
    CBKIND_LPCH,
    CBKIND_DRVR,
    CBKIND_GENERIC,
} cbkind_t;

typedef struct {
    uint8_t cc[4];
    cbkind_t kind;
    const char *header_label;
} cbtype_t;

static const cbtype_t g_code_bearing[] = {
    // 'CODE' takes the dedicated path; we still list it so the iteration
    // below stays uniform.
    {{'C', 'O', 'D', 'E'}, CBKIND_CODE,    NULL                                                                          },
    {{'l', 'p', 'c', 'h'}, CBKIND_LPCH,    "linked-patch resource (per LinkedPatchLoader.a §lpch format)"               },
    {{'D', 'R', 'V', 'R'}, CBKIND_DRVR,    "driver — 18-byte header + Pascal name; entry table at offsets 6/8/10/12/14"},
    {{'I', 'N', 'I', 'T'}, CBKIND_GENERIC, "INIT — code starts at offset 0"                                            },
    {{'M', 'D', 'E', 'F'}, CBKIND_GENERIC, "menu-definition proc"                                                        },
    {{'M', 'B', 'D', 'F'}, CBKIND_GENERIC, "menu-bar definition"                                                         },
    {{'W', 'D', 'E', 'F'}, CBKIND_GENERIC, "window-definition proc"                                                      },
    {{'C', 'D', 'E', 'F'}, CBKIND_GENERIC, "control-definition proc"                                                     },
    {{'L', 'D', 'E', 'F'}, CBKIND_GENERIC, "list-definition proc"                                                        },
    {{'P', 'A', 'C', 'K'}, CBKIND_GENERIC, "standard package (selector-switch dispatcher at offset 0)"                   },
    {{'F', 'K', 'E', 'Y'}, CBKIND_GENERIC, "function-key code"                                                           },
    {{'A', 'D', 'B', 'S'}, CBKIND_GENERIC, "ADB service routine"                                                         },
    {{'X', 'C', 'M', 'D'}, CBKIND_GENERIC, "HyperCard external command"                                                  },
    {{'X', 'F', 'C', 'N'}, CBKIND_GENERIC, "HyperCard external function"                                                 },
    {{'d', 'c', 'm', 'p'}, CBKIND_GENERIC, "resource decompressor"                                                       },
    {{'P', 'T', 'C', 'H'}, CBKIND_GENERIC, "ROM patch (legacy format)"                                                   },
    {{'p', 't', 'c', 'h'}, CBKIND_GENERIC, "ROM patch"                                                                   },
    {{'s', 'c', 'o', 'd'}, CBKIND_GENERIC, "system code module"                                                          },
    {{'b', 'o', 'o', 't'}, CBKIND_GENERIC, "boot block"                                                                  },
    {{'l', 'm', 'g', 'r'}, CBKIND_GENERIC, "low-memory manager"                                                          },
    {{'l', 'm', 'e', 'm'}, CBKIND_GENERIC, "low-memory code"                                                             },
    {{'m', 'c', 'k', 'y'}, CBKIND_GENERIC, "mouse cursor / Mickey code"                                                  },
    {{'p', 'r', 'o', 'c'}, CBKIND_GENERIC, "generic procedure"                                                           },
    {{'s', 'n', 't', 'h'}, CBKIND_GENERIC, "Sound Manager synthesiser"                                                   },
    {{'c', 'd', 'e', 'v'}, CBKIND_GENERIC, "Control Panel device"                                                        },
    {{'d', 'c', 'm', 'd'}, CBKIND_GENERIC, "MacsBug dcmd"                                                                },
};
static const size_t g_code_bearing_count = sizeof(g_code_bearing) / sizeof(g_code_bearing[0]);

static const cbtype_t *find_cbtype(const uint8_t cc[4]) {
    for (size_t i = 0; i < g_code_bearing_count; i++) {
        if (memcmp(g_code_bearing[i].cc, cc, 4) == 0)
            return &g_code_bearing[i];
    }
    return NULL;
}

// Write disasm/CODE-NNNN.s for every CODE id != 0 (and disasm/jump-table.s
// for CODE 0), plus disasm/<TYPE>-NNNN.s for every other code-bearing
// type in g_code_bearing, plus a consolidated symbols.txt at dst_dir's
// root.  Returns the number of disasm files written, or -1 on failure.
static int dump_disasm(const rfork_t *rf, const char *dst_dir) {
    static const uint8_t code_cc[4] = {'C', 'O', 'D', 'E'};
    // Count how many code-bearing resources we'll process.  Zero of any
    // type means no disasm dir at all.
    size_t total_cb = 0;
    for (size_t i = 0; i < g_code_bearing_count; i++)
        total_cb += rfork_num_resources(rf, g_code_bearing[i].cc);
    if (total_cb == 0)
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
    // Iterate every registered code-bearing type.  For each type we
    // walk every resource ID, parse the type-specific header (if any),
    // and write a per-resource .s file.
    for (size_t cb_i = 0; cb_i < g_code_bearing_count; cb_i++) {
        const cbtype_t *ct = &g_code_bearing[cb_i];
        size_t n_res = rfork_num_resources(rf, ct->cc);
        if (n_res == 0)
            continue;
        char type_path[16];
        rfork_type_to_path(ct->cc, type_path, sizeof(type_path));
        // Sanitise for use in a filename — replace spaces with '_'.
        char type_path_fs[16];
        for (size_t k = 0; k < sizeof(type_path_fs); k++) {
            char c = type_path[k];
            type_path_fs[k] = (c == ' ') ? '_' : c;
            if (c == '\0')
                break;
        }
        for (size_t r = 0; r < n_res; r++) {
            int16_t id = rfork_id_at(rf, ct->cc, r);
            const uint8_t *bytes = NULL;
            size_t sz = 0;
            if (rfork_lookup(rf, ct->cc, id, &bytes, &sz, NULL, NULL) < 0)
                continue;
            char path[PATH_MAX];
            if (ct->kind == CBKIND_CODE && id == 0)
                n = snprintf(path, sizeof(path), "%s/jump-table.s", dir);
            else
                n = snprintf(path, sizeof(path), "%s/%s-%04d.s", dir, type_path_fs, (int)id);
            if (n < 0 || (size_t)n >= sizeof(path))
                goto fail;
            FILE *fp = open_out_file(path);
            if (!fp)
                goto fail;
            switch (ct->kind) {
            case CBKIND_CODE:
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
                break;
            case CBKIND_LPCH: {
                size_t hb = 0, cl = 0;
                if (parse_lpch(bytes, sz, &hb, &cl) == 0) {
                    write_generic_code_listing(fp, type_path, id, ct->header_label, bytes, sz, hb, cl, &symbols);
                } else {
                    fprintf(fp, "; lpch %d — could not detect single-ROM / universal header layout\n", id);
                    fprintf(fp, "; disassembling from offset 0; expect garbage in the header bytes\n");
                    write_generic_code_listing(fp, type_path, id, ct->header_label, bytes, sz, 0, (size_t)-1, &symbols);
                }
                break;
            }
            case CBKIND_DRVR: {
                // 18-byte fixed header + driver name pstring (padded to
                // even length), then code.  We extract the name into a
                // comment but disasm from past the name; the per-entry
                // offsets in the header give callable entry points.
                if (sz < 19) {
                    fprintf(fp, "; DRVR %d too short for header\n", id);
                    break;
                }
                uint8_t pn = bytes[18];
                size_t name_end = 19 + pn;
                if (name_end & 1)
                    name_end++;
                if (name_end > sz)
                    name_end = sz;
                char name[64] = {0};
                size_t copy = pn < sizeof(name) - 1 ? pn : sizeof(name) - 1;
                memcpy(name, bytes + 19, copy);
                char hdr[160];
                snprintf(hdr, sizeof(hdr), "%s, driver name='%s'", ct->header_label, name);
                write_generic_code_listing(fp, type_path, id, hdr, bytes, sz, name_end, (size_t)-1, &symbols);
                break;
            }
            case CBKIND_GENERIC:
                write_generic_code_listing(fp, type_path, id, ct->header_label, bytes, sz, 0, (size_t)-1, &symbols);
                break;
            }
            fclose(fp);
            written++;
        }
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
// dump_disasm_code — disassemble one CODE resource to dst_file (or stdout)
// ============================================================================

int dump_disasm_code(const uint8_t *rsrc_bytes, size_t rsrc_len, int code_id, const char *dst_file) {
    if (!rsrc_bytes || rsrc_len == 0)
        return -EINVAL;
    if (code_id < INT16_MIN || code_id > INT16_MAX)
        return -EINVAL;

    const char *errmsg = NULL;
    rfork_t *rf = rfork_parse(rsrc_bytes, rsrc_len, &errmsg);
    if (!rf) {
        fprintf(stderr, "dump: corrupt resource fork: %s\n", errmsg ? errmsg : "unknown");
        return -EIO;
    }
    const uint8_t code_cc[4] = {'C', 'O', 'D', 'E'};
    const uint8_t *bytes = NULL;
    size_t sz = 0;
    if (rfork_lookup(rf, code_cc, (int16_t)code_id, &bytes, &sz, NULL, NULL) < 0) {
        fprintf(stderr, "dump: no CODE %d in resource fork\n", code_id);
        rfork_free(rf);
        return -ENOENT;
    }

    FILE *fp = (dst_file && *dst_file) ? open_out_file(dst_file) : stdout;
    if (!fp) {
        rfork_free(rf);
        return -EIO;
    }
    // For a single-segment call we still want JT-xref annotation, so
    // try to fetch and parse CODE 0 from the same resource fork.  Symbol
    // recovery runs locally for this segment only (no shared symbols.txt).
    re_jt_table_t jt;
    bool jt_valid = false;
    if (code_id != 0) {
        const uint8_t *c0_bytes = NULL;
        size_t c0_sz = 0;
        if (rfork_lookup(rf, code_cc, 0, &c0_bytes, &c0_sz, NULL, NULL) == 0) {
            if (re_parse_code0(c0_bytes, c0_sz, &jt) == 0)
                jt_valid = true;
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
    rfork_free(rf);
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
static int dump_manifest(const rfork_t *rf, const char *src_label, const char *dst_dir, size_t data_len,
                         size_t rsrc_len, const uint8_t *finder_info, size_t finder_info_len) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/manifest.json", dst_dir);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "re: cannot create '%s': %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "{\n  \"schema_version\": 1,\n  \"generator\": \"granny-smith dump v1\",\n");
    fprintf(fp, "  \"source\": {\n    \"label\": ");
    extern void re_json_write_string(FILE *, const char *);
    re_json_write_string(fp, src_label);
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

static void dump_readme(const struct rfork *rf, const char *src_label, const char *dst_dir, size_t data_len,
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
    fprintf(fp, "(see [docs/storage.md](../docs/storage.md) §11 for the convention).\n");
    fprintf(fp, "Every directory here is a layer of progressively-decoded\n");
    fprintf(fp, "data — raw bytes at the bottom, JSON / `.s` listings on top.\n\n");

    fprintf(fp, "## Source\n\n");
    fprintf(fp, "| Field | Value |\n|---|---|\n");
    fprintf(fp, "| Source | `%s` |\n", src_label ? src_label : "?");
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
    fprintf(fp, "Re-run any time with `dump --rsrc <file> <dir>` (pass `--force` to overwrite,\n");
    fprintf(fp, "`--no-decode` / `--no-disasm` to skip those layers).\n");

    fclose(fp);
}

// ============================================================================
// dump_decode_one — single-resource decode to dst_file (or stdout)
// ============================================================================

int dump_decode_one(const uint8_t *rsrc_bytes, size_t rsrc_len, const char *type_str, int id, const char *dst_file) {
    if (!rsrc_bytes || rsrc_len == 0 || !type_str || !*type_str)
        return -EINVAL;
    uint8_t type[4];
    if (rfork_type_from_path(type_str, type) != 0)
        return -EINVAL;
    if (!re_decode_dispatch_name(type))
        return -ENOENT;
    if (id < INT16_MIN || id > INT16_MAX)
        return -EINVAL;

    const char *errmsg = NULL;
    rfork_t *rf = rfork_parse(rsrc_bytes, rsrc_len, &errmsg);
    if (!rf) {
        fprintf(stderr, "dump: corrupt resource fork: %s\n", errmsg ? errmsg : "unknown");
        return -EIO;
    }
    const uint8_t *bytes = NULL;
    size_t sz = 0;
    if (rfork_lookup(rf, type, (int16_t)id, &bytes, &sz, NULL, NULL) < 0) {
        rfork_free(rf);
        return -ENOENT;
    }
    FILE *fp = (dst_file && *dst_file) ? fopen(dst_file, "wb") : stdout;
    if (!fp) {
        rfork_free(rf);
        return -EIO;
    }
    int rc = re_decode_dispatch(type, bytes, sz, fp, NULL);
    if (fp != stdout)
        fclose(fp);
    rfork_free(rf);
    return (rc > 0) ? 0 : -EIO;
}

// ============================================================================
// CLI
// ============================================================================

static void print_usage(const char *progname) {
    fprintf(stderr,
            "Usage:\n"
            "  %s [--data <data-file>] --rsrc <rsrc-file> [--finf <finf-file>] [<dst-dir>]\n"
            "      Full dump of a classic-Mac forked file.  --rsrc is required; --data\n"
            "      and --finf are optional (data fork and 32-byte Finder info sidecar).\n"
            "      <dst-dir> defaults to './dump-out'.\n"
            "\n"
            "  %s --coff <coff-file> [<dst-dir>]\n"
            "      Full dump of an A/UX COFF binary.  COFF magic (0x0150) is also\n"
            "      auto-detected when the file is passed via --data without --rsrc.\n"
            "\n"
            "  %s --identify --rsrc <rsrc-file> [--data <data-file>]\n"
            "  %s --identify --coff <coff-file>\n"
            "      Print a one-line identify summary and exit.\n"
            "\n"
            "  %s --disasm-code <id> --rsrc <rsrc-file> [-o <out-file>]\n"
            "      Disassemble one CODE resource by ID (0 = jump table).  Streams to\n"
            "      stdout unless -o is given.\n"
            "\n"
            "  %s --decode <type>:<id> --rsrc <rsrc-file> [-o <out-file>]\n"
            "      Run the per-type decoder on one resource (e.g. --decode vers:1).\n"
            "      Streams JSON to stdout unless -o is given.\n"
            "\n"
            "Behaviour flags (full-dump mode only):\n"
            "  --no-decode, -D    Skip per-type decoders + decoded/ output\n"
            "  --no-disasm, -S    Skip CODE disassembly + symbols.txt\n"
            "  --force,     -f    Overwrite an existing non-empty <dst-dir>\n"
            "\n"
            "  -h, --help         Show this help and exit\n",
            progname, progname, progname, progname, progname, progname);
}

typedef enum {
    MODE_DUMP = 0,
    MODE_IDENTIFY,
    MODE_DISASM_CODE,
    MODE_DECODE,
} cli_mode_t;

int main(int argc, char *argv[]) {
    const char *data_path = NULL;
    const char *rsrc_path = NULL;
    const char *finf_path = NULL;
    const char *coff_path = NULL;
    const char *out_file = NULL;
    cli_mode_t mode = MODE_DUMP;
    int disasm_id = 0;
    char decode_type[16] = {0};
    int decode_id = 0;
    uint32_t flags = 0;

    enum {
        OPT_DATA = 1000,
        OPT_RSRC,
        OPT_FINF,
        OPT_COFF,
        OPT_IDENTIFY,
        OPT_DISASM_CODE,
        OPT_DECODE,
        OPT_NO_DECODE,
        OPT_NO_DISASM,
        OPT_FORCE,
    };
    static struct option long_options[] = {
        {"data",        required_argument, NULL, OPT_DATA       },
        {"rsrc",        required_argument, NULL, OPT_RSRC       },
        {"finf",        required_argument, NULL, OPT_FINF       },
        {"coff",        required_argument, NULL, OPT_COFF       },
        {"identify",    no_argument,       NULL, OPT_IDENTIFY   },
        {"disasm-code", required_argument, NULL, OPT_DISASM_CODE},
        {"decode",      required_argument, NULL, OPT_DECODE     },
        {"no-decode",   no_argument,       NULL, OPT_NO_DECODE  },
        {"no-disasm",   no_argument,       NULL, OPT_NO_DISASM  },
        {"force",       no_argument,       NULL, OPT_FORCE      },
        {"output",      required_argument, NULL, 'o'            },
        {"help",        no_argument,       NULL, 'h'            },
        {NULL,          0,                 NULL, 0              },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "o:DSfh", long_options, NULL)) != -1) {
        switch (opt) {
        case OPT_DATA:
            data_path = optarg;
            break;
        case OPT_RSRC:
            rsrc_path = optarg;
            break;
        case OPT_FINF:
            finf_path = optarg;
            break;
        case OPT_COFF:
            coff_path = optarg;
            break;
        case OPT_IDENTIFY:
            mode = MODE_IDENTIFY;
            break;
        case OPT_DISASM_CODE:
            mode = MODE_DISASM_CODE;
            disasm_id = (int)strtol(optarg, NULL, 0);
            break;
        case OPT_DECODE: {
            mode = MODE_DECODE;
            const char *colon = strchr(optarg, ':');
            if (!colon) {
                fprintf(stderr, "dump: --decode expects <type>:<id> (e.g. vers:1)\n");
                return 2;
            }
            size_t tlen = (size_t)(colon - optarg);
            if (tlen == 0 || tlen >= sizeof(decode_type)) {
                fprintf(stderr, "dump: --decode type too long\n");
                return 2;
            }
            memcpy(decode_type, optarg, tlen);
            decode_type[tlen] = '\0';
            decode_id = (int)strtol(colon + 1, NULL, 0);
            break;
        }
        case OPT_NO_DECODE:
        case 'D':
            flags |= DUMP_NO_DECODE;
            break;
        case OPT_NO_DISASM:
        case 'S':
            flags |= DUMP_NO_DISASM;
            break;
        case OPT_FORCE:
        case 'f':
            flags |= DUMP_FORCE;
            break;
        case 'o':
            out_file = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 2;
        }
    }

    const char *dst_dir = (optind < argc) ? argv[optind] : "dump-out";

    // Load whichever buffers the mode + flags require.
    uint8_t *data_buf = NULL;
    size_t data_len = 0;
    uint8_t *rsrc_buf = NULL;
    size_t rsrc_len = 0;
    uint8_t *finf_buf = NULL;
    size_t finf_len = 0;
    uint8_t *coff_buf = NULL;
    size_t coff_len = 0;

    if (data_path) {
        data_buf = read_host_file(data_path, &data_len);
        if (!data_buf) {
            fprintf(stderr, "dump: cannot read --data '%s': %s\n", data_path, strerror(errno));
            return 1;
        }
    }
    if (rsrc_path) {
        rsrc_buf = read_host_file(rsrc_path, &rsrc_len);
        if (!rsrc_buf) {
            fprintf(stderr, "dump: cannot read --rsrc '%s': %s\n", rsrc_path, strerror(errno));
            free(data_buf);
            return 1;
        }
    }
    if (finf_path) {
        finf_buf = read_host_file(finf_path, &finf_len);
        if (!finf_buf) {
            fprintf(stderr, "dump: cannot read --finf '%s': %s\n", finf_path, strerror(errno));
            free(data_buf);
            free(rsrc_buf);
            return 1;
        }
    }
    if (coff_path) {
        coff_buf = read_host_file(coff_path, &coff_len);
        if (!coff_buf) {
            fprintf(stderr, "dump: cannot read --coff '%s': %s\n", coff_path, strerror(errno));
            free(data_buf);
            free(rsrc_buf);
            free(finf_buf);
            return 1;
        }
    }

    // COFF takes precedence: --coff or a --data file whose magic identifies it.
    const uint8_t *id_data = coff_buf ? coff_buf : data_buf;
    size_t id_data_len = coff_buf ? coff_len : data_len;
    const char *src_label = coff_path ? coff_path : (data_path ? data_path : (rsrc_path ? rsrc_path : "(unnamed)"));

    int rc = 0;
    switch (mode) {
    case MODE_IDENTIFY:
        rc = dump_identify(id_data, id_data_len, rsrc_buf, rsrc_len, src_label) ? 0 : 1;
        break;
    case MODE_DISASM_CODE:
        if (!rsrc_buf) {
            fprintf(stderr, "dump: --disasm-code requires --rsrc\n");
            rc = 2;
            break;
        }
        rc = dump_disasm_code(rsrc_buf, rsrc_len, disasm_id, out_file);
        if (rc < 0)
            rc = 1;
        break;
    case MODE_DECODE:
        if (!rsrc_buf) {
            fprintf(stderr, "dump: --decode requires --rsrc\n");
            rc = 2;
            break;
        }
        rc = dump_decode_one(rsrc_buf, rsrc_len, decode_type, decode_id, out_file);
        if (rc < 0)
            rc = 1;
        break;
    case MODE_DUMP:
        if (!coff_buf && !rsrc_buf && !coff_is_coff(data_buf, data_len)) {
            fprintf(stderr, "dump: nothing to dump — pass --rsrc, --coff, or a COFF file via --data\n");
            print_usage(argv[0]);
            rc = 2;
            break;
        }
        rc = dump_run(id_data, id_data_len, rsrc_buf, rsrc_len, finf_buf, finf_len, src_label, dst_dir, flags);
        if (rc < 0)
            rc = 1;
        break;
    }

    free(data_buf);
    free(rsrc_buf);
    free(finf_buf);
    free(coff_buf);
    return rc;
}
