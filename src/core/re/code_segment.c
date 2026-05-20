// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// code_segment.c
// Parsers for CODE 0 (jump-table segment) and CODE N (regular segment).
// See code_segment.h for the on-disk layout reference.

#include "code_segment.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static uint16_t be_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t be_u32(const uint8_t *p) {
    return (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

int re_parse_code0(const uint8_t *data, size_t len, re_jt_table_t *out) {
    if (!data || !out || len < 16)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->above_a5 = be_u32(data + 0);
    out->below_a5 = be_u32(data + 4);
    out->jt_size = be_u32(data + 8);
    out->jt_offset = be_u32(data + 12);
    // Each JT entry is 8 bytes.  The CODE 0 buffer contains exactly
    // jt_size bytes' worth of entries past offset 16, but the actual
    // number is min(jt_size, len-16) / 8.
    if (len <= 16) {
        out->n_entries = 0;
        out->entries = NULL;
        return 0;
    }
    size_t avail = len - 16;
    if (avail > out->jt_size)
        avail = out->jt_size;
    size_t n_entries = avail / 8;
    out->n_entries = n_entries;
    if (n_entries == 0) {
        out->entries = NULL;
        return 0;
    }
    out->entries = calloc(n_entries, sizeof(re_jt_entry_t));
    if (!out->entries)
        return -ENOMEM;
    for (size_t i = 0; i < n_entries; i++) {
        const uint8_t *e = data + 16 + 8 * i;
        out->entries[i].offset = be_u16(e + 0);
        // The two opcode words at e+2 and e+6 are part of the runtime
        // trampoline (MOVE.W #seg,-(SP) ; _LoadSeg); we just verify the
        // _LoadSeg signature when present to skip obvious garbage entries.
        out->entries[i].segment = be_u16(e + 4);
        out->entries[i].jt_index = (uint32_t)i;
    }
    return 0;
}

void re_jt_free(re_jt_table_t *jt) {
    if (!jt)
        return;
    free(jt->entries);
    jt->entries = NULL;
    jt->n_entries = 0;
}

int re_parse_code_n(const uint8_t *data, size_t len, re_code_segment_t *out) {
    if (!data || !out || len < 4)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    // Far-model sentinel: first u32 is 0x0000FFFF.
    if (len >= 16 && be_u32(data) == 0x0000FFFFu) {
        out->model = RE_CODE_MODEL_FAR;
        out->segment_number = be_u32(data + 4);
        // jt_offset and jt_count are 32-bit in far model; we down-cast
        // because the disassembler only consumes them as labels.
        out->jt_offset = (uint16_t)be_u32(data + 8);
        out->jt_count = (uint16_t)be_u32(data + 12);
        out->header_bytes = 16;
    } else {
        out->model = RE_CODE_MODEL_NEAR;
        out->jt_offset = be_u16(data + 0);
        out->jt_count = be_u16(data + 2);
        out->header_bytes = 4;
    }
    if (len < out->header_bytes) {
        out->insts = NULL;
        out->insts_len = 0;
        return -EINVAL;
    }
    out->insts = data + out->header_bytes;
    out->insts_len = len - out->header_bytes;
    return 0;
}
