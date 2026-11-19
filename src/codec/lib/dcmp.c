// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dcmp.c
// System 7 compressed resources (see codec.h): the 0xA89F6572 header and
// three of Apple's decompressors --
//   header version 8 + dcmp 0   DonnBits (Donn Denman, 1990-1991)
//   header version 9 + dcmp 2   GreggyBits (Greg Marriott, 1990-1991)
//   header version 9 + dcmp 3   InstaCompOne (undocumented; see its section)
// dcmp 0 and 2 are ported from Apple's open-sourced assembly in the System 7
// source drop (Patches/DeCompressDefProc.a, DeCompressCommon.A,
// GreggyBitsDefProc.a; (c) 1990-1991 Apple): the constant-word emit tables,
// opcode dispatch, var-table mechanics and GreggyBits' static table are
// reproduced verbatim, the assembly's Bsr / Bra flow rewritten as a C
// switch.  Other dcmp ids (1, 7, 8, the custom ones extensions ship) and
// dcmp 0 in a version-9 header are CODEC_ERR_UNSUPPORTED, so a caller can
// keep the raw bytes.
//
// A resource is at most CODEC_DCMP_MAX_SIZE decoded (a fork's data offsets
// are 24 bits) and its decoders address the whole output, so this one
// decodes the resource at once, on its first fill, and hands it out from
// there.  The stream's decoded size is the header's actualSize.
//
// On-disk layout:
//
//   Common header (bytes 0..11, both versions):
//     +0   signature (u32 BE) = 0xA89F6572
//     +4   header_length (u16 BE) = 0x12 (18 bytes)
//     +6   header_version (u8) = 8 (DonnBits) or 9 (GreggyBits)
//     +7   extended_attrs (u8)
//     +8   actual_size (u32 BE) — uncompressed byte count
//
//   Version-8 extension (bytes 12..17 — DonnBits format):
//     +12  var_table_ratio (u8) — 256ths-of-actual_size sized for var table
//     +13  overrun (u8) — extra bytes the decompressor may write
//     +14  dcmp_id (i16 BE) — 0 = DonnBits
//     +16  ctable_id (i16 BE) — auxiliary table
//
//   Version-9 extension (bytes 12..17 — GreggyBits and later formats):
//     +12  dcmp_id (i16 BE) — 2 = GreggyBits, others = custom
//     +14  decompress_slop (i16 BE)
//     +16  byte_table_size (u8) — number-of-words minus 1
//     +17  compress_flags (u8) — bit 0 = byteTableSaved, bit 1 = bitmappedData
//
//   +18  compressed payload starts here (both versions)
//
// The dcmp 0 payload is a stream of single-byte opcodes; each opcode either
// emits 16-bit words into the output buffer, slurps additional bytes
// from the input, or both.  The dispatch table is fixed at 256 entries:
//
//   0x00         LitWithLength      — encoded length N words, then 2N literal bytes
//   0x01..0x0F   LiteralN           — copies 2..30 literal bytes (N=opcode*2)
//   0x10         RememberWithLength — same as 0x00, but also stores in var table
//   0x11..0x1F   RememberN          — copies + remembers 2..30 literal bytes
//   0x20         ReuseByteLength    — 1-byte index + 40 → fetch from var table
//   0x21         ReuseByte2Length   — 1-byte index + 40 + 256
//   0x22         ReuseWordLength    — 2-byte index + 40
//   0x23..0x4A   ReuseData0..39     — direct fetch, var index = opcode - 0x23
//   0x4B..0xFD   constant-word emit — output one fixed 16-bit word (see table)
//   0xFE         HandleExtensions   — second byte selects sub-opcode (jump table,
//                                     entry vector, run-length, diff-encoded)
//   0xFF         end-of-stream      — terminate decompression
//
// The var table doubles as the LZ-style dictionary.  RememberN stuffs
// the literal that was just emitted into the table; subsequent
// ReuseData* opcodes pull it back out.  Initial table layout:
//   +0  next-slot index (u16)
//   +2  VarsList[0] = table_size (sentinel for the back-growing data)
//   +4..  VarsList[1..], one u16 per remembered string
//   data for each string lives at the END of the table, growing
//   downward.  String N occupies bytes [VarsList[N+1], VarsList[N]) of
//   the table buffer (where VarsList[N+1] is set when N is remembered).

#include "codec_internal.h"

#include <stdlib.h>

static const uint16_t g_const_words[179] = {
    0x0000, 0x4EBA, 0x0008, 0x4E75, 0x000C, 0x4EAD, 0x2053, 0x2F0B, // 0x4B..0x52
    0x6100, 0x0010, 0x7000, 0x2F00, 0x486E, 0x2050, 0x206E, 0x2F2E, // 0x53..0x5A
    0xFFFC, 0x48E7, 0x3F3C, 0x0004, 0xFFF8, 0x2F0C, 0x2006, 0x4EED, // 0x5B..0x62
    0x4E56, 0x2068, 0x4E5E, 0x0001, 0x588F, 0x4FEF, 0x0002, 0x0018, // 0x63..0x6A
    0x6000, 0xFFFF, 0x508F, 0x4E90, 0x0006, 0x266E, 0x0014, 0xFFF4, // 0x6B..0x72
    0x4CEE, 0x000A, 0x000E, 0x41EE, 0x4CDF, 0x48C0, 0xFFF0, 0x2D40, // 0x73..0x7A
    0x0012, 0x302E, 0x7001, 0x2F28, 0x2054, 0x6700, 0x0020, 0x001C, // 0x7B..0x82
    0x205F, 0x1800, 0x266F, 0x4878, 0x0016, 0x41FA, 0x303C, 0x2840, // 0x83..0x8A
    0x7200, 0x286E, 0x200C, 0x6600, 0x206B, 0x2F07, 0x558F, 0x0028, // 0x8B..0x92
    0xFFFE, 0xFFEC, 0x22D8, 0x200B, 0x000F, 0x598F, 0x2F3C, 0xFF00, // 0x93..0x9A
    0x0118, 0x81E1, 0x4A00, 0x4EB0, 0xFFE8, 0x48C7, 0x0003, 0x0022, // 0x9B..0xA2
    0x0007, 0x001A, 0x6706, 0x6708, 0x4EF9, 0x0024, 0x2078, 0x0800, // 0xA3..0xAA
    0x6604, 0x002A, 0x4ED0, 0x3028, 0x265F, 0x6704, 0x0030, 0x43EE, // 0xAB..0xB2
    0x3F00, 0x201F, 0x001E, 0xFFF6, 0x202E, 0x42A7, 0x2007, 0xFFFA, // 0xB3..0xBA
    0x6002, 0x3D40, 0x0C40, 0x6606, 0x0026, 0x2D48, 0x2F01, 0x70FF, // 0xBB..0xC2
    0x6004, 0x1880, 0x4A40, 0x0040, 0x002C, 0x2F08, 0x0011, 0xFFE4, // 0xC3..0xCA
    0x2140, 0x2640, 0xFFF2, 0x426E, 0x4EB9, 0x3D7C, 0x0038, 0x000D, // 0xCB..0xD2
    0x6006, 0x422E, 0x203C, 0x670C, 0x2D68, 0x6608, 0x4A2E, 0x4AAE, // 0xD3..0xDA
    0x002E, 0x4840, 0x225F, 0x2200, 0x670A, 0x3007, 0x4267, 0x0032, // 0xDB..0xE2
    0x2028, 0x0009, 0x487A, 0x0200, 0x2F2B, 0x0005, 0x226E, 0x6602, // 0xE3..0xEA
    0xE580, 0x670E, 0x660A, 0x0050, 0x3E00, 0x660C, 0x2E00, 0xFFEE, // 0xEB..0xF2
    0x206D, 0x2040, 0xFFE0, 0x5340, 0x6008, 0x0480, 0x0068, 0x0B7C, // 0xF3..0xFA
    0x4400, 0x41E8, 0x4841, // 0xFB..0xFD
};

// ---- Input and output cursors -----------------------------------------------
//
// `ic_t` reads the compressed payload from the decoder's input as it is
// needed; `oc_t` writes into the decoder's ring, which holds the whole
// resource (its window is the resource: dcmp 3 refers back anywhere in it).
// All bounds checks route through ic_get* / oc_put*, so a malformed stream
// produces a clean -1 return rather than walking off either buffer.

typedef struct {
    codec_in_t *in;
    uint8_t *scratch; // the last ic_get_n's bytes
    size_t scratch_cap;
} ic_t;

typedef struct {
    codec_ring_t *r;
    size_t cap; // the most this resource may write (with dcmp 0's overrun)
    size_t off; // bytes written
} oc_t;

static int ic_get_u8(ic_t *ic, uint8_t *out) {
    int b = codec_in_byte(ic->in);
    if (b < 0)
        return -1;
    *out = (uint8_t)b;
    return 0;
}
// The next `n` input bytes, valid until the next call.
static int ic_get_n(ic_t *ic, size_t n, const uint8_t **out) {
    static const uint8_t none[1];
    if (n == 0) {
        *out = none;
        return 0;
    }
    if (n > ic->scratch_cap) {
        uint8_t *b = realloc(ic->scratch, n);
        if (!b)
            return -1;
        ic->scratch = b;
        ic->scratch_cap = n;
    }
    if (codec_in_read(ic->in, ic->scratch, n) != n)
        return -1;
    *out = ic->scratch;
    return 0;
}
static int oc_put_u8(oc_t *oc, uint8_t b) {
    if (oc->off >= oc->cap)
        return -1;
    ring_put(oc->r, b);
    oc->off++;
    return 0;
}
static int oc_put_u16_be(oc_t *oc, uint16_t v) {
    if (oc->off + 2 > oc->cap)
        return -1;
    ring_put(oc->r, (uint8_t)(v >> 8));
    ring_put(oc->r, (uint8_t)v);
    oc->off += 2;
    return 0;
}
static int oc_put_n(oc_t *oc, const uint8_t *src, size_t n) {
    if (n > oc->cap - oc->off)
        return -1;
    ring_write(oc->r, src, n);
    oc->off += n;
    return 0;
}

// ---- GetEncodedValue ------------------------------------------------------
//
// Variable-length signed-integer encoding, lifted from DeCompressCommon.A:
//   Tag         Range            Bytes consumed
//   0..127      0..127           1
//   128..254    -16384..16127    2  (signed 16-bit, packed)
//   255         32-bit           5  (literal big-endian int32)

static int get_encoded(ic_t *ic, int32_t *out) {
    uint8_t b0;
    if (ic_get_u8(ic, &b0) < 0)
        return -1;
    if (b0 < 0x80) {
        *out = b0;
        return 0;
    }
    if (b0 == 0xFF) {
        const uint8_t *p;
        if (ic_get_n(ic, 4, &p) < 0)
            return -1;
        *out = (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
        return 0;
    }
    // 2-byte form: value = signed16 of ((b0 - 0xC0) << 8) | b1
    uint8_t b1;
    if (ic_get_u8(ic, &b1) < 0)
        return -1;
    int8_t hi = (int8_t)((int)b0 - 0xC0); // -64..62
    int16_t v = (int16_t)(hi * 256 + b1); // (hi << 8 | b1, without shifting a negative value)
    *out = (int32_t)v;
    return 0;
}

// ---- Variable table -------------------------------------------------------
//
// Layout (matches Apple's source exactly so a comparison against the
// disassembled DeCompressDefProc binary would line up):
//   +0..1  next-slot index (byte offset within the table where the
//          NEXT VarsList[] entry will be written; starts at 4)
//   +2..3  VarsList[0] = initial sentinel = table size (so the first
//          remembered string's offset is computed as table_size - len)
//   +4..   VarsList[1..], one u16 per remembered string (the offset
//          where that string's data starts inside the table)
// Remembered string data grows backwards from the table's end.

typedef struct {
    uint8_t *buf;
    size_t size;
} vt_t;

static void vt_init(vt_t *vt, uint8_t *buf, size_t size) {
    vt->buf = buf;
    vt->size = size;
    memset(buf, 0, size);
    if (size >= 4) {
        // next-slot index = 4 (the "VarsList[1]" slot)
        buf[0] = 0;
        buf[1] = 4;
        // VarsList[0] = size (sentinel)
        buf[2] = (uint8_t)(size >> 8);
        buf[3] = (uint8_t)size;
    }
}

// RememberData — stuff `len` bytes pointed to by `data` into the var table
// as a new variable.  The new entry is added to the index list (forward
// growth) with its data copied to the END of the table (backward growth).
// Returns 0 on success, -1 on table overflow.
static int vt_remember(vt_t *vt, const uint8_t *data, size_t len) {
    if (vt->size < 4)
        return -1;
    uint16_t next_idx = codec_be16(vt->buf);
    if ((size_t)next_idx + 2 > vt->size)
        return -1; // index list would collide with the data area
    // Previous entry's offset = end of the new string.
    uint16_t prev_off = codec_be16(vt->buf + next_idx - 2);
    if (len > prev_off)
        return -1; // data area exhausted
    uint16_t new_off = (uint16_t)(prev_off - len);
    // Index list and data area must not overlap.
    if (new_off < next_idx + 2)
        return -1;
    // Write the new offset into the index list.
    vt->buf[next_idx] = (uint8_t)(new_off >> 8);
    vt->buf[next_idx + 1] = (uint8_t)new_off;
    // Bump next-slot.
    uint16_t new_next = (uint16_t)(next_idx + 2);
    vt->buf[0] = (uint8_t)(new_next >> 8);
    vt->buf[1] = (uint8_t)new_next;
    // Copy the data into place.
    memcpy(vt->buf + new_off, data, len);
    return 0;
}

// FetchData — look up entry `idx` (0-based) and return a pointer into the
// var-table buffer + the length of that variable.  Returns -1 if the index
// goes past `next-slot`, indicating a corrupt stream.
static int vt_fetch(const vt_t *vt, uint32_t idx, const uint8_t **data, size_t *len) {
    if (vt->size < 4)
        return -1;
    uint16_t next_idx = codec_be16(vt->buf);
    // VarsList[idx] is at byte offset 2 + idx*2; VarsList[idx+1] at +2*idx+4.
    size_t a_off = (size_t)2 + (size_t)idx * 2;
    size_t b_off = a_off + 2;
    if (b_off + 2 > next_idx)
        return -1;
    uint16_t a = codec_be16(vt->buf + a_off); // string end
    uint16_t b = codec_be16(vt->buf + b_off); // string start
    if (b > a || a > vt->size)
        return -1;
    *data = vt->buf + b;
    *len = (size_t)(a - b);
    return 0;
}

// ---- Extension dispatch (opcode 0xFE) -------------------------------------
//
// HandleExtensions reads a second byte to pick the variant:
//   0  JumpTableTrans   — expand a CODE-0-style jump table (seg, count, deltas)
//   1  EntryVectorTrans — expand an A5-relative entry vector
//   2  RunLengthByte    — value + count, emit `count+1` bytes of value
//   3  RunLengthWord    — value + count, emit `count+1` words of value
//   4  DiffWordTrans    — value + count, emit words with byte-sized deltas
//   5  DiffEncWordTrans — value + count, emit words with encoded deltas
//   6  DiffEncLongTrans — value + count, emit longs with encoded deltas
//
// All seven sub-opcodes use the same encoded-integer reader, so each is
// just a small inner loop.

static int do_jt_trans(ic_t *ic, oc_t *oc) {
    int32_t seg, count, delta;
    if (get_encoded(ic, &seg) < 0)
        return -1;
    if (get_encoded(ic, &count) < 0)
        return -1;
    // The assembly initialises `offset` to 6 (bias absorbed by SubQ #6
    // before adding each delta), and emits `count` standard JT entries
    // followed by one trailing partial entry (the "_LoadSeg only" form).
    int32_t offset = 6;
    for (int32_t i = 0; i < count; i++) {
        if (get_encoded(ic, &delta) < 0)
            return -1;
        offset += (delta - 6);
        // 8-byte JT entry: 3F3C <seg> A9F0 <offset>
        if (oc_put_u16_be(oc, 0x3F3C) < 0 || oc_put_u16_be(oc, (uint16_t)seg) < 0 || oc_put_u16_be(oc, 0xA9F0) < 0 ||
            oc_put_u16_be(oc, (uint16_t)offset) < 0)
            return -1;
    }
    // Trailing partial entry — 3F3C <seg> A9F0, no offset.  Inside
    // Macintosh notes this is how CODE 0 marks the table's tail.
    if (oc_put_u16_be(oc, 0x3F3C) < 0 || oc_put_u16_be(oc, (uint16_t)seg) < 0 || oc_put_u16_be(oc, 0xA9F0) < 0)
        return -1;
    return 0;
}

static int do_entry_vector(ic_t *ic, oc_t *oc) {
    int32_t branch, delta_const, count, off;
    if (get_encoded(ic, &branch) < 0)
        return -1;
    if (get_encoded(ic, &delta_const) < 0)
        return -1;
    if (get_encoded(ic, &count) < 0)
        return -1;
    if (get_encoded(ic, &off) < 0)
        return -1;
    // 8-byte entry: 6100 <branch> 4EED <off>; for each subsequent entry,
    // branch decreases by 8 and off either gets the constant delta added
    // or comes from the next encoded value.
    int32_t cur_branch = branch;
    int32_t cur_off = off;
    for (int32_t i = 0; i <= count; i++) {
        if (oc_put_u16_be(oc, 0x6100) < 0 || oc_put_u16_be(oc, (uint16_t)cur_branch) < 0 ||
            oc_put_u16_be(oc, 0x4EED) < 0 || oc_put_u16_be(oc, (uint16_t)cur_off) < 0)
            return -1;
        cur_branch -= 8;
        if (i < count) {
            if (delta_const != 0) {
                cur_off += delta_const;
            } else {
                int32_t next;
                if (get_encoded(ic, &next) < 0)
                    return -1;
                cur_off = next;
            }
        }
    }
    return 0;
}

static int do_runlen_byte(ic_t *ic, oc_t *oc) {
    int32_t value, count;
    if (get_encoded(ic, &value) < 0)
        return -1;
    if (get_encoded(ic, &count) < 0)
        return -1;
    for (int32_t i = 0; i <= count; i++)
        if (oc_put_u8(oc, (uint8_t)value) < 0)
            return -1;
    return 0;
}

static int do_runlen_word(ic_t *ic, oc_t *oc) {
    int32_t value, count;
    if (get_encoded(ic, &value) < 0)
        return -1;
    if (get_encoded(ic, &count) < 0)
        return -1;
    for (int32_t i = 0; i <= count; i++)
        if (oc_put_u16_be(oc, (uint16_t)value) < 0)
            return -1;
    return 0;
}

static int do_diff_word(ic_t *ic, oc_t *oc) {
    int32_t value, count;
    if (get_encoded(ic, &value) < 0)
        return -1;
    if (get_encoded(ic, &count) < 0)
        return -1;
    if (oc_put_u16_be(oc, (uint16_t)value) < 0)
        return -1;
    for (int32_t i = 0; i < count; i++) {
        uint8_t db;
        if (ic_get_u8(ic, &db) < 0)
            return -1;
        value += (int8_t)db; // sign-extend per `Ext.W` in the original
        if (oc_put_u16_be(oc, (uint16_t)value) < 0)
            return -1;
    }
    return 0;
}

static int do_diff_enc_word(ic_t *ic, oc_t *oc) {
    int32_t value, count;
    if (get_encoded(ic, &value) < 0)
        return -1;
    if (get_encoded(ic, &count) < 0)
        return -1;
    if (oc_put_u16_be(oc, (uint16_t)value) < 0)
        return -1;
    for (int32_t i = 0; i < count; i++) {
        int32_t d;
        if (get_encoded(ic, &d) < 0)
            return -1;
        value += d;
        if (oc_put_u16_be(oc, (uint16_t)value) < 0)
            return -1;
    }
    return 0;
}

static int do_diff_enc_long(ic_t *ic, oc_t *oc) {
    int32_t value, count;
    if (get_encoded(ic, &value) < 0)
        return -1;
    if (get_encoded(ic, &count) < 0)
        return -1;
    uint32_t v32 = (uint32_t)value;
    if (oc_put_u8(oc, (uint8_t)(v32 >> 24)) < 0 || oc_put_u8(oc, (uint8_t)(v32 >> 16)) < 0 ||
        oc_put_u8(oc, (uint8_t)(v32 >> 8)) < 0 || oc_put_u8(oc, (uint8_t)v32) < 0)
        return -1;
    for (int32_t i = 0; i < count; i++) {
        int32_t d;
        if (get_encoded(ic, &d) < 0)
            return -1;
        v32 += (uint32_t)d;
        if (oc_put_u8(oc, (uint8_t)(v32 >> 24)) < 0 || oc_put_u8(oc, (uint8_t)(v32 >> 16)) < 0 ||
            oc_put_u8(oc, (uint8_t)(v32 >> 8)) < 0 || oc_put_u8(oc, (uint8_t)v32) < 0)
            return -1;
    }
    return 0;
}

static int do_extension(ic_t *ic, oc_t *oc) {
    uint8_t ext;
    if (ic_get_u8(ic, &ext) < 0)
        return -1;
    switch (ext) {
    case 0:
        return do_jt_trans(ic, oc);
    case 1:
        return do_entry_vector(ic, oc);
    case 2:
        return do_runlen_byte(ic, oc);
    case 3:
        return do_runlen_word(ic, oc);
    case 4:
        return do_diff_word(ic, oc);
    case 5:
        return do_diff_enc_word(ic, oc);
    case 6:
        return do_diff_enc_long(ic, oc);
    default:
        return -1;
    }
}

// ---- Main dispatch loop ---------------------------------------------------
//
// Opcodes 0x01..0x0F (Literal2..30) and 0x11..0x1F (Remember2..30) each
// carry their literal-byte count in the opcode: 2 * (opcode & 0x0F).
// The assembly hard-codes 15 entries each via MoveQ; we collapse those
// into the arithmetic since the table is otherwise identical.

#define MAX_1BYTE_REUSE 40

static int unpack_loop(ic_t *ic, oc_t *oc, vt_t *vt) {
    while (1) {
        uint8_t op;
        if (ic_get_u8(ic, &op) < 0)
            return -1;
        if (op == 0xFF)
            return 0; // ExitUnpack — clean end of stream

        if (op == 0x00 || op == 0x10) {
            // {Lit,Remember}WithLength: encoded N → 2N literal bytes
            int32_t n_words;
            if (get_encoded(ic, &n_words) < 0)
                return -1;
            if (n_words < 0 || (uint32_t)n_words > 0x100000)
                return -1;
            size_t nbytes = (size_t)n_words * 2;
            const uint8_t *src;
            if (ic_get_n(ic, nbytes, &src) < 0)
                return -1;
            if (op == 0x10) {
                if (vt_remember(vt, src, nbytes) < 0)
                    return -1;
            }
            if (oc_put_n(oc, src, nbytes) < 0)
                return -1;
            continue;
        }

        if (op <= 0x0F) {
            // Literal2..Literal30: copy (op*2) literal bytes
            size_t nbytes = (size_t)op * 2;
            const uint8_t *src;
            if (ic_get_n(ic, nbytes, &src) < 0)
                return -1;
            if (oc_put_n(oc, src, nbytes) < 0)
                return -1;
            continue;
        }

        if (op <= 0x1F) {
            // Remember2..Remember30: copy + store
            size_t nbytes = (size_t)(op - 0x10) * 2;
            const uint8_t *src;
            if (ic_get_n(ic, nbytes, &src) < 0)
                return -1;
            if (vt_remember(vt, src, nbytes) < 0)
                return -1;
            if (oc_put_n(oc, src, nbytes) < 0)
                return -1;
            continue;
        }

        if (op == 0x20) {
            // ReuseByteLength: 1 byte index + 40
            uint8_t b;
            if (ic_get_u8(ic, &b) < 0)
                return -1;
            uint32_t idx = (uint32_t)b + MAX_1BYTE_REUSE;
            const uint8_t *d;
            size_t n;
            if (vt_fetch(vt, idx, &d, &n) < 0)
                return -1;
            if (oc_put_n(oc, d, n) < 0)
                return -1;
            continue;
        }
        if (op == 0x21) {
            // ReuseByte2Length: 1 byte index + 40 + 256
            uint8_t b;
            if (ic_get_u8(ic, &b) < 0)
                return -1;
            uint32_t idx = (uint32_t)b + MAX_1BYTE_REUSE + 256;
            const uint8_t *d;
            size_t n;
            if (vt_fetch(vt, idx, &d, &n) < 0)
                return -1;
            if (oc_put_n(oc, d, n) < 0)
                return -1;
            continue;
        }
        if (op == 0x22) {
            // ReuseWordLength: 2-byte BE index + 40
            uint8_t hi, lo;
            if (ic_get_u8(ic, &hi) < 0)
                return -1;
            if (ic_get_u8(ic, &lo) < 0)
                return -1;
            uint32_t idx = (((uint32_t)hi << 8) | lo) + MAX_1BYTE_REUSE;
            const uint8_t *d;
            size_t n;
            if (vt_fetch(vt, idx, &d, &n) < 0)
                return -1;
            if (oc_put_n(oc, d, n) < 0)
                return -1;
            continue;
        }

        if (op <= 0x4A) {
            // ReuseData0..39: direct index = opcode - 0x23
            uint32_t idx = (uint32_t)(op - 0x23);
            const uint8_t *d;
            size_t n;
            if (vt_fetch(vt, idx, &d, &n) < 0)
                return -1;
            if (oc_put_n(oc, d, n) < 0)
                return -1;
            continue;
        }

        if (op <= 0xFD) {
            // Constant-word emit: output one fixed 16-bit word
            uint16_t w = g_const_words[op - 0x4B];
            if (oc_put_u16_be(oc, w) < 0)
                return -1;
            continue;
        }

        if (op == 0xFE) {
            if (do_extension(ic, oc) < 0)
                return -1;
            continue;
        }
        // Unreachable — opcode 0xFF handled above.
        return -1;
    }
}

// ===========================================================================
// dcmp 2 — GreggyBits (Greg Marriott, 1990-1991)
// ===========================================================================
//
// Ported from the System 7.1 source drop, Patches/GreggyBitsDefProc.a.
// The algorithm:
//   1. Each output word is either looked up via a byte->word translation
//      table, or copied verbatim (two raw input bytes -> one output word).
//   2. The table is either the 256-entry static_table baked in below
//      (when compressFlags bit 0 is clear), or read from the start of
//      the payload as `byte_table_size + 1` 16-bit words (bit 0 set).
//   3. Two encoding modes:
//      - Non-bitmapped (bit 1 clear): every input byte is an index into
//        the table; payload length == output word count.  If actual_size
//        is odd, the very last byte of the payload becomes the final
//        odd output byte verbatim.
//      - Bitmapped (bit 1 set): payload is a sequence of "8-word runs".
//        Each run starts with one bitmap byte; bits 7..0 of that bitmap
//        choose, for each of the next 8 output words: bit set = 1 input
//        byte indexed into the table; bit clear = 2 input bytes copied
//        verbatim.  The final run may be shorter than 8 words (size mod
//        8); odd `actual_size` adds a final literal byte as above.

// The 256-entry static byte->word table, lifted verbatim from
// GreggyBitsDefProc.a (StaticTable + DC.W rows).  Each byte index
// produces one 16-bit big-endian output word; the table holds the most
// common 16-bit values observed by Greg's analysis (small constants,
// frequent 68k opcodes, common A-line traps).
static const uint16_t g_greggy_static[256] = {
    0x0000, 0x0008, 0x4EBA, 0x206E, 0x4E75, 0x000C, 0x0004, 0x7000, // 0x00..0x07
    0x0010, 0x0002, 0x486E, 0xFFFC, 0x6000, 0x0001, 0x48E7, 0x2F2E, // 0x08..0x0F
    0x4E56, 0x0006, 0x4E5E, 0x2F00, 0x6100, 0xFFF8, 0x2F0B, 0xFFFF, // 0x10..0x17
    0x0014, 0x000A, 0x0018, 0x205F, 0x000E, 0x2050, 0x3F3C, 0xFFF4, // 0x18..0x1F
    0x4CEE, 0x302E, 0x6700, 0x4CDF, 0x266E, 0x0012, 0x001C, 0x4267, // 0x20..0x27
    0xFFF0, 0x303C, 0x2F0C, 0x0003, 0x4ED0, 0x0020, 0x7001, 0x0016, // 0x28..0x2F
    0x2D40, 0x48C0, 0x2078, 0x7200, 0x588F, 0x6600, 0x4FEF, 0x42A7, // 0x30..0x37
    0x6706, 0xFFFA, 0x558F, 0x286E, 0x3F00, 0xFFFE, 0x2F3C, 0x6704, // 0x38..0x3F
    0x598F, 0x206B, 0x0024, 0x201F, 0x41FA, 0x81E1, 0x6604, 0x6708, // 0x40..0x47
    0x001A, 0x4EB9, 0x508F, 0x202E, 0x0007, 0x4EB0, 0xFFF2, 0x3D40, // 0x48..0x4F
    0x001E, 0x2068, 0x6606, 0xFFF6, 0x4EF9, 0x0800, 0x0C40, 0x3D7C, // 0x50..0x57
    0xFFEC, 0x0005, 0x203C, 0xFFE8, 0xDEFC, 0x4A2E, 0x0030, 0x0028, // 0x58..0x5F
    0x2F08, 0x200B, 0x6002, 0x426E, 0x2D48, 0x2053, 0x2040, 0x1800, // 0x60..0x67
    0x6004, 0x41EE, 0x2F28, 0x2F01, 0x670A, 0x4840, 0x2007, 0x6608, // 0x68..0x6F
    0x0118, 0x2F07, 0x3028, 0x3F2E, 0x302B, 0x226E, 0x2F2B, 0x002C, // 0x70..0x77
    0x670C, 0x225F, 0x6006, 0x00FF, 0x3007, 0xFFEE, 0x5340, 0x0040, // 0x78..0x7F
    0xFFE4, 0x4A40, 0x660A, 0x000F, 0x4EAD, 0x70FF, 0x22D8, 0x486B, // 0x80..0x87
    0x0022, 0x204B, 0x670E, 0x4AAE, 0x4E90, 0xFFE0, 0xFFC0, 0x002A, // 0x88..0x8F
    0x2740, 0x6702, 0x51C8, 0x02B6, 0x487A, 0x2278, 0xB06E, 0xFFE6, // 0x90..0x97
    0x0009, 0x322E, 0x3E00, 0x4841, 0xFFEA, 0x43EE, 0x4E71, 0x7400, // 0x98..0x9F
    0x2F2C, 0x206C, 0x003C, 0x0026, 0x0050, 0x1880, 0x301F, 0x2200, // 0xA0..0xA7
    0x660C, 0xFFDA, 0x0038, 0x6602, 0x302C, 0x200C, 0x2D6E, 0x4240, // 0xA8..0xAF
    0xFFE2, 0xA9F0, 0xFF00, 0x377C, 0xE580, 0xFFDC, 0x4868, 0x594F, // 0xB0..0xB7
    0x0034, 0x3E1F, 0x6008, 0x2F06, 0xFFDE, 0x600A, 0x7002, 0x0032, // 0xB8..0xBF
    0xFFCC, 0x0080, 0x2251, 0x101F, 0x317C, 0xA029, 0xFFD8, 0x5240, // 0xC0..0xC7
    0x0100, 0x6710, 0xA023, 0xFFCE, 0xFFD4, 0x2006, 0x4878, 0x002E, // 0xC8..0xCF
    0x504F, 0x43FA, 0x6712, 0x7600, 0x41E8, 0x4A6E, 0x20D9, 0x005A, // 0xD0..0xD7
    0x7FFF, 0x51CA, 0x005C, 0x2E00, 0x0240, 0x48C7, 0x6714, 0x0C80, // 0xD8..0xDF
    0x2E9F, 0xFFD6, 0x8000, 0x1000, 0x4842, 0x4A6B, 0xFFD2, 0x0048, // 0xE0..0xE7
    0x4A47, 0x4ED1, 0x206F, 0x0041, 0x600C, 0x2A78, 0x422E, 0x3200, // 0xE8..0xEF
    0x6574, 0x6716, 0x0044, 0x486D, 0x2008, 0x486C, 0x0B7C, 0x2640, // 0xF0..0xF7
    0x0400, 0x0068, 0x206D, 0x000D, 0x2A40, 0x000B, 0x003E, 0x0220, // 0xF8..0xFF
};

#define GREGGY_BYTE_TABLE_SAVED 0x01u // payload begins with a dynamic table
#define GREGGY_BITMAPPED_DATA   0x02u // mixed compressed/uncompressed runs

// Run the GreggyBits (dcmp 2) decompression.  `payload`/`payload_len`
// are the bytes *after* the 18-byte common header.  `actual_size` is
// the target output byte count; `byte_table_size_field` is the raw
// `byte_table_size` byte from the header (interpreted as +1 -> word
// count when the dynamic-table flag is set); `compress_flags` is the
// header's compressFlags byte.  Returns 0 on success and fills
// `out_uncompressed` (caller-provided, capacity `actual_size`).
static int dcmp2_decompress(ic_t *icp, oc_t *ocp, size_t actual_size, uint8_t byte_table_size_field,
                            uint8_t compress_flags) {
    // Decide which table to read indices through.
    const uint16_t *table = g_greggy_static;
    uint16_t dyn_table[256];
    ic_t ic = *icp;
    if (compress_flags & GREGGY_BYTE_TABLE_SAVED) {
        // Dynamic table: read (byte_table_size_field + 1) 16-bit words
        // from the payload into dyn_table[0..N-1].  Entries past that
        // stay at zero, matching the assembly's BlockMove of exactly
        // the declared word count.
        size_t nwords = (size_t)byte_table_size_field + 1;
        if (nwords > 256)
            nwords = 256;
        memset(dyn_table, 0, sizeof(dyn_table));
        for (size_t i = 0; i < nwords; i++) {
            uint8_t hi, lo;
            if (ic_get_u8(&ic, &hi) < 0)
                return -1;
            if (ic_get_u8(&ic, &lo) < 0)
                return -1;
            dyn_table[i] = (uint16_t)((hi << 8) | lo);
        }
        table = dyn_table;
    }

    oc_t oc = *ocp;
    size_t total_words = actual_size / 2;
    bool odd_extra = (actual_size & 1) != 0;

    if (!(compress_flags & GREGGY_BITMAPPED_DATA)) {
        // Non-bitmapped: every input byte is a table index.
        for (size_t w = 0; w < total_words; w++) {
            uint8_t idx;
            if (ic_get_u8(&ic, &idx) < 0)
                return -1;
            if (oc_put_u16_be(&oc, table[idx]) < 0)
                return -1;
        }
        if (odd_extra) {
            uint8_t b;
            if (ic_get_u8(&ic, &b) < 0)
                return -1;
            if (oc_put_u8(&oc, b) < 0)
                return -1;
        }
        return 0;
    }

    // Bitmapped: process 8-word runs.  Each run begins with one bitmap
    // byte (high bit = first word).  Set bit => 1 input byte indexed into
    // the table; clear bit => 2 raw input bytes copied verbatim.
    size_t full_runs = total_words / 8;
    size_t last_run_words = total_words % 8;
    for (size_t r = 0; r < full_runs; r++) {
        uint8_t bitmap;
        if (ic_get_u8(&ic, &bitmap) < 0)
            return -1;
        if (bitmap == 0) {
            // Fast path: all 8 words are raw 2-byte literals.
            for (int i = 0; i < 16; i++) {
                uint8_t b;
                if (ic_get_u8(&ic, &b) < 0)
                    return -1;
                if (oc_put_u8(&oc, b) < 0)
                    return -1;
            }
            continue;
        }
        for (int i = 0; i < 8; i++) {
            bool compressed = (bitmap & 0x80) != 0;
            bitmap = (uint8_t)(bitmap << 1);
            if (compressed) {
                uint8_t idx;
                if (ic_get_u8(&ic, &idx) < 0)
                    return -1;
                if (oc_put_u16_be(&oc, table[idx]) < 0)
                    return -1;
            } else {
                uint8_t b0, b1;
                if (ic_get_u8(&ic, &b0) < 0)
                    return -1;
                if (ic_get_u8(&ic, &b1) < 0)
                    return -1;
                if (oc_put_u8(&oc, b0) < 0)
                    return -1;
                if (oc_put_u8(&oc, b1) < 0)
                    return -1;
            }
        }
    }
    if (last_run_words > 0) {
        uint8_t bitmap;
        if (ic_get_u8(&ic, &bitmap) < 0)
            return -1;
        for (size_t i = 0; i < last_run_words; i++) {
            bool compressed = (bitmap & 0x80) != 0;
            bitmap = (uint8_t)(bitmap << 1);
            if (compressed) {
                uint8_t idx;
                if (ic_get_u8(&ic, &idx) < 0)
                    return -1;
                if (oc_put_u16_be(&oc, table[idx]) < 0)
                    return -1;
            } else {
                uint8_t b0, b1;
                if (ic_get_u8(&ic, &b0) < 0)
                    return -1;
                if (ic_get_u8(&ic, &b1) < 0)
                    return -1;
                if (oc_put_u8(&oc, b0) < 0)
                    return -1;
                if (oc_put_u8(&oc, b1) < 0)
                    return -1;
            }
        }
    }
    if (odd_extra) {
        uint8_t b;
        if (ic_get_u8(&ic, &b) < 0)
            return -1;
        if (oc_put_u8(&oc, b) < 0)
            return -1;
    }
    return 0;
}

// ============================================================================
//  dcmp 3 — "InstaCompOne" (v9)
// ============================================================================
//
// LZ77 with static Huffman codes.  Conceptually Deflate-like (literals,
// back-references, prefix codes) but sharing no format details with Deflate.
// Never documented by Apple; the bitstream layout below was reconstructed
// from published reverse-engineering work (ResDecompress, resource_dasm —
// both MIT) and validated against the `dcmp` 3 code resource Apple's own
// Installer ships uncompressed.  No third-party code is reproduced here.
//
// Two properties make this different from most LZ77 variants:
//
//   1. Literals are emitted in *runs* with a Huffman-coded run length; they
//      are not individually flagged.
//   2. The distance code layout is keyed on how many bytes have been produced
//      so far, so it widens as the output grows.  There is no fixed window.
//
#include "dcmp3_tables.h"

// Stream layout: the bitstream starts at `hdr_len` (18).  The 4 bytes at
// +14..+17 are decompressor-specific parameters that this algorithm does not
// consume.  `actual_size` is the sole termination condition — there is no
// end-of-stream marker.

// Nothing in this format is byte-aligned; literal bytes are read as 8-bit
// fields wherever the bit position happens to be.
// The shared MSB-first reader (codec_internal.h, msb_*): bits past the end
// read as zeros, which lets a stream that ends mid-byte finish its final
// token.

// Peek `n` bits (1 <= n <= 24) without consuming.
static uint32_t bitr_peek(codec_in_t *in, unsigned n) {
    msb_fill(in);
    return msb_peek(in, (int)n);
}

static uint32_t bitr_get(codec_in_t *in, unsigned n) {
    return msb_get(in, (int)n);
}

// True once the reader has consumed a whole byte or more past the end of
// the input.  Used only as a corruption guard; a well-formed stream stops
// on actual_size.  Byte-granular on purpose: a stream that ends mid-byte
// may finish its final token on the zeros after it.
static bool bitr_overrun(const codec_in_t *in) {
    return (int)in->over * 8 - in->nbits >= 8;
}

// One entry of a static prefix code.  `nbits`/`code` are the prefix; the
// decoded value is `base + get(extra)`.  `extra == 0` means a direct value.
typedef struct {
    uint8_t nbits;
    uint16_t code;
    uint8_t extra;
    uint16_t base;
} dc3_code_t;

// Match-length table.  Decoded value Lc feeds the token loop below; the
// escape ladder (1110, 11110, 111110, ...) widens into larger ranges.
// Max Lc is 2042, so the longest match is 2045 bytes.
static const dc3_code_t DC3_LEN[] = {
    {2,  0x0,   0,  0   },
    {2,  0x1,   0,  1   },
    {3,  0x4,   0,  2   },
    {4,  0xA,   0,  3   },
    {4,  0xB,   0,  4   },
    {5,  0x18,  0,  5   },
    {5,  0x19,  0,  6   },
    {6,  0x34,  0,  7   },
    {6,  0x35,  0,  8   },
    {6,  0x36,  0,  9   },
    {6,  0x37,  0,  10  },
    {4,  0xE,   3,  11  },
    {5,  0x1E,  3,  19  },
    {6,  0x3E,  5,  27  },
    {7,  0x7E,  6,  59  },
    {8,  0xFE,  7,  123 },
    {9,  0x1FE, 8,  251 },
    {10, 0x3FE, 9,  507 },
    {11, 0x7FE, 10, 1019},
};

// Literal-run-length table.  Yields 1..63; a run of exactly 63 is the
// continuation sentinel (see the token loop).
static const dc3_code_t DC3_LIT[] = {
    {1, 0x0,  0, 1 },
    {3, 0x4,  0, 2 },
    {3, 0x5,  0, 3 },
    {5, 0x18, 0, 4 },
    {5, 0x19, 0, 5 },
    {5, 0x1A, 0, 6 },
    {5, 0x1B, 0, 7 },
    {7, 0x70, 0, 8 },
    {7, 0x71, 0, 9 },
    {7, 0x72, 0, 10},
    {7, 0x73, 0, 11},
    {7, 0x74, 0, 12},
    {7, 0x75, 0, 13},
    {7, 0x76, 0, 14},
    {7, 0x77, 0, 15},
    {5, 0x1E, 4, 16},
    {5, 0x1F, 5, 32},
};

// Decode one symbol from a static table.  Both tables are prefix-free, so
// matching the shortest candidate first is unambiguous.  Returns -1 if no
// prefix matches (corrupt stream).
static int32_t dc3_decode(codec_in_t *br, const dc3_code_t *tbl, size_t n) {
    for (unsigned bits = 1; bits <= 11; bits++) {
        uint32_t v = bitr_peek(br, bits);
        for (size_t i = 0; i < n; i++) {
            if (tbl[i].nbits == bits && tbl[i].code == v) {
                msb_drop(br, (int)bits);
                return (int32_t)tbl[i].base + (int32_t)(tbl[i].extra ? bitr_get(br, tbl[i].extra) : 0);
            }
        }
    }
    return -1;
}

// Decode a back-reference distance.  `mag` is the number of bytes already
// produced, which is also the largest legal distance.
//
// A band is selected from `mag` (DC3_DISPATCH), giving a three-tier code:
//
//   prefix 0   -> get(t0_bits) + t0_base
//   prefix 10  -> get(t1_bits) + t1_base
//   prefix 11  -> a step ladder keyed on `mag`, so the encoder spends only as
//                 many bits as the current output length can possibly need.
//
// The ladder is genuinely irregular — some steps repeat a width, and one
// threshold in band 0xA80 reproduces an off-by-one in Apple's original
// encoder.  It therefore cannot be reduced to a closed form; the table in
// dcmp3_tables.h is the specification.
static int64_t dc3_distance(codec_in_t *br, uint32_t mag) {
    if (mag == 0)
        return -1;

    uint32_t bandmax = DC3_DISPATCH_LAST;
    for (size_t i = 0; i < sizeof(DC3_DISPATCH) / sizeof(DC3_DISPATCH[0]); i++) {
        if (mag <= DC3_DISPATCH[i].max) {
            bandmax = DC3_DISPATCH[i].band;
            break;
        }
    }
    const dc3_band_t *b = NULL;
    for (size_t i = 0; i < sizeof(DC3_BANDS) / sizeof(DC3_BANDS[0]); i++) {
        if (DC3_BANDS[i].band == bandmax) {
            b = &DC3_BANDS[i];
            break;
        }
    }
    if (!b)
        return -1;

    if (bitr_get(br, 1) == 0)
        return (int64_t)bitr_get(br, b->t0_bits) + b->t0_base;
    if (bitr_get(br, 1) == 0)
        return (int64_t)bitr_get(br, b->t1_bits) + b->t1_base;
    for (uint8_t j = 0; j < b->nsteps; j++) {
        if (mag <= b->steps[j].max)
            return (int64_t)bitr_get(br, b->steps[j].bits) + b->steps[j].base;
    }
    return -1;
}

// Token loop.  `after_literal` overloads length code 0: false means "a
// literal run follows", true means "a real match of length 3".  The +1 in
// the after-literal branch keeps the minimum match length at 3 either way.
// Writes exactly `actual_size` bytes.  Returns 0 on success.
static int dcmp3_decompress(codec_in_t *br, codec_ring_t *r, size_t actual_size) {
    size_t produced = 0;
    bool after_literal = false;

    while (produced < actual_size) {
        int32_t lc = dc3_decode(br, DC3_LEN, sizeof(DC3_LEN) / sizeof(DC3_LEN[0]));
        if (lc < 0 || bitr_overrun(br))
            return -1;

        if (lc > 0 || after_literal) {
            size_t mlen = (size_t)lc + 2 + (after_literal ? 1u : 0u);
            int64_t dist = dc3_distance(br, (uint32_t)produced);
            if (dist <= 0 || (uint64_t)dist > produced)
                return -1;
            if (produced + mlen > actual_size)
                mlen = actual_size - produced; // tolerate a trailing partial match
            // Forward, overlapping as LZ77 does: dist < mlen is legal and
            // produces run-length repetition.
            ring_copy(r, (size_t)dist, mlen);
            produced += mlen;
            after_literal = false;
        } else {
            int32_t n = dc3_decode(br, DC3_LIT, sizeof(DC3_LIT) / sizeof(DC3_LIT[0]));
            if (n < 1)
                return -1;
            size_t run = (size_t)n;
            if (produced + run > actual_size)
                run = actual_size - produced;
            for (size_t i = 0; i < run; i++)
                ring_put(r, (uint8_t)bitr_get(br, 8));
            produced += run;
            // A maximal run is a chunk of a longer run and must be able to
            // continue, so it does not arm the after-literal state.
            after_literal = (n < 63);
        }
        if (bitr_overrun(br))
            return -1;
    }
    return produced == actual_size ? 0 : -1;
}

// ============================================================================
// The decoder
// ============================================================================

#define DCMP_HDR_MIN 18
#define DCMP_OVERRUN 256 // dcmp 0's header `overrun` (a byte): the most it may write past the size

typedef struct {
    ic_t ic; // its scratch buffer
} dcmp_t;

// Decode the whole resource into the ring, in one fill: its window is the
// resource, and the ring is sized to hold all of it.
static int dcmp_fill(codec_dec_t *d) {
    dcmp_t *s = d->st;
    codec_in_t *in = &d->in;
    uint8_t h[DCMP_HDR_MIN];
    if (codec_in_read(in, h, sizeof(h)) != sizeof(h))
        return codec_fail(d, CODEC_ERR_TRUNCATED, "dcmp: truncated header");
    if (codec_be32(h) != CODEC_DCMP_MAGIC)
        return codec_fail(d, CODEC_ERR_DATA, "dcmp: not a compressed resource");
    size_t hdr_len = codec_be16(h + 4);
    unsigned hdr_ver = h[6];
    uint32_t actual_size = codec_be32(h + 8);
    if (hdr_len < DCMP_HDR_MIN)
        return codec_fail(d, CODEC_ERR_DATA, "dcmp: bad header length");
    for (size_t k = DCMP_HDR_MIN; k < hdr_len; k++)
        if (codec_in_byte(in) < 0)
            return codec_fail(d, CODEC_ERR_TRUNCATED, "dcmp: truncated header");
    // Bound the declared size before anything is sized from it: unbounded,
    // dcmp 0's actual_size + overrun wrapped a 32-bit size_t.
    if (actual_size > CODEC_DCMP_MAX_SIZE)
        return codec_fail(d, CODEC_ERR_DATA, "dcmp: declared size too large");
    // v8 puts dcmp_id at +14; v9 at +12 (over v8's ratio and overrun).
    int dcmp_id;
    if (hdr_ver == 8)
        dcmp_id = (int16_t)codec_be16(h + 14);
    else if (hdr_ver == 9)
        dcmp_id = (int16_t)codec_be16(h + 12);
    else
        return codec_fail(d, CODEC_ERR_UNSUPPORTED, "dcmp: unsupported header version");
    if (!((dcmp_id == 0 && hdr_ver == 8) || ((dcmp_id == 2 || dcmp_id == 3) && hdr_ver == 9)))
        return codec_fail(d, CODEC_ERR_UNSUPPORTED, "dcmp: unsupported decompressor");
    // The stream states its size: exactly that many bytes, and a window of
    // all of them.
    int rc = codec_set_size(d, actual_size);
    if (rc == 0)
        rc = codec_ring_alloc(d, actual_size, DCMP_OVERRUN);
    if (rc < 0)
        return rc;
    s->ic.in = in;
    oc_t oc = {&d->ring, actual_size, 0};

    if (dcmp_id == 0) {
        // DonnBits.  The output may overshoot by the header's `overrun`
        // bytes while decoding (the assembly allows it; codec.c drops them);
        // the var table is actual_size * ratio / 256, rounded up to even.
        oc.cap += h[13];
        size_t vt_size = ((size_t)actual_size * h[12]) / 256;
        if (vt_size < 4)
            vt_size = 4;
        vt_size += vt_size & 1;
        uint8_t *vtbuf = malloc(vt_size);
        if (!vtbuf)
            return CODEC_ERR_NOMEM;
        vt_t vt;
        vt_init(&vt, vtbuf, vt_size);
        rc = unpack_loop(&s->ic, &oc, &vt);
        free(vtbuf);
        if (rc < 0)
            return codec_fail(d, CODEC_ERR_DATA, "dcmp 0: corrupt stream");
        if (oc.off < actual_size)
            ring_fill(&d->ring, 0, actual_size - oc.off); // a short stream is zero-padded
    } else if (dcmp_id == 2) {
        if (dcmp2_decompress(&s->ic, &oc, actual_size, h[16], h[17]) < 0)
            return codec_fail(d, CODEC_ERR_DATA, "dcmp 2: corrupt stream");
    } else if (dcmp3_decompress(in, &d->ring, actual_size) < 0)
        return codec_fail(d, CODEC_ERR_DATA, "dcmp 3: corrupt stream");
    return CODEC_END;
}

static void dcmp_free(codec_dec_t *d) {
    free(((dcmp_t *)d->st)->ic.scratch);
}

// The ring is sized from the header (the whole resource), so none is declared.
const codec_dec_ops_t codec_dcmp_dec = {.state_size = sizeof(dcmp_t), .fill = dcmp_fill, .free = dcmp_free};
