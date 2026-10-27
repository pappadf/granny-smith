// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pef.h
// Parser for PEF (Preferred Executable Format) code fragments — the
// container the Code Fragment Manager loads on PowerPC Macs (applications,
// shared libraries, 'ndrv' native drivers).  Reference: Apple, "Mac OS
// Runtime Architectures" (1997), ch. 8 "PEF Structure".
//
// The parser works on an in-memory fragment (the data-fork bytes a 'cfrg'
// member points at) and produces:
//   - the container header and every section header (name, kind, sizes);
//   - each instantiated section's contents, with pattern-initialized data
//     ('pidata') unpacked and the zero-filled tail appended;
//   - the loader section: main/init/term entry points, imported libraries
//     and symbols, exported symbols;
//   - the relocations, run symbolically: every relocated word is recorded
//     as "section S + addend" or "import I + addend" rather than resolved
//     against a load address, so the dump can name what each pointer means.
//
// Also: a scanner for the traceback tables compilers place after each
// function (they carry the function's name in debug builds), and a reader
// for the 'cfrg' resource that locates fragments inside the data fork.
//
// All multi-byte fields are big-endian.  No global state; everything a
// parse allocates hangs off pef_t and is released by pef_free().

#pragma once

#ifndef GS_TOOLS_PEF_H
#define GS_TOOLS_PEF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Section kinds (sectionKind byte of a section header)
enum {
    PEF_SECT_CODE = 0, // executable code
    PEF_SECT_UNPACKED_DATA = 1, // initialized data, stored verbatim
    PEF_SECT_PATTERN_DATA = 2, // initialized data, pattern-compressed (pidata)
    PEF_SECT_CONSTANT = 3, // read-only data
    PEF_SECT_LOADER = 4, // imports, exports, relocations (not instantiated)
    PEF_SECT_DEBUG = 5, // reserved
    PEF_SECT_EXEC_DATA = 6, // code and data together
    PEF_SECT_EXCEPTION = 7, // reserved
    PEF_SECT_TRACEBACK = 8, // reserved
};

// Imported/exported symbol classes (low 4 bits of the class byte)
enum {
    PEF_SYM_CODE = 0, // code address
    PEF_SYM_DATA = 1, // data address
    PEF_SYM_TVECT = 2, // transition vector (code address + TOC)
    PEF_SYM_TOC = 3, // TOC entry
    PEF_SYM_GLUE = 4, // linker-inserted glue
};

// Relocation target kinds
enum {
    PEF_RTGT_SECTION = 0, // base of an instantiated section + addend
    PEF_RTGT_IMPORT = 1, // imported symbol + addend
};

// One section header plus its instantiated contents.
typedef struct {
    int index; // position in the section table
    char name[64]; // from the section name table; empty when unnamed
    uint32_t default_address; // preferred load address
    uint32_t total_size; // in-memory size (>= unpacked_size; tail is zero)
    uint32_t unpacked_size; // size of the initialized part
    uint32_t packed_size; // bytes occupied in the container
    uint32_t container_offset; // file offset of the packed bytes
    uint8_t kind; // PEF_SECT_*
    uint8_t share_kind; // 1 process, 4 global, 5 protected
    uint8_t alignment; // log2 of the required alignment
    bool instantiated; // true for kinds that occupy memory at run time
    uint8_t *contents; // total_size bytes (NULL for non-instantiated kinds)
} pef_section_t;

// One imported library description.
typedef struct {
    char name[64]; // library name, e.g. "DriverServicesLib"
    uint32_t old_imp_version; // oldest implementation version accepted
    uint32_t current_version; // version linked against
    uint32_t first_symbol; // index of its first symbol in imports[]
    uint32_t symbol_count; // number of symbols imported from it
    uint8_t options; // 0x40 weak library, 0x80 init before
} pef_library_t;

// One imported symbol.
typedef struct {
    char name[128]; // symbol name
    uint8_t sym_class; // PEF_SYM_*
    bool weak; // weak import (may be missing at run time)
    int library; // index into libraries[]
} pef_import_t;

// One exported symbol.
typedef struct {
    char name[128]; // symbol name
    uint8_t sym_class; // PEF_SYM_*
    uint32_t value; // offset within its section (or absolute)
    int16_t section; // section index; -2 absolute, -3 re-exported import
} pef_export_t;

// One relocated 32-bit word, recorded symbolically.
typedef struct {
    int section; // section the word lives in
    uint32_t offset; // byte offset of the word within that section
    int kind; // PEF_RTGT_SECTION or PEF_RTGT_IMPORT
    int target; // section index or import index
    uint32_t addend; // the word's stored value (added to the target)
} pef_reloc_t;

// Parsed fragment.  Arrays are owned by the struct.
typedef struct {
    // Container header
    uint32_t architecture; // 'pwpc' or 'm68k'
    uint32_t format_version; // 1
    uint32_t date_time_stamp; // seconds since 1904
    uint32_t old_def_version, old_imp_version, current_version; // fragment versions
    uint16_t section_count; // total sections
    uint16_t inst_section_count; // instantiated sections (they come first)

    pef_section_t *sections; // section_count entries

    // Loader section (absent when no loader section exists)
    bool has_loader; // a loader section was found and parsed
    int32_t main_section, init_section, term_section; // -1 when absent
    uint32_t main_offset, init_offset, term_offset; // offsets within those sections
    pef_library_t *libraries; // imported libraries
    size_t n_libraries;
    pef_import_t *imports; // imported symbols, in symbol-index order
    size_t n_imports;
    pef_export_t *exports; // exported symbols, in table order
    size_t n_exports;
    pef_reloc_t *relocs; // relocated words, in execution order
    size_t n_relocs;
    char warning[160]; // first non-fatal problem met while parsing, or ""
} pef_t;

// A function boundary recovered from a traceback table.
typedef struct {
    uint32_t start; // offset of the function's first instruction (valid when has_start)
    bool has_start; // false for tables without tb_offset (e.g. glue stubs)
    uint32_t tb_offset; // offset of the traceback table's zero word
    uint32_t tb_end; // offset just past the table, rounded up to a word
    char name[128]; // the name stored in the table ("" when not present)
} pef_tbfunc_t;

// One fragment located by a 'cfrg' resource member.
typedef struct {
    uint32_t architecture; // 'pwpc' / 'm68k'
    uint8_t usage; // 0 import library, 1 application, 2 drop-in, 3 stub, 4 weak stub
    uint8_t where; // 1 data fork, 2 memory, 3 resource
    uint32_t offset; // offset within the data fork
    uint32_t length; // length (0 = to the end of the fork)
    uint32_t current_version; // fragment version
    char name[64]; // member name ("" when unnamed)
} pef_cfrg_member_t;

// True when `bytes` begins with the PEF container tags 'Joy!' 'peff'.
bool pef_is_pef(const uint8_t *bytes, size_t len);

// Parse a fragment.  On success returns 0 and fills *out (release with
// pef_free).  On failure returns a negative value and sets *errmsg to a
// static description; *out is left empty and needs no pef_free.
int pef_parse(const uint8_t *bytes, size_t len, pef_t *out, const char **errmsg);

// Release everything pef_parse allocated.
void pef_free(pef_t *pef);

// Unpack pattern-initialized data into `dst` (dst_cap bytes).  Returns the
// number of bytes produced, or -1 on a malformed stream or overflow.
long pef_unpack_pidata(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap);

// Human-readable names for section kinds and symbol classes.
const char *pef_section_kind_name(uint8_t kind);
const char *pef_symbol_class_name(uint8_t sym_class);

// Find the relocation record covering `offset` in `section`, or NULL.
const pef_reloc_t *pef_find_reloc(const pef_t *pef, int section, uint32_t offset);

// Scan a PowerPC code section for traceback tables (malloc'd array in
// address order; the count goes to *n_out).  Tables with a tb_offset give
// the function's start; tables without one (glue, hand-written stubs) are
// only accepted right after a blr/bctr and give just their own extent.
// Returns NULL with *n_out = 0 when none is found.
pef_tbfunc_t *pef_scan_traceback(const uint8_t *code, size_t len, size_t *n_out);

// Parse a 'cfrg' resource.  Returns the number of members written to
// `out` (at most `cap`), or -1 when the resource is malformed.
int pef_parse_cfrg(const uint8_t *bytes, size_t len, pef_cfrg_member_t *out, int cap);

#endif // GS_TOOLS_PEF_H
