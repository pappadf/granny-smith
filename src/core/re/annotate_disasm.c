// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// annotate_disasm.c
// Shared disassembly + annotation pipeline.  Two passes per segment:
//
//   Pass 1 — populate `ctx->symbols` with:
//     * MacsBug name trailers — high-bit-set length byte after RTS/RTD/
//       JMP (A0), followed by `len & 0x7F` ASCII bytes; the name labels
//       the start of the *next* function.
//     * Jump-table entry points — every CODE 0 entry whose target
//       matches `ctx->code_id` contributes a JT_<id>_<index> label at
//       the entry's offset.
//   Pass 2 — stream the listing line-by-line with:
//     * Branch-destination annotation on PC-relative operands.
//     * Trap-name annotation on $Axxx opcodes.
//     * Low-memory global names on absolute operands < 0x800.
//     * JT cross-refs on `(d16, A5)` operands intersecting the JT range.
//     * Label emission at any address resolved by `ctx->symbols`.
//
// Tools that don't carry a context (e.g. tools/disasm/disasm) pass NULL
// and get the original behaviour: branch annotation + trap names only.

#include "annotate_disasm.h"

#include "aux_syscalls.h"
#include "code_segment.h"
#include "cpu.h"
#include "debug_mac.h"
#include "symbols.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void re_annotate_branch_destination(char *buf, size_t buf_size, const char *mnemonic, const char *operands_text,
                                    uint32_t instr_addr) {
    (void)mnemonic; // annotate all PC-relative operands, regardless of mnemonic
    snprintf(buf, buf_size, "%s", operands_text);
    const char *star = strchr(operands_text, '*');
    if (!star)
        return;
    char sign;
    unsigned int hex_val;
    if (sscanf(star, "*%c$%x", &sign, &hex_val) != 2)
        return;
    if (sign != '+' && sign != '-')
        return;
    uint32_t dest = (sign == '+') ? (instr_addr + hex_val) : (instr_addr - hex_val);
    size_t cur_len = strlen(buf);
    snprintf(buf + cur_len, buf_size - cur_len, "\t; -> $%08X", dest);
}

// Convert a big-endian byte buffer into a host-order uint16 array padded
// with 8 zero words for disassembler lookahead.  Returns malloc'd storage
// the caller frees via free().  *out_words receives the count.
static uint16_t *bytes_to_words(const uint8_t *bytes, size_t bytes_len, size_t *out_words) {
    size_t n = bytes_len / 2;
    uint16_t *w = (uint16_t *)calloc(n + 8, sizeof(uint16_t));
    if (!w) {
        *out_words = 0;
        return NULL;
    }
    for (size_t i = 0; i < n; i++)
        w[i] = (uint16_t)((bytes[i * 2] << 8) | bytes[i * 2 + 1]);
    *out_words = n;
    return w;
}

// === Pass 1 — MacsBug name trailers + JT entries ============================
//
// MacsBug procedure names are encoded into the code stream as a high-bit
// length byte (0x81..0xBF) at an even offset after a function epilogue,
// followed by `len & 0x7F` ASCII bytes, padded to even total length.
// The name labels the *function that just returned*, so the label
// applies to whichever earlier address we tracked as the previous
// function boundary.  When no prior boundary exists (start of segment)
// the name labels offset 0 / `start_offset`.

static bool is_function_epilogue(uint16_t op) {
    return op == 0x4E75u || op == 0x4E74u || op == 0x4ED0u; // RTS / RTD / JMP (A0)
}

// True if `byte` is a plausible MacsBug name character.  Names are
// printable ASCII; many compilers also pad with NULs or use _ . $ + as
// continuation chars in mangled names.
static bool macsbug_char(uint8_t b) {
    return b >= 0x20 && b < 0x7F;
}

// Try to read a MacsBug name starting at byte offset `at` in `bytes`.
// Returns the number of bytes consumed (length byte + name + pad) on
// success, or 0 on miss.  Writes the unpadded name into `out` (cap is
// the buffer size).  Names are at most 63 bytes per the encoding.
static size_t try_read_macsbug_name(const uint8_t *bytes, size_t bytes_len, size_t at, char *out, size_t cap) {
    if (at >= bytes_len)
        return 0;
    uint8_t lb = bytes[at];
    if ((lb & 0x80) == 0)
        return 0;
    uint8_t len = lb & 0x7F;
    if (len < 1 || len > 63)
        return 0;
    if (at + 1 + len > bytes_len)
        return 0;
    // Every byte must be printable ASCII or NUL-padding at the end.
    for (size_t i = 0; i < len; i++) {
        if (!macsbug_char(bytes[at + 1 + i]))
            return 0;
    }
    size_t copy = len < cap - 1 ? len : cap - 1;
    memcpy(out, bytes + at + 1, copy);
    out[copy] = '\0';
    // Total bytes (length+payload) padded to even length.
    size_t total = 1 + len;
    if (total & 1)
        total++;
    return total;
}

// Pass 1: walk the segment and record MacsBug names + JT-entry labels.
// We re-decode with cpu_disasm so we can advance by the same instruction
// count as the writer and find the post-RTS gap.  `next_entry_addr`
// tracks the start of the next function (the address at which the name
// applies).
static void pass1_collect_symbols(const uint8_t *bytes, size_t bytes_len, uint32_t base_addr,
                                  const re_annotate_ctx_t *ctx) {
    if (!ctx || !ctx->symbols)
        return;
    // Pre-seed labels from CODE 0's jump table for entries targeting
    // this segment.  These survive even when no MacsBug name follows.
    if (ctx->jt) {
        for (size_t i = 0; i < ctx->jt->n_entries; i++) {
            const re_jt_entry_t *e = &ctx->jt->entries[i];
            if (e->segment == (uint16_t)ctx->code_id) {
                char name[64];
                snprintf(name, sizeof(name), "JT_%04d_%04zu", (int)ctx->code_id, i);
                re_symbols_add(ctx->symbols, ctx->code_id, base_addr + e->offset, name, RE_SYMSRC_JUMPTABLE);
            }
        }
    }

    size_t word_count = 0;
    uint16_t *words = bytes_to_words(bytes, bytes_len, &word_count);
    if (!words)
        return;
    char dis[256];
    size_t pos = 0;
    uint32_t func_start = base_addr;
    bool just_returned = false;
    while (pos < word_count) {
        int nw = cpu_disasm(&words[pos], dis);
        if (nw < 1)
            nw = 1;
        if (pos + (size_t)nw > word_count)
            nw = (int)(word_count - pos);
        uint32_t addr = base_addr + (uint32_t)(pos * 2);
        if (is_function_epilogue(words[pos])) {
            just_returned = true;
            // After the RTS/RTD/JMP(A0), the next bytes might be a
            // MacsBug name.  Look at the byte position immediately after
            // this instruction.
            size_t name_byte_off = (pos + (size_t)nw) * 2;
            char name[64];
            size_t consumed = try_read_macsbug_name(bytes, bytes_len, name_byte_off, name, sizeof(name));
            if (consumed > 0) {
                // Apply the name to the function we just returned from.
                re_symbols_add(ctx->symbols, ctx->code_id, func_start, name, RE_SYMSRC_MACSBUG);
                // Advance pos past the name so we don't try to
                // disassemble it as code.
                pos += (size_t)nw + (consumed / 2);
                func_start = base_addr + (uint32_t)(pos * 2);
                continue;
            }
        } else if (just_returned) {
            // First instruction after a return without a MacsBug name
            // starts the next function.
            func_start = addr;
            just_returned = false;
            char name[32];
            snprintf(name, sizeof(name), "sub_%04X", addr);
            re_symbols_add(ctx->symbols, ctx->code_id, func_start, name, RE_SYMSRC_BOUNDARY);
        }
        pos += (size_t)nw;
    }
    free(words);
}

// === Pass 2 — line-by-line stream with annotations =========================

// Look for an A5-relative operand of the form "(d16, A5)" or "-N(A5)" /
// "N(A5)" and, if d16 falls inside the jump-table range, append a JT[i]
// = CODE s:offset annotation to `buf`.
static void annotate_jt_xref(char *buf, size_t cap, const char *operands, const re_jt_table_t *jt) {
    if (!jt || jt->n_entries == 0)
        return;
    // Most matches come from operands that contain "(A5)" and a hex
    // displacement; scan for "(A5)" and back-walk to find the
    // displacement integer.
    const char *a5 = strstr(operands, "(A5)");
    if (!a5)
        return;
    // Back-walk to the displacement start.  Accepts forms like
    // "$952(A5)" or "-$1234(A5)" or "$ABC(A5)".  cpu_disasm formats
    // negatives with a literal '-' sign before the $.
    const char *p = a5;
    while (p > operands && p[-1] != ',' && p[-1] != ' ' && p[-1] != '(' && p[-1] != '\t')
        p--;
    int sign = 1;
    if (*p == '-') {
        sign = -1;
        p++;
    }
    if (*p != '$')
        return;
    p++;
    unsigned int v = 0;
    int n = sscanf(p, "%x", &v);
    if (n != 1)
        return;
    int32_t d16 = sign * (int32_t)v;
    if (d16 < (int32_t)jt->jt_offset)
        return;
    uint32_t off_within_jt = (uint32_t)(d16 - (int32_t)jt->jt_offset);
    if (off_within_jt >= jt->jt_size)
        return;
    uint32_t idx = off_within_jt / 8;
    if (idx >= jt->n_entries)
        return;
    const re_jt_entry_t *e = &jt->entries[idx];
    size_t cur = strlen(buf);
    snprintf(buf + cur, cap - cur, "\t; xref JT[%u] = CODE %u:0x%04X", idx, e->segment, e->offset);
}

// Look for a likely absolute-address operand and annotate it with the
// matching low-memory global name when one exists.  Three filters cut
// the false-positive rate from cpu_disasm's `$NNNN` rendering:
//
//   - Skip tokens preceded by `#` — those are immediates, not addresses.
//   - Skip tokens preceded by `*+` or `*-` — those are PC-relative
//     displacements (the resolved target lives in the comment, not the
//     operand string we're scanning).
//   - Skip tokens followed by `(` — those are register-indexed
//     displacements like `$952(A5)`; the A5 xref pass handles those.
//
// The remaining hits are bare absolute operands, which is exactly the
// shape `MOVE.W $172,D0` produces (low-memory global access).
static void annotate_global(char *buf, size_t cap, const char *operands) {
    const char *p = operands;
    int matched = 0;
    while ((p = strchr(p, '$')) && matched < 4) {
        // Reject the three displacement forms via single-char lookbehind.
        bool reject = false;
        if (p > operands) {
            char prev = p[-1];
            if (prev == '#' || prev == '+' || prev == '-')
                reject = true;
        }
        if (reject) {
            p++;
            continue;
        }
        unsigned int v = 0;
        int chars_read = 0;
        int n = sscanf(p, "$%x%n", &v, &chars_read);
        if (n != 1 || chars_read <= 1) {
            p++;
            continue;
        }
        // Register-indexed displacement (e.g. "$952(A5)") — skip; the JT
        // xref pass owns this shape.
        if (p[chars_read] == '(') {
            p += chars_read;
            continue;
        }
        if (v < 0x800) {
            const char *name = debug_mac_lookup_global_name(v);
            if (name && name[0]) {
                size_t cur = strlen(buf);
                snprintf(buf + cur, cap - cur, "\t; global %s", name);
                matched++;
                break;
            }
        }
        p += chars_read;
    }
}

size_t re_annotate_disasm_write(FILE *out, const uint8_t *bytes, size_t bytes_len, uint32_t base_addr, uint32_t flags,
                                re_annotate_ctx_t *ctx) {
    if (!out || !bytes)
        return 0;

    // Pass 1: build the symbol table (idempotent — re_symbols_add dedups).
    if (ctx && (flags & RE_DISASM_ANNOTATE_MACSBUG))
        pass1_collect_symbols(bytes, bytes_len, base_addr, ctx);

    size_t word_count = 0;
    uint16_t *words = bytes_to_words(bytes, bytes_len, &word_count);
    if (!words)
        return 0;

    char disasm_buf[256];
    char annotated_buf[512];
    char mnemonic[64];
    char operands[256];
    size_t pos = 0;
    size_t emitted = 0;

    while (pos < word_count) {
        uint32_t addr = base_addr + (uint32_t)(pos * 2);

        // Emit a "label:" line before the instruction at this address
        // when a symbol resolves there.  Done before the bytes line so
        // hand-reading the listing has the call-target identifier first.
        if (ctx && ctx->symbols && (flags & RE_DISASM_ANNOTATE_LABELS)) {
            const re_symbol_t *s = re_symbols_find(ctx->symbols, ctx->code_id, addr);
            if (s)
                fprintf(out, "\n%s:\n", s->name);
        }

        int nwords = cpu_disasm(&words[pos], disasm_buf);
        if (nwords < 1)
            nwords = 1;
        if (pos + (size_t)nwords > word_count)
            nwords = (int)(word_count - pos);

        if (disasm_buf[0] == '\0') {
            snprintf(mnemonic, sizeof(mnemonic), "%s", "ILLEGAL");
            operands[0] = '\0';
        } else {
            size_t i = 0;
            while (disasm_buf[i] && disasm_buf[i] != '\t' && i + 1 < sizeof(mnemonic)) {
                mnemonic[i] = disasm_buf[i];
                i++;
            }
            mnemonic[i] = '\0';
            if (disasm_buf[i] == '\t')
                snprintf(operands, sizeof(operands), "%s", disasm_buf + i + 1);
            else
                operands[0] = '\0';
        }

        if (flags & RE_DISASM_ANNOTATE_BRANCH)
            re_annotate_branch_destination(annotated_buf, sizeof(annotated_buf), mnemonic, operands, addr);
        else
            snprintf(annotated_buf, sizeof(annotated_buf), "%s", operands);

        if (flags & RE_DISASM_ANNOTATE_GLOBALS)
            annotate_global(annotated_buf, sizeof(annotated_buf), operands);
        if ((flags & RE_DISASM_ANNOTATE_JTXREF) && ctx && ctx->jt)
            annotate_jt_xref(annotated_buf, sizeof(annotated_buf), operands, ctx->jt);

        char trap_note[64] = {0};
        if ((flags & RE_DISASM_ANNOTATE_TRAPS) && (words[pos] & 0xF000u) == 0xA000u) {
            const char *name = macos_atrap_name(words[pos]);
            if (name && name[0])
                snprintf(trap_note, sizeof(trap_note), "\t; trap %s", name);
        }

        // A/UX syscall annotation.  TRAP #0 (opcode 0x4E40) is the SVR2
        // syscall entry; the number lives in D0.  We look back at the
        // single preceding word for `MOVEQ #N,D0` (0x70NN, the most
        // common idiom — covers syscall numbers 0..127, which is the
        // entire SVR2 range we have names for).  `MOVE.W #N,D0` (0x303C
        // + word) and `MOVE.L #N,D0` (0x203C + 2 words) need deeper
        // lookback we don't track here; sites using those emit the
        // raw "TRAP #0" without annotation but the symbol-table-driven
        // labels at function entries usually give the syscall away
        // anyway.
        if ((flags & RE_DISASM_ANNOTATE_AUX_SYSC) && words[pos] == 0x4E40u && pos >= 1) {
            uint16_t prev = words[pos - 1];
            if ((prev & 0xFF00u) == 0x7000u) {
                uint8_t n = (uint8_t)(prev & 0xFFu);
                const char *name = aux_syscall_name(n);
                if (name)
                    snprintf(trap_note, sizeof(trap_note), "\t; syscall %s", name);
                else
                    snprintf(trap_note, sizeof(trap_note), "\t; syscall #%u", n);
            }
        }

        fprintf(out, "$%08X  %04x  %-10s%s%s\n", (unsigned int)addr, (int)words[pos], mnemonic, annotated_buf,
                trap_note);
        pos += (size_t)nwords;
        emitted++;
    }

    free(words);
    return emitted;
}
