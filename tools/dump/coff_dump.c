// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// coff_dump.c
// re.dump path for A/UX COFF binaries.  See coff_dump.h.
//
// The output shape deliberately mirrors the Mac path so downstream
// tooling — manifest.json consumers, diff scripts, etc. — sees the
// same conventions:
//   - schema_version=1 manifest with a "source" block
//   - a sections[] / symbols[] roll-up that maps each artefact to its
//     on-disk file path
//   - a hand-readable README at the top
// Differences from the Mac path are confined to the disasm pass:
//   - no resource fork / data fork distinction (just the file)
//   - no $Axxx trap names, no jump-table xrefs, no low-mem globals
//   - PLUS A/UX TRAP #0 syscall-name annotation
//   - PLUS symbol-table-driven labels at every defined symbol
//     (the COFF symbol table is unstripped so this is "free")

#include "coff_dump.h"

#include "annotate_disasm.h"
#include "coff.h"
#include "symbols.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

// Local copy of the mkdir-p helper used by re.c — coff_dump is a
// separate translation unit, and pulling re.c's static helper would
// require a wider refactor.  Same shape (idempotent on EEXIST).
static int cd_mkdir_p(const char *path) {
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

static int cd_write_blob(const char *path, const void *bytes, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "re: cannot create '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (len > 0 && fwrite(bytes, 1, len, fp) != len) {
        fprintf(stderr, "re: write error on '%s': %s\n", path, strerror(errno));
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

// Filesystem-safe version of a section name (replace anything outside
// the typical ASCII-name set with '_'; collapse the leading dot the
// Unix linker emits so "/disasm/.text.s" doesn't look like a hidden
// file to ls).
static void cd_safe_name(const char *src, char *dst, size_t cap) {
    size_t i = 0;
    if (cap == 0)
        return;
    while (src[i] && i + 1 < cap) {
        char c = src[i];
        if (c == '.' && i == 0) {
            dst[i] = '_'; // leading dot -> underscore so it isn't "hidden"
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.') {
            dst[i] = c;
        } else {
            dst[i] = '_';
        }
        i++;
    }
    dst[i] = '\0';
}

// Storage-class name (for symbols.txt).  Only the ones we actually
// emit get a name; everything else shows as "class<N>".
static const char *cd_sclass_name(uint8_t sclass) {
    switch (sclass) {
    case COFF_C_EXT:
        return "external";
    case COFF_C_STAT:
        return "static";
    case COFF_C_LABEL:
        return "label";
    case COFF_C_AUTO:
        return "auto";
    case COFF_C_REG:
        return "register";
    default:
        return "other";
    }
}

// Should we surface this symbol in symbols.txt + as a disasm label?
// Yes for defined external/static symbols and labels with a non-zero
// section; no for debugging / aux records (already filtered out by the
// parser via NULL slots) and undefined references.
static bool cd_keep_symbol(const coff_symbol_t *s) {
    if (!s)
        return false;
    if (s->scnum <= 0)
        return false;
    if (s->name[0] == '\0')
        return false;
    switch (s->sclass) {
    case COFF_C_EXT:
    case COFF_C_STAT:
    case COFF_C_LABEL:
        return true;
    default:
        return false;
    }
}

// Section flag formatter (joined string for manifest).
static void cd_format_flags(uint32_t flags, char *out, size_t cap) {
    out[0] = '\0';
    bool first = true;
#define APPEND(flag, label)                                                                                            \
    do {                                                                                                               \
        if (flags & (flag)) {                                                                                          \
            size_t off = strlen(out);                                                                                  \
            snprintf(out + off, cap - off, "%s%s", first ? "" : "|", (label));                                         \
            first = false;                                                                                             \
        }                                                                                                              \
    } while (0)
    APPEND(COFF_STYP_TEXT, "TEXT");
    APPEND(COFF_STYP_DATA, "DATA");
    APPEND(COFF_STYP_BSS, "BSS");
    APPEND(COFF_STYP_INFO, "INFO");
    APPEND(COFF_STYP_NOLOAD, "NOLOAD");
    APPEND(COFF_STYP_LOADER, "LOADER");
    APPEND(COFF_STYP_LIB, "LIB");
#undef APPEND
    if (out[0] == '\0')
        snprintf(out, cap, "0x%x", flags);
}

// Sort comparator for symbol table output (by vaddr, then name).
static int cd_cmp_symbol(const void *a, const void *b) {
    const coff_symbol_t *const *sa = a;
    const coff_symbol_t *const *sb = b;
    if ((*sa)->value < (*sb)->value)
        return -1;
    if ((*sa)->value > (*sb)->value)
        return 1;
    return strcmp((*sa)->name, (*sb)->name);
}

// ===========================================================================
// Disassembly pass — one .s file per executable section
// ===========================================================================
//
// We populate a re_symbols_t from the COFF symbol table, mapping each
// kept symbol's vaddr -> name, then run re_annotate_disasm_write with
// the RE_DISASM_AUX flag set.  The annotator emits "<name>:" labels at
// every symbol-table-resolvable address, branch destinations as
// "; -> $addr", and TRAP #0 sites as "; syscall <name>".

static int cd_disasm_section(const coff_t *cf, const coff_section_t *s, FILE *fp, re_symbols_t *symbols) {
    fprintf(fp, "; ============================================================\n");
    fprintf(fp, "; section %s\n", s->name);
    fprintf(fp, "; vaddr=0x%08x  size=%u (0x%x)  flags=0x%x\n", s->vaddr, s->size, s->size, s->flags);
    fprintf(fp, "; ============================================================\n\n");
    const uint8_t *bytes = coff_section_data(cf, s);
    if (!bytes) {
        fprintf(fp, "; (no raw data — bss or stripped)\n");
        return 0;
    }
    // Use the section's virtual address as the base so symbol-table
    // values (which are also VAs) line up with the labels the
    // annotator emits.  The `code_id` slot is overloaded here: we
    // only have one effective "section id" per disasm, so we set it
    // to 0 and store all symbols under code_id=0.
    re_annotate_ctx_t ctx = {.jt = NULL, .code_id = 0, .symbols = symbols};
    re_annotate_disasm_write(fp, bytes, s->size, s->vaddr, RE_DISASM_AUX, &ctx);
    return 0;
}

// ===========================================================================
// Manifest writer
// ===========================================================================

static void cd_write_string_json(FILE *fp, const char *s) {
    fputc('"', fp);
    if (!s) {
        fputc('"', fp);
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            fputc('\\', fp);
            fputc(c, fp);
        } else if (c < 0x20) {
            fprintf(fp, "\\u%04x", c);
        } else {
            fputc(c, fp);
        }
    }
    fputc('"', fp);
}

static int cd_write_manifest(const coff_t *cf, const char *vfs_path, const char *dst_dir, size_t file_size,
                             size_t n_kept_symbols) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/manifest.json", dst_dir);
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;

    fprintf(fp, "{\n  \"schema_version\": 1,\n  \"generator\": \"granny-smith re v1\",\n");
    fprintf(fp, "  \"format\": \"coff-m68k\",\n");
    fprintf(fp, "  \"source\": {\n    \"vfs_path\": ");
    cd_write_string_json(fp, vfs_path);
    fprintf(fp, ",\n    \"file_size\": %zu,\n", file_size);
    fprintf(fp, "    \"magic\": \"0x%04x\",\n", coff_magic(cf));
    fprintf(fp, "    \"flags\": \"0x%04x\",\n", coff_flags(cf));
    fprintf(fp, "    \"timestamp\": %u\n  },\n", coff_timestamp(cf));
    fprintf(fp,
            "  \"a_out\": {\"text_size\": %u, \"data_size\": %u, \"bss_size\": %u, "
            "\"entry\": \"0x%08x\", \"text_start\": \"0x%08x\", \"data_start\": \"0x%08x\"},\n",
            coff_text_size(cf), coff_data_size(cf), coff_bss_size(cf), coff_entry_point(cf), coff_text_start(cf),
            coff_data_start(cf));

    fprintf(fp, "  \"sections\": [\n");
    size_t nsec = coff_num_sections(cf);
    for (size_t i = 0; i < nsec; i++) {
        const coff_section_t *s = coff_section_at(cf, i);
        char fs_name[64];
        cd_safe_name(s->name, fs_name, sizeof(fs_name));
        char flag_str[64];
        cd_format_flags(s->flags, flag_str, sizeof(flag_str));
        bool exec = (s->flags & COFF_STYP_TEXT) != 0;
        fprintf(fp, "    {\"index\": %zu, \"name\": ", i + 1);
        cd_write_string_json(fp, s->name);
        fprintf(fp, ", \"vaddr\": \"0x%08x\", \"size\": %u, \"flags\": ", s->vaddr, s->size);
        cd_write_string_json(fp, flag_str);
        fprintf(fp, ", \"files\": {\"bin\": \"sections/%s\", \"disasm\": ", fs_name);
        if (exec) {
            fprintf(fp, "\"disasm/%s.s\"}}", fs_name);
        } else {
            fprintf(fp, "null}}");
        }
        fprintf(fp, "%s\n", i + 1 < nsec ? "," : "");
    }
    fprintf(fp, "  ],\n");
    fprintf(fp, "  \"symbols\": {\"total\": %zu, \"kept\": %zu, \"file\": \"symbols.txt\"}\n", coff_num_symbols(cf),
            n_kept_symbols);
    fprintf(fp, "}\n");
    fclose(fp);
    return 0;
}

// ===========================================================================
// README writer (mirrors the Mac dump's README)
// ===========================================================================

static void cd_write_readme(const coff_t *cf, const char *vfs_path, const char *dst_dir, size_t file_size,
                            size_t n_kept_symbols) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/README.md", dst_dir);
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return;

    fprintf(fp, "# re.dump — exploded A/UX COFF binary\n\n");
    fprintf(fp, "Self-contained dump of an A/UX (Common Object File Format, M68K) executable\n");
    fprintf(fp, "produced by `re.dump`.  Same broad layout as the Mac-binary dumps under\n");
    fprintf(fp, "this tool, but tailored for COFF: sections instead of resources, an\n");
    fprintf(fp, "unstripped symbol table instead of MacsBug name heuristics, and TRAP #0\n");
    fprintf(fp, "syscall annotation in place of $Axxx Mac-toolbox traps.\n\n");

    fprintf(fp, "## Source\n\n");
    fprintf(fp, "| Field | Value |\n|---|---|\n");
    fprintf(fp, "| Path | `%s` |\n", vfs_path ? vfs_path : "?");
    fprintf(fp, "| File size | %zu bytes |\n", file_size);
    fprintf(fp, "| COFF magic | `0x%04x` (M68K) |\n", coff_magic(cf));
    fprintf(fp, "| Flags | `0x%04x` |\n", coff_flags(cf));
    fprintf(fp, "| Build timestamp | %u (unix epoch) |\n", coff_timestamp(cf));
    fprintf(fp, "| Text size | %u bytes |\n", coff_text_size(cf));
    fprintf(fp, "| Data size | %u bytes |\n", coff_data_size(cf));
    fprintf(fp, "| BSS size | %u bytes |\n", coff_bss_size(cf));
    fprintf(fp, "| Entry point | `0x%08x` |\n", coff_entry_point(cf));
    fprintf(fp, "| Text base address | `0x%08x` |\n", coff_text_start(cf));
    fprintf(fp, "| Data base address | `0x%08x` |\n", coff_data_start(cf));
    fprintf(fp, "| Sections | %zu |\n", coff_num_sections(cf));
    fprintf(fp, "| Symbols (raw / kept) | %zu / %zu |\n\n", coff_num_symbols(cf), n_kept_symbols);

    fprintf(fp, "## Directory layout\n\n");
    fprintf(fp, "| Path | What it contains |\n|---|---|\n");
    fprintf(fp, "| `data.bin` | The entire input COFF file verbatim. |\n");
    fprintf(fp, "| `manifest.json` | Machine-readable index — sections, symbol summary, per-file paths. |\n");
    fprintf(fp, "| `README.md` | This file. |\n");
    fprintf(fp, "| `sections/<name>` | Raw bytes of each section.  BSS-style sections that carry\n");
    fprintf(fp, "  no on-disk data produce an empty placeholder so the manifest path stays valid. |\n");
    fprintf(fp, "| `symbols.txt` | Every defined external / static / label symbol from the COFF\n");
    fprintf(fp, "  symbol table, sorted by virtual address: `<addr> <section> <class> <name>`. |\n");
    fprintf(fp, "| `disasm/<name>.s` | Per-section annotated 68k disassembly for every executable\n");
    fprintf(fp, "  (STYP_TEXT) section.  Output carries PC-relative branch resolution, TRAP #0\n");
    fprintf(fp, "  syscall labels (`; syscall write`), and `<symbol>:` lines at every defined\n");
    fprintf(fp, "  address — the symbol table is unstripped so every named routine is reachable. |\n\n");

    fprintf(fp, "## Sections\n\n");
    fprintf(fp, "| # | Name | VA | Size | Flags | Disasm |\n|---|---|---|---|---|---|\n");
    size_t nsec = coff_num_sections(cf);
    for (size_t i = 0; i < nsec; i++) {
        const coff_section_t *s = coff_section_at(cf, i);
        char flag_str[64];
        cd_format_flags(s->flags, flag_str, sizeof(flag_str));
        bool exec = (s->flags & COFF_STYP_TEXT) != 0;
        fprintf(fp, "| %zu | `%s` | `0x%08x` | %u | %s | %s |\n", i + 1, s->name, s->vaddr, s->size, flag_str,
                exec ? "yes" : "—");
    }
    fputc('\n', fp);

    fprintf(fp, "## A/UX disassembly conventions\n\n");
    fprintf(fp, "- The A/UX syscall ABI passes the call number in `D0` and dispatches via `TRAP #0`.\n");
    fprintf(fp, "  Our annotator looks back one instruction for `MOVEQ #N,D0` and labels the trap\n");
    fprintf(fp, "  site as `; syscall <name>` using a built-in SVR2 syscall name table.\n");
    fprintf(fp, "- Symbol values are *virtual addresses*.  Each section's `.s` file uses the\n");
    fprintf(fp, "  section's own `vaddr` as the base address, so labels line up 1:1.\n");
    fprintf(fp, "- Unstripped: every defined function gets a `<name>:` label.  Local labels\n");
    fprintf(fp, "  fall back to `sub_<offset>` only when the boundary heuristic finds a\n");
    fprintf(fp, "  function entry not in the symbol table (rare for COFF).\n");

    fclose(fp);
}

// ===========================================================================
// Public entry point
// ===========================================================================

int re_coff_dump(const uint8_t *bytes, size_t len, const char *vfs_path, const char *dst_dir) {
    if (!bytes || !dst_dir || !*dst_dir)
        return -EINVAL;
    if (cd_mkdir_p(dst_dir) != 0) {
        fprintf(stderr, "re: cannot create output directory '%s': %s\n", dst_dir, strerror(errno));
        return -EIO;
    }

    const char *err = NULL;
    coff_t *cf = coff_parse(bytes, len, &err);
    if (!cf) {
        fprintf(stderr, "re: COFF parse failed for '%s': %s\n", vfs_path ? vfs_path : "?", err ? err : "unknown");
        return -EIO;
    }

    // data.bin — round-trip the input.
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/data.bin", dst_dir);
    if (cd_write_blob(path, bytes, len) != 0) {
        coff_free(cf);
        return -EIO;
    }

    // sections/ — one file per section.
    snprintf(path, sizeof(path), "%s/sections", dst_dir);
    if (cd_mkdir_p(path) != 0) {
        coff_free(cf);
        return -EIO;
    }
    size_t nsec = coff_num_sections(cf);
    for (size_t i = 0; i < nsec; i++) {
        const coff_section_t *s = coff_section_at(cf, i);
        char fs_name[64];
        cd_safe_name(s->name, fs_name, sizeof(fs_name));
        snprintf(path, sizeof(path), "%s/sections/%s", dst_dir, fs_name);
        const uint8_t *sdata = coff_section_data(cf, s);
        if (sdata) {
            cd_write_blob(path, sdata, s->size);
        } else {
            cd_write_blob(path, NULL, 0); // 0-byte placeholder for BSS
        }
    }

    // Build a symbol table for the disassembly annotator, and write
    // symbols.txt at the same time.
    re_symbols_t symbols;
    re_symbols_init(&symbols);
    size_t n_kept = 0;

    // Collect-then-sort so symbols.txt comes out in vaddr order.
    size_t n_total = coff_num_symbols(cf);
    const coff_symbol_t **kept = calloc(n_total > 0 ? n_total : 1, sizeof(*kept));
    if (kept) {
        for (size_t i = 0; i < n_total; i++) {
            const coff_symbol_t *s = coff_symbol_at(cf, i);
            if (cd_keep_symbol(s))
                kept[n_kept++] = s;
        }
        if (n_kept > 1)
            qsort(kept, n_kept, sizeof(*kept), cd_cmp_symbol);
    }

    for (size_t i = 0; i < n_kept; i++) {
        const coff_symbol_t *s = kept[i];
        re_symbols_add(&symbols, 0, s->value, s->name,
                       s->sclass == COFF_C_EXT ? "extern" : (s->sclass == COFF_C_STAT ? "static" : "label"));
    }

    snprintf(path, sizeof(path), "%s/symbols.txt", dst_dir);
    FILE *sfp = fopen(path, "wb");
    if (sfp) {
        fprintf(sfp, "; A/UX COFF symbol table (defined symbols only)\n");
        fprintf(sfp, "; format: <hex_vaddr> <section_num> <class> <name>\n");
        for (size_t i = 0; i < n_kept; i++) {
            const coff_symbol_t *s = kept[i];
            fprintf(sfp, "0x%08x  scn=%d  %-9s  %s\n", s->value, (int)s->scnum, cd_sclass_name(s->sclass), s->name);
        }
        fclose(sfp);
    }
    free(kept);

    // disasm/ — every executable section.
    snprintf(path, sizeof(path), "%s/disasm", dst_dir);
    if (cd_mkdir_p(path) != 0) {
        re_symbols_free(&symbols);
        coff_free(cf);
        return -EIO;
    }
    int disasm_count = 0;
    for (size_t i = 0; i < nsec; i++) {
        const coff_section_t *s = coff_section_at(cf, i);
        if (!(s->flags & COFF_STYP_TEXT))
            continue;
        char fs_name[64];
        cd_safe_name(s->name, fs_name, sizeof(fs_name));
        snprintf(path, sizeof(path), "%s/disasm/%s.s", dst_dir, fs_name);
        FILE *fp = fopen(path, "wb");
        if (!fp)
            continue;
        cd_disasm_section(cf, s, fp, &symbols);
        fclose(fp);
        disasm_count++;
    }

    cd_write_manifest(cf, vfs_path, dst_dir, len, n_kept);
    cd_write_readme(cf, vfs_path, dst_dir, len, n_kept);

    printf("re.dump (coff): %s -> %s (%zu sections, %d disassembled, %zu symbols)\n", vfs_path ? vfs_path : "?",
           dst_dir, nsec, disasm_count, n_kept);

    re_symbols_free(&symbols);
    coff_free(cf);
    return 0;
}
