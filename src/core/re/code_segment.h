// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// code_segment.h
// Parsing of classic-Mac CODE 0 (the jump-table segment) and CODE N (a
// regular code segment in either the near or far model).  Used by re.dump
// to drive per-segment disassembly with a correct entry-point map and to
// pull out the A5 world size.
//
// Format references:
//   - CODE 0 (jump table):
//       +0  above_a5  (u32) — application globals above A5
//       +4  below_a5  (u32) — application parameters/data below A5
//       +8  jt_size   (u32) — total jump-table size, bytes
//       +12 jt_offset (u32) — jump-table offset from A5 (typically 0x20)
//       +16 entries:  8 bytes each
//                       +0 offset within target segment (u16)
//                       +2 $3F3C — MOVE.W #imm,-(SP)
//                       +4 target segment number (u16)
//                       +6 $A9F0 — _LoadSeg trap
//   - CODE N near (segment <= 32 KB):
//       +0  jt_offset (u16) — JT offset for this segment
//       +2  jt_count  (u16) — number of JT entries for this segment
//       +4  68k instructions begin here (4-byte header skipped)
//   - CODE N far (segment > 32 KB, sentinel 0x0000FFFF):
//       +0  sentinel  (u32) = 0x0000FFFF
//       +4  segment_number (u32)
//       +8  jt_offset (u32)
//       +12 jt_count  (u32)
//       +16 instructions begin here

#pragma once

#ifndef GS_RE_CODE_SEGMENT_H
#define GS_RE_CODE_SEGMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === CODE 0 (the jump table) ================================================

typedef struct re_jt_entry {
    uint16_t segment; // target CODE segment number (1..N)
    uint16_t offset; // byte offset within that segment past its 4-byte header
    uint32_t jt_index; // 0-based index in CODE 0's jump table
} re_jt_entry_t;

typedef struct re_jt_table {
    uint32_t above_a5; // bytes
    uint32_t below_a5; // bytes
    uint32_t jt_size; // bytes
    uint32_t jt_offset; // offset of JT from A5 (typically 0x20)
    size_t n_entries;
    re_jt_entry_t *entries; // owned; free with re_jt_free()
} re_jt_table_t;

// Parse CODE 0 from a buffer.  Returns 0 on success and populates *out;
// returns -EINVAL on malformed data.  Callers free `out->entries` with
// re_jt_free().
int re_parse_code0(const uint8_t *data, size_t len, re_jt_table_t *out);
void re_jt_free(re_jt_table_t *jt);

// === CODE N (a single code segment) =========================================

typedef enum re_code_model {
    RE_CODE_MODEL_NEAR = 1,
    RE_CODE_MODEL_FAR = 2,
} re_code_model_t;

typedef struct re_code_segment {
    re_code_model_t model;
    uint32_t segment_number; // valid for far model; 0 for near (CODE id is the segment number)
    uint16_t jt_offset; // (or 32-bit value cast down for far)
    uint16_t jt_count;
    size_t header_bytes; // bytes to skip before the first instruction (4 or 16)
    const uint8_t *insts; // pointer into the input buffer (no copy)
    size_t insts_len; // bytes of code
} re_code_segment_t;

// Parse a CODE N segment header and populate `out` with pointers into
// `data`.  Returns 0 on success, -EINVAL on a too-short input.
int re_parse_code_n(const uint8_t *data, size_t len, re_code_segment_t *out);

#endif // GS_RE_CODE_SEGMENT_H
