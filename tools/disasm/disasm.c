// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// disasm.c
// Standalone 68000/68030 disassembler tool for binary files.
// Uses the existing cpu_disasm.c decoder with minimal dependencies.

#include "cpu.h"

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Check if a mnemonic is a branch instruction that uses format_pc_displacement.
// Returns true for Bcc, BRA, BSR (and .S/.W/.L variants), DBcc, FBcc, PBcc.
static bool is_branch_mnemonic(const char *mnem) {
    // strip trailing .S / .W / .L for comparison
    char base[16];
    int i;
    for (i = 0; i < 15 && mnem[i] && mnem[i] != '.'; i++)
        base[i] = mnem[i];
    base[i] = '\0';

    // DBcc: DB followed by condition code
    if (base[0] == 'D' && base[1] == 'B')
        return true;

    // FBcc: FB followed by condition
    if (base[0] == 'F' && base[1] == 'B')
        return true;

    // PBcc: PB followed by condition
    if (base[0] == 'P' && base[1] == 'B')
        return true;

    // BRA, BSR
    if (strcmp(base, "BRA") == 0 || strcmp(base, "BSR") == 0)
        return true;

    // Bcc: B followed by a 2-letter condition code
    static const char *conds[] = {"HI", "LS", "CC", "CS", "NE", "EQ", "VC", "VS",
                                  "PL", "MI", "GE", "LT", "GT", "LE", NULL};
    if (base[0] == 'B') {
        for (const char **c = conds; *c; c++) {
            if (strcmp(base + 1, *c) == 0)
                return true;
        }
    }

    return false;
}

// Annotate operands with the absolute branch target address.
// mnemonic: the instruction mnemonic (e.g. "BNE.S", "DBF", "JMP")
// operands_text: the operand string (e.g. "*-$0006", "D0,*+$1234")
// instr_addr: the virtual address of the instruction
// Writes annotated operands into buf.
static void annotate_branch_destination(char *buf, size_t buf_size, const char *mnemonic, const char *operands_text,
                                        uint32_t instr_addr) {
    // copy raw operands into output buffer first
    snprintf(buf, buf_size, "%s", operands_text);

    // find the asterisk indicating a PC-relative operand
    const char *star = strchr(operands_text, '*');
    if (!star)
        return;

    // only annotate actual branch mnemonics (not BTST, BCHG, etc.)
    if (!is_branch_mnemonic(mnemonic))
        return;

    // parse sign and hex value after the asterisk
    char sign;
    unsigned int hex_val;
    if (sscanf(star, "*%c$%x", &sign, &hex_val) != 2)
        return;
    if (sign != '+' && sign != '-')
        return;

    // the displacement text encodes (displacement + 2) relative to
    // the instruction address.
    uint32_t dest;
    if (sign == '+')
        dest = instr_addr + hex_val;
    else
        dest = instr_addr - hex_val;

    // special case: DBcc always prints *+$HHHH with unsigned value =
    // (uint16_t)raw_disp16 + 2.  The real displacement is sign-extended.
    if (mnemonic[0] == 'D' && mnemonic[1] == 'B') {
        uint16_t raw_disp = (uint16_t)(hex_val - 2);
        int16_t signed_disp = (int16_t)raw_disp;
        dest = instr_addr + 2 + (int32_t)signed_disp;
    }

    // append annotation
    size_t cur_len = strlen(buf);
    snprintf(buf + cur_len, buf_size - cur_len, "\t; -> $%08X", dest);
}

// Print usage information
static void print_usage(const char *progname) {
    fprintf(stderr,
            "Usage: %s [OPTIONS] <input_file>\n"
            "\n"
            "Disassemble 68000/68030 binary code from a file.\n"
            "\n"
            "Options:\n"
            "  -o, --offset <bytes>          Starting byte offset in the file (decimal or 0xHEX). Default: 0\n"
            "  -l, --length <bytes>          Number of bytes to disassemble. Default: entire file from offset\n"
            "  -a, --address-offset <addr>   Base address for display (hex). Default: 0\n"
            "  -n, --count <n>               Maximum number of instructions to disassemble\n"
            "  -h, --help                    Show this help message\n",
            progname);
}

int main(int argc, char *argv[]) {
    // default option values
    uint32_t offset = 0;
    uint32_t length = 0;
    bool length_set = false;
    uint32_t address_offset = 0;
    uint32_t max_instructions = 0; // 0 = unlimited
    const char *input_file = NULL;

    // long options table
    static struct option long_options[] = {
        {"offset",         required_argument, NULL, 'o'},
        {"length",         required_argument, NULL, 'l'},
        {"address-offset", required_argument, NULL, 'a'},
        {"count",          required_argument, NULL, 'n'},
        {"help",           no_argument,       NULL, 'h'},
        {NULL,             0,                 NULL, 0  },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "o:l:a:n:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'o':
            offset = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'l':
            length = (uint32_t)strtoul(optarg, NULL, 0);
            length_set = true;
            break;
        case 'a':
            address_offset = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'n':
            max_instructions = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    // positional argument: input file
    if (optind >= argc) {
        fprintf(stderr, "Error: no input file specified.\n");
        print_usage(argv[0]);
        return 1;
    }
    input_file = argv[optind];

    // open file
    FILE *fp = fopen(input_file, "rb");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", input_file, strerror(errno));
        return 1;
    }

    // determine file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    if (file_size < 0) {
        fprintf(stderr, "Error: cannot determine file size.\n");
        fclose(fp);
        return 1;
    }

    // validate offset
    if (offset >= (uint32_t)file_size) {
        fprintf(stderr, "Error: offset 0x%X exceeds file size (%ld bytes).\n", offset, file_size);
        fclose(fp);
        return 1;
    }

    // determine length to read
    uint32_t available = (uint32_t)file_size - offset;
    if (!length_set || length > available)
        length = available;

    // ensure even length (68K instructions are word-aligned)
    length &= ~1u;
    if (length == 0) {
        fprintf(stderr, "Error: no data to disassemble.\n");
        fclose(fp);
        return 1;
    }

    // allocate buffer and read data
    uint8_t *raw_buf = malloc(length + 16); // extra padding for disassembler lookahead
    if (!raw_buf) {
        fprintf(stderr, "Error: out of memory.\n");
        fclose(fp);
        return 1;
    }
    memset(raw_buf, 0, length + 16);

    fseek(fp, offset, SEEK_SET);
    size_t nread = fread(raw_buf, 1, length, fp);
    fclose(fp);

    if (nread < length)
        length = (uint32_t)(nread & ~1u);

    // convert from big-endian file bytes to host uint16_t array
    // cpu_disasm expects an array of uint16_t in host byte order
    uint32_t word_count = length / 2;
    uint16_t *words = (uint16_t *)malloc((word_count + 8) * sizeof(uint16_t));
    if (!words) {
        fprintf(stderr, "Error: out of memory.\n");
        free(raw_buf);
        return 1;
    }
    for (uint32_t i = 0; i < word_count; i++) {
        words[i] = (uint16_t)(raw_buf[i * 2] << 8 | raw_buf[i * 2 + 1]);
    }
    // zero-fill padding words
    for (uint32_t i = word_count; i < word_count + 8; i++)
        words[i] = 0;

    free(raw_buf);

    // disassembly loop
    char disasm_buf[256];
    char annotated_buf[512];
    char mnemonic[64], operands[128];
    uint32_t pos = 0; // position in words
    uint32_t instr_count = 0;

    while (pos < word_count) {
        // current virtual address: base + file offset + position within buffer
        uint32_t addr = address_offset + offset + pos * 2;

        // disassemble one instruction
        int nwords = cpu_disasm(&words[pos], disasm_buf);
        if (nwords < 1)
            nwords = 1; // safety: advance at least one word

        // clamp to remaining data
        if (pos + nwords > word_count)
            nwords = word_count - pos;

        // split mnemonic and operands at the tab character,
        // matching the emulator's debug.c disasm() function
        int i;
        if (strlen(disasm_buf) == 0) {
            strcpy(mnemonic, "ILLEGAL");
            operands[0] = '\0';
        } else {
            for (i = 0; disasm_buf[i] != '\0' && disasm_buf[i] != '\t'; i++)
                mnemonic[i] = disasm_buf[i];
            mnemonic[i] = '\0';
            if (disasm_buf[i] == '\t')
                snprintf(operands, sizeof(operands), "%s", disasm_buf + i + 1);
            else
                operands[0] = '\0';
        }

        // annotate branch destinations on the operands string
        annotate_branch_destination(annotated_buf, sizeof(annotated_buf), mnemonic, operands, addr);

        // format exactly like the emulator: "%08x  %04x  %-10s%-12s"
        // then append the branch annotation (if any) after the base output
        char base_line[256];
        sprintf(base_line, "%08x  %04x  %-10s%s", (int)addr, (int)words[pos], mnemonic, annotated_buf);

        printf("%s\n", base_line);

        pos += nwords;
        instr_count++;

        if (max_instructions > 0 && instr_count >= max_instructions)
            break;
    }

    free(words);
    return 0;
}
