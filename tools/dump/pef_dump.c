// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pef_dump.c
// re.dump path for PEF code fragments.  See pef_dump.h for the layout.
//
// No load address is assumed: every address in the output is a section
// name plus an offset ("code+0x01A4", "data+0x0040"), which is also how
// the relocations are recorded.  The disassembly is annotated with what
// the fragment itself can tell us:
//   - function labels from exports, the CFM entry points (resolved through
//     their transition vectors), the names in traceback tables, cross-TOC
//     glue stubs, and otherwise-unnamed `bl` targets;
//   - every load/store/addi relative to r2 (the TOC) resolved to the TOC
//     slot it touches and, through the relocations, to what that slot
//     points at: an imported symbol, a function, or a string in data.

#include "pef_dump.h"

#include "pef.h"
#include "ppc_disasm.h"
#include "storage_util.h"
#include "decoders/decoders.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Big-endian word reader
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Label sources, best first: when two passes name the same address the
// lower-numbered source wins.
enum {
    SRC_EXPORT = 0, // exported symbol
    SRC_TRACEBACK, // name stored in a traceback table
    SRC_ENTRY, // CFM main/init/term entry point
    SRC_GLUE, // cross-TOC glue stub for an import
    SRC_FUNCTION, // traceback-table boundary without a name
    SRC_BRANCH, // otherwise-unnamed `bl` target
};

static const char *const g_src_names[] = {"export", "traceback", "entry", "glue", "function", "branch"};

// One named code address.
typedef struct {
    uint32_t off; // offset within its section
    int src; // SRC_*
    char name[160]; // label text
} pd_sym_t;

// Per-section label list.
typedef struct {
    pd_sym_t *v; // entries, sorted by offset once finalized
    size_t n, cap; // count and capacity
} pd_symtab_t;

// State shared by the writers.
typedef struct {
    const pef_t *pef; // parsed fragment
    char (*label)[24]; // short section names ("code", "data", or "data#2")
    pd_symtab_t *syms; // labels per section
    pef_tbfunc_t **tb; // traceback functions per section
    size_t *n_tb; // their counts
    bool has_toc; // a TOC base was found
    int toc_section; // section holding the TOC
    uint32_t toc_off; // offset of the TOC base (the value of r2) in it
} pd_ctx_t;

// ============================================================================
// Small helpers
// ============================================================================

// Write `len` bytes to `path`; -1 with a message on failure.
static int pd_write_blob(const char *path, const void *bytes, size_t len) {
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

// fopen for writing, with a message on failure.
static FILE *pd_open(const char *dir, const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *fp = fopen(path, "wb");
    if (!fp)
        fprintf(stderr, "re: cannot create '%s': %s\n", path, strerror(errno));
    return fp;
}

// Render an OSType ('pwpc') as text, or hex when not printable.
static void ostype_str(uint32_t v, char *out, size_t cap) {
    char c[4] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
    for (int i = 0; i < 4; i++)
        if (c[i] < 0x20 || c[i] > 0x7E) {
            snprintf(out, cap, "0x%08X", v);
            return;
        }
    snprintf(out, cap, "%.4s", c);
}

// Add a label; duplicates are resolved in pd_symtab_finish.
static void sym_add(pd_symtab_t *t, uint32_t off, int src, const char *name) {
    if (t->n == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 64;
        pd_sym_t *v = realloc(t->v, ncap * sizeof(*v));
        if (!v)
            return;
        t->v = v;
        t->cap = ncap;
    }
    pd_sym_t *s = &t->v[t->n++];
    s->off = off;
    s->src = src;
    snprintf(s->name, sizeof(s->name), "%s", name);
}

// Order labels by offset, then by source priority.
static int cmp_sym(const void *a, const void *b) {
    const pd_sym_t *x = a, *y = b;
    if (x->off != y->off)
        return x->off < y->off ? -1 : 1;
    return x->src - y->src;
}

// Sort and keep the best label per address.
static void symtab_finish(pd_symtab_t *t) {
    if (t->n < 2)
        return;
    qsort(t->v, t->n, sizeof(*t->v), cmp_sym);
    size_t w = 0;
    for (size_t r = 0; r < t->n; r++)
        if (w == 0 || t->v[w - 1].off != t->v[r].off)
            t->v[w++] = t->v[r];
    t->n = w;
}

// Look up the label at exactly `off`, or NULL.
static const pd_sym_t *sym_at(const pd_symtab_t *t, uint32_t off) {
    size_t lo = 0, hi = t->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t->v[mid].off == off)
            return &t->v[mid];
        if (t->v[mid].off < off)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

// True for sections that hold code.
static bool is_code_section(const pef_section_t *s) {
    return s->kind == PEF_SECT_CODE || s->kind == PEF_SECT_EXEC_DATA;
}

// Copy a C or Pascal string found at `off` in a section into out (quoted),
// or leave out empty when the bytes there do not look like text.
static void string_at(const pef_section_t *s, uint32_t off, char *out, size_t cap) {
    out[0] = '\0';
    if (!s->contents || off >= s->total_size)
        return;
    const uint8_t *p = s->contents + off;
    size_t avail = s->total_size - off;
    size_t n = 0;
    while (n < avail && n < 80 && p[n] >= 0x20 && p[n] < 0x7F)
        n++;
    if (n >= 3 && n < avail && p[n] == 0) { // NUL-terminated C string
        snprintf(out, cap, "\"%.*s\"", (int)n, (const char *)p);
        return;
    }
    // Pascal string: length byte then that many printable bytes
    size_t pl = p[0];
    if (pl >= 3 && pl < avail) {
        for (size_t k = 1; k <= pl; k++)
            if (p[k] < 0x20 || p[k] >= 0x7F)
                return;
        snprintf(out, cap, "\"\\p%.*s\"", (int)(pl > 80 ? 80 : pl), (const char *)p + 1);
    }
}

// Describe a relocation target: "import Foo", "code+0x01A4 <Name>", or
// "data+0x0040 \"text\"".
static void describe_target(const pd_ctx_t *c, int kind, int target, uint32_t addend, char *out, size_t cap) {
    const pef_t *pef = c->pef;
    if (kind == PEF_RTGT_IMPORT) {
        const pef_import_t *im = &pef->imports[target];
        const char *lib = im->library >= 0 ? pef->libraries[im->library].name : "?";
        if (addend)
            snprintf(out, cap, "import %s+0x%X [%s]", im->name, addend, lib);
        else
            snprintf(out, cap, "import %s [%s]", im->name, lib);
        return;
    }
    if (target < 0 || target >= pef->section_count) {
        snprintf(out, cap, "section %d+0x%X", target, addend);
        return;
    }
    const pef_section_t *s = &pef->sections[target];
    int n = snprintf(out, cap, "%s+0x%04X", c->label[target], addend);
    if (n < 0 || (size_t)n >= cap)
        return;
    const pd_sym_t *sym = is_code_section(s) ? sym_at(&c->syms[target], addend) : NULL;
    if (sym) {
        snprintf(out + n, cap - (size_t)n, " <%s>", sym->name);
    } else { // compilers keep string literals in code as well as data
        char str[100];
        string_at(s, addend, str, sizeof(str));
        if (str[0])
            snprintf(out + n, cap - (size_t)n, " %s", str);
    }
}

// Resolve a transition vector at (section, offset) to its code address.
static bool tvector_code(const pef_t *pef, int section, uint32_t offset, int *code_sect, uint32_t *code_off) {
    const pef_reloc_t *r = pef_find_reloc(pef, section, offset);
    if (!r || r->kind != PEF_RTGT_SECTION)
        return false;
    *code_sect = r->target;
    *code_off = r->addend;
    return true;
}

// Resolve the TOC word of a transition vector at (section, offset).
static bool tvector_toc(const pef_t *pef, int section, uint32_t offset, int *toc_sect, uint32_t *toc_off) {
    const pef_reloc_t *r = pef_find_reloc(pef, section, offset + 4);
    if (!r || r->kind != PEF_RTGT_SECTION)
        return false;
    *toc_sect = r->target;
    *toc_off = r->addend;
    return true;
}

// ============================================================================
// Analysis: section labels, TOC base, function labels
// ============================================================================

// Name each section by its kind, numbering kinds that repeat.
static void build_section_labels(pd_ctx_t *c) {
    const pef_t *pef = c->pef;
    for (int i = 0; i < pef->section_count; i++) {
        const pef_section_t *s = &pef->sections[i];
        // Pattern-initialized data is plain data once loaded
        const char *base = s->name[0]                         ? s->name
                           : s->kind == PEF_SECT_PATTERN_DATA ? "data"
                                                              : pef_section_kind_name(s->kind);
        bool dup = false;
        for (int j = 0; j < pef->section_count; j++) {
            const pef_section_t *o = &pef->sections[j];
            bool o_data = o->kind == PEF_SECT_UNPACKED_DATA || o->kind == PEF_SECT_PATTERN_DATA;
            bool s_data = s->kind == PEF_SECT_UNPACKED_DATA || s->kind == PEF_SECT_PATTERN_DATA;
            if (j != i && !s->name[0] && !o->name[0] && (o->kind == s->kind || (o_data && s_data)))
                dup = true;
        }
        if (dup)
            snprintf(c->label[i], sizeof(c->label[i]), "%.16s#%d", base, i);
        else
            snprintf(c->label[i], sizeof(c->label[i]), "%.20s", base);
    }
}

// Find r2's value from any transition vector the fragment defines.
static void find_toc(pd_ctx_t *c) {
    const pef_t *pef = c->pef;
    int ts;
    uint32_t to;
    // The entry points are transition vectors when present.
    const int32_t sects[3] = {pef->init_section, pef->main_section, pef->term_section};
    const uint32_t offs[3] = {pef->init_offset, pef->main_offset, pef->term_offset};
    for (int k = 0; k < 3; k++)
        if (sects[k] >= 0 && tvector_toc(pef, sects[k], offs[k], &ts, &to)) {
            c->has_toc = true;
            c->toc_section = ts;
            c->toc_off = to;
            return;
        }
    for (size_t i = 0; i < pef->n_exports; i++) {
        const pef_export_t *ex = &pef->exports[i];
        if (ex->sym_class == PEF_SYM_TVECT && ex->section >= 0 && tvector_toc(pef, ex->section, ex->value, &ts, &to)) {
            c->has_toc = true;
            c->toc_section = ts;
            c->toc_off = to;
            return;
        }
    }
}

// The import a TOC-relative word load at `w` reads, or -1.
static int toc_load_import(const pd_ctx_t *c, uint32_t w) {
    if (!c->has_toc || (w >> 26) != 32 || ((w >> 16) & 31) != 2) // lwz rD, d(r2)
        return -1;
    int32_t d = (int16_t)(w & 0xFFFF);
    const pef_reloc_t *r = pef_find_reloc(c->pef, c->toc_section, (uint32_t)((int64_t)c->toc_off + d));
    return (r && r->kind == PEF_RTGT_IMPORT) ? r->target : -1;
}

// Collect every label: exports, entry points, traceback names, glue, bl targets.
static void build_labels(pd_ctx_t *c) {
    const pef_t *pef = c->pef;
    char buf[160];
    // Exports: code symbols directly, transition vectors through their code word
    for (size_t i = 0; i < pef->n_exports; i++) {
        const pef_export_t *ex = &pef->exports[i];
        int cs;
        uint32_t co;
        if (ex->section < 0 || ex->section >= pef->section_count)
            continue;
        if (is_code_section(&pef->sections[ex->section]) && ex->sym_class == PEF_SYM_CODE)
            sym_add(&c->syms[ex->section], ex->value, SRC_EXPORT, ex->name);
        else if (ex->sym_class == PEF_SYM_TVECT && tvector_code(pef, ex->section, ex->value, &cs, &co))
            sym_add(&c->syms[cs], co, SRC_EXPORT, ex->name);
    }
    // CFM entry points
    const char *const entry_names[3] = {"cfm_init", "cfm_main", "cfm_term"};
    const int32_t sects[3] = {pef->init_section, pef->main_section, pef->term_section};
    const uint32_t offs[3] = {pef->init_offset, pef->main_offset, pef->term_offset};
    for (int k = 0; k < 3; k++) {
        int cs;
        uint32_t co;
        if (sects[k] >= 0 && tvector_code(pef, sects[k], offs[k], &cs, &co))
            sym_add(&c->syms[cs], co, SRC_ENTRY, entry_names[k]);
    }
    for (int i = 0; i < pef->section_count; i++) {
        const pef_section_t *s = &pef->sections[i];
        if (!is_code_section(s) || !s->contents)
            continue;
        // Traceback tables: function boundaries, usually with names
        c->tb[i] = pef_scan_traceback(s->contents, s->unpacked_size, &c->n_tb[i]);
        for (size_t k = 0; k < c->n_tb[i]; k++) {
            const pef_tbfunc_t *f = &c->tb[i][k];
            if (!f->has_start)
                continue;
            if (f->name[0]) {
                sym_add(&c->syms[i], f->start, SRC_TRACEBACK, f->name);
            } else {
                snprintf(buf, sizeof(buf), "fn_%05X", f->start);
                sym_add(&c->syms[i], f->start, SRC_FUNCTION, buf);
            }
        }
        // bl targets: glue stubs are named after the import they reach
        for (uint32_t off = 0; off + 4 <= s->unpacked_size; off += 4) {
            uint32_t w = rd32(s->contents + off);
            if ((w & 0xFC000003) != 0x48000001) // bl (AA=0, LK=1)
                continue;
            int32_t disp = (int32_t)(w & 0x03FFFFFC);
            if (disp & 0x02000000)
                disp -= 0x04000000; // sign-extend the 26-bit displacement
            int64_t tgt = (int64_t)off + disp;
            if (tgt < 0 || tgt + 4 > (int64_t)s->unpacked_size)
                continue;
            int imp = toc_load_import(c, rd32(s->contents + tgt));
            if (imp >= 0) {
                snprintf(buf, sizeof(buf), "glue_%s", pef->imports[imp].name);
                sym_add(&c->syms[i], (uint32_t)tgt, SRC_GLUE, buf);
            } else {
                snprintf(buf, sizeof(buf), "sub_%05X", (uint32_t)tgt);
                sym_add(&c->syms[i], (uint32_t)tgt, SRC_BRANCH, buf);
            }
        }
    }
    for (int i = 0; i < pef->section_count; i++)
        symtab_finish(&c->syms[i]);
}

// ============================================================================
// Writers
// ============================================================================

// Write the plain-text summary: header, sections, entry points, imports, exports.
static void write_summary(const pd_ctx_t *c, const char *src_label, FILE *fp) {
    const pef_t *pef = c->pef;
    char arch[16], tgt[256];
    ostype_str(pef->architecture, arch, sizeof(arch));
    fprintf(fp, "PEF container: %s\n", src_label);
    fprintf(fp, "  architecture      %s\n", arch);
    fprintf(fp, "  format version    %u\n", pef->format_version);
    fprintf(fp, "  timestamp         0x%08X (seconds since 1904)\n", pef->date_time_stamp);
    fprintf(fp, "  versions          current 0x%08X  old-def 0x%08X  old-imp 0x%08X\n", pef->current_version,
            pef->old_def_version, pef->old_imp_version);
    fprintf(fp, "  sections          %u (%u instantiated)\n", pef->section_count, pef->inst_section_count);
    if (pef->warning[0])
        fprintf(fp, "  WARNING           %s\n", pef->warning);

    fprintf(fp, "\nSections\n  idx  label        kind       total    unpacked  packed   offset   share align\n");
    for (int i = 0; i < pef->section_count; i++) {
        const pef_section_t *s = &pef->sections[i];
        fprintf(fp, "  %3d  %-12s %-9s  %-8u %-9u %-8u 0x%06X %-5u %u\n", i, c->label[i],
                pef_section_kind_name(s->kind), s->total_size, s->unpacked_size, s->packed_size, s->container_offset,
                s->share_kind, 1u << (s->alignment & 31));
    }

    fprintf(fp, "\nEntry points (transition vectors)\n");
    const char *const names[3] = {"main", "init", "term"};
    const int32_t sects[3] = {pef->main_section, pef->init_section, pef->term_section};
    const uint32_t offs[3] = {pef->main_offset, pef->init_offset, pef->term_offset};
    for (int k = 0; k < 3; k++) {
        int cs;
        uint32_t co;
        if (sects[k] < 0) {
            fprintf(fp, "  %-5s none\n", names[k]);
        } else if (tvector_code(pef, sects[k], offs[k], &cs, &co)) {
            describe_target(c, PEF_RTGT_SECTION, cs, co, tgt, sizeof(tgt));
            fprintf(fp, "  %-5s %s+0x%04X -> %s\n", names[k], c->label[sects[k]], offs[k], tgt);
        } else {
            fprintf(fp, "  %-5s %s+0x%04X\n", names[k], c->label[sects[k]], offs[k]);
        }
    }
    if (c->has_toc)
        fprintf(fp, "  TOC   r2 = %s+0x%04X\n", c->label[c->toc_section], c->toc_off);

    fprintf(fp, "\nImported libraries (%zu) and symbols (%zu)\n", pef->n_libraries, pef->n_imports);
    for (size_t l = 0; l < pef->n_libraries; l++) {
        const pef_library_t *lib = &pef->libraries[l];
        fprintf(fp, "  %s  (current 0x%08X, old-imp 0x%08X%s%s)\n", lib->name, lib->current_version,
                lib->old_imp_version, (lib->options & 0x40) ? ", weak" : "",
                (lib->options & 0x80) ? ", init-before" : "");
        for (size_t k = 0; k < pef->n_imports; k++)
            if (pef->imports[k].library == (int)l)
                fprintf(fp, "    [%3zu] %-8s %s%s\n", k, pef_symbol_class_name(pef->imports[k].sym_class),
                        pef->imports[k].name, pef->imports[k].weak ? "  (weak)" : "");
    }

    fprintf(fp, "\nExported symbols (%zu)\n", pef->n_exports);
    for (size_t i = 0; i < pef->n_exports; i++) {
        const pef_export_t *ex = &pef->exports[i];
        int cs;
        uint32_t co;
        if (ex->section >= 0 && ex->section < pef->section_count) {
            fprintf(fp, "  %-8s %s+0x%04X  %s", pef_symbol_class_name(ex->sym_class), c->label[ex->section], ex->value,
                    ex->name);
            if (ex->sym_class == PEF_SYM_TVECT && tvector_code(pef, ex->section, ex->value, &cs, &co)) {
                describe_target(c, PEF_RTGT_SECTION, cs, co, tgt, sizeof(tgt));
                fprintf(fp, "  -> %s", tgt);
            }
            fputc('\n', fp);
        } else {
            fprintf(fp, "  %-8s section %d value 0x%08X  %s\n", pef_symbol_class_name(ex->sym_class), ex->section,
                    ex->value, ex->name);
        }
    }
}

// Write every relocated word with its symbolic target.
static void write_relocations(const pd_ctx_t *c, FILE *fp) {
    const pef_t *pef = c->pef;
    char tgt[256];
    fprintf(fp, "; PEF relocations: %zu relocated words\n", pef->n_relocs);
    fprintf(fp, "; <section>+<offset> [toc offset]: -> <target>   (the stored word is the addend)\n\n");
    for (size_t i = 0; i < pef->n_relocs; i++) {
        const pef_reloc_t *r = &pef->relocs[i];
        describe_target(c, r->kind, r->target, r->addend, tgt, sizeof(tgt));
        fprintf(fp, "%s+0x%05X", c->label[r->section], r->offset);
        if (c->has_toc && r->section == c->toc_section)
            fprintf(fp, " [toc%+d]", (int)((int64_t)r->offset - c->toc_off));
        fprintf(fp, ": -> %s\n", tgt);
    }
}

// Write the consolidated label table.
static void write_symbols(const pd_ctx_t *c, FILE *fp) {
    fprintf(fp, "; <section> <offset> <source> <name>\n");
    for (int i = 0; i < c->pef->section_count; i++)
        for (size_t k = 0; k < c->syms[i].n; k++) {
            const pd_sym_t *s = &c->syms[i].v[k];
            fprintf(fp, "%s 0x%05X %s %s\n", c->label[i], s->off, g_src_names[s->src], s->name);
        }
}

// Comment for a D-form instruction relative to r2, or "" if not one.
static void toc_comment(const pd_ctx_t *c, uint32_t w, char *out, size_t cap) {
    out[0] = '\0';
    if (!c->has_toc || ((w >> 16) & 31) != 2)
        return;
    uint32_t op = w >> 26;
    bool is_addi = (op == 14);
    // lwz lwzu lbz lbzu stw stwu stb stbu lhz lhzu lha lhau sth sthu lmw stmw lfs lfsu lfd lfdu stfs stfsu stfd stfdu
    bool is_mem = (op >= 32 && op <= 55);
    if (!is_addi && !is_mem)
        return;
    int32_t d = (int16_t)(w & 0xFFFF);
    uint32_t slot = (uint32_t)((int64_t)c->toc_off + d);
    const char *sect = c->label[c->toc_section];
    if (is_addi) { // address computation: describe the location itself
        char tgt[256];
        describe_target(c, PEF_RTGT_SECTION, c->toc_section, slot, tgt, sizeof(tgt));
        snprintf(out, cap, "toc%+d = &%s", d, tgt);
        return;
    }
    const pef_reloc_t *r = pef_find_reloc(c->pef, c->toc_section, slot);
    if (op == 32 && r) { // lwz of a relocated word: name what it points to
        char tgt[256];
        describe_target(c, r->kind, r->target, r->addend, tgt, sizeof(tgt));
        snprintf(out, cap, "toc%+d = %s+0x%04X -> %s", d, sect, slot, tgt);
    } else {
        snprintf(out, cap, "toc%+d = %s+0x%04X", d, sect, slot);
    }
}

// Disassemble one code section with labels and annotations.
static void write_code_listing(const pd_ctx_t *c, int idx, FILE *fp) {
    const pef_section_t *s = &c->pef->sections[idx];
    const pd_symtab_t *syms = &c->syms[idx];
    fprintf(fp, "; ============================================================\n");
    fprintf(fp, "; PEF section %d (%s)%s%s\n", idx, pef_section_kind_name(s->kind), s->name[0] ? "  name=" : "",
            s->name);
    fprintf(fp, "; code_bytes=%u  default_address=0x%08X  functions(traceback)=%zu  labels=%zu\n", s->unpacked_size,
            s->default_address, c->n_tb[idx], syms->n);
    if (c->has_toc)
        fprintf(fp, "; TOC: r2 = %s+0x%04X\n", c->label[c->toc_section], c->toc_off);
    fprintf(fp, "; Addresses are offsets within the section.\n");
    fprintf(fp, "; ============================================================\n");

    size_t next_tb = 0; // next traceback table in address order
    for (uint32_t off = 0; off + 4 <= s->unpacked_size; off += 4) {
        const pd_sym_t *sym = sym_at(syms, off);
        if (sym)
            fprintf(fp, "\n%s:\n", sym->name);
        uint32_t w = rd32(s->contents + off);
        // Traceback tables are data: show them as words, not instructions.
        while (next_tb < c->n_tb[idx] && c->tb[idx][next_tb].tb_end <= off)
            next_tb++;
        if (next_tb < c->n_tb[idx] && off >= c->tb[idx][next_tb].tb_offset) {
            const pef_tbfunc_t *f = &c->tb[idx][next_tb];
            fprintf(fp, "$%08X  %08X  .long     0x%08X", off, w, w);
            if (off == f->tb_offset)
                fprintf(fp, "\t; traceback table%s%s", f->name[0] ? " for " : "", f->name);
            fputc('\n', fp);
            continue;
        }
        ppc_insn in;
        ppc_disassemble(w, off, &in);
        // Split "mnemonic\toperands" so the columns line up.
        const char *tab = strchr(in.text, '\t');
        int mlen = tab ? (int)(tab - in.text) : (int)strlen(in.text);
        if (tab)
            fprintf(fp, "$%08X  %08X  %-9.*s %s", off, w, mlen, in.text, tab + 1);
        else // no operands: no trailing padding
            fprintf(fp, "$%08X  %08X  %s", off, w, in.text);
        char note[320] = "";
        if (in.is_branch && in.has_target) {
            const pd_sym_t *t = in.target < s->unpacked_size ? sym_at(syms, in.target) : NULL;
            if (t)
                snprintf(note, sizeof(note), "-> %s", t->name);
            else
                snprintf(note, sizeof(note), "-> $%08X", in.target);
        } else {
            toc_comment(c, w, note, sizeof(note));
        }
        if (note[0])
            fprintf(fp, "\t; %s", note);
        fputc('\n', fp);
    }
}

// Write manifest.json.
static void write_manifest(const pd_ctx_t *c, const char *src_label, int disasm_written, FILE *fp) {
    const pef_t *pef = c->pef;
    char arch[16];
    ostype_str(pef->architecture, arch, sizeof(arch));
    fprintf(fp, "{\n  \"schema_version\": 1,\n  \"generator\": \"granny-smith dump v1 (pef)\",\n  \"source\": ");
    re_json_write_string(fp, src_label);
    fprintf(fp, ",\n  \"container\": {\"architecture\": ");
    re_json_write_string(fp, arch);
    fprintf(fp,
            ", \"format_version\": %u, \"timestamp\": %u, \"current_version\": %u, \"old_def_version\": %u, "
            "\"old_imp_version\": %u},\n",
            pef->format_version, pef->date_time_stamp, pef->current_version, pef->old_def_version,
            pef->old_imp_version);
    fprintf(fp, "  \"warning\": ");
    re_json_write_string(fp, pef->warning);
    fprintf(fp, ",\n  \"sections\": [\n");
    for (int i = 0; i < pef->section_count; i++) {
        const pef_section_t *s = &pef->sections[i];
        fprintf(fp, "    {\"index\": %d, \"label\": ", i);
        re_json_write_string(fp, c->label[i]);
        fprintf(fp, ", \"kind\": \"%s\", \"total_size\": %u, \"unpacked_size\": %u, \"packed_size\": %u, ",
                pef_section_kind_name(s->kind), s->total_size, s->unpacked_size, s->packed_size);
        if (s->instantiated)
            fprintf(fp, "\"file\": \"sections/%02d-%s.bin\"", i, pef_section_kind_name(s->kind));
        else
            fprintf(fp, "\"file\": null");
        if (is_code_section(s) && disasm_written >= 0)
            fprintf(fp, ", \"disasm\": \"disasm/%02d-%s.s\"", i, pef_section_kind_name(s->kind));
        fprintf(fp, "}%s\n", i + 1 < pef->section_count ? "," : "");
    }
    fprintf(fp, "  ],\n  \"toc\": ");
    if (c->has_toc)
        fprintf(fp, "{\"section\": %d, \"offset\": %u}", c->toc_section, c->toc_off);
    else
        fprintf(fp, "null");
    fprintf(fp, ",\n  \"entry_points\": {\"main\": [%d, %u], \"init\": [%d, %u], \"term\": [%d, %u]},\n",
            pef->main_section, pef->main_offset, pef->init_section, pef->init_offset, pef->term_section,
            pef->term_offset);
    fprintf(fp, "  \"libraries\": [\n");
    for (size_t l = 0; l < pef->n_libraries; l++) {
        fprintf(fp, "    {\"name\": ");
        re_json_write_string(fp, pef->libraries[l].name);
        fprintf(fp, ", \"symbols\": [");
        bool first = true;
        for (size_t k = 0; k < pef->n_imports; k++)
            if (pef->imports[k].library == (int)l) {
                fprintf(fp, "%s", first ? "" : ", ");
                re_json_write_string(fp, pef->imports[k].name);
                first = false;
            }
        fprintf(fp, "]}%s\n", l + 1 < pef->n_libraries ? "," : "");
    }
    fprintf(fp, "  ],\n  \"exports\": [\n");
    for (size_t i = 0; i < pef->n_exports; i++) {
        const pef_export_t *ex = &pef->exports[i];
        fprintf(fp, "    {\"name\": ");
        re_json_write_string(fp, ex->name);
        fprintf(fp, ", \"class\": \"%s\", \"section\": %d, \"value\": %u}%s\n", pef_symbol_class_name(ex->sym_class),
                ex->section, ex->value, i + 1 < pef->n_exports ? "," : "");
    }
    size_t n_labels = 0, n_funcs = 0;
    for (int i = 0; i < pef->section_count; i++) {
        n_labels += c->syms[i].n;
        n_funcs += c->n_tb[i];
    }
    fprintf(fp,
            "  ],\n  \"relocations\": {\"count\": %zu, \"file\": \"relocations.txt\"},\n"
            "  \"labels\": {\"count\": %zu, \"traceback_functions\": %zu, \"file\": \"symbols.txt\"}\n}\n",
            pef->n_relocs, n_labels, n_funcs);
}

// Write the directory README.
static void write_readme(const pd_ctx_t *c, const char *src_label, int disasm_written, FILE *fp) {
    const pef_t *pef = c->pef;
    char arch[16];
    ostype_str(pef->architecture, arch, sizeof(arch));
    size_t n_funcs = 0, n_named = 0;
    for (int i = 0; i < pef->section_count; i++) {
        n_funcs += c->n_tb[i];
        for (size_t k = 0; k < c->n_tb[i]; k++)
            n_named += c->tb[i][k].name[0] != '\0';
    }
    fprintf(fp, "# re.dump — PEF code fragment\n\n");
    fprintf(fp, "| Field | Value |\n|---|---|\n");
    fprintf(fp, "| Source | `%s` |\n", src_label);
    fprintf(fp, "| Architecture | `%s` |\n", arch);
    fprintf(fp, "| Sections | %u (%u instantiated) |\n", pef->section_count, pef->inst_section_count);
    fprintf(fp, "| Imported libraries / symbols | %zu / %zu |\n", pef->n_libraries, pef->n_imports);
    fprintf(fp, "| Exported symbols | %zu |\n", pef->n_exports);
    fprintf(fp, "| Relocated words | %zu |\n", pef->n_relocs);
    fprintf(fp, "| Traceback tables (named) | %zu (%zu) |\n", n_funcs, n_named);
    fprintf(fp, "| Code sections disassembled | %d |\n", disasm_written);
    if (pef->warning[0])
        fprintf(fp, "| Warning | %s |\n", pef->warning);
    fprintf(fp, "\n## Files\n\n| Path | What it contains |\n|---|---|\n");
    fprintf(fp, "| `summary.txt` | Header, sections, entry points, imports by library, exports. Start here. |\n");
    fprintf(fp, "| `manifest.json` | The same, machine-readable. |\n");
    fprintf(fp, "| `sections/NN-<kind>.bin` | Each instantiated section as it is laid out in memory\n");
    fprintf(fp, "  (pattern-initialized data unpacked, zero-filled tail included). |\n");
    fprintf(fp, "| `relocations.txt` | Every relocated word and what it points to: an import, a\n");
    fprintf(fp, "  function, or a string. TOC slots carry their `[toc±d]` offset. |\n");
    fprintf(fp, "| `symbols.txt` | Every label: `<section> <offset> <source> <name>`. |\n");
    if (disasm_written > 0)
        fprintf(fp, "| `disasm/NN-code.s` | Annotated PowerPC disassembly. |\n");
    fprintf(fp, "\n## Conventions\n\n");
    fprintf(fp, "- Addresses are `<section>+<offset>`; no load address is assumed.\n");
    fprintf(fp, "- Labels come from exports, the CFM entry points (`cfm_init`/`cfm_main`/\n");
    fprintf(fp, "  `cfm_term`, resolved through their transition vectors), traceback-table\n");
    fprintf(fp, "  names, cross-TOC glue (`glue_<import>`), and otherwise-unnamed `bl`\n");
    fprintf(fp, "  targets (`sub_XXXXX`); unnamed traceback functions are `fn_XXXXX`.\n");
    fprintf(fp, "- `; toc+d = data+0x0040 -> import Foo` on an `r2`-relative instruction names\n");
    fprintf(fp, "  the TOC slot it touches and, for `lwz`, what the slot points to.\n");
    fprintf(fp, "- Traceback tables are listed as `.long` words, not instructions.\n");
}

// ============================================================================
// Entry point
// ============================================================================

int re_pef_dump(const uint8_t *bytes, size_t len, const char *src_label, const char *dst_dir, bool disasm) {
    pef_t pef;
    const char *err = NULL;
    if (pef_parse(bytes, len, &pef, &err) != 0) {
        fprintf(stderr, "dump: cannot parse PEF in '%s': %s\n", src_label, err ? err : "unknown");
        return -EIO;
    }
    int rc = -EIO;
    pd_ctx_t c = {.pef = &pef};
    size_t n = pef.section_count ? pef.section_count : 1;
    c.label = calloc(n, sizeof(*c.label));
    c.syms = calloc(n, sizeof(*c.syms));
    c.tb = calloc(n, sizeof(*c.tb));
    c.n_tb = calloc(n, sizeof(*c.n_tb));
    char path[PATH_MAX];
    int disasm_written = 0;
    if (!c.label || !c.syms || !c.tb || !c.n_tb)
        goto out;

    build_section_labels(&c);
    find_toc(&c);
    build_labels(&c);

    // Sections in memory form
    snprintf(path, sizeof(path), "%s/sections", dst_dir);
    if (mkdir_p(path) != 0) {
        fprintf(stderr, "dump: cannot create '%s': %s\n", path, strerror(errno));
        goto out;
    }
    for (int i = 0; i < pef.section_count; i++) {
        const pef_section_t *s = &pef.sections[i];
        if (!s->instantiated)
            continue;
        snprintf(path, sizeof(path), "%s/sections/%02d-%s.bin", dst_dir, i, pef_section_kind_name(s->kind));
        if (pd_write_blob(path, s->contents, s->total_size) != 0)
            goto out;
    }

    // Annotated disassembly of every PowerPC code section
    if (disasm && pef.architecture == 0x70777063) { // 'pwpc'
        snprintf(path, sizeof(path), "%s/disasm", dst_dir);
        if (mkdir_p(path) != 0)
            goto out;
        for (int i = 0; i < pef.section_count; i++) {
            if (!is_code_section(&pef.sections[i]) || !pef.sections[i].contents)
                continue;
            char name[64];
            snprintf(name, sizeof(name), "disasm/%02d-%s.s", i, pef_section_kind_name(pef.sections[i].kind));
            FILE *fp = pd_open(dst_dir, name);
            if (!fp)
                goto out;
            write_code_listing(&c, i, fp);
            fclose(fp);
            disasm_written++;
        }
    }

    // Text and JSON indexes
    FILE *fp;
    if (!(fp = pd_open(dst_dir, "summary.txt")))
        goto out;
    write_summary(&c, src_label, fp);
    fclose(fp);
    if (!(fp = pd_open(dst_dir, "relocations.txt")))
        goto out;
    write_relocations(&c, fp);
    fclose(fp);
    if (!(fp = pd_open(dst_dir, "symbols.txt")))
        goto out;
    write_symbols(&c, fp);
    fclose(fp);
    if (!(fp = pd_open(dst_dir, "manifest.json")))
        goto out;
    write_manifest(&c, src_label, disasm ? disasm_written : -1, fp);
    fclose(fp);
    if (!(fp = pd_open(dst_dir, "README.md")))
        goto out;
    write_readme(&c, src_label, disasm_written, fp);
    fclose(fp);

    if (pef.warning[0])
        fprintf(stderr, "dump: warning: %s: %s\n", src_label, pef.warning);
    rc = disasm_written;

out:
    if (c.syms)
        for (int i = 0; i < pef.section_count; i++)
            free(c.syms[i].v);
    if (c.tb)
        for (int i = 0; i < pef.section_count; i++)
            free(c.tb[i]);
    free(c.label);
    free(c.syms);
    free(c.tb);
    free(c.n_tb);
    pef_free(&pef);
    return rc;
}
