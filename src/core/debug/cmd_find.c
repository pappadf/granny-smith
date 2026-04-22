// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_find.c
// "find" shell command: memory search (proposal-debug-tooling.md §3.1).
// Sub-commands:
//   find str   <text>    [range] [all]   - ASCII/UTF-8 literal search
//   find bytes <hex...>  [range] [all]   - byte-sequence search (2-digit hex tokens)
//   find word  <value>   [range] [all]   - 16-bit value search (68K big-endian)
//   find long  <value>   [range] [all]   - 32-bit value search (68K big-endian)
// Range forms: "<start>..<end>" (half-open) or "<start> <count>".
// Omitted range → whole address space (bounded by g_address_mask).
// A trailing "all" token removes the default FIND_DEFAULT_LIMIT hit cap.

#include "addr_format.h"
#include "cmd_types.h"
#include "memory.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default number of hits reported before "... (N more, use 'find … all')".
#define FIND_DEFAULT_LIMIT 16

// Cap on pattern length; protects against absurd inputs and bounds the scan buffer.
#define FIND_MAX_PATTERN_LEN 256

// Parse one hex byte token (exactly two hex digits); returns true on success.
static bool parse_hex_byte(const char *tok, uint8_t *byte_out) {
    if (!tok || !tok[0] || !tok[1] || tok[2] != '\0')
        return false;
    int hi = 0, lo = 0;
    // Decode high nibble
    if (!isxdigit((unsigned char)tok[0]) || !isxdigit((unsigned char)tok[1]))
        return false;
    hi = (tok[0] <= '9') ? (tok[0] - '0') : (tolower((unsigned char)tok[0]) - 'a' + 10);
    lo = (tok[1] <= '9') ? (tok[1] - '0') : (tolower((unsigned char)tok[1]) - 'a' + 10);
    *byte_out = (uint8_t)((hi << 4) | lo);
    return true;
}

// Split a "<start>..<end>" token; returns true and fills start/end (half-open → inclusive end).
static bool parse_range_dotdot(const char *tok, uint32_t *start_out, uint32_t *end_incl_out, const char **err_out) {
    const char *dots = strstr(tok, "..");
    if (!dots)
        return false;
    char start_buf[64];
    size_t n = (size_t)(dots - tok);
    if (n == 0 || n >= sizeof(start_buf)) {
        *err_out = "malformed range";
        return false;
    }
    memcpy(start_buf, tok, n);
    start_buf[n] = '\0';
    addr_space_t s0, s1;
    uint32_t start, end;
    if (!parse_address(start_buf, &start, &s0) || !parse_address(dots + 2, &end, &s1)) {
        *err_out = "invalid range endpoint";
        return false;
    }
    if (end <= start) {
        *err_out = "range end must be greater than start";
        return false;
    }
    *start_out = start;
    *end_incl_out = end - 1;
    return true;
}

// Parse trailing positional tokens as an optional range and/or trailing "all" token.
// On return, *start/*end_incl are set (default: full address space), *limit is the hit cap.
// Returns false on parse error (msg via *err_out); true otherwise.
// tokens: raw_argv slice starting after the pattern args.
static bool parse_tail(int n_tokens, char **tokens, uint32_t *start_out, uint32_t *end_incl_out, int *limit_out,
                       const char **err_out) {
    *start_out = 0;
    *end_incl_out = g_address_mask;
    *limit_out = FIND_DEFAULT_LIMIT;

    int i = 0;
    if (i < n_tokens && strcasecmp(tokens[i], "all") != 0) {
        // Either "<start>..<end>" or "<start>" (possibly followed by "<count>")
        uint32_t start, end_incl;
        if (parse_range_dotdot(tokens[i], &start, &end_incl, err_out)) {
            *start_out = start;
            *end_incl_out = end_incl;
            i++;
        } else if (*err_out) {
            return false;
        } else {
            // Try "<start> <count>" pair
            addr_space_t sp;
            if (!parse_address(tokens[i], &start, &sp)) {
                *err_out = "invalid range start";
                return false;
            }
            if (i + 1 >= n_tokens || strcasecmp(tokens[i + 1], "all") == 0) {
                *err_out = "expected '<start>..<end>' or '<start> <count>'";
                return false;
            }
            char *endp;
            unsigned long count = strtoul(tokens[i + 1], &endp, 0);
            if (*endp != '\0' || count == 0) {
                *err_out = "invalid count";
                return false;
            }
            *start_out = start;
            *end_incl_out = start + (uint32_t)(count - 1);
            i += 2;
        }
    }

    // Trailing "all" removes the hit cap
    if (i < n_tokens) {
        if (strcasecmp(tokens[i], "all") == 0) {
            *limit_out = 0; // 0 = unlimited
            i++;
        }
    }
    if (i != n_tokens) {
        *err_out = "unexpected extra arguments";
        return false;
    }
    return true;
}

// Linear scan of [start..end_incl] for `pattern` of length plen; prints matches to ctx->out.
// Returns the number of hits found (capped at limit if > 0).
static uint32_t scan_memory(struct cmd_context *ctx, uint32_t start, uint32_t end_incl, const uint8_t *pattern,
                            size_t plen, int limit, const char *label, uint32_t *hits_shown_out) {
    uint32_t hits = 0;
    uint32_t shown = 0;

    if (plen == 0)
        return 0;
    // Avoid wraparound: clamp so end_incl - (plen-1) doesn't underflow
    uint64_t last = (uint64_t)end_incl;
    if (last + 1 < plen)
        return 0;
    uint64_t stop = last - (plen - 1);

    // Byte-by-byte rolling compare; good enough for PR1 (memory reads are fast path on RAM).
    for (uint64_t a = start; a <= stop; a++) {
        if (memory_read_uint8((uint32_t)a) != pattern[0])
            continue;
        bool match = true;
        for (size_t k = 1; k < plen; k++) {
            if (memory_read_uint8((uint32_t)(a + k)) != pattern[k]) {
                match = false;
                break;
            }
        }
        if (!match)
            continue;
        hits++;
        if (limit == 0 || shown < (uint32_t)limit) {
            cmd_printf(ctx, "$%08X  %s\n", (uint32_t)a, label);
            shown++;
        }
    }
    if (hits_shown_out)
        *hits_shown_out = shown;
    return hits;
}

// Build a short label string for printing alongside each match.
// For `str`: the printable/escaped text, truncated if long.
// For `bytes`: "hex hex ..." of the first few bytes.
static void format_label(char *buf, size_t bufsz, const char *subcmd, const uint8_t *pat, size_t plen) {
    if (strcmp(subcmd, "str") == 0) {
        size_t o = 0;
        buf[o++] = '"';
        for (size_t i = 0; i < plen && o < bufsz - 2; i++) {
            uint8_t b = pat[i];
            if (b >= 0x20 && b <= 0x7e && b != '"' && b != '\\') {
                buf[o++] = (char)b;
            } else {
                if (o + 4 >= bufsz)
                    break;
                o += (size_t)snprintf(buf + o, bufsz - o, "\\x%02X", b);
            }
        }
        if (o < bufsz - 1)
            buf[o++] = '"';
        buf[o] = '\0';
    } else if (strcmp(subcmd, "word") == 0 && plen == 2) {
        // Print the matched value as a single $XXXX literal (reconstruct from big-endian bytes).
        uint16_t v = (uint16_t)(((uint16_t)pat[0] << 8) | pat[1]);
        snprintf(buf, bufsz, "$%04X", v);
    } else if (strcmp(subcmd, "long") == 0 && plen == 4) {
        // Print the matched value as a single $XXXXXXXX literal (reconstruct from big-endian bytes).
        uint32_t v = ((uint32_t)pat[0] << 24) | ((uint32_t)pat[1] << 16) | ((uint32_t)pat[2] << 8) | pat[3];
        snprintf(buf, bufsz, "$%08X", v);
    } else {
        size_t o = 0;
        size_t show = plen < 8 ? plen : 8;
        for (size_t i = 0; i < show; i++) {
            o += (size_t)snprintf(buf + o, bufsz - o, "%s%02X", i == 0 ? "" : " ", pat[i]);
        }
        if (plen > show && o + 4 < bufsz)
            snprintf(buf + o, bufsz - o, " ...");
    }
}

// "find" command handler; dispatches on subcmd name.
void cmd_find_handler(struct cmd_context *ctx, struct cmd_result *res) {
    const char *subcmd = ctx->subcmd;
    if (subcmd == NULL) {
        cmd_err(res, "usage: find {str|bytes|word|long} <pattern> [range] [all]");
        return;
    }

    uint8_t pattern[FIND_MAX_PATTERN_LEN];
    size_t plen = 0;
    int tail_idx = 0; // first raw_argv index of range/all tokens

    if (strcmp(subcmd, "str") == 0) {
        // raw_argv[0]=find, [1]=str, [2]=text, [3..]=optional range/all
        if (ctx->raw_argc < 3) {
            cmd_err(res, "usage: find str <text> [range] [all]");
            return;
        }
        const char *text = ctx->raw_argv[2];
        size_t n = strlen(text);
        if (n == 0) {
            cmd_err(res, "find str: empty pattern");
            return;
        }
        if (n > FIND_MAX_PATTERN_LEN) {
            cmd_err(res, "find str: pattern too long (max %d)", FIND_MAX_PATTERN_LEN);
            return;
        }
        memcpy(pattern, text, n);
        plen = n;
        tail_idx = 3;
    } else if (strcmp(subcmd, "bytes") == 0) {
        // Consume hex tokens until a non-hex token appears (which starts the range/all tail).
        if (ctx->raw_argc < 3) {
            cmd_err(res, "usage: find bytes <hex...> [range] [all]");
            return;
        }
        int i = 2;
        for (; i < ctx->raw_argc; i++) {
            uint8_t b;
            if (!parse_hex_byte(ctx->raw_argv[i], &b))
                break;
            if (plen >= FIND_MAX_PATTERN_LEN) {
                cmd_err(res, "find bytes: pattern too long (max %d)", FIND_MAX_PATTERN_LEN);
                return;
            }
            pattern[plen++] = b;
        }
        if (plen == 0) {
            cmd_err(res, "find bytes: expected 2-digit hex tokens (e.g. '4E B9')");
            return;
        }
        tail_idx = i;
    } else if (strcmp(subcmd, "word") == 0 || strcmp(subcmd, "long") == 0) {
        // Numeric value → big-endian byte pattern (68K memory is big-endian).
        if (ctx->raw_argc < 3) {
            cmd_err(res, "usage: find %s <value> [range] [all]", subcmd);
            return;
        }
        // Reuse parse_address for $/0x/bare-hex parsing; the space qualifier is ignored here.
        uint32_t value;
        addr_space_t sp;
        if (!parse_address(ctx->raw_argv[2], &value, &sp)) {
            cmd_err(res, "find %s: invalid value '%s'", subcmd, ctx->raw_argv[2]);
            return;
        }
        if (strcmp(subcmd, "word") == 0) {
            // Reject values that don't fit in 16 bits so a typo can't silently truncate.
            if (value > 0xFFFFu) {
                cmd_err(res, "find word: value $%X exceeds 16 bits", value);
                return;
            }
            pattern[0] = (uint8_t)(value >> 8);
            pattern[1] = (uint8_t)value;
            plen = 2;
        } else {
            pattern[0] = (uint8_t)(value >> 24);
            pattern[1] = (uint8_t)(value >> 16);
            pattern[2] = (uint8_t)(value >> 8);
            pattern[3] = (uint8_t)value;
            plen = 4;
        }
        tail_idx = 3;
    } else {
        cmd_err(res, "find: unknown sub-command '%s' (expected str, bytes, word, or long)", subcmd);
        return;
    }

    // Parse optional range + trailing "all"
    uint32_t start = 0, end_incl = g_address_mask;
    int limit = FIND_DEFAULT_LIMIT;
    const char *err = NULL;
    int n_tail = ctx->raw_argc - tail_idx;
    if (!parse_tail(n_tail, &ctx->raw_argv[tail_idx], &start, &end_incl, &limit, &err)) {
        cmd_err(res, "find %s: %s", subcmd, err ? err : "invalid arguments");
        return;
    }

    // Format match label (what pattern was found)
    char label[64];
    format_label(label, sizeof(label), subcmd, pattern, plen);

    uint32_t shown = 0;
    uint32_t total = scan_memory(ctx, start, end_incl, pattern, plen, limit, label, &shown);

    if (total == 0) {
        cmd_printf(ctx, "no matches in $%08X..$%08X\n", start, end_incl);
    } else if (limit > 0 && total > shown) {
        cmd_printf(ctx, "... (%u more, use 'find %s ... all')\n", (unsigned)(total - shown), subcmd);
    }
    // Exit code 0 for success (with or without matches) so scripted drivers
    // don't treat "found 17 hits" as a shell error.  The hit count is in the
    // printed output; the result stays OK.
    cmd_ok(res);
}
