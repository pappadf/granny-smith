// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// bin.c — MacBinary II (.bin) format peeler.
//
// Format spec: docs/bin.md
//
// MacBinary wraps a single Macintosh file (both forks + metadata) in a
// simple container (bin.md § 3):
//
//   1. 128-byte header — filename, type/creator, Finder flags, fork
//      lengths, CRC-16.  (bin.md § 4)
//
//   2. Data fork — raw bytes, padded to 128-byte boundary.  (bin.md § 10)
//
//   3. Resource fork — raw bytes, padded to 128-byte boundary.  (bin.md § 10)

#include "internal.h"

// ============================================================================
// Constants and Macros
// ============================================================================

// bin.md § 2.2 — header and alignment block size.
#define MB_BLOCK 128

// bin.md § 4.1 — maximum filename length in a MacBinary header.
#define MB_NAME_MAX 63

// bin.md § 8.1 — Finder flag bits to clear on decode:
// kIsOnDesktop (0), bFOwnAppl (1), kHasBeenInited (8),
// kHasCustomIcon (9), kIsShared (10).
#define FINDER_CLEAR_MASK \
    ((1u << 0) | (1u << 1) | (1u << 8) | (1u << 9) | (1u << 10))

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// Parsed fields from the MacBinary header.
// bin.md § 4.1 — field offsets and types.
typedef struct {
    char name[MB_NAME_MAX + 1]; // Filename (null-terminated)
    uint8_t name_len;           // Original filename length
    uint32_t mac_type;          // File type (offset 65)
    uint32_t mac_creator;       // Creator code (offset 69)
    uint16_t finder_flags;      // Finder flags (offsets 73 + 101)
    uint32_t data_len;          // Data fork length (offset 83)
    uint32_t rsrc_len;          // Resource fork length (offset 87)
    uint16_t sec_hdr_len;       // Secondary header length (offset 120)
} bin_header_t;

// ============================================================================
// Static Helpers
// ============================================================================

// bin.md § 2.2 — compute padding to the next 128-byte boundary.
static size_t pad128(size_t n) {
    return (MB_BLOCK - (n % MB_BLOCK)) % MB_BLOCK;
}

// bin.md § 16.2 — detect if a buffer begins with a StuffIt archive signature.
// Checks both classic SIT ("SIT!" etc. + "rLau") and SIT5 signatures.
static bool looks_like_sit(const uint8_t *buf, size_t len) {
    // SIT5: "StuffIt (c)1997-" at offset 0 and Aladdin URL at offset 20
    if (len >= 80) {
        if (memcmp(buf, "StuffIt (c)1997-", 16) == 0 &&
            memcmp(buf + 20,
                   " Aladdin Systems, Inc., "
                   "http://www.aladdinsys.com/StuffIt/",
                   58) == 0) {
            return true;
        }
    }
    // Classic SIT: one of several 4-byte magic values + "rLau" at offset 10
    if (len >= 14) {
        static const char *sigs[] = {
            "SIT!", "ST46", "ST50", "ST60", "ST65",
            "STin", "STi2", "STi3", "STi4",
        };
        for (int i = 0; i < (int)(sizeof(sigs) / sizeof(sigs[0])); i++) {
            if (memcmp(buf, sigs[i], 4) == 0 &&
                memcmp(buf + 10, "rLau", 4) == 0) {
                return true;
            }
        }
    }
    return false;
}

// bin.md § 6 — validate a 128-byte header buffer as MacBinary II.
// Returns true if the buffer passes all required validation checks.
static bool bin_validate(const uint8_t *hdr) {
    // bin.md § 6.1 — byte 0 must be 0 for file records
    if (hdr[0] != 0) {
        return false;
    }

    // bin.md § 6.1 — byte 74 must be 0
    if (hdr[74] != 0) {
        return false;
    }

    // bin.md § 6.3 — filename length must be 1–63
    uint8_t name_len = hdr[1];
    if (name_len == 0 || name_len > MB_NAME_MAX) {
        return false;
    }

    // bin.md § 6.2 — CRC-16/XMODEM over bytes 0–123, stored at 124–125
    uint16_t crc_calc = crc16_ccitt(hdr, 124);
    uint16_t crc_stored = rd16be(hdr + 124);
    if (crc_calc != crc_stored) {
        // bin.md § 6.2 — MacBinary I fallback: accept if byte 82 is 0
        if (hdr[82] != 0) {
            return false;
        }
    }

    return true;
}

// bin.md § 4.1 — extract metadata fields from a validated 128-byte header.
static bin_header_t bin_parse_header(const uint8_t *hdr) {
    bin_header_t h;
    memset(&h, 0, sizeof(h));

    // bin.md § 4.1 — filename: Pascal string at offset 1 (length) / 2 (data)
    h.name_len = hdr[1];
    size_t copy = h.name_len;
    if (copy > MB_NAME_MAX) {
        copy = MB_NAME_MAX;
    }
    memcpy(h.name, hdr + 2, copy);
    h.name[copy] = '\0';

    // bin.md § 4.1 — file type at offset 65, creator at offset 69
    h.mac_type = rd32be(hdr + 65);
    h.mac_creator = rd32be(hdr + 69);

    // bin.md § 4.2 — Finder flags: high byte at offset 73, low byte at 101
    h.finder_flags = (uint16_t)((uint16_t)hdr[73] << 8 | (uint16_t)hdr[101]);

    // bin.md § 4.1 — data fork length at offset 83, resource fork at 87
    h.data_len = rd32be(hdr + 83);
    h.rsrc_len = rd32be(hdr + 87);

    // bin.md § 4.1 — secondary header length at offset 120
    h.sec_hdr_len = rd16be(hdr + 120);

    return h;
}

// ============================================================================
// Static Helpers — Full Decode Pipeline
// ============================================================================

// Decode a MacBinary file into a peel_file_t with both forks and metadata.
// This is the shared implementation for both peel_bin and peel_bin_file.
// bin.md § 14.1 — decoding steps for a MacBinary II file record.
static peel_file_t bin_decode(const uint8_t *src, size_t len,
                              decode_ctx_t *ctx) {
    // bin.md § 14.1 step 1 — need at least 128 bytes for the header
    if (len < MB_BLOCK) {
        decode_abort(ctx, "MacBinary: input too short (%zu bytes)", len);
    }

    // bin.md § 14.1 step 2 — validate header
    if (!bin_validate(src)) {
        decode_abort(ctx, "MacBinary: invalid header");
    }

    // Parse header metadata
    bin_header_t hdr = bin_parse_header(src);

    // bin.md § 6.3 — bounds-check fork lengths
    if (hdr.data_len > 0x7FFFFFFFu || hdr.rsrc_len > 0x7FFFFFFFu) {
        decode_abort(ctx, "MacBinary: fork length exceeds maximum");
    }

    // bin.md § 14.1 step 3 — advance past header and optional secondary header
    size_t pos = MB_BLOCK;
    if (hdr.sec_hdr_len > 0) {
        // bin.md § 9.2 — skip secondary header + alignment padding
        pos += hdr.sec_hdr_len + pad128(hdr.sec_hdr_len);
    }

    // bin.md § 14.1 step 4 — read the data fork
    if (pos + hdr.data_len > len) {
        decode_abort(ctx, "MacBinary: data fork truncated");
    }

    peel_buf_t data_fork = {0};
    if (hdr.data_len > 0) {
        peel_err_t *copy_err = NULL;
        data_fork = peel_buf_copy(src + pos, hdr.data_len, &copy_err);
        if (copy_err) {
            peel_err_free(copy_err);
            decode_abort(ctx, "MacBinary: out of memory for data fork");
        }
    }

    // bin.md § 10.1 — skip data fork + padding to reach resource fork
    pos += hdr.data_len + pad128(hdr.data_len);

    // bin.md § 14.1 step 5 — read the resource fork
    if (pos + hdr.rsrc_len > len) {
        peel_free(&data_fork);
        decode_abort(ctx, "MacBinary: resource fork truncated");
    }

    peel_buf_t rsrc_fork = {0};
    if (hdr.rsrc_len > 0) {
        peel_err_t *copy_err = NULL;
        rsrc_fork = peel_buf_copy(src + pos, hdr.rsrc_len, &copy_err);
        if (copy_err) {
            peel_err_free(copy_err);
            peel_free(&data_fork);
            decode_abort(ctx, "MacBinary: out of memory for resource fork");
        }
    }

    // Assemble the result file
    peel_file_t file;
    memset(&file, 0, sizeof(file));

    // Copy filename into metadata
    size_t nl = hdr.name_len;
    if (nl > sizeof(file.meta.name) - 1) {
        nl = sizeof(file.meta.name) - 1;
    }
    memcpy(file.meta.name, hdr.name, nl);
    file.meta.name[nl] = '\0';

    file.meta.mac_type = hdr.mac_type;
    file.meta.mac_creator = hdr.mac_creator;

    // bin.md § 14.1 step 8 / § 8.1 — sanitize Finder flags
    file.meta.finder_flags = hdr.finder_flags & (uint16_t)~FINDER_CLEAR_MASK;

    file.data_fork = data_fork;
    file.resource_fork = rsrc_fork;

    return file;
}

// ============================================================================
// Operations (Public API) — Detection
// ============================================================================

// bin.md § 6 — probe input for a valid MacBinary II header.
bool bin_detect(const uint8_t *src, size_t len) {
    if (len < MB_BLOCK) {
        return false;
    }
    return bin_validate(src);
}

// ============================================================================
// Operations (Public API) — Wrapper Peel
// ============================================================================

// Decode a MacBinary file and return a single fork as a flat buffer.
// bin.md § 10.3 — if the data fork does not begin with a recognized StuffIt
// signature and a resource fork exists, prefer the resource fork (common
// pattern for .sea.bin self-extracting archives).
peel_buf_t peel_bin(const uint8_t *src, size_t len, peel_err_t **err) {
    *err = NULL;

    // Use setjmp/longjmp for deep-error abort throughout the decode pipeline
    decode_ctx_t ctx;
    if (setjmp(ctx.jmp) != 0) {
        *err = make_err("%s", ctx.errmsg);
        return (peel_buf_t){0};
    }

    peel_file_t file = bin_decode(src, len, &ctx);

    // bin.md § 10.3 — apply fork selection heuristic
    peel_buf_t result;
    bool data_is_sit = file.data_fork.data &&
                       looks_like_sit(file.data_fork.data, file.data_fork.size);

    if (data_is_sit || file.resource_fork.size == 0) {
        // Data fork is a StuffIt archive, or no resource fork — use data fork
        result = file.data_fork;
        peel_free(&file.resource_fork);
    } else {
        // bin.md § 16.2 — prefer resource fork for downstream processing
        result = file.resource_fork;
        peel_free(&file.data_fork);
    }

    return result;
}

// Decode a MacBinary file and return both forks plus metadata.
peel_file_t peel_bin_file(const uint8_t *src, size_t len, peel_err_t **err) {
    *err = NULL;

    decode_ctx_t ctx;
    if (setjmp(ctx.jmp) != 0) {
        *err = make_err("%s", ctx.errmsg);
        return (peel_file_t){0};
    }

    return bin_decode(src, len, &ctx);
}
