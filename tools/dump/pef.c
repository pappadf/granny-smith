// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pef.c
// PEF container parser.  See pef.h for the scope; field layouts follow
// Apple, "Mac OS Runtime Architectures" (1997), ch. 8, and the
// PEFBinaryFormat.h interface file.

#include "pef.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Fixed record sizes from the PEF specification
#define PEF_CONTAINER_HEADER_SIZE 40u // 'Joy!' 'peff' ... reservedA
#define PEF_SECTION_HEADER_SIZE   28u // per section
#define PEF_LOADER_HEADER_SIZE    56u // loader info header
#define PEF_LIBRARY_SIZE          24u // imported library description
#define PEF_RELOC_HEADER_SIZE     12u // per relocated section
#define PEF_EXPORT_SIZE           10u // exported symbol entry
#define CFRG_HEADER_SIZE          32u // 'cfrg' resource header
#define CFRG_MEMBER_FIXED_SIZE    42u // member bytes before the name

// Upper bound on recorded relocations; a corrupt stream with huge repeat
// counts would otherwise allocate without limit.
#define PEF_MAX_RELOCS (1u << 22)

// Maximum nesting of repeat instructions inside repeated blocks
#define PEF_MAX_REPEAT_DEPTH 8

// Big-endian readers
static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Copy a NUL-terminated string from `base + off` (bounded by `len`) into dst.
static void copy_cstr(char *dst, size_t cap, const uint8_t *base, size_t len, size_t off) {
    size_t n = 0;
    if (off < len)
        while (off + n < len && base[off + n] && n + 1 < cap)
            n++;
    if (off < len)
        memcpy(dst, base + off, n);
    dst[n] = '\0';
}

// Copy a counted string (no terminator in the source) into dst.
static void copy_counted(char *dst, size_t cap, const uint8_t *base, size_t len, size_t off, size_t n) {
    if (off > len || n > len - off)
        n = 0;
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, base + off, n);
    dst[n] = '\0';
}

// Record the first non-fatal problem; later ones are dropped.
static void set_warning(pef_t *pef, const char *msg) {
    if (!pef->warning[0])
        snprintf(pef->warning, sizeof(pef->warning), "%s", msg);
}

bool pef_is_pef(const uint8_t *bytes, size_t len) {
    return bytes && len >= PEF_CONTAINER_HEADER_SIZE && memcmp(bytes, "Joy!peff", 8) == 0;
}

const char *pef_section_kind_name(uint8_t kind) {
    static const char *const names[] = {"code",  "data",     "pidata",    "constant", "loader",
                                        "debug", "execdata", "exception", "traceback"};
    return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

const char *pef_symbol_class_name(uint8_t sym_class) {
    static const char *const names[] = {"code", "data", "tvector", "toc", "glue"};
    return sym_class < sizeof(names) / sizeof(names[0]) ? names[sym_class] : "unknown";
}

// ============================================================================
// Pattern-initialized data
// ============================================================================

// Read one variable-length argument: 7 bits per byte, high bit = more.
static int read_arg(const uint8_t *src, size_t len, size_t *pos, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        if (*pos >= len)
            return -1;
        uint8_t b = src[(*pos)++];
        v = (v << 7) | (b & 0x7F); // accumulate the 7 payload bits
        if (!(b & 0x80)) {
            *out = v;
            return 0;
        }
    }
    return -1; // more than 5 bytes cannot be a 32-bit value
}

long pef_unpack_pidata(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    size_t pos = 0, out = 0;
    while (pos < src_len) {
        uint8_t op = src[pos++];
        uint32_t count = op & 0x1F; // low 5 bits: count, 0 = argument follows
        if (count == 0 && read_arg(src, src_len, &pos, &count) != 0)
            return -1;
        switch (op >> 5) {
        case 0: // Zero: count zero bytes
            if (count > dst_cap - out)
                return -1;
            memset(dst + out, 0, count);
            out += count;
            break;
        case 1: // BlockCopy: count literal bytes
            if (count > src_len - pos || count > dst_cap - out)
                return -1;
            memcpy(dst + out, src + pos, count);
            pos += count;
            out += count;
            break;
        case 2: { // RepeatedBlock: count-byte block emitted repeat+1 times
            uint32_t repeat;
            if (read_arg(src, src_len, &pos, &repeat) != 0 || count > src_len - pos)
                return -1;
            for (uint64_t i = 0; i <= repeat; i++) {
                if (count > dst_cap - out)
                    return -1;
                memcpy(dst + out, src + pos, count);
                out += count;
            }
            pos += count;
            break;
        }
        case 3: // InterleaveRepeatBlockWithBlockCopy
        case 4: { // InterleaveRepeatBlockWithZero
            // common, then repeat x (custom, common); the stream holds the
            // common block (opcode 3 only) followed by every custom block.
            uint32_t custom, repeat;
            if (read_arg(src, src_len, &pos, &custom) != 0 || read_arg(src, src_len, &pos, &repeat) != 0)
                return -1;
            bool zero = (op >> 5) == 4;
            const uint8_t *common = src + pos;
            if (!zero) {
                if (count > src_len - pos)
                    return -1;
                pos += count;
            }
            for (uint64_t i = 0; i <= repeat; i++) {
                // Common part (zeros or the shared block)
                if (count > dst_cap - out)
                    return -1;
                if (zero)
                    memset(dst + out, 0, count);
                else
                    memcpy(dst + out, common, count);
                out += count;
                if (i == repeat)
                    break;
                // Custom part, taken from the stream in order
                if (custom > src_len - pos || custom > dst_cap - out)
                    return -1;
                memcpy(dst + out, src + pos, custom);
                pos += custom;
                out += custom;
            }
            break;
        }
        default: // opcodes 5..7 are undefined
            return -1;
        }
    }
    return (long)out;
}

// ============================================================================
// Relocations
// ============================================================================

// Interpreter state for one relocated section.
typedef struct {
    pef_t *pef; // fragment being built
    int section; // section the relocations apply to
    const uint8_t *instr; // the section's relocation instruction words
    uint32_t n_instr; // number of 16-bit instruction words
    uint32_t addr; // relocAddress: byte offset within the section
    uint32_t import_index; // next import for ImportRun
    int sect_c, sect_d; // current sectionC / sectionD indices
    size_t cap; // capacity of pef->relocs
    bool failed; // stop on the first malformed instruction
} reloc_state_t;

// Record one relocated word at the current address and advance by 4.
static void reloc_word(reloc_state_t *st, int kind, int target) {
    pef_t *pef = st->pef;
    const pef_section_t *s = &pef->sections[st->section];
    if (!s->contents || st->addr > s->total_size || s->total_size - st->addr < 4) {
        set_warning(pef, "relocation outside its section");
        st->failed = true;
        return;
    }
    if (pef->n_relocs >= PEF_MAX_RELOCS) {
        set_warning(pef, "relocation count limit reached");
        st->failed = true;
        return;
    }
    if (pef->n_relocs == st->cap) {
        size_t ncap = st->cap ? st->cap * 2 : 256;
        pef_reloc_t *n = realloc(pef->relocs, ncap * sizeof(*n));
        if (!n) {
            st->failed = true;
            return;
        }
        pef->relocs = n;
        st->cap = ncap;
    }
    pef_reloc_t *r = &pef->relocs[pef->n_relocs++];
    r->section = st->section;
    r->offset = st->addr;
    r->kind = kind;
    r->target = target;
    r->addend = be32(s->contents + st->addr); // the stored value is the addend
    st->addr += 4;
}

// Relocate one word by an import and remember where ImportRun continues.
static void reloc_import(reloc_state_t *st, uint32_t index) {
    if (index >= st->pef->n_imports) {
        set_warning(st->pef, "relocation names a missing import");
        st->failed = true;
        return;
    }
    reloc_word(st, PEF_RTGT_IMPORT, (int)index);
    st->import_index = index + 1;
}

// Execute instruction words [begin, end); `depth` bounds nested repeats.
static void reloc_exec(reloc_state_t *st, uint32_t begin, uint32_t end, int depth) {
    uint32_t i = begin;
    while (i < end && !st->failed) {
        uint16_t w = be16(st->instr + 2 * i);
        uint32_t at = i++; // index of this instruction, for repeats
        if ((w & 0xC000) == 0x0000) { // RelocBySectDWithSkip
            st->addr += ((w >> 6) & 0xFF) * 4; // skip count words
            for (uint32_t k = 0; k < (w & 0x3Fu) && !st->failed; k++)
                reloc_word(st, PEF_RTGT_SECTION, st->sect_d);
        } else if ((w & 0xE000) == 0x4000) { // relocation run group
            uint32_t run = (w & 0x1FFu) + 1;
            for (uint32_t k = 0; k < run && !st->failed; k++) {
                switch ((w >> 9) & 0xF) {
                case 0: // RelocBySectC
                    reloc_word(st, PEF_RTGT_SECTION, st->sect_c);
                    break;
                case 1: // RelocBySectD
                    reloc_word(st, PEF_RTGT_SECTION, st->sect_d);
                    break;
                case 2: // RelocTVector12: code, TOC, then one unrelocated word
                    reloc_word(st, PEF_RTGT_SECTION, st->sect_c);
                    reloc_word(st, PEF_RTGT_SECTION, st->sect_d);
                    st->addr += 4;
                    break;
                case 3: // RelocTVector8: code, TOC
                    reloc_word(st, PEF_RTGT_SECTION, st->sect_c);
                    reloc_word(st, PEF_RTGT_SECTION, st->sect_d);
                    break;
                case 4: // RelocVTable8: data pointer, then one unrelocated word
                    reloc_word(st, PEF_RTGT_SECTION, st->sect_d);
                    st->addr += 4;
                    break;
                case 5: // RelocImportRun
                    reloc_import(st, st->import_index);
                    break;
                default:
                    set_warning(st->pef, "unknown relocation run subopcode");
                    st->failed = true;
                }
            }
        } else if ((w & 0xE000) == 0x6000) { // small-index group
            uint32_t index = w & 0x1FF;
            switch ((w >> 9) & 0xF) {
            case 0: // RelocSmByImport
                reloc_import(st, index);
                break;
            case 1: // RelocSmSetSectC
                st->sect_c = (int)index;
                break;
            case 2: // RelocSmSetSectD
                st->sect_d = (int)index;
                break;
            case 3: // RelocSmBySection
                reloc_word(st, PEF_RTGT_SECTION, (int)index);
                break;
            default:
                set_warning(st->pef, "unknown small-index relocation subopcode");
                st->failed = true;
            }
        } else if ((w & 0xF000) == 0x8000) { // RelocIncrPosition
            st->addr += (w & 0x0FFFu) + 1;
        } else if ((w & 0xF000) == 0x9000) { // RelocSmRepeat
            uint32_t chunks = ((w >> 8) & 0xF) + 1;
            uint32_t repeat = (w & 0xFFu) + 1;
            if (chunks > at - begin || depth >= PEF_MAX_REPEAT_DEPTH) {
                set_warning(st->pef, "malformed relocation repeat");
                st->failed = true;
                break;
            }
            for (uint32_t k = 0; k < repeat && !st->failed; k++)
                reloc_exec(st, at - chunks, at, depth + 1);
        } else if ((w & 0xF000) == 0xA000 || (w & 0xF000) == 0xB000) { // two-word forms
            if (i >= end) {
                set_warning(st->pef, "truncated relocation instruction");
                st->failed = true;
                break;
            }
            uint16_t w2 = be16(st->instr + 2 * i++);
            switch (w & 0xFC00) {
            case 0xA000: // RelocSetPosition
                st->addr = ((uint32_t)(w & 0x3FF) << 16) | w2;
                break;
            case 0xA400: // RelocLgByImport
                reloc_import(st, ((uint32_t)(w & 0x3FF) << 16) | w2);
                break;
            case 0xB000: { // RelocLgRepeat
                uint32_t chunks = ((w >> 6) & 0xF) + 1;
                uint32_t repeat = ((uint32_t)(w & 0x3F) << 16) | w2;
                if (chunks > at - begin || depth >= PEF_MAX_REPEAT_DEPTH) {
                    set_warning(st->pef, "malformed relocation repeat");
                    st->failed = true;
                    break;
                }
                for (uint32_t k = 0; k < repeat && !st->failed; k++)
                    reloc_exec(st, at - chunks, at, depth + 1);
                break;
            }
            case 0xB400: { // RelocLgSetOrBySection
                uint32_t index = ((uint32_t)(w & 0x3F) << 16) | w2;
                switch ((w >> 6) & 0xF) {
                case 0: // RelocLgBySection
                    reloc_word(st, PEF_RTGT_SECTION, (int)index);
                    break;
                case 1: // RelocLgSetSectC
                    st->sect_c = (int)index;
                    break;
                case 2: // RelocLgSetSectD
                    st->sect_d = (int)index;
                    break;
                default:
                    set_warning(st->pef, "unknown large set-or-by-section subopcode");
                    st->failed = true;
                }
                break;
            }
            default:
                set_warning(st->pef, "unknown relocation opcode");
                st->failed = true;
            }
        } else {
            set_warning(st->pef, "unknown relocation opcode");
            st->failed = true;
        }
    }
}

// Order relocations by (section, offset) for binary search.
static int cmp_reloc(const void *a, const void *b) {
    const pef_reloc_t *x = a, *y = b;
    if (x->section != y->section)
        return x->section < y->section ? -1 : 1;
    return x->offset < y->offset ? -1 : (x->offset > y->offset);
}

const pef_reloc_t *pef_find_reloc(const pef_t *pef, int section, uint32_t offset) {
    size_t lo = 0, hi = pef->n_relocs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const pef_reloc_t *r = &pef->relocs[mid];
        if (r->section == section && r->offset == offset)
            return r;
        if (r->section < section || (r->section == section && r->offset < offset))
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

// ============================================================================
// Loader section
// ============================================================================

// Parse the loader section's imports, exports and relocations.
static int parse_loader(pef_t *pef, const uint8_t *ld, size_t len, const char **errmsg) {
    if (len < PEF_LOADER_HEADER_SIZE) {
        *errmsg = "loader section too small";
        return -1;
    }
    pef->has_loader = true;
    pef->main_section = (int32_t)be32(ld + 0);
    pef->main_offset = be32(ld + 4);
    pef->init_section = (int32_t)be32(ld + 8);
    pef->init_offset = be32(ld + 12);
    pef->term_section = (int32_t)be32(ld + 16);
    pef->term_offset = be32(ld + 20);
    uint32_t n_libs = be32(ld + 24);
    uint32_t n_syms = be32(ld + 28);
    uint32_t n_reloc_sects = be32(ld + 32);
    uint32_t reloc_instr_off = be32(ld + 36);
    uint32_t strings_off = be32(ld + 40);
    uint32_t hash_off = be32(ld + 44);
    uint32_t hash_power = be32(ld + 48);
    uint32_t n_exports = be32(ld + 52);

    // Imported libraries and symbols follow the header back to back.
    uint64_t libs_end = PEF_LOADER_HEADER_SIZE + (uint64_t)n_libs * PEF_LIBRARY_SIZE;
    uint64_t syms_end = libs_end + (uint64_t)n_syms * 4;
    uint64_t rhdr_end = syms_end + (uint64_t)n_reloc_sects * PEF_RELOC_HEADER_SIZE;
    if (rhdr_end > len || strings_off > len) {
        *errmsg = "loader tables overrun the loader section";
        return -1;
    }
    const uint8_t *strs = ld + strings_off;
    size_t strs_len = len - strings_off;

    pef->libraries = calloc(n_libs ? n_libs : 1, sizeof(*pef->libraries));
    pef->imports = calloc(n_syms ? n_syms : 1, sizeof(*pef->imports));
    if (!pef->libraries || !pef->imports) {
        *errmsg = "out of memory";
        return -1;
    }
    pef->n_libraries = n_libs;
    pef->n_imports = n_syms;
    for (uint32_t i = 0; i < n_syms; i++)
        pef->imports[i].library = -1;
    for (uint32_t i = 0; i < n_libs; i++) {
        const uint8_t *e = ld + PEF_LOADER_HEADER_SIZE + i * PEF_LIBRARY_SIZE;
        pef_library_t *lib = &pef->libraries[i];
        copy_cstr(lib->name, sizeof(lib->name), strs, strs_len, be32(e));
        lib->old_imp_version = be32(e + 4);
        lib->current_version = be32(e + 8);
        lib->symbol_count = be32(e + 12);
        lib->first_symbol = be32(e + 16);
        lib->options = e[20];
        // Tag each imported symbol with its library
        for (uint32_t k = 0; k < lib->symbol_count; k++)
            if ((uint64_t)lib->first_symbol + k < n_syms)
                pef->imports[lib->first_symbol + k].library = (int)i;
    }
    for (uint32_t i = 0; i < n_syms; i++) {
        uint32_t v = be32(ld + libs_end + i * 4);
        pef_import_t *im = &pef->imports[i];
        im->sym_class = (uint8_t)((v >> 24) & 0x0F); // low 4 bits of the class byte
        im->weak = (v >> 24) & 0x80; // high bit: weak import
        copy_cstr(im->name, sizeof(im->name), strs, strs_len, v & 0xFFFFFF);
    }

    // Exports: hash table, then key table (name lengths), then symbols.
    if (n_exports) {
        uint64_t keys_off = hash_off + ((uint64_t)4 << (hash_power & 31));
        uint64_t syms_off = keys_off + (uint64_t)n_exports * 4;
        if (hash_power > 24 || syms_off + (uint64_t)n_exports * PEF_EXPORT_SIZE > len) {
            set_warning(pef, "export tables overrun the loader section");
        } else {
            pef->exports = calloc(n_exports, sizeof(*pef->exports));
            if (!pef->exports) {
                *errmsg = "out of memory";
                return -1;
            }
            pef->n_exports = n_exports;
            for (uint32_t i = 0; i < n_exports; i++) {
                const uint8_t *k = ld + keys_off + i * 4;
                const uint8_t *s = ld + syms_off + i * PEF_EXPORT_SIZE;
                pef_export_t *ex = &pef->exports[i];
                uint32_t cn = be32(s);
                ex->sym_class = (uint8_t)((cn >> 24) & 0x0F);
                ex->value = be32(s + 4);
                ex->section = (int16_t)be16(s + 8);
                copy_counted(ex->name, sizeof(ex->name), strs, strs_len, cn & 0xFFFFFF, be16(k)); // key high half
            }
        }
    }

    // Relocations, one header per relocated section.
    for (uint32_t i = 0; i < n_reloc_sects; i++) {
        const uint8_t *h = ld + syms_end + i * PEF_RELOC_HEADER_SIZE;
        uint16_t sect = be16(h);
        uint32_t count = be32(h + 4);
        uint64_t first = (uint64_t)reloc_instr_off + be32(h + 8);
        if (sect >= pef->section_count || first + (uint64_t)count * 2 > len) {
            set_warning(pef, "relocation header out of range");
            continue;
        }
        reloc_state_t st = {
            .pef = pef,
            .section = sect,
            .instr = ld + first,
            .n_instr = count,
            .sect_c = 0, // sectionC starts at section 0
            .sect_d = 1, // sectionD starts at section 1
            .cap = pef->n_relocs, // realloc grows from the current size
        };
        reloc_exec(&st, 0, count, 0);
    }
    if (pef->n_relocs > 1)
        qsort(pef->relocs, pef->n_relocs, sizeof(*pef->relocs), cmp_reloc);
    return 0;
}

// ============================================================================
// Container
// ============================================================================

int pef_parse(const uint8_t *bytes, size_t len, pef_t *out, const char **errmsg) {
    static const char *dummy;
    if (!errmsg)
        errmsg = &dummy;
    memset(out, 0, sizeof(*out));
    if (!pef_is_pef(bytes, len)) {
        *errmsg = "not a PEF container (no 'Joy!peff' tag)";
        return -1;
    }
    out->architecture = be32(bytes + 8);
    out->format_version = be32(bytes + 12);
    out->date_time_stamp = be32(bytes + 16);
    out->old_def_version = be32(bytes + 20);
    out->old_imp_version = be32(bytes + 24);
    out->current_version = be32(bytes + 28);
    out->section_count = be16(bytes + 32);
    out->inst_section_count = be16(bytes + 34);
    out->main_section = out->init_section = out->term_section = -1;

    size_t names_off = PEF_CONTAINER_HEADER_SIZE + (size_t)out->section_count * PEF_SECTION_HEADER_SIZE;
    if (names_off > len) {
        *errmsg = "section table overruns the container";
        return -1;
    }
    out->sections = calloc(out->section_count ? out->section_count : 1, sizeof(*out->sections));
    if (!out->sections) {
        *errmsg = "out of memory";
        return -1;
    }

    int loader = -1;
    for (int i = 0; i < out->section_count; i++) {
        const uint8_t *h = bytes + PEF_CONTAINER_HEADER_SIZE + (size_t)i * PEF_SECTION_HEADER_SIZE;
        pef_section_t *s = &out->sections[i];
        s->index = i;
        int32_t name_off = (int32_t)be32(h);
        if (name_off >= 0)
            copy_cstr(s->name, sizeof(s->name), bytes, len, names_off + (size_t)name_off);
        s->default_address = be32(h + 4);
        s->total_size = be32(h + 8);
        s->unpacked_size = be32(h + 12);
        s->packed_size = be32(h + 16);
        s->container_offset = be32(h + 20);
        s->kind = h[24];
        s->share_kind = h[25];
        s->alignment = h[26];
        if ((uint64_t)s->container_offset + s->packed_size > len) {
            *errmsg = "section data overruns the container";
            pef_free(out);
            return -1;
        }
        if (s->kind == PEF_SECT_LOADER)
            loader = i;
        // Code, data, pidata, constant and executable data occupy memory.
        s->instantiated = s->kind <= PEF_SECT_CONSTANT || s->kind == PEF_SECT_EXEC_DATA;
        if (!s->instantiated)
            continue;
        // A pathological size cannot be real; cap allocations at 256 MiB.
        if (s->total_size > (256u << 20) || s->unpacked_size > s->total_size) {
            *errmsg = "implausible section size";
            pef_free(out);
            return -1;
        }
        s->contents = calloc(s->total_size ? s->total_size : 1, 1);
        if (!s->contents) {
            *errmsg = "out of memory";
            pef_free(out);
            return -1;
        }
        const uint8_t *src = bytes + s->container_offset;
        if (s->kind == PEF_SECT_PATTERN_DATA) {
            long n = pef_unpack_pidata(src, s->packed_size, s->contents, s->unpacked_size);
            if (n < 0)
                set_warning(out, "malformed pattern-initialized data");
            else if ((uint32_t)n != s->unpacked_size)
                set_warning(out, "pattern-initialized data unpacked to the wrong size");
        } else {
            uint32_t n = s->packed_size < s->unpacked_size ? s->packed_size : s->unpacked_size;
            memcpy(s->contents, src, n); // the rest stays zero
        }
    }

    if (loader >= 0) {
        const pef_section_t *ls = &out->sections[loader];
        if (parse_loader(out, bytes + ls->container_offset, ls->packed_size, errmsg) != 0) {
            pef_free(out);
            return -1;
        }
    }
    return 0;
}

void pef_free(pef_t *pef) {
    if (!pef)
        return;
    if (pef->sections)
        for (int i = 0; i < pef->section_count; i++)
            free(pef->sections[i].contents);
    free(pef->sections);
    free(pef->libraries);
    free(pef->imports);
    free(pef->exports);
    free(pef->relocs);
    memset(pef, 0, sizeof(*pef));
}

// ============================================================================
// Traceback tables
// ============================================================================
//
// The AIX/PowerOpen ABI compilers (MrC, CodeWarrior, xlc) end each function
// with a zero word followed by a traceback table:
//   +0 version  +1 lang  +2 flags1  +3 flags2  +4 flags3  +5 flags4
//   +6 fixedparms  +7 floatparms<<1 | parmsonstk
// then optional fields in this order: parminfo (when any parameters),
// tb_offset (has_tboff), hand_mask (int_hndl), ctl_info + displacements
// (has_ctl), name_len + name (name_present), alloca_reg (uses_alloca).
// tb_offset is the distance from the function's first instruction to the
// table, which is what turns a table into a function boundary.

pef_tbfunc_t *pef_scan_traceback(const uint8_t *code, size_t len, size_t *n_out) {
    pef_tbfunc_t *funcs = NULL;
    size_t n = 0, cap = 0;
    *n_out = 0;
    for (size_t i = 4; i + 12 <= len; i += 4) {
        if (be32(code + i) != 0)
            continue;
        const uint8_t *t = code + i + 4;
        size_t avail = len - (i + 4);
        bool has_tboff = t[2] & 0x20;
        bool has_ctl = t[2] & 0x08;
        bool int_hndl = t[3] & 0x80;
        bool name_present = t[3] & 0x40;
        // version 0 and a known language id
        if (t[0] != 0 || t[1] > 0x0F)
            continue;
        // Without tb_offset the table must follow a return (blr or bctr).
        uint32_t prev = be32(code + i - 4);
        if (!has_tboff && prev != 0x4E800020 && prev != 0x4E800420)
            continue;
        size_t p = 8;
        if (t[6] || (t[7] >> 1)) // parminfo present when any parameter is
            p += 4;
        uint32_t tb_off = 0;
        if (has_tboff) {
            if (p + 4 > avail)
                continue;
            tb_off = be32(t + p);
            p += 4;
        }
        if (int_hndl)
            p += 4;
        if (has_ctl) {
            if (p + 4 > avail)
                continue;
            uint32_t nctl = be32(t + p);
            if (nctl > 64)
                continue;
            p += 4 + 4 * (size_t)nctl;
        }
        // The function must start on a word boundary before the table.
        if (has_tboff && (tb_off == 0 || tb_off > i || (tb_off & 3)))
            continue;
        pef_tbfunc_t f = {.start = (uint32_t)(i - tb_off), .has_start = has_tboff, .tb_offset = (uint32_t)i};
        if (name_present) {
            if (p + 2 > avail)
                continue;
            uint16_t nl = be16(t + p);
            if (nl == 0 || nl >= sizeof(f.name) || p + 2 + nl > avail)
                continue;
            bool printable = true;
            for (uint16_t k = 0; k < nl; k++)
                if (t[p + 2 + k] < 0x20 || t[p + 2 + k] > 0x7E)
                    printable = false;
            if (!printable)
                continue;
            memcpy(f.name, t + p + 2, nl);
            f.name[nl] = '\0';
            p += 2 + (size_t)nl;
        }
        if (t[3] & 0x20) // uses_alloca: one register byte closes the table
            p += 1;
        f.tb_end = (uint32_t)((i + 4 + p + 3) & ~(size_t)3); // table end, word-aligned
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            pef_tbfunc_t *g = realloc(funcs, cap * sizeof(*g));
            if (!g) {
                free(funcs);
                return NULL;
            }
            funcs = g;
        }
        funcs[n++] = f;
    }
    *n_out = n;
    return funcs; // tables are found in address order
}

// ============================================================================
// 'cfrg' resource
// ============================================================================

int pef_parse_cfrg(const uint8_t *bytes, size_t len, pef_cfrg_member_t *out, int cap) {
    if (!bytes || len < CFRG_HEADER_SIZE)
        return -1;
    uint16_t count = be16(bytes + 30); // memberCount closes the header
    size_t off = CFRG_HEADER_SIZE;
    int n = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (off + CFRG_MEMBER_FIXED_SIZE + 1 > len)
            return -1;
        const uint8_t *m = bytes + off;
        uint16_t member_size = be16(m + 40); // after uWhere1 (4), uWhere2 (2), extensionCount (2)
        if (member_size < CFRG_MEMBER_FIXED_SIZE + 1 || off + member_size > len)
            return -1;
        if (n < cap) {
            pef_cfrg_member_t *o = &out[n++];
            o->architecture = be32(m);
            o->current_version = be32(m + 8);
            o->usage = m[22];
            o->where = m[23];
            o->offset = be32(m + 24);
            o->length = be32(m + 28);
            copy_counted(o->name, sizeof(o->name), m, member_size, CFRG_MEMBER_FIXED_SIZE + 1,
                         m[CFRG_MEMBER_FIXED_SIZE]);
        }
        off += member_size;
    }
    return n;
}
