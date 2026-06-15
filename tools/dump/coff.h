// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// coff.h
// Read-only parser for the A/UX flavour of Common Object File Format
// (COFF) — the executable format used by A/UX 1.x..3.x on 68k Macs.
// Same general shape as SysV-R2 COFF: 20-byte filhdr, optional aouthdr,
// section-header table, symbol table, string table.
//
// Big-endian throughout (68k native order).  We support the M68KMAGIC
// (0x0150) variant; older / cross-targets / S5 little-endian flavours
// produce a "wrong magic" parse failure.
//
// Symbol storage is by file offset (we don't copy strings).  Names are
// resolved on demand via coff_symbol_name() so the parsed structure
// doesn't bloat with 5000+ duplicated heap strings.

#pragma once

#ifndef GS_COFF_H
#define GS_COFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Recognised file-header magic numbers.
#define COFF_M68K_MAGIC 0x0150u

// Section flags (s_flags) — only the ones we care about.
#define COFF_STYP_REG    0x0000u // ordinary section
#define COFF_STYP_DSECT  0x0001u // dummy
#define COFF_STYP_NOLOAD 0x0002u // not loaded at runtime
#define COFF_STYP_GROUP  0x0004u
#define COFF_STYP_PAD    0x0008u
#define COFF_STYP_COPY   0x0010u
#define COFF_STYP_TEXT   0x0020u // executable instructions
#define COFF_STYP_DATA   0x0040u // initialised data
#define COFF_STYP_BSS    0x0080u // uninitialised data
#define COFF_STYP_INFO   0x0200u // comment / debug info
#define COFF_STYP_LIB    0x0800u
#define COFF_STYP_LOADER 0x1000u

// Symbol storage classes (n_sclass) — only the externally-meaningful
// ones.  Debugging classes (C_FCN, C_BLOCK, C_FILE) and stab-style
// classes (C_DECL, C_AUTO, C_REG, ...) round-trip but we don't surface
// them as labels.
#define COFF_C_NULL   0
#define COFF_C_AUTO   1
#define COFF_C_EXT    2 // external symbol (visible to linker)
#define COFF_C_STAT   3 // static (file-local) symbol
#define COFF_C_REG    4
#define COFF_C_EXTDEF 5
#define COFF_C_LABEL  6 // local label
#define COFF_C_ULABEL 7
#define COFF_C_FCN    101 // .bf / .ef function boundary
#define COFF_C_BLOCK  100
#define COFF_C_FILE   103

typedef struct coff_section {
    char name[9]; // null-terminated (or "/<offset>" for long names — we resolve via string table)
    uint32_t vaddr; // virtual address where loader places this section
    uint32_t size; // section size in bytes
    uint32_t file_offset; // file offset of the section's raw data (0 = no raw data, e.g. .bss)
    uint32_t flags; // COFF_STYP_*
} coff_section_t;

typedef struct coff_symbol {
    char name[33]; // resolved name (long names via string table); empty for unnamed
    uint32_t value; // symbol value (virtual address for defined symbols)
    int16_t scnum; // 1-based section number, or 0 (undefined), -1 (absolute), -2 (debug)
    uint16_t type; // n_type word
    uint8_t sclass; // n_sclass — COFF_C_*
} coff_symbol_t;

typedef struct coff coff_t;

// Parse a COFF binary from a contiguous buffer.  `data` must outlive the
// returned coff_t (no copy).  Returns NULL on parse failure with *errmsg
// set to a static description.  Caller frees with coff_free().
coff_t *coff_parse(const uint8_t *data, size_t len, const char **errmsg);
void coff_free(coff_t *cf);

// Header introspection.
uint16_t coff_magic(const coff_t *cf);
uint16_t coff_flags(const coff_t *cf);
uint32_t coff_timestamp(const coff_t *cf);
uint32_t coff_text_size(const coff_t *cf);
uint32_t coff_data_size(const coff_t *cf);
uint32_t coff_bss_size(const coff_t *cf);
uint32_t coff_entry_point(const coff_t *cf);
uint32_t coff_text_start(const coff_t *cf);
uint32_t coff_data_start(const coff_t *cf);

// Sections.
size_t coff_num_sections(const coff_t *cf);
const coff_section_t *coff_section_at(const coff_t *cf, size_t idx);
// Returns a pointer to the section's raw bytes inside the original
// input buffer, or NULL for sections without raw data (e.g. .bss).
const uint8_t *coff_section_data(const coff_t *cf, const coff_section_t *s);

// Symbols.  Returns the total symbol count *including* aux records;
// coff_symbol_at returns NULL for aux entries (only "real" entries
// surface).  Iterate with `for i in 0..coff_num_symbols(cf)` and skip
// NULL returns.
size_t coff_num_symbols(const coff_t *cf);
const coff_symbol_t *coff_symbol_at(const coff_t *cf, size_t idx);

// Quick test: does the buffer start with a recognised COFF magic?
bool coff_is_coff(const uint8_t *data, size_t len);

#endif // GS_COFF_H
