// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// annotate_disasm.c
// Shared disassembly + annotation pipeline.  Replaces the duplicated
// branch annotator that used to live inside tools/disasm/disasm.c; the
// `re` orchestrator and the standalone tool both link against this now.

#include "annotate_disasm.h"

#include "cpu.h"
#include "debug_mac.h"

#include <stdio.h>
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

size_t re_annotate_disasm_write(FILE *out, const uint8_t *bytes, size_t bytes_len, uint32_t base_addr, uint32_t flags) {
    if (!out || !bytes)
        return 0;
    size_t word_count = 0;
    uint16_t *words = bytes_to_words(bytes, bytes_len, &word_count);
    if (!words)
        return 0;

    char disasm_buf[256];
    char annotated_buf[512];
    char mnemonic[64];
    char operands[256];
    size_t pos = 0; // index in words
    size_t emitted = 0;

    while (pos < word_count) {
        uint32_t addr = base_addr + (uint32_t)(pos * 2);

        int nwords = cpu_disasm(&words[pos], disasm_buf);
        if (nwords < 1)
            nwords = 1; // safety advance
        if (pos + (size_t)nwords > word_count)
            nwords = (int)(word_count - pos);

        // Split mnemonic and operands at the tab character produced by
        // cpu_disasm (same convention as debug.c and the standalone tool).
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

        // Trap-name annotation: a single 16-bit opcode in the Axxx range
        // is a Mac trap.  cpu_disasm already emits the trap name as the
        // operand, but for `DC.W $Axxx` style decode the comment helps.
        char trap_note[64] = {0};
        if ((flags & RE_DISASM_ANNOTATE_TRAPS) && (words[pos] & 0xF000u) == 0xA000u) {
            const char *name = macos_atrap_name(words[pos]);
            if (name && name[0])
                snprintf(trap_note, sizeof(trap_note), "\t; trap %s", name);
        }

        fprintf(out, "$%08X  %04x  %-10s%s%s\n", (unsigned int)addr, (int)words[pos], mnemonic, annotated_buf,
                trap_note);

        pos += (size_t)nwords;
        emitted++;
    }

    free(words);
    return emitted;
}
