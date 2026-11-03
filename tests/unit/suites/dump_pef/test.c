// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the dump tool's PEF parser (tools/dump/pef.c).  Builds a
// small container by hand -- a code section with one named function, a
// pattern-initialized data section holding a transition vector and two
// import slots, and a loader section with one library, two imports, one
// export and a relocation program -- and checks every layer the parser
// produces.  Also covers each pidata opcode, the traceback scanner and the
// 'cfrg' resource reader on their own.

#include "pef.h"
#include "test_assert.h"

#include <stdint.h>
#include <string.h>

// Big-endian writers for building fixtures
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Fixture offsets within the container
enum {
    LOADER_OFF = 128, // loader section
    LOADER_LEN = 154, // its length
    CODE_OFF = 284, // code section
    CODE_LEN = 32, // its length
    DATA_OFF = 316, // packed data section
    DATA_PACKED = 17, // packed length
    CONTAINER_LEN = 333, // whole container
};

// Code: li r3,0 / blr, then a traceback table naming the function "Func".
static void build_code(uint8_t *c) {
    put32(c + 0, 0x38600000); // li r3,0
    put32(c + 4, 0x4E800020); // blr
    put32(c + 8, 0); // traceback table marker
    put32(c + 12, 0x00002040); // version 0, lang 0, has_tboff, name_present
    put32(c + 16, 0); // no saved registers, no parameters
    put32(c + 20, 8); // tb_offset: the function starts 8 bytes earlier
    put16(c + 24, 4); // name length
    memcpy(c + 26, "Func", 4);
    put16(c + 30, 0); // padding
}

// Build the whole container into buf (CONTAINER_LEN bytes).
static void build_pef(uint8_t *buf) {
    memset(buf, 0, CONTAINER_LEN);
    // Container header
    memcpy(buf, "Joy!peffpwpc", 12);
    put32(buf + 12, 1); // format version
    put16(buf + 32, 3); // section count
    put16(buf + 34, 2); // instantiated sections
    // Section headers: code, pidata, loader (no names)
    uint8_t *h = buf + 40;
    put32(h + 0, 0xFFFFFFFF);
    put32(h + 8, CODE_LEN);
    put32(h + 12, CODE_LEN);
    put32(h + 16, CODE_LEN);
    put32(h + 20, CODE_OFF);
    h[24] = 0; // code
    h[25] = 4;
    h[26] = 4;
    h += 28;
    put32(h + 0, 0xFFFFFFFF);
    put32(h + 8, 24); // total: 16 initialized + 8 zero-filled
    put32(h + 12, 16);
    put32(h + 16, DATA_PACKED);
    put32(h + 20, DATA_OFF);
    h[24] = 2; // pidata
    h[25] = 1;
    h[26] = 4;
    h += 28;
    put32(h + 0, 0xFFFFFFFF);
    put32(h + 16, LOADER_LEN);
    put32(h + 20, LOADER_OFF);
    h[24] = 4; // loader

    build_code(buf + CODE_OFF);

    // Data: one block copy of 16 bytes -- TVector {0, 8}, then two slots (0, 4)
    uint8_t *d = buf + DATA_OFF;
    d[0] = 0x30; // BlockCopy, count 16
    put32(d + 1, 0);
    put32(d + 5, 8);
    put32(d + 9, 0);
    put32(d + 13, 4);

    // Loader section
    uint8_t *l = buf + LOADER_OFF;
    put32(l + 0, 0xFFFFFFFF); // no main
    put32(l + 8, 0xFFFFFFFF); // no init
    put32(l + 16, 0xFFFFFFFF); // no term
    put32(l + 24, 1); // libraries
    put32(l + 28, 2); // imported symbols
    put32(l + 32, 1); // relocated sections
    put32(l + 36, 100); // relocation instructions
    put32(l + 40, 108); // strings
    put32(l + 44, 136); // export hash table
    put32(l + 48, 0); // hash power: one slot
    put32(l + 52, 1); // exports
    // Library "TestLib": symbols 0..1
    put32(l + 56, 0);
    put32(l + 68, 2);
    put32(l + 72, 0);
    // Imported symbols: tvector "Alpha", weak tvector "Beta"
    put32(l + 80, (2u << 24) | 8);
    put32(l + 84, (0x82u << 24) | 14);
    // Relocation header: section 1, three instructions at +0
    put16(l + 88, 1);
    put32(l + 92, 3);
    put32(l + 96, 0);
    // Relocation program
    put16(l + 100, 0x4600); // RelocTVector8, run of 1: code word, TOC word
    put16(l + 102, 0x4A00); // RelocImportRun, run of 1: import 0
    put16(l + 104, 0x6001); // RelocSmByImport: import 1
    // Strings
    memcpy(l + 108, "TestLib\0Alpha\0Beta\0Exported", 27);
    // Export hash slot, key (name length 8), and symbol
    put32(l + 136, 1u << 18);
    put32(l + 140, 8u << 16);
    put32(l + 144, (2u << 24) | 19);
    put32(l + 148, 0);
    put16(l + 152, 1);
}

// Header, sections and pattern-initialized data
TEST(test_pef_container_and_sections) {
    uint8_t buf[CONTAINER_LEN];
    build_pef(buf);
    ASSERT_TRUE(pef_is_pef(buf, sizeof(buf)));
    pef_t pef;
    const char *err = NULL;
    ASSERT_EQ_INT(pef_parse(buf, sizeof(buf), &pef, &err), 0);
    ASSERT_EQ_INT(pef.architecture, 0x70777063);
    ASSERT_EQ_INT(pef.section_count, 3);
    ASSERT_EQ_INT(pef.sections[0].kind, PEF_SECT_CODE);
    ASSERT_TRUE(pef.sections[0].instantiated);
    ASSERT_TRUE(!pef.sections[2].instantiated);
    ASSERT_TRUE(memcmp(pef.sections[0].contents, buf + CODE_OFF, CODE_LEN) == 0);
    // pidata: unpacked TVector and slots, then the zero-filled tail
    const uint8_t *d = pef.sections[1].contents;
    ASSERT_EQ_INT(d[7], 8);
    ASSERT_EQ_INT(d[15], 4);
    for (int i = 16; i < 24; i++)
        ASSERT_EQ_INT(d[i], 0);
    ASSERT_EQ_INT(pef.warning[0], 0);
    pef_free(&pef);
}

// Loader: library, imports, export
TEST(test_pef_loader_tables) {
    uint8_t buf[CONTAINER_LEN];
    build_pef(buf);
    pef_t pef;
    ASSERT_EQ_INT(pef_parse(buf, sizeof(buf), &pef, NULL), 0);
    ASSERT_TRUE(pef.has_loader);
    ASSERT_EQ_INT(pef.init_section, -1);
    ASSERT_EQ_INT((int)pef.n_libraries, 1);
    ASSERT_TRUE(strcmp(pef.libraries[0].name, "TestLib") == 0);
    ASSERT_EQ_INT((int)pef.n_imports, 2);
    ASSERT_TRUE(strcmp(pef.imports[0].name, "Alpha") == 0);
    ASSERT_TRUE(strcmp(pef.imports[1].name, "Beta") == 0);
    ASSERT_EQ_INT(pef.imports[0].sym_class, PEF_SYM_TVECT);
    ASSERT_TRUE(!pef.imports[0].weak);
    ASSERT_TRUE(pef.imports[1].weak);
    ASSERT_EQ_INT(pef.imports[1].library, 0);
    ASSERT_EQ_INT((int)pef.n_exports, 1);
    ASSERT_TRUE(strcmp(pef.exports[0].name, "Exported") == 0);
    ASSERT_EQ_INT(pef.exports[0].sym_class, PEF_SYM_TVECT);
    ASSERT_EQ_INT(pef.exports[0].section, 1);
    pef_free(&pef);
}

// Relocations are recorded symbolically with the stored word as addend.
TEST(test_pef_relocations) {
    uint8_t buf[CONTAINER_LEN];
    build_pef(buf);
    pef_t pef;
    ASSERT_EQ_INT(pef_parse(buf, sizeof(buf), &pef, NULL), 0);
    ASSERT_EQ_INT((int)pef.n_relocs, 4);
    const pef_reloc_t *r = pef_find_reloc(&pef, 1, 0); // TVector code word
    ASSERT_TRUE(r && r->kind == PEF_RTGT_SECTION && r->target == 0 && r->addend == 0);
    r = pef_find_reloc(&pef, 1, 4); // TVector TOC word
    ASSERT_TRUE(r && r->kind == PEF_RTGT_SECTION && r->target == 1 && r->addend == 8);
    r = pef_find_reloc(&pef, 1, 8); // ImportRun
    ASSERT_TRUE(r && r->kind == PEF_RTGT_IMPORT && r->target == 0);
    r = pef_find_reloc(&pef, 1, 12); // SmByImport, with an addend
    ASSERT_TRUE(r && r->kind == PEF_RTGT_IMPORT && r->target == 1 && r->addend == 4);
    ASSERT_TRUE(pef_find_reloc(&pef, 1, 16) == NULL);
    pef_free(&pef);
}

// Repeat and position instructions, run on a fixture variant.
TEST(test_pef_relocation_repeat_and_position) {
    uint8_t buf[CONTAINER_LEN];
    build_pef(buf);
    uint8_t *l = buf + LOADER_OFF;
    put16(l + 100, 0x6600); // RelocSmBySection: section 0 at offset 0
    put16(l + 102, 0x9100); // RelocSmRepeat: repeat the last 2 chunks once (only 1 precedes: malformed)
    put16(l + 104, 0x8003); // RelocIncrPosition by 4
    pef_t pef;
    ASSERT_EQ_INT(pef_parse(buf, sizeof(buf), &pef, NULL), 0);
    ASSERT_TRUE(pef.warning[0] != 0); // the bad repeat is reported, not fatal
    ASSERT_EQ_INT((int)pef.n_relocs, 1);
    pef_free(&pef);

    build_pef(buf);
    put16(l + 100, 0x6600); // RelocSmBySection: section 0 at offset 0
    put16(l + 102, 0x9000); // RelocSmRepeat: repeat the last chunk once -> offset 4
    put16(l + 104, 0x8003); // RelocIncrPosition by 4 -> offset 12
    put32(l + 92, 4); // four instructions
    put16(l + 106, 0x6600); // and one more word at offset 12
    // Strings start at 108 so the program fits exactly.
    ASSERT_EQ_INT(pef_parse(buf, sizeof(buf), &pef, NULL), 0);
    ASSERT_EQ_INT(pef.warning[0], 0);
    ASSERT_EQ_INT((int)pef.n_relocs, 3);
    ASSERT_TRUE(pef_find_reloc(&pef, 1, 0) && pef_find_reloc(&pef, 1, 4) && pef_find_reloc(&pef, 1, 12));
    pef_free(&pef);
}

// Every pattern-initialized data opcode, the variable-length argument, and errors.
TEST(test_pef_pidata_opcodes) {
    uint8_t out[300];
    // Zero 3, BlockCopy 2 "hi"
    const uint8_t a[] = {0x03, 0x22, 'h', 'i'};
    ASSERT_EQ_INT(pef_unpack_pidata(a, sizeof(a), out, sizeof(out)), 5);
    ASSERT_TRUE(memcmp(out, "\0\0\0hi", 5) == 0);
    // RepeatedBlock: "ab" emitted 3 times (repeat count 2)
    const uint8_t b[] = {0x42, 0x02, 'a', 'b'};
    ASSERT_EQ_INT(pef_unpack_pidata(b, sizeof(b), out, sizeof(out)), 6);
    ASSERT_TRUE(memcmp(out, "ababab", 6) == 0);
    // InterleaveRepeatBlockWithBlockCopy: common "X", customs "ab" "cd"
    const uint8_t c[] = {0x61, 0x02, 0x02, 'X', 'a', 'b', 'c', 'd'};
    ASSERT_EQ_INT(pef_unpack_pidata(c, sizeof(c), out, sizeof(out)), 7);
    ASSERT_TRUE(memcmp(out, "XabXcdX", 7) == 0);
    // InterleaveRepeatBlockWithZero: one zero, custom "Q", once
    const uint8_t z[] = {0x81, 0x01, 0x01, 'Q'};
    ASSERT_EQ_INT(pef_unpack_pidata(z, sizeof(z), out, sizeof(out)), 3);
    ASSERT_TRUE(memcmp(out, "\0Q\0", 3) == 0);
    // Count 0 takes a variable-length argument: 0x81 0x02 = 130 zero bytes
    const uint8_t v[] = {0x00, 0x81, 0x02};
    ASSERT_EQ_INT(pef_unpack_pidata(v, sizeof(v), out, sizeof(out)), 130);
    // Overflowing the destination and undefined opcodes are errors
    ASSERT_EQ_INT(pef_unpack_pidata(v, sizeof(v), out, 100), -1);
    const uint8_t bad[] = {0xA1};
    ASSERT_EQ_INT(pef_unpack_pidata(bad, sizeof(bad), out, sizeof(out)), -1);
}

// Traceback tables give named function boundaries.
TEST(test_pef_traceback_scan) {
    uint8_t code[CODE_LEN];
    build_code(code);
    size_t n = 0;
    pef_tbfunc_t *f = pef_scan_traceback(code, sizeof(code), &n);
    ASSERT_EQ_INT((int)n, 1);
    ASSERT_TRUE(f[0].has_start);
    ASSERT_EQ_INT(f[0].start, 0);
    ASSERT_EQ_INT(f[0].tb_offset, 8);
    ASSERT_EQ_INT(f[0].tb_end, 32);
    ASSERT_TRUE(strcmp(f[0].name, "Func") == 0);
    free(f);
}

// 'cfrg' members locate fragments in the data fork.
TEST(test_pef_cfrg) {
    uint8_t r[80];
    memset(r, 0, sizeof(r));
    put16(r + 10, 1); // resource version
    put16(r + 30, 1); // one member
    uint8_t *m = r + 32;
    memcpy(m, "pwpc", 4);
    put32(m + 8, 0x01008000); // current version
    m[22] = 1; // application
    m[23] = 1; // in the data fork
    put32(m + 24, 0x100);
    put32(m + 28, 0x200);
    put16(m + 40, 48); // member size: 42 fixed + name, padded
    m[42] = 5;
    memcpy(m + 43, "Hello", 5);
    pef_cfrg_member_t out[2];
    ASSERT_EQ_INT(pef_parse_cfrg(r, sizeof(r), out, 2), 1);
    ASSERT_EQ_INT(out[0].architecture, 0x70777063);
    ASSERT_EQ_INT(out[0].where, 1);
    ASSERT_EQ_INT(out[0].offset, 0x100);
    ASSERT_EQ_INT(out[0].length, 0x200);
    ASSERT_TRUE(strcmp(out[0].name, "Hello") == 0);
    ASSERT_EQ_INT(pef_parse_cfrg(r, 40, out, 2), -1); // truncated
}

int main(void) {
    RUN(test_pef_container_and_sections);
    RUN(test_pef_loader_tables);
    RUN(test_pef_relocations);
    RUN(test_pef_relocation_repeat_and_position);
    RUN(test_pef_pidata_opcodes);
    RUN(test_pef_traceback_scan);
    RUN(test_pef_cfrg);
    return 0;
}
