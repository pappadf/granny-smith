// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sit.c — StuffIt (.sit) classic and SIT5 archive peeler.
//
// Format spec: docs/sit.md
//
// StuffIt archives come in two structurally incompatible layouts that share
// the .sit extension:
//   - Classic (versions 1.x–4.x): sequential 112-byte entry headers.
//   - SIT5 (version 5.x): linked-list entry headers with 80-byte ASCII magic.
//
// Both layouts are parsed by this file and exposed through a single
// peel_sit() entry point that returns a peel_file_list_t.
//
// Supported compression methods (sit.md § 6 "Compression Methods"):
//   0  = raw copy                    (sit.md § 7 "Method 0: None")
//   1  = RLE90 escape-based RLE      (sit.md § 8 "Method 1: RLE90")
//   2  = LZW (14-bit max, LE bits)   (sit.md § 9 "Method 2: LZW")
//   13 = LZSS+Huffman (sit13.c)      (sit.md § 10 "Method 13")
//   15 = Arsenic/BWT (sit15.c)       (sit.md § 11 "Method 15")

// Ensure strnlen is declared under strict C99 mode.
#define _POSIX_C_SOURCE 200809L

#include "internal.h"

// ============================================================================
// Forward Declarations — sit13 / sit15 helpers
// ============================================================================

// Decompress a method-3 fork into a freshly allocated buffer.
peel_buf_t peel_sit3(const uint8_t *src, size_t len, size_t uncomp_len,
                     peel_err_t **err);

// Decompress a method-13 fork into a freshly allocated buffer.
peel_buf_t peel_sit13(const uint8_t *src, size_t len, size_t uncomp_len,
                      peel_err_t **err);

// Decompress a method-15 fork into a freshly allocated buffer.
peel_buf_t peel_sit15(const uint8_t *src, size_t len, size_t uncomp_len,
                      peel_err_t **err);

// ============================================================================
// Constants and Macros
// ============================================================================

// sit.md § 4.2 "Main Archive Header" — the classic archive header is 22 bytes.
#define SIT_CLASSIC_HDR_SIZE  22

// sit.md § 4.3 "File / Folder Header" — each entry header is 112 bytes.
#define SIT_ENTRY_HDR_SIZE    112

// sit.md § 5.2 "Top Header" — minimum SIT5 archive size is 100 bytes.
#define SIT5_MIN_SIZE         100

// sit.md § 4.4 "Method Byte Encoding" — folder start/end markers.
#define SIT_FOLDER_START  0x20
#define SIT_FOLDER_END    0x21

// sit.md § 4.7 "Classic Iteration Rules" — max folder nesting depth.
#define SIT_MAX_DEPTH  10

// sit.md § 5.7 "Iteration Rules" — max directory map entries for SIT5.
#define SIT5_MAX_DIRS  32

// sit.md § 5.3 "Entry Header" — SIT5 entry magic value.
#define SIT5_ENTRY_MAGIC  0xA5A5A5A5

// sit.md § 9.2 "Parameters" — LZW constants.
#define LZW_MAX_BITS    14
#define LZW_TABLE_CAP   (1 << LZW_MAX_BITS)
#define LZW_CLEAR_CODE  256
#define LZW_FIRST_NEW   257

// Number of known classic SIT signatures.
#define SIT_NUM_SIGS  9

// Maximum number of files from a single archive (safety limit).
#define SIT_MAX_FILES  65536

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// Per-fork metadata unpacked from an entry header.
// sit.md § 4.3 "File / Folder Header"
typedef struct {
    uint32_t       raw_len;     // Uncompressed length
    uint32_t       packed_len;  // Compressed length
    uint16_t       crc;         // CRC-16 from header
    uint8_t        method;      // Compression method ID (low nibble)
    const uint8_t *data;        // Pointer to compressed bytes in archive
} sit_fork_info_t;

// A single parsed file entry (metadata + fork info + path).
typedef struct {
    char             name[512];    // Full path (folders prepended)
    uint32_t         mac_type;     // Mac file type
    uint32_t         mac_creator;  // Mac creator code
    uint16_t         finder_flags; // Finder flags
    sit_fork_info_t  data_fork;    // Data fork info
    sit_fork_info_t  rsrc_fork;    // Resource fork info
    bool             has_rsrc;     // Resource fork present
} sit_entry_t;

// Growable list of parsed file entries.
typedef struct {
    sit_entry_t *items;
    int          count;
    int          cap;
} sit_entry_list_t;

// LZW decoder state.
// sit.md § 9.3 "Dictionary Structure" — struct-of-arrays layout.
typedef struct {
    const uint8_t *src;        // Compressed bytestream
    size_t         src_bytes;  // Length of compressed data
    size_t         bit_pos;    // Current bit position in stream

    uint16_t prev_code[LZW_TABLE_CAP]; // Back-link to parent code
    uint8_t  suffix[LZW_TABLE_CAP];    // Byte appended at this entry
    uint8_t  head[LZW_TABLE_CAP];      // First byte of the chain
    uint16_t chain_len[LZW_TABLE_CAP]; // Length of expanded string

    int      tbl_next;     // Next free dictionary slot
    int      code_bits;    // Current code width
    int      prev;         // Previous code (-1 = none)
    int      block_count;  // Codes emitted since last clear

    uint8_t  stage[LZW_TABLE_CAP]; // Staging buffer for reversed expansion
    size_t   stage_rd;             // Read position in staging buffer
    size_t   stage_len;            // Valid bytes in staging buffer
} lzw_state_t;

// SIT5 directory map entry for path construction.
// sit.md § 5.7 "Iteration Rules"
typedef struct {
    uint32_t offset;    // Byte offset of the folder header
    char     path[512]; // Reconstructed full path
} sit5_dir_entry_t;

// ============================================================================
// Static Helpers — CRC-16
// ============================================================================

// sit.md § 3 "CRC-16 Integrity Check" — reflected CRC-16, poly 0x8005
// (CRC-16/IBM), NOT CCITT.  Table generated with reflected polynomial 0xA001.
// sit.md § "Appendix A: CRC-16 Lookup Table"
static const uint16_t sit_crc_table[256] = {
    0x0000,0xC0C1,0xC181,0x0140,0xC301,0x03C0,0x0280,0xC241,
    0xC601,0x06C0,0x0780,0xC741,0x0500,0xC5C1,0xC481,0x0440,
    0xCC01,0x0CC0,0x0D80,0xCD41,0x0F00,0xCFC1,0xCE81,0x0E40,
    0x0A00,0xCAC1,0xCB81,0x0B40,0xC901,0x09C0,0x0880,0xC841,
    0xD801,0x18C0,0x1980,0xD941,0x1B00,0xDBC1,0xDA81,0x1A40,
    0x1E00,0xDEC1,0xDF81,0x1F40,0xDD01,0x1DC0,0x1C80,0xDC41,
    0x1400,0xD4C1,0xD581,0x1540,0xD701,0x17C0,0x1680,0xD641,
    0xD201,0x12C0,0x1380,0xD341,0x1100,0xD1C1,0xD081,0x1040,
    0xF001,0x30C0,0x3180,0xF141,0x3300,0xF3C1,0xF281,0x3240,
    0x3600,0xF6C1,0xF781,0x3740,0xF501,0x35C0,0x3480,0xF441,
    0x3C00,0xFCC1,0xFD81,0x3D40,0xFF01,0x3FC0,0x3E80,0xFE41,
    0xFA01,0x3AC0,0x3B80,0xFB41,0x3900,0xF9C1,0xF881,0x3840,
    0x2800,0xE8C1,0xE981,0x2940,0xEB01,0x2BC0,0x2A80,0xEA41,
    0xEE01,0x2EC0,0x2F80,0xEF41,0x2D00,0xEDC1,0xEC81,0x2C40,
    0xE401,0x24C0,0x2580,0xE541,0x2700,0xE7C1,0xE681,0x2640,
    0x2200,0xE2C1,0xE381,0x2340,0xE101,0x21C0,0x2080,0xE041,
    0xA001,0x60C0,0x6180,0xA141,0x6300,0xA3C1,0xA281,0x6240,
    0x6600,0xA6C1,0xA781,0x6740,0xA501,0x65C0,0x6480,0xA441,
    0x6C00,0xACC1,0xAD81,0x6D40,0xAF01,0x6FC0,0x6E80,0xAE41,
    0xAA01,0x6AC0,0x6B80,0xAB41,0x6900,0xA9C1,0xA881,0x6840,
    0x7800,0xB8C1,0xB981,0x7940,0xBB01,0x7BC0,0x7A80,0xBA41,
    0xBE01,0x7EC0,0x7F80,0xBF41,0x7D00,0xBDC1,0xBC81,0x7C40,
    0xB401,0x74C0,0x7580,0xB541,0x7700,0xB7C1,0xB681,0x7640,
    0x7200,0xB2C1,0xB381,0x7340,0xB101,0x71C0,0x7080,0xB041,
    0x5000,0x90C1,0x9181,0x5140,0x9301,0x53C0,0x5280,0x9241,
    0x9601,0x56C0,0x5780,0x9741,0x5500,0x95C1,0x9481,0x5440,
    0x9C01,0x5CC0,0x5D80,0x9D41,0x5F00,0x9FC1,0x9E81,0x5E40,
    0x5A00,0x9AC1,0x9B81,0x5B40,0x9901,0x59C0,0x5880,0x9841,
    0x8801,0x48C0,0x4980,0x8941,0x4B00,0x8BC1,0x8A81,0x4A40,
    0x4E00,0x8EC1,0x8F81,0x4F40,0x8D01,0x4DC0,0x4C80,0x8C41,
    0x4400,0x84C1,0x8581,0x4540,0x8701,0x47C0,0x4680,0x8641,
    0x8201,0x42C0,0x4380,0x8341,0x4100,0x81C1,0x8081,0x4040,
};

// sit.md § 3.3 "Byte-at-a-Time Update" — feed bytes into running CRC.
static uint16_t sit_crc_update(uint16_t crc, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; ++i)
        crc = sit_crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

// Compute a complete CRC-16 over a buffer (initial value 0).
static inline uint16_t sit_crc(const uint8_t *buf, size_t len) {
    return sit_crc_update(0, buf, len);
}

// ============================================================================
// Static Helpers — Entry List
// ============================================================================

// Initialize an empty entry list.
static void entry_list_init(sit_entry_list_t *list) {
    list->items = NULL;
    list->count = 0;
    list->cap   = 0;
}

// Append one entry, growing the backing array if needed.
static sit_entry_t *entry_list_push(sit_entry_list_t *list, peel_err_t **err) {
    if (list->count >= SIT_MAX_FILES) {
        *err = make_err("SIT: too many files in archive (limit %d)", SIT_MAX_FILES);
        return NULL;
    }
    if (list->count == list->cap) {
        // Double the capacity (start at 16)
        int new_cap = list->cap ? list->cap * 2 : 16;
        sit_entry_t *tmp = realloc(list->items,
                                   (size_t)new_cap * sizeof(sit_entry_t));
        if (!tmp) {
            *err = make_err("SIT: out of memory growing entry list");
            return NULL;
        }
        list->items = tmp;
        list->cap   = new_cap;
    }
    sit_entry_t *e = &list->items[list->count++];
    memset(e, 0, sizeof(*e));
    return e;
}

// Release the entry list backing storage.
static void entry_list_free(sit_entry_list_t *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap   = 0;
}

// ============================================================================
// Static Helpers — Path Construction
// ============================================================================

// Build "dir/name" into dst.  Either part may be empty.
// sit.md § 5.7 "Iteration Rules" — paths are built by resolving parent_offset.
static void build_path(char *dst, size_t cap, const char *dir, const char *name) {
    if (!cap) return;
    dst[0] = '\0';
    if (dir && dir[0]) {
        size_t dl = strnlen(dir, cap - 1);
        memcpy(dst, dir, dl);
        size_t p = dl;
        // Append separator
        if (p < cap - 1)
            dst[p++] = '/';
        if (name) {
            size_t nl = strnlen(name, cap - 1 - p);
            if (nl) memcpy(dst + p, name, nl);
            p += nl;
        }
        dst[p < cap ? p : cap - 1] = '\0';
    } else if (name) {
        size_t nl = strnlen(name, cap - 1);
        memcpy(dst, name, nl);
        dst[nl] = '\0';
    }
}

// ============================================================================
// Static Helpers — LZW Decoder
// ============================================================================

// sit.md § 9.3 "Dictionary Structure" — initialize the LZW decoder with
// root entries 0–255, code width 9, first free slot at 257.
static lzw_state_t *lzw_create(const uint8_t *src, size_t src_bytes) {
    lzw_state_t *z = calloc(1, sizeof(*z));
    if (!z) return NULL;
    z->src       = src;
    z->src_bytes = src_bytes;
    z->code_bits = 9;
    z->tbl_next  = LZW_FIRST_NEW;
    z->prev      = -1;
    // Initialize root entries (single-byte identity codes)
    for (int i = 0; i < 256; ++i) {
        z->prev_code[i] = UINT16_MAX;
        z->suffix[i]    = (uint8_t)i;
        z->head[i]      = (uint8_t)i;
        z->chain_len[i] = 1;
    }
    return z;
}

// sit.md § 9.4 "Bit Packing" — read one code from the LE bitstream.
// Returns -1 on input exhaustion.
static int lzw_next_code(lzw_state_t *z) {
    size_t byte_off = z->bit_pos >> 3;
    if (byte_off >= z->src_bytes)
        return -1;
    // Read up to 4 bytes starting at the byte boundary (little-endian)
    uint32_t acc = 0;
    size_t avail = z->src_bytes - byte_off;
    if (avail > 4) avail = 4;
    memcpy(&acc, z->src + byte_off, avail);
    int shift = (int)(z->bit_pos & 7);
    int mask  = (1 << z->code_bits) - 1;
    int code  = (int)((acc >> shift) & (uint32_t)mask);
    z->bit_pos += (size_t)z->code_bits;
    z->block_count++;
    return code;
}

// sit.md § 9.5 "Decoding Loop" — expand a code into the staging buffer
// by walking the dictionary chain backward.
static void lzw_expand(lzw_state_t *z, int code) {
    int len = z->chain_len[code];
    if (len > (int)sizeof(z->stage))
        len = (int)sizeof(z->stage);
    int pos = len;
    int cur = code;
    // Walk backward through chain, storing bytes in reverse
    while (cur != (int)UINT16_MAX && pos > 0) {
        z->stage[--pos] = z->suffix[cur];
        cur = z->prev_code[cur];
    }
    // Shift left if we stopped short (shouldn't normally happen)
    if (pos > 0) {
        memmove(z->stage, z->stage + pos, (size_t)(len - pos));
        len -= pos;
    }
    z->stage_rd  = 0;
    z->stage_len = (size_t)len;
}

// sit.md § 9.7 "Code Width Expansion" — add a new dictionary entry and
// widen code width at power-of-two boundaries.
static void lzw_add_entry(lzw_state_t *z, int prev_val, uint8_t first_byte) {
    if (z->tbl_next >= LZW_TABLE_CAP)
        return;
    int idx = z->tbl_next;
    z->prev_code[idx] = (uint16_t)prev_val;
    z->suffix[idx]    = first_byte;
    z->head[idx]      = z->head[prev_val];
    z->chain_len[idx] = z->chain_len[prev_val] + 1;
    z->tbl_next++;
    // Widen code when table size reaches a power of two (max 14 bits)
    if (z->tbl_next < LZW_TABLE_CAP &&
        (z->tbl_next & (z->tbl_next - 1)) == 0 &&
        z->code_bits < LZW_MAX_BITS)
        z->code_bits++;
}

// sit.md § 9.5 "Decoding Loop" — produce up to `want` decompressed bytes.
// Returns number of bytes produced (0 = EOF).
static size_t lzw_decode(lzw_state_t *z, uint8_t *dst, size_t want) {
    size_t got = 0;
    while (got < want) {
        // Drain staging buffer first
        if (z->stage_rd < z->stage_len) {
            size_t n = z->stage_len - z->stage_rd;
            if (n > want - got) n = want - got;
            memcpy(dst + got, z->stage + z->stage_rd, n);
            z->stage_rd += n;
            got += n;
            continue;
        }
        int code = lzw_next_code(z);
        if (code < 0)
            break;
        // sit.md § 9.6 "Clear Code and Block Alignment" — clear code 256
        // resets dictionary and skips remaining 8-code block
        if (code == LZW_CLEAR_CODE) {
            if (z->block_count & 7)
                z->bit_pos += (size_t)(z->code_bits *
                                       (8 - (z->block_count & 7)));
            z->tbl_next    = LZW_FIRST_NEW;
            z->code_bits   = 9;
            z->prev        = -1;
            z->block_count = 0;
            continue;
        }
        // First code after reset: single byte, no dict entry added
        if (z->prev < 0) {
            if (code < 256)
                dst[got++] = (uint8_t)code;
            z->prev = code;
            continue;
        }
        // sit.md § 9.8 "The KwKwK Case" — determine first byte of expansion
        uint8_t first_ch;
        if (code < z->tbl_next)
            first_ch = z->head[code];
        else
            first_ch = z->head[z->prev];
        // Add new dictionary entry: prev + first_ch
        lzw_add_entry(z, z->prev, first_ch);
        // Expand current code (or synthesize KwKwK)
        if (code < z->tbl_next) {
            lzw_expand(z, code);
        } else {
            // KwKwK: expand prev chain, then append first_ch
            int len = (int)z->chain_len[z->prev] + 1;
            if (len > (int)sizeof(z->stage))
                len = (int)sizeof(z->stage);
            int pos = len;
            z->stage[--pos] = first_ch;
            int cur = z->prev;
            while (cur != (int)UINT16_MAX && pos > 0) {
                z->stage[--pos] = z->suffix[cur];
                cur = z->prev_code[cur];
            }
            if (pos > 0) {
                memmove(z->stage, z->stage + pos, (size_t)(len - pos));
                len -= pos;
            }
            z->stage_rd  = 0;
            z->stage_len = (size_t)len;
        }
        z->prev = code;
    }
    return got;
}

// Free an LZW decoder.
static void lzw_destroy(lzw_state_t *z) {
    free(z);
}

// ============================================================================
// Static Helpers — Fork Decompression
// ============================================================================

// Decompress a single fork using the specified compression method.
// Returns an owned buffer on success, or a zero buffer with *err set.
// sit.md § 6 "Compression Methods" — dispatch by method ID.
static peel_buf_t decompress_fork(const sit_fork_info_t *fi, peel_err_t **err) {
    uint32_t raw_len    = fi->raw_len;
    uint32_t packed_len = fi->packed_len;
    uint16_t expect_crc = fi->crc;
    uint8_t  method     = fi->method;
    const uint8_t *src  = fi->data;

    // sit.md § 10.A "Method 3" — delegated to sit3.c (static Huffman)
    if (method == 3) {
        peel_buf_t result = peel_sit3(src, packed_len, raw_len, err);
        if (*err) return (peel_buf_t){0};
        // sit.md § 6.3 "CRC Verification Rule" — verify CRC over decompressed
        uint16_t actual = sit_crc(result.data, result.size);
        if (actual != expect_crc) {
            *err = make_err("SIT: fork CRC mismatch (expected 0x%04X, got 0x%04X)",
                            expect_crc, actual);
            peel_free(&result);
            return (peel_buf_t){0};
        }
        return result;
    }

    // sit.md § 10 "Method 13" — delegated to sit13.c
    if (method == 13) {
        peel_buf_t result = peel_sit13(src, packed_len, raw_len, err);
        if (*err) return (peel_buf_t){0};
        // sit.md § 6.3 "CRC Verification Rule" — verify CRC over decompressed
        uint16_t actual = sit_crc(result.data, result.size);
        if (actual != expect_crc) {
            *err = make_err("SIT: fork CRC mismatch (expected 0x%04X, got 0x%04X)",
                            expect_crc, actual);
            peel_free(&result);
            return (peel_buf_t){0};
        }
        return result;
    }

    // sit.md § 11 "Method 15" — delegated to sit15.c
    // sit.md § 6.3 — method 15 handles integrity internally; skip CRC check.
    if (method == 15) {
        return peel_sit15(src, packed_len, raw_len, err);
    }

    // Allocate output buffer for methods 0, 1, 2
    uint8_t *out = malloc(raw_len);
    if (!out) {
        *err = make_err("SIT: out of memory allocating %u bytes for fork",
                        raw_len);
        return (peel_buf_t){0};
    }

    size_t produced = 0;

    switch (method) {
    case 0: {
        // sit.md § 7 "Method 0: None" — raw copy
        if (packed_len < raw_len) {
            *err = make_err("SIT: method 0 packed (%u) < raw (%u)",
                            packed_len, raw_len);
            free(out);
            return (peel_buf_t){0};
        }
        memcpy(out, src, raw_len);
        produced = raw_len;
        break;
    }

    case 1: {
        // sit.md § 8 "Method 1: RLE90" — escape-based run-length encoding
        // sit.md § 8.2 "State" — last_byte initialized to 0
        uint8_t last_byte = 0;
        size_t  src_off = 0;
        while (produced < raw_len && src_off < packed_len) {
            uint8_t b = src[src_off++];
            if (b != 0x90) {
                // Literal byte
                out[produced++] = b;
                last_byte = b;
            } else {
                // sit.md § 8.3 "Algorithm" — escape marker 0x90
                if (src_off >= packed_len) break;
                uint8_t n = src[src_off++];
                if (n == 0) {
                    // Literal 0x90 (do not update last_byte)
                    out[produced++] = 0x90;
                } else if (n > 1) {
                    // Repeat last_byte (n-1) additional times
                    size_t repeats = (size_t)(n - 1);
                    if (produced + repeats > raw_len)
                        repeats = raw_len - produced;
                    memset(out + produced, last_byte, repeats);
                    produced += repeats;
                }
                // n == 1 means zero additional copies
            }
        }
        break;
    }

    case 2: {
        // sit.md § 9 "Method 2: LZW" — 14-bit max, LE bit packing
        lzw_state_t *lzw = lzw_create(src, packed_len);
        if (!lzw) {
            *err = make_err("SIT: out of memory creating LZW decoder");
            free(out);
            return (peel_buf_t){0};
        }
        produced = lzw_decode(lzw, out, raw_len);
        lzw_destroy(lzw);
        break;
    }

    default:
        // sit.md § 12 "Unsupported Methods" — fatal error
        *err = make_err("SIT: unsupported compression method %d", method);
        free(out);
        return (peel_buf_t){0};
    }

    // sit.md § 6.3 "CRC Verification Rule" — verify CRC over decompressed data
    uint16_t actual_crc = sit_crc(out, produced);
    if (actual_crc != expect_crc) {
        *err = make_err("SIT: fork CRC mismatch (expected 0x%04X, got 0x%04X)",
                        expect_crc, actual_crc);
        free(out);
        return (peel_buf_t){0};
    }

    return (peel_buf_t){.data = out, .size = produced, .owned = true};
}

// ============================================================================
// Static Helpers — Classic Archive Parsing
// ============================================================================

// The 9 recognized 4-byte signatures for classic StuffIt.
// sit.md § 4.1 "Identification"
static const char *classic_sigs[SIT_NUM_SIGS] = {
    "SIT!", "ST46", "ST50", "ST60", "ST65", "STin", "STi2", "STi3", "STi4"
};

// Scan the input for the classic SIT magic (any of 9 signatures + "rLau").
// Returns the byte offset of the archive start, or -1 if not found.
// sit.md § 4.1 "Identification" and § 14.2 "Embedded Archive Detection"
static int64_t find_classic_magic(const uint8_t *src, size_t len) {
    if (len < SIT_CLASSIC_HDR_SIZE) return -1;
    size_t limit = len - 14;
    for (size_t off = 0; off <= limit; ++off) {
        // Check for "rLau" at offset 10–13 first (fast rejection)
        if (memcmp(src + off + 10, "rLau", 4) != 0)
            continue;
        // Check for any of the 9 known signatures at offset 0–3
        for (int s = 0; s < SIT_NUM_SIGS; ++s) {
            if (memcmp(src + off, classic_sigs[s], 4) == 0)
                return (int64_t)off;
        }
    }
    return -1;
}

// Parse all file entries from a classic StuffIt archive.
// sit.md § 4.7 "Classic Iteration Rules" and Appendix B
static bool parse_classic(const uint8_t *blob, size_t blob_len,
                          size_t archive_off, sit_entry_list_t *entries,
                          peel_err_t **err) {
    const uint8_t *base = blob + archive_off;
    size_t avail = blob_len - archive_off;

    if (avail < SIT_CLASSIC_HDR_SIZE) {
        *err = make_err("SIT classic: archive too small");
        return false;
    }

    // sit.md § 4.2 "Main Archive Header" — file_count at offset 4
    uint16_t file_count = rd16be(base + 4);
    uint32_t cursor = SIT_CLASSIC_HDR_SIZE;
    uint32_t done = 0;

    // sit.md § 4.7 — folder stack of up to 10 nesting levels
    char dirs[SIT_MAX_DEPTH][64];
    int  depth = 0;

    while (done < file_count) {
        if ((size_t)cursor + SIT_ENTRY_HDR_SIZE > avail) break;

        const uint8_t *hdr = base + cursor;
        uint8_t rm = hdr[0];
        uint8_t dm = hdr[1];

        // sit.md § 4.4 — folder start marker (0x20)
        // sit.md § 4.7 — file_count counts top-level entries only; a folder
        // (open through close) is one top-level entry regardless of contents.
        // The structural markers themselves do not increment `done`; the
        // outermost folder-end does (when depth pops back to 0).
        if (rm == SIT_FOLDER_START || dm == SIT_FOLDER_START) {
            uint8_t nlen = hdr[2];
            if (depth < SIT_MAX_DEPTH && nlen < 64) {
                memcpy(dirs[depth], hdr + 3, nlen);
                dirs[depth][nlen] = '\0';
            }
            if (depth < SIT_MAX_DEPTH) depth++;
            cursor += SIT_ENTRY_HDR_SIZE;
            continue;
        }

        // sit.md § 4.4 — folder end marker (0x21)
        if (rm == SIT_FOLDER_END || dm == SIT_FOLDER_END) {
            if (depth > 0) depth--;
            cursor += SIT_ENTRY_HDR_SIZE;
            if (depth == 0) done++;
            continue;
        }

        // sit.md § 4.4 — skip entries with unknown high bits
        if ((rm & 0xE0) || (dm & 0xE0)) {
            cursor += SIT_ENTRY_HDR_SIZE;
            if (depth == 0) done++;
            continue;
        }

        // ---- Regular file entry ----
        // sit.md § 4.3 "File / Folder Header (Fixed 112 Bytes)"
        uint8_t nlen = hdr[2];
        char fname[64];
        if (nlen >= sizeof(fname)) nlen = (uint8_t)(sizeof(fname) - 1);
        memcpy(fname, hdr + 3, nlen);
        fname[nlen] = '\0';

        // Build full path from folder stack
        char path[512] = "";
        size_t p = 0;
        for (int d = 0; d < depth; d++) {
            size_t sl = strlen(dirs[d]);
            if (p + sl + 1 >= sizeof(path)) break;
            memcpy(path + p, dirs[d], sl);
            p += sl;
            path[p++] = '/';
        }
        // Append file name
        size_t fl = strnlen(fname, sizeof(path) - 1 - p);
        if (fl > 0) memcpy(path + p, fname, fl);
        path[p + fl] = '\0';

        // sit.md § 4.3 — type at 66, creator at 70, finder flags at 74
        uint32_t ftype    = rd32be(hdr + 66);
        uint32_t fcreator = rd32be(hdr + 70);
        uint16_t fflags   = rd16be(hdr + 74);

        // sit.md § 4.3 — fork lengths and CRCs
        uint32_t rulen = rd32be(hdr + 84);
        uint32_t dulen = rd32be(hdr + 88);
        uint32_t rclen = rd32be(hdr + 92);
        uint32_t dclen = rd32be(hdr + 96);
        uint16_t rcrc  = rd16be(hdr + 100);
        uint16_t dcrc  = rd16be(hdr + 102);

        // sit.md § 4.5 "Fork Data Layout" — rsrc first, then data
        const uint8_t *rsrc_ptr = base + cursor + SIT_ENTRY_HDR_SIZE;
        const uint8_t *data_ptr = rsrc_ptr + rclen;

        // Bounds check
        if ((size_t)(data_ptr - blob) + dclen > blob_len) {
            *err = make_err("SIT classic: fork data extends past archive end");
            return false;
        }

        // Add entry to the list
        sit_entry_t *ent = entry_list_push(entries, err);
        if (!ent) return false;

        strncpy(ent->name, path, sizeof(ent->name) - 1);
        ent->mac_type     = ftype;
        ent->mac_creator  = fcreator;
        ent->finder_flags = fflags;
        ent->data_fork = (sit_fork_info_t){
            .raw_len    = dulen,
            .packed_len = dclen,
            .crc        = dcrc,
            .method     = (uint8_t)(dm & 0x0F),
            .data       = data_ptr
        };
        ent->rsrc_fork = (sit_fork_info_t){
            .raw_len    = rulen,
            .packed_len = rclen,
            .crc        = rcrc,
            .method     = (uint8_t)(rm & 0x0F),
            .data       = rsrc_ptr
        };
        ent->has_rsrc = (rulen > 0);

        // Advance past both fork data regions
        cursor = (uint32_t)((size_t)(data_ptr - base) + dclen);
        if (depth == 0) done++;
    }

    return true;
}

// ============================================================================
// Static Helpers — SIT5 Archive Parsing
// ============================================================================

// Scan the input for the SIT5 magic string.
// Returns the byte offset of the archive start, or -1 if not found.
// sit.md § 5.1 "Identification"
static int64_t find_sit5_magic(const uint8_t *src, size_t len) {
    if (len < 80) return -1;
    size_t limit = len - 80;
    for (size_t off = 0; off <= limit; ++off) {
        // sit.md § 5.1 — check two validated substrings; bytes 16–19 (year)
        // and bytes 78–79 (CR LF) are NOT validated.
        if (memcmp(src + off, "StuffIt (c)1997-", 16) == 0 &&
            memcmp(src + off + 20,
                   " Aladdin Systems, Inc., "
                   "http://www.aladdinsys.com/StuffIt/",
                   58) == 0)
            return (int64_t)off;
    }
    return -1;
}

// Parse all file entries from a SIT5 archive.
// sit.md § 5.7 "Iteration Rules" and Appendix C
static bool parse_sit5(const uint8_t *blob, size_t blob_len,
                       size_t archive_off, sit_entry_list_t *entries,
                       peel_err_t **err) {
    const uint8_t *base = blob + archive_off;
    size_t avail = blob_len - archive_off;

    if (avail < SIT5_MIN_SIZE) {
        *err = make_err("SIT5: archive too small (%zu bytes)", avail);
        return false;
    }

    // sit.md § 5.2 "Top Header" — entry count at offset 92, cursor at 94
    uint16_t entry_count = rd16be(base + 92);
    uint32_t cursor      = rd32be(base + 94);
    uint32_t remaining   = entry_count;

    // Directory map for path resolution
    sit5_dir_entry_t dmap[SIT5_MAX_DIRS];
    int dmap_cnt = 0;

    while (remaining > 0 && cursor != 0 &&
           (size_t)cursor + 48 <= avail) {
        const uint8_t *h1 = base + cursor;

        // sit.md § 5.3 "Entry Header" — validate entry magic
        if (rd32be(h1) != SIT5_ENTRY_MAGIC) {
            *err = make_err("SIT5: invalid entry magic at offset %u", cursor);
            return false;
        }

        // sit.md § 5.3 — only version 1 is supported
        if (h1[4] != 1) {
            *err = make_err("SIT5: unsupported entry version %d", h1[4]);
            return false;
        }

        uint16_t h1_len = rd16be(h1 + 6);
        if ((size_t)cursor + h1_len > avail) {
            *err = make_err("SIT5: header1 extends past archive end");
            return false;
        }

        // sit.md § 3.5 "Where CRCs Are Used" — verify header 1 CRC
        // (bytes 32–33 zeroed before computation)
        {
            uint8_t *tmp = malloc(h1_len);
            if (!tmp) {
                *err = make_err("SIT5: out of memory for header CRC");
                return false;
            }
            memcpy(tmp, h1, h1_len);
            tmp[32] = tmp[33] = 0;
            uint16_t computed = sit_crc(tmp, h1_len);
            uint16_t stored   = rd16be(h1 + 32);
            free(tmp);
            if (computed != stored) {
                *err = make_err("SIT5: header CRC mismatch at offset %u",
                                cursor);
                return false;
            }
        }

        uint32_t h2_off       = cursor + h1_len;
        uint8_t  flags        = h1[9];
        uint32_t parent_off   = rd32be(h1 + 26);
        uint16_t namelen      = rd16be(h1 + 30);
        uint32_t d_raw_len    = rd32be(h1 + 34);
        uint32_t d_packed_len = rd32be(h1 + 38);
        uint16_t d_crc        = rd16be(h1 + 42);


        // Read entry name (starts at byte 48 of header 1)
        char namebuf[256];
        {
            size_t cl = namelen;
            if (cl > sizeof(namebuf) - 1) cl = sizeof(namebuf) - 1;
            if ((size_t)cursor + 48 + cl > avail) cl = avail - (size_t)cursor - 48;
            memcpy(namebuf, h1 + 48, cl);
            namebuf[cl] = '\0';
        }

        // sit.md § 5.6 "Special Markers" — 0xFFFFFFFF entries are skipped
        // before header 2 parsing, since header 2 may not exist.
        if (d_raw_len == 0xFFFFFFFF) {
            // sit.md § 5.6 — folder markers: increment remaining by 1
            if (flags & 0x40)
                remaining++;
            cursor = h2_off;
            continue;
        }

        // Parse header 2
        // sit.md § 5.4 "Secondary Header (Header 2)"
        if ((size_t)h2_off + 32 > avail) {
            *err = make_err("SIT5: header2 extends past archive end");
            return false;
        }
        const uint8_t *h2 = base + h2_off;
        uint16_t flags2   = rd16be(h2 + 0);
        uint32_t ftype    = rd32be(h2 + 4);
        uint32_t fcreator = rd32be(h2 + 8);
        uint16_t fflags   = rd16be(h2 + 12);

        // sit.md § 5.4 — version-dependent skip past header 2 prefix
        uint32_t skip_extra = (h1[4] == 1) ? 22 : 18;
        bool     rsrc_present = (flags2 & 0x01) != 0;
        const uint8_t *after_prefix = h2 + 14 + skip_extra;
        const uint8_t *payload_ptr  = after_prefix;

        // sit.md § 5.4 — resource fork fields (conditional)
        uint32_t r_raw_len = 0, r_packed_len = 0;
        uint16_t r_crc     = 0;
        uint8_t  r_algo    = 0;
        if (rsrc_present) {
            if ((size_t)(after_prefix - blob) + 14 > blob_len) {
                *err = make_err("SIT5: resource info past archive end");
                return false;
            }
            r_raw_len    = rd32be(after_prefix + 0);
            r_packed_len = rd32be(after_prefix + 4);
            r_crc        = rd16be(after_prefix + 8);
            r_algo       = after_prefix[12];
            uint8_t rpass = after_prefix[13];
            payload_ptr  = after_prefix + 14 + rpass;
        }

        // sit.md § 5.3 — folder entries (flags bit 6)
        if (flags & 0x40) {
            uint16_t child_count = rd16be(h1 + 46);

            // Build parent path
            char ppath[512] = "";
            if (parent_off != 0) {
                for (int i = 0; i < dmap_cnt; ++i) {
                    if (dmap[i].offset == parent_off) {
                        strncpy(ppath, dmap[i].path, sizeof(ppath) - 1);
                        ppath[sizeof(ppath) - 1] = '\0';
                        break;
                    }
                }
            }

            // Record folder in directory map
            char folder_full[512];
            build_path(folder_full, sizeof(folder_full), ppath, namebuf);
            if (dmap_cnt < SIT5_MAX_DIRS) {
                dmap[dmap_cnt].offset = cursor;
                strncpy(dmap[dmap_cnt].path, folder_full,
                        sizeof(dmap[dmap_cnt].path) - 1);
                dmap[dmap_cnt].path[sizeof(dmap[dmap_cnt].path) - 1] = '\0';
                dmap_cnt++;
            }

            // sit.md § 5.7 — add child count, advance into children
            remaining += child_count;
            cursor = (uint32_t)(payload_ptr - base);
            continue;
        }

        // ---- Regular file entry ----
        // sit.md § 5.3 — data method at byte 46, password at byte 47
        uint8_t d_algo    = h1[46];
        uint8_t d_passlen = h1[47];

        // sit.md § 13.2 "Decompression Errors" — reject encrypted entries
        if ((flags & 0x20) && d_raw_len && d_passlen) {
            *err = make_err("SIT5: encrypted entries are not supported");
            return false;
        }

        // Build full path from parent
        char ppath[512] = "";
        if (parent_off != 0) {
            for (int i = 0; i < dmap_cnt; ++i) {
                if (dmap[i].offset == parent_off) {
                    strncpy(ppath, dmap[i].path, sizeof(ppath) - 1);
                    ppath[sizeof(ppath) - 1] = '\0';
                    break;
                }
            }
        }
        char full_name[512];
        build_path(full_name, sizeof(full_name), ppath, namebuf);

        // sit.md § 5.5 "Fork Data Layout" — resource fork first, then data
        const uint8_t *r_base = payload_ptr;
        const uint8_t *d_base = payload_ptr + (rsrc_present ? r_packed_len : 0);
        if ((size_t)(d_base - blob) + d_packed_len > blob_len) {
            *err = make_err("SIT5: data fork extends past archive end");
            return false;
        }

        // Add entry to the list
        sit_entry_t *ent = entry_list_push(entries, err);
        if (!ent) return false;

        strncpy(ent->name, full_name, sizeof(ent->name) - 1);
        ent->mac_type     = ftype;
        ent->mac_creator  = fcreator;
        ent->finder_flags = fflags;

        ent->data_fork = (sit_fork_info_t){
            .raw_len    = d_raw_len,
            .packed_len = d_packed_len,
            .crc        = d_crc,
            .method     = (uint8_t)(d_algo & 0x0F),
            .data       = d_base
        };
        ent->has_rsrc = rsrc_present && r_raw_len > 0;
        if (ent->has_rsrc) {
            ent->rsrc_fork = (sit_fork_info_t){
                .raw_len    = r_raw_len,
                .packed_len = r_packed_len,
                .crc        = r_crc,
                .method     = (uint8_t)(r_algo & 0x0F),
                .data       = r_base
            };
        }

        // Advance cursor past the fork data
        cursor = (uint32_t)((size_t)(d_base - base) + d_packed_len);
        remaining--;
    }

    return true;
}

// ============================================================================
// Static Helpers — Build File List from Entries
// ============================================================================

// Decompress all forks and produce the final peel_file_list_t.
static peel_file_list_t build_file_list(const sit_entry_list_t *entries,
                                        peel_err_t **err) {
    if (entries->count == 0) {
        return (peel_file_list_t){.files = NULL, .count = 0};
    }

    // Count entries with at least one non-empty fork
    int file_count = 0;
    for (int i = 0; i < entries->count; ++i) {
        const sit_entry_t *e = &entries->items[i];
        if (e->data_fork.raw_len > 0 ||
            (e->has_rsrc && e->rsrc_fork.raw_len > 0)) {
            file_count++;
        }
    }

    peel_file_t *files = calloc((size_t)file_count, sizeof(peel_file_t));
    if (!files) {
        *err = make_err("SIT: out of memory for file list (%d files)",
                        file_count);
        return (peel_file_list_t){0};
    }

    int fi = 0;
    for (int i = 0; i < entries->count && fi < file_count; ++i) {
        const sit_entry_t *ent = &entries->items[i];

        // Skip entries with no non-empty forks
        if (ent->data_fork.raw_len == 0 &&
            !(ent->has_rsrc && ent->rsrc_fork.raw_len > 0))
            continue;

        peel_file_t *f = &files[fi];

        // Copy metadata
        strncpy(f->meta.name, ent->name, sizeof(f->meta.name) - 1);
        f->meta.mac_type     = ent->mac_type;
        f->meta.mac_creator  = ent->mac_creator;
        f->meta.finder_flags = ent->finder_flags;

        // Decompress data fork
        if (ent->data_fork.raw_len > 0) {
            f->data_fork = decompress_fork(&ent->data_fork, err);
            if (*err) {
                // Clean up previously allocated files
                for (int j = 0; j < fi; ++j) {
                    peel_free(&files[j].data_fork);
                    peel_free(&files[j].resource_fork);
                }
                free(files);
                return (peel_file_list_t){0};
            }
        }

        // Decompress resource fork
        if (ent->has_rsrc && ent->rsrc_fork.raw_len > 0) {
            f->resource_fork = decompress_fork(&ent->rsrc_fork, err);
            if (*err) {
                // Clean up this file's data fork and all prior files
                peel_free(&f->data_fork);
                for (int j = 0; j < fi; ++j) {
                    peel_free(&files[j].data_fork);
                    peel_free(&files[j].resource_fork);
                }
                free(files);
                return (peel_file_list_t){0};
            }
        }

        fi++;
    }

    return (peel_file_list_t){.files = files, .count = file_count};
}

// ============================================================================
// Operations (Public API) — Detection
// ============================================================================

// sit.md § 2.3 "Detection Strategy" — check for classic or SIT5 magic.
bool sit_detect(const uint8_t *src, size_t len) {
    if (find_classic_magic(src, len) >= 0) return true;
    if (find_sit5_magic(src, len) >= 0)    return true;
    return false;
}

// ============================================================================
// Operations (Public API) — Archive Extraction
// ============================================================================

// Detect, parse, and extract all files from a StuffIt archive.
// Supports both classic (1.x–4.x) and SIT5 (5.x) formats.
// sit.md § 2.3 "Detection Strategy" — prefer earliest match.
peel_file_list_t peel_sit(const uint8_t *src, size_t len, peel_err_t **err) {
    *err = NULL;

    int64_t classic_off = find_classic_magic(src, len);
    int64_t sit5_off    = find_sit5_magic(src, len);

    sit_entry_list_t entries;
    entry_list_init(&entries);

    // Prefer the earliest match
    if (classic_off >= 0 && (sit5_off < 0 || classic_off <= sit5_off)) {
        if (!parse_classic(src, len, (size_t)classic_off, &entries, err)) {
            entry_list_free(&entries);
            return (peel_file_list_t){0};
        }
    } else if (sit5_off >= 0) {
        if (!parse_sit5(src, len, (size_t)sit5_off, &entries, err)) {
            entry_list_free(&entries);
            return (peel_file_list_t){0};
        }
    } else {
        *err = make_err("SIT: no valid StuffIt signature found");
        entry_list_free(&entries);
        return (peel_file_list_t){0};
    }

    // Decompress all forks and build the result
    peel_file_list_t result = build_file_list(&entries, err);
    entry_list_free(&entries);
    return result;
}
