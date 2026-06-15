// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// coff.c
// A/UX flavour COFF parser.  See coff.h for the public surface.
//
// On-disk layout (big-endian throughout):
//
//   filhdr (20 bytes):
//     +0  f_magic   u16   0x0150 for M68K
//     +2  f_nscns   u16   number of section headers
//     +4  f_timdat  u32   build time (unix epoch)
//     +8  f_symptr  u32   file offset to symbol table
//     +12 f_nsyms   u32   number of symbol-table entries (counting aux)
//     +16 f_opthdr  u16   size of optional header (28 for A/UX)
//     +18 f_flags   u16   misc flags
//
//   aouthdr (28 bytes when present):
//     +0  magic       u16   0x0108 (A/UX PAGEMAGIC) typical
//     +2  vstamp      u16
//     +4  tsize       u32   text-segment byte count
//     +8  dsize       u32   data-segment byte count
//     +12 bsize       u32   bss size
//     +16 entry       u32   entry point VA
//     +20 text_start  u32
//     +24 data_start  u32
//
//   scnhdr × f_nscns (40 bytes each):
//     +0   s_name      8 B    8-byte name (null-padded) or "/<offset>"
//                              into the string table for long names
//     +8   s_paddr     u32    physical address
//     +12  s_vaddr     u32    virtual address
//     +16  s_size      u32    section byte size
//     +20  s_scnptr    u32    file offset to raw bytes (0 = no data)
//     +24  s_relptr    u32    file offset to relocations
//     +28  s_lnnoptr   u32    file offset to line numbers
//     +32  s_nreloc    u16
//     +34  s_nlnno     u16
//     +36  s_flags     u32
//
//   syment × f_nsyms (18 bytes each):
//     +0   n_name      8 B    inline 8-char name OR (zero u32 then
//                              4-byte string-table offset)
//     +8   n_value     u32    virtual address (defined) or zero (undef)
//     +12  n_scnum     i16    section number (1..n, 0 undef, -1 abs, -2 dbg)
//     +14  n_type      u16
//     +16  n_sclass    u8     COFF_C_*
//     +17  n_numaux    u8     number of auxiliary entries that follow
//                              (each aux entry is also 18 bytes)
//
//   string table: starts at f_symptr + f_nsyms*18
//     +0   total_length u32   includes these 4 length bytes
//     +4.. NUL-terminated strings; symbols reference by byte offset >= 4

#include "coff.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COFF_FILHDR_SIZE 20
#define COFF_SCNHDR_SIZE 40
#define COFF_SYMENT_SIZE 18

static uint16_t be_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t be_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static int16_t be_i16(const uint8_t *p) {
    return (int16_t)be_u16(p);
}

struct coff {
    const uint8_t *data;
    size_t len;
    // File-header fields.
    uint16_t magic;
    uint16_t flags;
    uint32_t timestamp;
    // Optional-header fields (zero when no aouthdr).
    bool has_aouthdr;
    uint16_t aout_magic;
    uint32_t tsize, dsize, bsize;
    uint32_t entry;
    uint32_t text_start, data_start;
    // Sections.
    size_t n_sections;
    coff_section_t *sections;
    // Symbol table: total raw entries (including aux records) and a
    // flattened list with NULL slots for aux entries — index into this
    // matches the on-disk symbol index, so other tooling can cross-
    // reference without remapping.
    size_t n_symbols_raw;
    coff_symbol_t **symbols; // n_symbols_raw slots; aux entries are NULL
    // String table (borrowed pointer into data).
    const uint8_t *strtab; // NULL when absent
    size_t strtab_len;
};

bool coff_is_coff(const uint8_t *data, size_t len) {
    return data && len >= 2 && be_u16(data) == COFF_M68K_MAGIC;
}

// Resolve an n_name field into the caller's buffer.  Handles both the
// inline 8-char form and the (0, offset) string-table form.  Always
// NUL-terminates.  Returns the number of bytes used (excluding NUL).
static size_t resolve_name(const uint8_t *n_name, const uint8_t *strtab, size_t strtab_len, char *out, size_t cap) {
    if (cap == 0)
        return 0;
    if (be_u32(n_name) == 0) {
        // Long-name form: offset is in the second u32.
        uint32_t off = be_u32(n_name + 4);
        if (strtab && off < strtab_len) {
            size_t i = 0;
            while (i + 1 < cap && off + i < strtab_len && strtab[off + i] != '\0') {
                out[i] = (char)strtab[off + i];
                i++;
            }
            out[i] = '\0';
            return i;
        }
        out[0] = '\0';
        return 0;
    }
    // Inline 8-char form, null-padded.
    size_t i = 0;
    while (i < 8 && i + 1 < cap && n_name[i] != '\0') {
        out[i] = (char)n_name[i];
        i++;
    }
    out[i] = '\0';
    return i;
}

coff_t *coff_parse(const uint8_t *data, size_t len, const char **errmsg) {
#define FAIL(msg)                                                                                                      \
    do {                                                                                                               \
        if (errmsg)                                                                                                    \
            *errmsg = (msg);                                                                                           \
        coff_free(cf);                                                                                                 \
        return NULL;                                                                                                   \
    } while (0)
    if (errmsg)
        *errmsg = NULL;
    coff_t *cf = NULL;

    if (!data || len < COFF_FILHDR_SIZE)
        FAIL("truncated file header");

    uint16_t f_magic = be_u16(data + 0);
    if (f_magic != COFF_M68K_MAGIC)
        FAIL("not a COFF M68K binary");

    cf = calloc(1, sizeof(*cf));
    if (!cf)
        FAIL("out of memory");
    cf->data = data;
    cf->len = len;
    cf->magic = f_magic;

    uint16_t f_nscns = be_u16(data + 2);
    cf->timestamp = be_u32(data + 4);
    uint32_t f_symptr = be_u32(data + 8);
    uint32_t f_nsyms = be_u32(data + 12);
    uint16_t f_opthdr = be_u16(data + 16);
    cf->flags = be_u16(data + 18);

    // Bound checks.
    if ((uint64_t)COFF_FILHDR_SIZE + f_opthdr > len)
        FAIL("optional header out of range");

    // Optional header (may be absent — f_opthdr == 0 — but A/UX usually
    // has the standard 28-byte aouthdr).
    if (f_opthdr >= 28) {
        const uint8_t *opt = data + COFF_FILHDR_SIZE;
        cf->has_aouthdr = true;
        cf->aout_magic = be_u16(opt + 0);
        // skip vstamp at +2
        cf->tsize = be_u32(opt + 4);
        cf->dsize = be_u32(opt + 8);
        cf->bsize = be_u32(opt + 12);
        cf->entry = be_u32(opt + 16);
        cf->text_start = be_u32(opt + 20);
        cf->data_start = be_u32(opt + 24);
    }

    // Section headers — immediately after the optional header.
    size_t scn_table_off = COFF_FILHDR_SIZE + f_opthdr;
    size_t scn_table_bytes = (size_t)f_nscns * COFF_SCNHDR_SIZE;
    if (scn_table_off + scn_table_bytes > len)
        FAIL("section table out of range");

    cf->n_sections = f_nscns;
    if (f_nscns > 0) {
        cf->sections = calloc(f_nscns, sizeof(coff_section_t));
        if (!cf->sections)
            FAIL("out of memory");
    }

    // The string table lives after the symbol table (if any).  Load its
    // length first so we can resolve long section names via /N offsets.
    // String table base = f_symptr + f_nsyms*18; first u32 = total length.
    size_t strtab_off = (size_t)f_symptr + (size_t)f_nsyms * COFF_SYMENT_SIZE;
    if (f_symptr != 0 && f_nsyms > 0 && strtab_off + 4 <= len) {
        uint32_t strtab_len = be_u32(data + strtab_off);
        if (strtab_len >= 4 && strtab_off + strtab_len <= len) {
            cf->strtab = data + strtab_off;
            cf->strtab_len = strtab_len;
        }
    }

    for (size_t i = 0; i < f_nscns; i++) {
        const uint8_t *sh = data + scn_table_off + i * COFF_SCNHDR_SIZE;
        coff_section_t *s = &cf->sections[i];
        // Section name: 8-byte field.  Long names start with '/' followed
        // by a decimal string-table offset; we copy the first 8 chars
        // either way and let downstream tooling resolve "/123" if needed.
        memcpy(s->name, sh, 8);
        s->name[8] = '\0';
        if (s->name[0] == '/' && cf->strtab) {
            // Resolve long name from string table.
            char numbuf[9];
            memcpy(numbuf, s->name + 1, 7);
            numbuf[7] = '\0';
            char *end = NULL;
            unsigned long off = strtoul(numbuf, &end, 10);
            if (end && *end == '\0' && off < cf->strtab_len) {
                size_t i2 = 0;
                while (i2 + 1 < sizeof(s->name) && off + i2 < cf->strtab_len && cf->strtab[off + i2] != '\0') {
                    s->name[i2] = (char)cf->strtab[off + i2];
                    i2++;
                }
                s->name[i2] = '\0';
            }
        }
        s->vaddr = be_u32(sh + 12);
        s->size = be_u32(sh + 16);
        s->file_offset = be_u32(sh + 20);
        s->flags = be_u32(sh + 36);
        // Sanity: section's raw bytes (if any) must fit inside the file.
        if (s->file_offset != 0 && (uint64_t)s->file_offset + s->size > len) {
            // Clamp rather than fail — A/UX BSS-style sections sometimes
            // declare a non-zero file_offset with zero raw bytes (size=0)
            // and that's fine; the read accessor handles it.
        }
    }

    // Symbol table — slot per on-disk entry.  Aux records leave a NULL
    // in the array so the indices align 1:1 with the on-disk table (the
    // relocation entries reference symbol indices that count aux
    // records as occupying slots).
    if (f_nsyms > 0 && f_symptr != 0) {
        if ((uint64_t)f_symptr + (uint64_t)f_nsyms * COFF_SYMENT_SIZE > len)
            FAIL("symbol table out of range");

        cf->n_symbols_raw = f_nsyms;
        cf->symbols = calloc(f_nsyms, sizeof(coff_symbol_t *));
        if (!cf->symbols)
            FAIL("out of memory");

        size_t i = 0;
        while (i < f_nsyms) {
            const uint8_t *e = data + f_symptr + i * COFF_SYMENT_SIZE;
            coff_symbol_t *s = calloc(1, sizeof(coff_symbol_t));
            if (!s)
                FAIL("out of memory");
            resolve_name(e, cf->strtab, cf->strtab_len, s->name, sizeof(s->name));
            s->value = be_u32(e + 8);
            s->scnum = be_i16(e + 12);
            s->type = be_u16(e + 14);
            s->sclass = e[16];
            uint8_t n_aux = e[17];
            cf->symbols[i] = s;
            i++;
            // Skip aux records — they belong to the symbol that owns them.
            // We leave their slots NULL.  Guard against an n_aux that
            // would walk past the table.
            for (uint8_t a = 0; a < n_aux && i < f_nsyms; a++) {
                i++;
            }
        }
    }

    return cf;
#undef FAIL
}

void coff_free(coff_t *cf) {
    if (!cf)
        return;
    free(cf->sections);
    if (cf->symbols) {
        for (size_t i = 0; i < cf->n_symbols_raw; i++)
            free(cf->symbols[i]);
        free(cf->symbols);
    }
    free(cf);
}

uint16_t coff_magic(const coff_t *cf) {
    return cf ? cf->magic : 0;
}
uint16_t coff_flags(const coff_t *cf) {
    return cf ? cf->flags : 0;
}
uint32_t coff_timestamp(const coff_t *cf) {
    return cf ? cf->timestamp : 0;
}
uint32_t coff_text_size(const coff_t *cf) {
    return cf ? cf->tsize : 0;
}
uint32_t coff_data_size(const coff_t *cf) {
    return cf ? cf->dsize : 0;
}
uint32_t coff_bss_size(const coff_t *cf) {
    return cf ? cf->bsize : 0;
}
uint32_t coff_entry_point(const coff_t *cf) {
    return cf ? cf->entry : 0;
}
uint32_t coff_text_start(const coff_t *cf) {
    return cf ? cf->text_start : 0;
}
uint32_t coff_data_start(const coff_t *cf) {
    return cf ? cf->data_start : 0;
}

size_t coff_num_sections(const coff_t *cf) {
    return cf ? cf->n_sections : 0;
}
const coff_section_t *coff_section_at(const coff_t *cf, size_t idx) {
    if (!cf || idx >= cf->n_sections)
        return NULL;
    return &cf->sections[idx];
}
const uint8_t *coff_section_data(const coff_t *cf, const coff_section_t *s) {
    if (!cf || !s || s->file_offset == 0)
        return NULL;
    if ((uint64_t)s->file_offset + s->size > cf->len)
        return NULL;
    return cf->data + s->file_offset;
}

size_t coff_num_symbols(const coff_t *cf) {
    return cf ? cf->n_symbols_raw : 0;
}
const coff_symbol_t *coff_symbol_at(const coff_t *cf, size_t idx) {
    if (!cf || idx >= cf->n_symbols_raw || !cf->symbols)
        return NULL;
    return cf->symbols[idx];
}
