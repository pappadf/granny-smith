// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_symbol.c
// Unified symbol resolver for $-prefixed tokens.
// Consolidates register resolution from addr_format.c and Mac global
// lookup from debug_mac.c into a single resolve_symbol() entry point.

#include "cmd_symbol.h"

#include "alias.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "fpu.h"
#include "memory.h"
#include "mmu.h"
#include "system.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

// Mac global variable info (defined in mac_globals_data.c)
extern struct {
    const char *name;
    uint32_t address;
    int size;
    const char *description;
} mac_global_vars[];
extern const size_t mac_global_vars_count;

// Try to resolve a name as a CPU register
static bool resolve_cpu_register(const char *name, struct resolved_symbol *out) {
    cpu_t *cpu = system_cpu();
    if (!cpu)
        return false;

    // Case-insensitive comparison
    char lower[16];
    size_t len = strlen(name);
    if (len == 0 || len > 15)
        return false;
    for (size_t i = 0; i <= len; i++)
        lower[i] = tolower((unsigned char)name[i]);

    out->kind = SYM_REGISTER;
    out->size = 4; // most registers are 32-bit

    // PC
    if (strcmp(lower, "pc") == 0) {
        out->name = "PC";
        out->value = cpu_get_pc(cpu);
        out->address = 0;
        return true;
    }
    // SP (alias for A7)
    if (strcmp(lower, "sp") == 0) {
        out->name = "SP";
        out->value = cpu_get_an(cpu, 7);
        out->address = 0;
        return true;
    }
    // SSP
    if (strcmp(lower, "ssp") == 0) {
        out->name = "SSP";
        out->value = cpu_get_ssp(cpu);
        out->address = 0;
        return true;
    }
    // USP
    if (strcmp(lower, "usp") == 0) {
        out->name = "USP";
        out->value = cpu_get_usp(cpu);
        out->address = 0;
        return true;
    }
    // MSP (68030 only)
    if (strcmp(lower, "msp") == 0) {
        out->name = "MSP";
        out->value = cpu->msp;
        out->address = 0;
        return true;
    }

    // D0-D7
    if (lower[0] == 'd' && lower[1] >= '0' && lower[1] <= '7' && lower[2] == '\0') {
        int reg = lower[1] - '0';
        static const char *dnames[] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"};
        out->name = dnames[reg];
        out->value = cpu_get_dn(cpu, reg);
        out->address = 0;
        return true;
    }

    // A0-A7
    if (lower[0] == 'a' && lower[1] >= '0' && lower[1] <= '7' && lower[2] == '\0') {
        int reg = lower[1] - '0';
        static const char *anames[] = {"A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"};
        out->name = anames[reg];
        out->value = cpu_get_an(cpu, reg);
        out->address = 0;
        return true;
    }

    // SR (16-bit)
    if (strcmp(lower, "sr") == 0) {
        out->name = "SR";
        out->value = cpu_get_sr(cpu);
        out->address = 0;
        out->size = 2;
        return true;
    }
    // CCR (8-bit, lower byte of SR)
    if (strcmp(lower, "ccr") == 0) {
        out->name = "CCR";
        out->value = cpu_get_sr(cpu) & 0xFF;
        out->address = 0;
        out->size = 1;
        return true;
    }

    // Individual condition code flags (1-bit, returned as 0/1)
    uint16_t sr = cpu_get_sr(cpu);
    if (strcmp(lower, "c") == 0) {
        out->name = "C";
        out->value = (sr & 0x0001) ? 1 : 0;
        out->address = 0;
        out->size = 1;
        return true;
    }
    if (strcmp(lower, "v") == 0) {
        out->name = "V";
        out->value = (sr & 0x0002) ? 1 : 0;
        out->address = 0;
        out->size = 1;
        return true;
    }
    if (strcmp(lower, "z") == 0) {
        out->name = "Z";
        out->value = (sr & 0x0004) ? 1 : 0;
        out->address = 0;
        out->size = 1;
        return true;
    }
    if (strcmp(lower, "n") == 0) {
        out->name = "N";
        out->value = (sr & 0x0008) ? 1 : 0;
        out->address = 0;
        out->size = 1;
        return true;
    }
    if (strcmp(lower, "x") == 0) {
        out->name = "X";
        out->value = (sr & 0x0010) ? 1 : 0;
        out->address = 0;
        out->size = 1;
        return true;
    }

    // S, T, IM flags from SR
    if (strcmp(lower, "s") == 0 && len == 1) {
        out->name = "S";
        out->value = (sr >> 13) & 1;
        out->address = 0;
        out->size = 1;
        return true;
    }
    if (strcmp(lower, "t") == 0 && len == 1) {
        out->name = "T";
        out->value = (sr >> 15) & 1;
        out->address = 0;
        out->size = 1;
        return true;
    }

    return false;
}

// Try to resolve a name as an FPU register
static bool resolve_fpu_register(const char *name, struct resolved_symbol *out) {
    cpu_t *cpu = system_cpu();
    if (!cpu)
        return false;
    fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;
    if (!fpu)
        return false;

    char lower[16];
    size_t len = strlen(name);
    if (len == 0 || len > 15)
        return false;
    for (size_t i = 0; i <= len; i++)
        lower[i] = tolower((unsigned char)name[i]);

    out->kind = SYM_REGISTER;

    // FP0-FP7
    if (lower[0] == 'f' && lower[1] == 'p' && lower[2] >= '0' && lower[2] <= '7' && lower[3] == '\0') {
        int reg = lower[2] - '0';
        static const char *fpnames[] = {"FP0", "FP1", "FP2", "FP3", "FP4", "FP5", "FP6", "FP7"};
        out->name = fpnames[reg];
        // For FPU data regs, we store the raw exponent+mantissa combined (not a simple 32-bit value)
        // The value field is limited, so store 0 and let commands handle FP specially
        out->value = 0;
        out->address = 0;
        out->size = 10; // 80-bit extended
        return true;
    }

    // FPCR
    if (strcmp(lower, "fpcr") == 0) {
        out->name = "FPCR";
        out->value = fpu->fpcr;
        out->address = 0;
        out->size = 4;
        return true;
    }
    // FPSR
    if (strcmp(lower, "fpsr") == 0) {
        out->name = "FPSR";
        out->value = fpu->fpsr;
        out->address = 0;
        out->size = 4;
        return true;
    }
    // FPIAR
    if (strcmp(lower, "fpiar") == 0) {
        out->name = "FPIAR";
        out->value = fpu->fpiar;
        out->address = 0;
        out->size = 4;
        return true;
    }

    return false;
}

// Try to resolve a name as an MMU register
static bool resolve_mmu_register(const char *name, struct resolved_symbol *out) {
    if (!g_mmu)
        return false;

    char lower[16];
    size_t len = strlen(name);
    if (len == 0 || len > 15)
        return false;
    for (size_t i = 0; i <= len; i++)
        lower[i] = tolower((unsigned char)name[i]);

    out->kind = SYM_REGISTER;
    out->address = 0;
    out->size = 4;

    if (strcmp(lower, "tc") == 0) {
        out->name = "TC";
        out->value = g_mmu->tc;
        return true;
    }
    if (strcmp(lower, "tt0") == 0) {
        out->name = "TT0";
        out->value = g_mmu->tt0;
        return true;
    }
    if (strcmp(lower, "tt1") == 0) {
        out->name = "TT1";
        out->value = g_mmu->tt1;
        return true;
    }
    // CRP and SRP are 64-bit; store lower 32 bits in value
    if (strcmp(lower, "crp") == 0) {
        out->name = "CRP";
        out->value = (uint32_t)(g_mmu->crp & 0xFFFFFFFF);
        out->size = 8;
        return true;
    }
    if (strcmp(lower, "srp") == 0) {
        out->name = "SRP";
        out->value = (uint32_t)(g_mmu->srp & 0xFFFFFFFF);
        out->size = 8;
        return true;
    }

    return false;
}

// Try to resolve a name as a Mac low-memory global
static bool resolve_mac_global(const char *name, struct resolved_symbol *out) {
    for (size_t i = 0; i < mac_global_vars_count; i++) {
        if (strcasecmp(mac_global_vars[i].name, name) == 0) {
            out->name = mac_global_vars[i].name;
            out->address = mac_global_vars[i].address;
            out->size = mac_global_vars[i].size;
            out->kind = SYM_MAC_GLOBAL;
            // Read current value from memory
            if (system_memory()) {
                switch (out->size) {
                case 1:
                    out->value = memory_read_uint8(out->address);
                    break;
                case 2:
                    out->value = memory_read_uint16(out->address);
                    break;
                case 4:
                    out->value = memory_read_uint32(out->address);
                    break;
                default:
                    out->value = memory_read_uint32(out->address);
                    break;
                }
            } else {
                out->value = 0;
            }
            return true;
        }
    }
    return false;
}

// Resolve a symbol name (without `$` prefix). Consults the new alias
// table first (proposal-module-object-model.md §4.4) and dispatches to
// the appropriate legacy reader based on the alias's target path.
//
// On miss in the alias table, falls through to the legacy chain so
// MMU registers, SR flag bits (`c`/`v`/`z`/…), and case-insensitive
// matches keep working.
bool resolve_symbol(const char *name, struct resolved_symbol *out) {
    if (!name || !out)
        return false;

    memset(out, 0, sizeof(*out));

    // Tier 1: the canonical alias directory.
    const char *path = alias_lookup(name, NULL);
    if (path) {
        if (strncmp(path, "cpu.fpu.", 8) == 0) {
            if (resolve_fpu_register(path + 8, out))
                return true;
        } else if (strncmp(path, "cpu.", 4) == 0) {
            if (resolve_cpu_register(path + 4, out))
                return true;
        } else if (strncmp(path, "mac.", 4) == 0) {
            if (resolve_mac_global(path + 4, out))
                return true;
        }
        // Alias maps to a path no legacy reader knows how to handle —
        // fall through.
    }

    // Tier 2: legacy chain — covers MMU registers and the SR flag bits
    // that aren't aliased per §4.4.1, plus case-insensitive matches.
    if (resolve_cpu_register(name, out))
        return true;
    if (resolve_fpu_register(name, out))
        return true;
    if (resolve_mmu_register(name, out))
        return true;
    if (resolve_mac_global(name, out))
        return true;

    out->kind = SYM_UNKNOWN;
    return false;
}

// Complete symbol names matching a prefix (without '$')
void complete_symbols(const char *prefix, struct completion *out) {
    if (!prefix || !out)
        return;

    size_t plen = strlen(prefix);

    // CPU register names
    static const char *regs[] = {"pc",   "sp",   "d0",    "d1", "d2",  "d3",  "d4",  "d5",  "d6",  "d7",  "a0",  "a1",
                                 "a2",   "a3",   "a4",    "a5", "a6",  "a7",  "sr",  "ssp", "usp", "msp", "ccr", "c",
                                 "v",    "z",    "n",     "x",  "fp0", "fp1", "fp2", "fp3", "fp4", "fp5", "fp6", "fp7",
                                 "fpcr", "fpsr", "fpiar", "tc", "crp", "srp", "tt0", "tt1", NULL};
    for (int i = 0; regs[i]; i++) {
        if (strncasecmp(regs[i], prefix, plen) == 0 && out->count < CMD_MAX_COMPLETIONS)
            out->items[out->count++] = regs[i];
    }

    // Mac globals
    for (size_t i = 0; i < mac_global_vars_count; i++) {
        if (strncasecmp(mac_global_vars[i].name, prefix, plen) == 0 && out->count < CMD_MAX_COMPLETIONS)
            out->items[out->count++] = mac_global_vars[i].name;
    }
}
