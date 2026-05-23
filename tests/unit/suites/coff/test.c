// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the A/UX COFF parser (src/core/storage/coff.c).
// Constructs a tiny but format-faithful COFF binary by hand and walks
// it through coff_parse + accessors, exercising:
//   - file-header magic check
//   - optional-header (aouthdr) field reads
//   - section header table (2 sections: .text + .data)
//   - symbol table with aux records (skipped) and string-table lookup
//   - rejection of non-COFF buffers

#include "coff.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void w_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void w_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Build a minimal COFF M68K binary with two sections (.text, .data),
// three symbols (one with a long name forcing string-table use, plus
// one aux record we expect to be skipped), and inline section data.
// Returns a malloc'd buffer and *out_len.
static uint8_t *build_coff(size_t *out_len) {
    // Layout (offsets):
    //   0..19   filhdr
    //   20..47  aouthdr (28 bytes)
    //   48..127 section header table (2 × 40 bytes)
    //   128..131 .text raw data (4 bytes)
    //   132..135 .data raw data (4 bytes)
    //   136..189 symbol table (3 entries × 18 = 54 bytes — but 1 is aux,
    //            so 2 "real" symbols + 1 aux = 3 entries)
    //   190.. string table
    const uint8_t text_bytes[4] = {0x4E, 0x71, 0x4E, 0x75}; // NOP; RTS
    const uint8_t data_bytes[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    const char *long_name = "very_long_function_name";
    size_t long_name_len = strlen(long_name);

    // String table: 4-byte length prefix + null-terminated names that
    // symbol entries reference by offset.  We put the long name at
    // offset 4 (first usable byte past the length prefix).
    size_t strtab_off = 0; // populated below
    size_t strtab_len = 4 + long_name_len + 1; // prefix + name + NUL
    if (strtab_len & 1)
        strtab_len++; // pad to even

    size_t pos = 0;
    size_t filhdr_off = pos;
    pos += 20;
    size_t opthdr_off = pos;
    pos += 28;
    size_t scn1_off = pos;
    pos += 40;
    size_t scn2_off = pos;
    pos += 40;
    size_t text_raw_off = pos;
    pos += 4;
    size_t data_raw_off = pos;
    pos += 4;
    size_t symtab_off = pos;
    size_t n_symbols = 3; // 2 real + 1 aux
    pos += n_symbols * 18;
    strtab_off = pos;
    pos += strtab_len;

    uint8_t *buf = calloc(1, pos);
    if (!buf)
        return NULL;

    // filhdr
    w_u16(buf + filhdr_off + 0, 0x0150); // f_magic
    w_u16(buf + filhdr_off + 2, 2); // f_nscns
    w_u32(buf + filhdr_off + 4, 0x12345678); // f_timdat
    w_u32(buf + filhdr_off + 8, (uint32_t)symtab_off); // f_symptr
    w_u32(buf + filhdr_off + 12, (uint32_t)n_symbols); // f_nsyms
    w_u16(buf + filhdr_off + 16, 28); // f_opthdr
    w_u16(buf + filhdr_off + 18, 0x0203); // f_flags

    // aouthdr
    w_u16(buf + opthdr_off + 0, 0x0108); // magic
    w_u16(buf + opthdr_off + 2, 0); // vstamp
    w_u32(buf + opthdr_off + 4, 4); // tsize
    w_u32(buf + opthdr_off + 8, 4); // dsize
    w_u32(buf + opthdr_off + 12, 0); // bsize
    w_u32(buf + opthdr_off + 16, 0x10000000); // entry
    w_u32(buf + opthdr_off + 20, 0x10000000); // text_start
    w_u32(buf + opthdr_off + 24, 0x11000000); // data_start

    // .text section header
    memcpy(buf + scn1_off + 0, ".text\0\0\0", 8);
    w_u32(buf + scn1_off + 8, 0x10000000); // s_paddr
    w_u32(buf + scn1_off + 12, 0x10000000); // s_vaddr
    w_u32(buf + scn1_off + 16, 4); // s_size
    w_u32(buf + scn1_off + 20, (uint32_t)text_raw_off); // s_scnptr
    w_u32(buf + scn1_off + 36, 0x20); // s_flags = STYP_TEXT

    // .data section header
    memcpy(buf + scn2_off + 0, ".data\0\0\0", 8);
    w_u32(buf + scn2_off + 8, 0x11000000);
    w_u32(buf + scn2_off + 12, 0x11000000);
    w_u32(buf + scn2_off + 16, 4);
    w_u32(buf + scn2_off + 20, (uint32_t)data_raw_off);
    w_u32(buf + scn2_off + 36, 0x40); // STYP_DATA

    // Section raw bytes
    memcpy(buf + text_raw_off, text_bytes, 4);
    memcpy(buf + data_raw_off, data_bytes, 4);

    // Symbol 0: external, inline 8-char name "_start", value=0x10000000,
    // section=1 (.text), sclass=C_EXT=2, n_aux=1 (forcing skip of next).
    uint8_t *s0 = buf + symtab_off;
    memcpy(s0 + 0, "_start\0\0", 8);
    w_u32(s0 + 8, 0x10000000);
    w_u16(s0 + 12, 1); // scnum
    w_u16(s0 + 14, 0); // type
    s0[16] = COFF_C_EXT;
    s0[17] = 1; // n_aux

    // Symbol 1: aux record — content is opaque; the parser must SKIP it.
    // We fill it with arbitrary bytes to ensure the parser doesn't try to
    // interpret it as a regular symbol.
    uint8_t *s1 = buf + symtab_off + 18;
    memset(s1, 0xAA, 18);

    // Symbol 2: long-name form referencing the string table.  n_name's
    // first u32 is 0, second u32 is the offset within the string table.
    // String table offset 4 = first byte after the length prefix.
    uint8_t *s2 = buf + symtab_off + 36;
    w_u32(s2 + 0, 0);
    w_u32(s2 + 4, 4); // offset 4 = "very_long_function_name"
    w_u32(s2 + 8, 0x11000000);
    w_u16(s2 + 12, 2); // .data
    w_u16(s2 + 14, 0);
    s2[16] = COFF_C_STAT;
    s2[17] = 0;

    // String table: length prefix at +0 (includes the 4-byte length
    // itself), then "very_long_function_name\0".
    w_u32(buf + strtab_off + 0, (uint32_t)strtab_len);
    memcpy(buf + strtab_off + 4, long_name, long_name_len);
    // Trailing NUL plus optional even-padding byte already zero from calloc.

    *out_len = pos;
    return buf;
}

TEST(test_coff_is_coff) {
    uint8_t magic[2] = {0x01, 0x50};
    ASSERT_TRUE(coff_is_coff(magic, 2));
    uint8_t notmagic[2] = {0x01, 0x51};
    ASSERT_TRUE(!coff_is_coff(notmagic, 2));
    ASSERT_TRUE(!coff_is_coff(NULL, 0));
    ASSERT_TRUE(!coff_is_coff(magic, 1));
}

TEST(test_coff_rejects_bad_magic) {
    uint8_t bad[20] = {0xDE, 0xAD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const char *err = NULL;
    coff_t *cf = coff_parse(bad, sizeof(bad), &err);
    ASSERT_TRUE(cf == NULL);
    ASSERT_TRUE(err != NULL);
}

TEST(test_coff_rejects_truncated) {
    uint8_t hdr[10] = {0x01, 0x50, 0, 0, 0, 0, 0, 0, 0, 0};
    const char *err = NULL;
    coff_t *cf = coff_parse(hdr, sizeof(hdr), &err);
    ASSERT_TRUE(cf == NULL);
}

TEST(test_coff_parses_minimal) {
    size_t len = 0;
    uint8_t *buf = build_coff(&len);
    ASSERT_TRUE(buf != NULL);
    const char *err = NULL;
    coff_t *cf = coff_parse(buf, len, &err);
    ASSERT_TRUE(cf != NULL);

    // Header fields
    ASSERT_EQ_INT(0x0150, coff_magic(cf));
    ASSERT_EQ_INT(0x0203, coff_flags(cf));
    ASSERT_EQ_INT((int)0x12345678, (int)coff_timestamp(cf));
    ASSERT_EQ_INT(4, (int)coff_text_size(cf));
    ASSERT_EQ_INT(4, (int)coff_data_size(cf));
    ASSERT_EQ_INT(0, (int)coff_bss_size(cf));
    ASSERT_EQ_INT((int)0x10000000, (int)coff_entry_point(cf));
    ASSERT_EQ_INT((int)0x10000000, (int)coff_text_start(cf));
    ASSERT_EQ_INT((int)0x11000000, (int)coff_data_start(cf));

    // Sections
    ASSERT_EQ_INT(2, (int)coff_num_sections(cf));
    const coff_section_t *s0 = coff_section_at(cf, 0);
    const coff_section_t *s1 = coff_section_at(cf, 1);
    ASSERT_TRUE(s0 && s1);
    ASSERT_EQ_INT(0, strcmp(s0->name, ".text"));
    ASSERT_EQ_INT(0, strcmp(s1->name, ".data"));
    ASSERT_EQ_INT((int)0x20, (int)s0->flags); // STYP_TEXT
    ASSERT_EQ_INT((int)0x40, (int)s1->flags); // STYP_DATA
    ASSERT_EQ_INT(4, (int)s0->size);
    ASSERT_EQ_INT(4, (int)s1->size);
    const uint8_t *text_data = coff_section_data(cf, s0);
    ASSERT_TRUE(text_data != NULL);
    ASSERT_EQ_INT(0x4E, text_data[0]);
    ASSERT_EQ_INT(0x71, text_data[1]);
    ASSERT_EQ_INT(0x4E, text_data[2]);
    ASSERT_EQ_INT(0x75, text_data[3]);

    // Symbols.  3 raw entries, but the middle one is aux — should be NULL.
    ASSERT_EQ_INT(3, (int)coff_num_symbols(cf));
    const coff_symbol_t *sym0 = coff_symbol_at(cf, 0);
    const coff_symbol_t *sym1 = coff_symbol_at(cf, 1);
    const coff_symbol_t *sym2 = coff_symbol_at(cf, 2);
    ASSERT_TRUE(sym0 != NULL);
    ASSERT_TRUE(sym1 == NULL); // aux slot
    ASSERT_TRUE(sym2 != NULL);
    ASSERT_EQ_INT(0, strcmp(sym0->name, "_start"));
    ASSERT_EQ_INT((int)0x10000000, (int)sym0->value);
    ASSERT_EQ_INT(1, sym0->scnum);
    ASSERT_EQ_INT(COFF_C_EXT, sym0->sclass);
    // Long-name resolution via string table:
    ASSERT_EQ_INT(0, strcmp(sym2->name, "very_long_function_name"));
    ASSERT_EQ_INT(COFF_C_STAT, sym2->sclass);
    ASSERT_EQ_INT(2, sym2->scnum);

    coff_free(cf);
    free(buf);
}

int main(void) {
    RUN(test_coff_is_coff);
    RUN(test_coff_rejects_bad_magic);
    RUN(test_coff_rejects_truncated);
    RUN(test_coff_parses_minimal);
    fprintf(stderr, "All coff tests passed.\n");
    return 0;
}
