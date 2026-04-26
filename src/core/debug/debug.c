// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug.c
// Debugging, breakpoints, logpoints, and trace functionality.

// ============================================================================
// Includes
// ============================================================================

#include "debug.h"

#include "addr_format.h"
#include "cmd_symbol.h"
#include "cmd_types.h"
#include "common.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "debug_mac.h"
#include "fpu.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "scheduler.h"
#include "shell.h"
#include "system.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Type Definitions
// ============================================================================

struct breakpoint {

    uint32_t addr;
    addr_space_t space; // logical or physical address space

    // Optional condition expression — evaluated at each hit.  Breakpoint only
    // fires when the expression evaluates to true.  NULL = always fire.
    char *condition;

    breakpoint_t *next;
};

// Logpoint structure - like breakpoint but doesn't stop execution
struct logpoint {

    uint32_t addr; // start address of range
    uint32_t end_addr; // end address of range (inclusive), same as addr for single address
    addr_space_t space; // logical or physical (memory logpoints only; PC is always logical)

    // Kind: PC logpoints fire in the step hook; memory logpoints fire from
    // the memory slow path via g_mem_logpoint_hook.
    int kind; // enum logpoint_kind

    // Log category and level for this logpoint
    log_category_t *category;
    int level;

    // Optional message to display when hit
    char *message;

    // Hit counter for this logpoint
    uint32_t hit_count;

    // For logical-space memory logpoints, the physical page range bumped at
    // install time (to catch current aliases).  end_phys_page < start_phys_page
    // means "no physical range was installed" (e.g. MMU off, or P:-space lp).
    uint32_t start_phys_page;
    uint32_t end_phys_page;

    // Optional value filter for memory logpoints: when value_filter_active is
    // true, the hook fires only if the access value equals value_filter.
    // Useful for needle-in-haystack searches (e.g. "find the write of
    // 0x4244E607 to this page") where most accesses on a hot page are noise.
    bool value_filter_active;
    uint32_t value_filter;

    logpoint_t *next;
};

// ============================================================================
// Static Helpers
// ============================================================================

static uint16_t cpu_get_uint16(uint32_t addr) {
    if (!system_memory())
        return 0;
    return memory_read_uint16(addr);
}

// ============================================================================
// Operations
// ============================================================================

breakpoint_t *set_breakpoint(debug_t *debug, uint32_t addr, addr_space_t space) {

    breakpoint_t *bp = malloc(sizeof(breakpoint_t));

    if (bp == NULL)
        return NULL;

    bp->addr = addr;
    bp->space = space;
    bp->condition = NULL;

    // add bp to a linked list
    bp->next = debug->breakpoints;
    debug->breakpoints = bp;

    debug->active = true;

    return bp;
}

// Evaluate a condition expression.  Supports a small set of pseudo-variables
// so breakpoints can be post-MMU only, etc.  Examples:
//   mmu.enabled            — true when the 68030 MMU is on
//   cpu.supervisor         — true in supervisor mode
//   cpu.d0 == $12345678    — comparison (==, !=, <, >, <=, >=)
//   $tc != 0               — register comparison
// Unknown expressions evaluate to true (safer default — don't drop hits).
static bool eval_breakpoint_condition(const char *expr) {
    if (!expr || !*expr)
        return true;
    // Skip leading whitespace
    while (*expr == ' ' || *expr == '\t')
        expr++;

    // Bare flag shortcuts
    if (strcmp(expr, "mmu.enabled") == 0)
        return g_mmu && g_mmu->enabled;
    if (strcmp(expr, "!mmu.enabled") == 0)
        return !g_mmu || !g_mmu->enabled;
    if (strcmp(expr, "cpu.supervisor") == 0) {
        cpu_t *cpu = system_cpu();
        return cpu && cpu->supervisor;
    }
    if (strcmp(expr, "!cpu.supervisor") == 0) {
        cpu_t *cpu = system_cpu();
        return !cpu || !cpu->supervisor;
    }

    // Parse "<lhs> <op> <rhs>"
    char lhs[64], op[4], rhs[64];
    if (sscanf(expr, " %63s %3s %63s", lhs, op, rhs) != 3)
        return true; // unrecognised — default to firing
    cpu_t *cpu = system_cpu();
    uint32_t lv = 0, rv = 0;
    // Resolve LHS
    if (strcmp(lhs, "mmu.enabled") == 0)
        lv = (g_mmu && g_mmu->enabled) ? 1 : 0;
    else if (strcmp(lhs, "cpu.supervisor") == 0)
        lv = (cpu && cpu->supervisor) ? 1 : 0;
    else if (strncmp(lhs, "cpu.d", 5) == 0 && lhs[5] >= '0' && lhs[5] <= '7' && lhs[6] == '\0')
        lv = cpu ? cpu->d[lhs[5] - '0'] : 0;
    else if (strncmp(lhs, "cpu.a", 5) == 0 && lhs[5] >= '0' && lhs[5] <= '7' && lhs[6] == '\0')
        lv = cpu ? cpu->a[lhs[5] - '0'] : 0;
    else if (strcasecmp(lhs, "$tc") == 0 || strcasecmp(lhs, "tc") == 0)
        lv = g_mmu ? g_mmu->tc : 0;
    else if (strcasecmp(lhs, "$sr") == 0 || strcasecmp(lhs, "sr") == 0)
        lv = cpu ? cpu_get_sr(cpu) : 0;
    else if (strcasecmp(lhs, "$vbr") == 0 || strcasecmp(lhs, "vbr") == 0)
        lv = cpu ? cpu->vbr : 0;
    else {
        // Try parse as number
        const char *s = lhs;
        if (*s == '$')
            s++;
        char *end;
        lv = (uint32_t)strtoul(s, &end, 16);
        if (*end != '\0')
            return true;
    }
    // RHS
    {
        const char *s = rhs;
        if (*s == '$')
            s++;
        char *end;
        rv = (uint32_t)strtoul(s, &end, 16);
        if (*end != '\0')
            return true;
    }
    if (strcmp(op, "==") == 0)
        return lv == rv;
    if (strcmp(op, "!=") == 0)
        return lv != rv;
    if (strcmp(op, "<") == 0)
        return lv < rv;
    if (strcmp(op, ">") == 0)
        return lv > rv;
    if (strcmp(op, "<=") == 0)
        return lv <= rv;
    if (strcmp(op, ">=") == 0)
        return lv >= rv;
    return true;
}

// Set a logpoint at the specified address range (end_addr == addr for single address)
logpoint_t *set_logpoint(debug_t *debug, uint32_t addr, uint32_t end_addr, log_category_t *category, int level) {

    logpoint_t *lp = malloc(sizeof(logpoint_t));

    if (lp == NULL)
        return NULL;

    lp->addr = addr;
    lp->end_addr = end_addr;
    lp->space = ADDR_LOGICAL;
    lp->kind = LP_KIND_PC;
    lp->category = category;
    lp->level = level;
    lp->hit_count = 0;
    lp->message = NULL;
    // PC logpoints don't touch the memory-logpoint page refcounts (those are
    // for the memory slow-path hook), but we DO record the install-time
    // physical page range so the per-instruction check can fire when the
    // same physical instruction is executed via an aliased VA — same fix as
    // de9bde3 for memory logpoints.  When MMU is off or translation fails,
    // leave the range "empty" (end < start) and fall back to logical match.
    lp->start_phys_page = 1;
    lp->end_phys_page = 0;
    if (g_mmu && g_mmu->enabled) {
        bool is_identity, valid;
        uint32_t phys_start = debug_translate_address(addr, &is_identity, NULL, &valid);
        if (valid) {
            uint32_t phys_end = debug_translate_address(end_addr, &is_identity, NULL, &valid);
            if (valid) {
                lp->start_phys_page = phys_start >> PAGE_SHIFT;
                lp->end_phys_page = phys_end >> PAGE_SHIFT;
                if (lp->end_phys_page < lp->start_phys_page) {
                    uint32_t tmp = lp->start_phys_page;
                    lp->start_phys_page = lp->end_phys_page;
                    lp->end_phys_page = tmp;
                }
            }
        }
    }
    lp->value_filter_active = false;
    lp->value_filter = 0;

    // add lp to a linked list
    lp->next = debug->logpoints;
    debug->logpoints = lp;

    debug->active = true;

    return lp;
}

// Install a memory-access logpoint (write/read/rw).  Forces the covered pages
// through the memory slow path so the hook can observe every access.  No
// impact on the fast path for other pages.  When space == ADDR_LOGICAL the
// current MMU mapping is also consulted and the corresponding physical pages
// are watched, so an access via an alias of the same physical page still
// fires the hook.  When space == ADDR_PHYSICAL only the physical watch is
// installed (no logical-page watch) — the caller observes every alias.
logpoint_t *set_memory_logpoint(debug_t *debug, uint32_t addr, uint32_t end_addr, addr_space_t space, int kind,
                                log_category_t *category, int level) {
    logpoint_t *lp = malloc(sizeof(logpoint_t));
    if (!lp)
        return NULL;
    lp->addr = addr;
    lp->end_addr = end_addr;
    lp->space = space;
    lp->kind = kind;
    lp->category = category;
    lp->level = level;
    lp->hit_count = 0;
    lp->message = NULL;
    // Mark "no physical range installed" until we do so below.
    lp->start_phys_page = 1;
    lp->end_phys_page = 0;
    lp->value_filter_active = false;
    lp->value_filter = 0;
    lp->next = debug->logpoints;
    debug->logpoints = lp;
    debug->active = true;

    uint32_t start_page = addr >> PAGE_SHIFT;
    uint32_t end_page = end_addr >> PAGE_SHIFT;

    if (space == ADDR_LOGICAL) {
        memory_logpoint_install(start_page, end_page);
        // Also watch the physical pages the current MMU mapping points at —
        // catches aliases (same physical reached via different logical addrs).
        // Translate via the current CPU mode rather than hardcoded supervisor
        // so that under TC.SRE=1 (separate user/supervisor roots) a logpoint
        // installed while user code is running watches the user mapping.
        if (g_mmu && g_mmu->enabled) {
            cpu_t *cpu = system_cpu();
            bool supervisor = cpu ? (cpu->supervisor != 0) : true;
            uint32_t phys_start = mmu_translate_debug(g_mmu, addr, supervisor) >> PAGE_SHIFT;
            uint32_t phys_end = mmu_translate_debug(g_mmu, end_addr, supervisor) >> PAGE_SHIFT;
            if (phys_end < phys_start) {
                uint32_t tmp = phys_start;
                phys_start = phys_end;
                phys_end = tmp;
            }
            memory_logpoint_install_phys(phys_start, phys_end);
            lp->start_phys_page = phys_start;
            lp->end_phys_page = phys_end;
        }
    } else {
        // Physical-space logpoint: only the physical array is bumped.
        memory_logpoint_install_phys(start_page, end_page);
        lp->start_phys_page = start_page;
        lp->end_phys_page = end_page;
    }
    return lp;
}

// Parse an optional "+HEX" offset suffix from `src` (already consumed up to
// `pos`).  Returns the offset value (0 if no suffix), or UINT32_MAX if the
// suffix is malformed (caller should treat as failure).
static uint32_t parse_lp_offset(const char *src, size_t len, size_t pos) {
    if (pos >= len)
        return 0;
    if (src[pos] != '+')
        return UINT32_MAX;
    pos++;
    if (pos >= len)
        return UINT32_MAX;
    uint32_t v = 0;
    for (; pos < len; pos++) {
        char c = src[pos];
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F')
            d = 10 + (c - 'A');
        else
            return UINT32_MAX;
        v = (v << 4) | (uint32_t)d;
    }
    return v;
}

// Resolve a "source" sub-token to a 32-bit address.  Accepts:
//   "cpu.aN[+HEX]" / "cpu.dN[+HEX]" — register file with optional hex offset
//   "xHEX[+HEX]"                    — hex literal address with optional offset
// Returns false if not recognized or offset is malformed.
static bool resolve_lp_addr(const char *src, size_t len, cpu_t *cpu, uint32_t *out_addr) {
    if (len >= 6 && memcmp(src, "cpu.a", 5) == 0 && src[5] >= '0' && src[5] <= '7') {
        uint32_t offs = parse_lp_offset(src, len, 6);
        if (offs == UINT32_MAX && len > 6)
            return false;
        if (len > 6 && offs == UINT32_MAX)
            return false;
        if (len == 6)
            offs = 0;
        *out_addr = (cpu ? cpu->a[src[5] - '0'] : 0) + offs;
        return true;
    }
    if (len >= 6 && memcmp(src, "cpu.d", 5) == 0 && src[5] >= '0' && src[5] <= '7') {
        uint32_t offs = parse_lp_offset(src, len, 6);
        if (len > 6 && offs == UINT32_MAX)
            return false;
        if (len == 6)
            offs = 0;
        *out_addr = (cpu ? cpu->d[src[5] - '0'] : 0) + offs;
        return true;
    }
    if (len >= 2 && src[0] == 'x') {
        // Parse hex digits up to '+' (offset) or end of token.
        uint32_t v = 0;
        size_t i = 1;
        for (; i < len; i++) {
            char c = src[i];
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F')
                d = 10 + (c - 'A');
            else if (c == '+')
                break;
            else
                return false;
            v = (v << 4) | (uint32_t)d;
        }
        uint32_t offs = parse_lp_offset(src, len, i);
        if (i < len && offs == UINT32_MAX)
            return false;
        if (i == len)
            offs = 0;
        *out_addr = v + offs;
        return true;
    }
    return false;
}

// Read a single byte from the guest's user-mode address space, regardless of
// the CPU's current S bit.  Used by the logpoint formatter when a logpoint
// fires in supervisor mode (e.g., kernel syscall entry) but wants to inspect
// userspace pointers.  Falls back to the active context if the MMU isn't
// enabled or no separate user mapping exists.
static uint8_t lp_read_user_uint8(uint32_t addr) {
    if (g_mmu && g_mmu->enabled) {
        uint32_t phys = mmu_translate_debug(g_mmu, addr, /*supervisor=*/false);
        return mmu_read_physical_uint8(g_mmu, phys);
    }
    return memory_read_uint8(addr);
}
static uint32_t lp_read_user_uint32(uint32_t addr) {
    if (g_mmu && g_mmu->enabled) {
        uint32_t phys = mmu_translate_debug(g_mmu, addr, /*supervisor=*/false);
        return mmu_read_physical_uint32(g_mmu, phys);
    }
    return memory_read_uint32(addr);
}

// Append a C-string read from guest memory at `addr` to val[], escaping non-
// printable bytes and stopping at NUL or after `max_chars` bytes.  Reads via
// the user MMU context so kernel-side logpoints can inspect userspace strings.
static void append_guest_cstring(char *val, size_t valsz, uint32_t addr, size_t max_chars) {
    size_t out = 0;
    if (out < valsz)
        val[out++] = '"';
    for (size_t i = 0; i < max_chars && out + 4 < valsz; i++) {
        uint8_t b = lp_read_user_uint8(addr + (uint32_t)i);
        if (b == 0)
            break;
        if (b >= 0x20 && b <= 0x7E) {
            val[out++] = (char)b;
        } else {
            int n = snprintf(val + out, valsz - out, "\\x%02X", b);
            if (n < 0)
                break;
            out += (size_t)n;
        }
    }
    if (out + 1 < valsz)
        val[out++] = '"';
    val[out] = '\0';
}

// Expand a logpoint message, substituting $pc, $value, $instruction_pc,
// register names (cpu.d0, cpu.a0, ...), and memory dereferences:
//   $mem.b.<src> / $mem.w.<src> / $mem.l.<src>      — raw value at addr
//   $str.<src>                                     — C string at addr
//   $str.deref.<src>                               — read long at <src>, then C string at that pointer
// where <src> is "cpu.aN", "cpu.dN", or "xHEX".  Simple replacement, not a
// full expression language.  Writes up to buf_size-1 chars into buf.
static void format_logpoint_message(char *buf, size_t buf_size, const char *msg, uint32_t addr, uint32_t value,
                                    unsigned size) {
    if (!msg) {
        buf[0] = '\0';
        return;
    }
    cpu_t *cpu = system_cpu();
    size_t out = 0;
    for (const char *p = msg; *p && out + 32 < buf_size;) {
        if (*p != '$') {
            buf[out++] = *p++;
            continue;
        }
        // match "$name"  ('+' allowed for src+offset notation in $mem/$str ops)
        p++;
        const char *name = p;
        while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == '+'))
            p++;
        size_t nlen = (size_t)(p - name);
        char tok[64];
        if (nlen >= sizeof(tok)) {
            // too long, just copy as-is
            if (out + nlen + 2 < buf_size) {
                buf[out++] = '$';
                memcpy(buf + out, name, nlen);
                out += nlen;
            }
            continue;
        }
        memcpy(tok, name, nlen);
        tok[nlen] = '\0';
        char val[160];
        val[0] = '\0';
        if (strcmp(tok, "value") == 0) {
            int w = (size == 1) ? 2 : (size == 2) ? 4 : 8;
            snprintf(val, sizeof(val), "$%0*X", w, value);
        } else if (strcmp(tok, "addr") == 0 || strcmp(tok, "address") == 0) {
            snprintf(val, sizeof(val), "$%08X", addr);
        } else if (strcmp(tok, "pc") == 0) {
            snprintf(val, sizeof(val), "$%08X", cpu ? cpu_get_pc(cpu) : 0);
        } else if (strcmp(tok, "instruction_pc") == 0) {
            snprintf(val, sizeof(val), "$%08X", cpu ? cpu->instruction_pc : 0);
        } else if (strncmp(tok, "cpu.d", 5) == 0 && nlen == 6 && tok[5] >= '0' && tok[5] <= '7') {
            snprintf(val, sizeof(val), "$%08X", cpu ? cpu->d[tok[5] - '0'] : 0);
        } else if (strncmp(tok, "cpu.a", 5) == 0 && nlen == 6 && tok[5] >= '0' && tok[5] <= '7') {
            snprintf(val, sizeof(val), "$%08X", cpu ? cpu->a[tok[5] - '0'] : 0);
        } else if (nlen > 6 && memcmp(tok, "mem.", 4) == 0 && tok[5] == '.' &&
                   (tok[4] == 'b' || tok[4] == 'w' || tok[4] == 'l')) {
            // $mem.{b,w,l}.<src>
            uint32_t a = 0;
            if (resolve_lp_addr(tok + 6, nlen - 6, cpu, &a)) {
                if (tok[4] == 'b')
                    snprintf(val, sizeof(val), "$%02X", memory_read_uint8(a));
                else if (tok[4] == 'w')
                    snprintf(val, sizeof(val), "$%04X", memory_read_uint16(a));
                else
                    snprintf(val, sizeof(val), "$%08X", memory_read_uint32(a));
            } else {
                snprintf(val, sizeof(val), "<bad-src>");
            }
        } else if (nlen > 11 && memcmp(tok, "str.deref2.", 11) == 0) {
            // $str.deref2.<src> — read long at <src>, then long at that pointer, then C string at **<src>.
            // The first read uses the supervisor/active context (kernel u_arg slot is kernel-mapped),
            // subsequent reads chase user-space pointers via the user MMU.
            uint32_t a = 0;
            if (resolve_lp_addr(tok + 11, nlen - 11, cpu, &a)) {
                uint32_t p1 = memory_read_uint32(a);
                uint32_t p2 = lp_read_user_uint32(p1);
                append_guest_cstring(val, sizeof(val), p2, 96);
            } else {
                snprintf(val, sizeof(val), "<bad-src>");
            }
        } else if (nlen > 10 && memcmp(tok, "str.deref.", 10) == 0) {
            // $str.deref.<src> — read long at <src> (kernel/active context), then C string via user MMU.
            uint32_t a = 0;
            if (resolve_lp_addr(tok + 10, nlen - 10, cpu, &a)) {
                uint32_t ptr = memory_read_uint32(a);
                append_guest_cstring(val, sizeof(val), ptr, 96);
            } else {
                snprintf(val, sizeof(val), "<bad-src>");
            }
        } else if (nlen > 4 && memcmp(tok, "str.", 4) == 0) {
            // $str.<src>
            uint32_t a = 0;
            if (resolve_lp_addr(tok + 4, nlen - 4, cpu, &a)) {
                append_guest_cstring(val, sizeof(val), a, 96);
            } else {
                snprintf(val, sizeof(val), "<bad-src>");
            }
        } else {
            // unknown: re-emit literally
            if (out + nlen + 2 < buf_size) {
                buf[out++] = '$';
                memcpy(buf + out, tok, nlen);
                out += nlen;
            }
            continue;
        }
        size_t vl = strlen(val);
        if (out + vl < buf_size) {
            memcpy(buf + out, val, vl);
            out += vl;
        }
    }
    buf[out] = '\0';
}

// ============================================================================
// Exception trace ring (IMP from notes/09 diagnostic patch)
// ============================================================================

#define EXC_TRACE_RING_SIZE 256

static exc_trace_entry_t s_exc_trace_ring[EXC_TRACE_RING_SIZE];
static uint32_t s_exc_trace_head = 0; // next write slot
static uint64_t s_exc_trace_count = 0; // total events ever recorded
static log_category_t *s_exc_trace_category = NULL; // lazily registered

// Return the exceptions category (lazy init).  Level = 0 means "ring only,
// no streaming"; level >= 1 enables streaming via the standard log pipeline.
static log_category_t *exc_trace_get_category(void) {
    if (!s_exc_trace_category)
        s_exc_trace_category = log_register_category("exceptions");
    return s_exc_trace_category;
}

void exc_trace_record(uint32_t vector, uint32_t faulting_pc, uint32_t saved_pc, uint32_t fault_addr, uint32_t rw,
                      uint32_t vbr, uint16_t sr, uint16_t format_frame, int double_fault_kind) {
    uint32_t idx = s_exc_trace_head % EXC_TRACE_RING_SIZE;
    exc_trace_entry_t *e = &s_exc_trace_ring[idx];
    e->ts = cpu_instr_count();
    e->faulting_pc = faulting_pc;
    e->saved_pc = saved_pc;
    e->fault_addr = fault_addr;
    e->vbr = vbr;
    e->vector = vector;
    e->sr = sr;
    e->format_frame = format_frame;
    e->rw = (uint8_t)rw;
    e->double_fault_kind = (uint8_t)double_fault_kind;
    s_exc_trace_head = (s_exc_trace_head + 1) % EXC_TRACE_RING_SIZE;
    s_exc_trace_count++;

    // Stream to the log pipeline if the exceptions category is enabled.
    // The LOG_WITH macro short-circuits when level > threshold, so this adds
    // only a single memory load + branch when streaming is off.
    log_category_t *cat = exc_trace_get_category();
    LOG_WITH(cat, 1, "[EXC] vec=$%03X fmt=$%X rw=%s addr=$%08X pc=$%08X saved_pc=$%08X sr=$%04X vbr=$%08X%s", vector,
             format_frame, rw ? "R" : "W", fault_addr, faulting_pc, saved_pc, sr, vbr,
             double_fault_kind ? "  [DOUBLE FAULT]" : "");
}

// Dump the most recent N entries from the exception trace ring
static void exc_trace_dump(int count) {
    if (count <= 0 || count > EXC_TRACE_RING_SIZE)
        count = EXC_TRACE_RING_SIZE;
    uint32_t available = (s_exc_trace_count < (uint64_t)count) ? (uint32_t)s_exc_trace_count : (uint32_t)count;
    if (available == 0) {
        printf("No exceptions recorded\n");
        return;
    }
    printf("Last %u exception(s) (total %llu):\n", available, (unsigned long long)s_exc_trace_count);
    // Walk oldest-first
    uint32_t start = (s_exc_trace_head + EXC_TRACE_RING_SIZE - available) % EXC_TRACE_RING_SIZE;
    for (uint32_t i = 0; i < available; i++) {
        exc_trace_entry_t *e = &s_exc_trace_ring[(start + i) % EXC_TRACE_RING_SIZE];
        printf("  ts=%llu vec=$%03X fmt=$%X rw=%s addr=$%08X pc=$%08X saved_pc=$%08X sr=$%04X vbr=$%08X%s\n",
               (unsigned long long)e->ts, e->vector, e->format_frame, e->rw ? "R" : "W", e->fault_addr, e->faulting_pc,
               e->saved_pc, e->sr, e->vbr, e->double_fault_kind ? "  [DOUBLE FAULT]" : "");
    }
}

// Hook invoked from the memory slow path for every access on a logpoint page.
// Walks the logpoint list and emits a log line for each memory logpoint that
// matches this access.  Cost is O(num memory logpoints) per access on logged
// pages only — unrelated accesses take the fast path and never reach here.
static void debug_memory_logpoint_hook(uint32_t addr, unsigned size, uint32_t value, bool is_write) {
    debug_t *debug = system_debug();
    if (!debug)
        return;
    uint32_t phys_addr = addr;
    bool phys_computed = false;
    for (logpoint_t *lp = debug->logpoints; lp; lp = lp->next) {
        if (lp->kind == LP_KIND_PC)
            continue;
        bool match_kind = (lp->kind == LP_KIND_RW) || (is_write && lp->kind == LP_KIND_WRITE) ||
                          (!is_write && lp->kind == LP_KIND_READ);
        if (!match_kind)
            continue;
        // Check the access against the logpoint's address range — using the
        // physical address for P:-space logpoints, logical for L:-space.
        uint32_t cmp_addr;
        if (lp->space == ADDR_PHYSICAL) {
            if (!phys_computed) {
                bool supervisor = (g_active_write == g_supervisor_write);
                phys_addr = (g_mmu && g_mmu->enabled) ? mmu_translate_debug(g_mmu, addr, supervisor) : addr;
                phys_computed = true;
            }
            cmp_addr = phys_addr;
        } else {
            cmp_addr = addr;
        }
        uint32_t a_end = cmp_addr + size - 1;
        if (a_end < lp->addr || cmp_addr > lp->end_addr)
            continue;
        // Optional value filter: skip non-matching values silently.  The
        // compare uses the size-truncated value to match the bus access width
        // (e.g. .b filter on 0x42 fires on byte writes of 0x42, but a 4-byte
        // write of 0x4244E607 has byte-extracted view that we don't compute
        // here — match the full transaction width instead).
        if (lp->value_filter_active) {
            uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);
            if ((value & mask) != (lp->value_filter & mask))
                continue;
        }
        lp->hit_count++;
        char formatted[256];
        if (lp->message) {
            format_logpoint_message(formatted, sizeof(formatted), lp->message, addr, value, size);
            LOG_WITH(lp->category, lp->level, "logpoint %s $%08X (size=%u, value=$%0*X): %s",
                     is_write ? "WRITE" : "READ", addr, size, (int)(size * 2), value, formatted);
        } else {
            cpu_t *cpu = system_cpu();
            uint32_t pc = cpu ? cpu_get_pc(cpu) : 0;
            LOG_WITH(lp->category, lp->level, "logpoint %s $%08X.%c value=$%0*X pc=$%08X", is_write ? "WRITE" : "READ",
                     addr,
                     (size == 1)   ? 'b'
                     : (size == 2) ? 'w'
                                   : 'l',
                     (int)(size * 2), value, pc);
        }
    }
}

// Hook fired by the value-trap fast-path check when (PA, size, value) match.
// Logs a single line with PC, logical addr, physical addr, and value.  The
// hook keeps firing until disarmed via `value-trap off`.
static void debug_value_trap_hook(uint32_t logical_addr, uint32_t phys_addr, uint32_t value, unsigned size) {
    cpu_t *cpu = system_cpu();
    uint32_t pc = cpu ? cpu_get_pc(cpu) : 0;
    bool supervisor = cpu ? (cpu->supervisor != 0) : false;
    log_category_t *cat = log_get_category("logpoint");
    if (cpu) {
        LOG_WITH(cat, 1,
                 "value-trap WRITE pa=$%08X va=$%08X size=%u value=$%0*X pc=$%08X mode=%c "
                 "a0=$%08X a1=$%08X a2=$%08X a3=$%08X a5=$%08X d2=$%08X",
                 phys_addr, logical_addr, size, (int)(size * 2), value, pc, supervisor ? 'S' : 'U', cpu->a[0],
                 cpu->a[1], cpu->a[2], cpu->a[3], cpu->a[5], cpu->d[2]);
    }
}

extern int cpu_disasm(uint16_t *instr, char *buf);
extern int disasm_68000(uint16_t *instr, char *buf);

// Forward declarations for trace functions
static void trace_add_pc_entry(debug_t *debug, uint32_t pc);

// Forward declarations for logpoint management (IMP-604)
static void list_logpoints(debug_t *debug);
static int delete_logpoint(debug_t *debug, uint32_t addr);
static void delete_all_logpoints(debug_t *debug);

static int disasm(uint16_t *instr, char *mnemonic, char *operands) {
    char buf[100];
    int i, n;

    n = cpu_disasm(instr, buf);

    if (strlen(buf) == 0) {
        sprintf(mnemonic, "ILLEGAL");
        sprintf(operands, "");
    } else {
        for (i = 0; buf[i] != '\0' && buf[i] != '\t'; i++)
            mnemonic[i] = buf[i];
        mnemonic[i] = '\0';
        if (buf[i] == '\t')
            sprintf(operands, "%s", buf + i + 1);
        else
            operands[0] = '\0';
    }

    return n;
}

// Disassemble instruction at addr, write to buf, return instruction length in words
int debugger_disasm(char *buf, size_t buf_size, uint32_t addr) {
    uint16_t words[16];
    char mnemonic[32], operands[80];

    int i;
    for (i = 0; i < 16; i++)
        words[i] = cpu_get_uint16(addr + i * 2);

    int n = disasm(words, mnemonic, operands);

    // Format with address prefix.  Use snprintf to bound output: addr_str is
    // up to 39 chars, mnemonic up to 31, operands up to 79 — worst case
    // ~160 bytes, larger than the 100-byte caller buffers historically used.
    // dc19792 enlarged the inner operands buffer but missed callers; this
    // bounds the final write so a complex full-extension-word instruction
    // can't overflow.
    char addr_str[40];
    format_address_pair(addr_str, sizeof(addr_str), addr);
    if (buf_size > 0)
        snprintf(buf, buf_size, "%s  %04x  %-10s%-12s", addr_str, (int)words[0], mnemonic, operands);

    return n;
}

// Disassemble instruction at current PC, write to buf (bounded by buf_size)
void debugger_disasm_pc(char *buf, size_t buf_size) {
    if (!buf || buf_size == 0)
        return;
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        buf[0] = '\0';
        return;
    }
    debugger_disasm(buf, buf_size, cpu_get_pc(cpu));
}

static void cmd_d(uint32_t n) {
    cpu_t *cpu = system_cpu();
    if (!cpu)
        return;
    uint32_t pc = cpu_get_pc(cpu);

    debug_t *debug = system_debug();

    for (unsigned int i = 0; i < n; i++) {
        char buf[160];
        pc += 2 * debugger_disasm(buf, sizeof(buf), pc);
        printf("%s\n", buf);
    }
}

// Check if execution should break and trace current instruction
int debug_break_and_trace(void) {
    debug_t *debug = system_debug();
    cpu_t *cpu = system_cpu();
    if (!debug || !cpu)
        return false;
    bool stop = false;
    uint32_t current_pc = cpu_get_pc(cpu);

    // If we have a last_breakpoint_pc set, this means we need to skip checking
    // for breakpoints at that specific PC address one time (to allow resuming execution)
    if (debug->last_breakpoint_pc != 0 && current_pc == debug->last_breakpoint_pc) {
        // Clear the flag after skipping once
        debug->last_breakpoint_pc = 0;
    } else {
        // Check for breakpoints at current PC
        breakpoint_t *bp = debug->breakpoints;
        while (bp != NULL) {
            bool hit = false;
            if (bp->space == ADDR_LOGICAL) {
                // Logical breakpoint: compare directly with PC
                hit = (bp->addr == current_pc);
            } else {
                // Physical breakpoint: translate PC to physical and compare
                bool is_identity, valid;
                uint32_t phys_pc = debug_translate_address(current_pc, &is_identity, NULL, &valid);
                hit = valid && (bp->addr == phys_pc);
            }
            if (hit) {
                // Evaluate optional condition — skip the break if false
                if (bp->condition && !eval_breakpoint_condition(bp->condition)) {
                    bp = bp->next;
                    continue;
                }
                if (bp->space == ADDR_PHYSICAL) {
                    printf("breakpoint hit at P:$%08X (PC=$%08X)\n", bp->addr, current_pc);
                } else {
                    printf("breakpoint hit at $%08X\n", bp->addr);
                }
                // Remember this PC to skip it next time we check
                debug->last_breakpoint_pc = current_pc;
                // Auto-delete temporary breakpoints (IMP-601)
                debug_check_tbreak(debug, current_pc);
                stop = true;
                break;
            }
            bp = bp->next;
        }
    }

    // Check for logpoints at current PC (these don't stop execution).
    // Match on either logical PC (install-time VA) or physical page (catches
    // the same physical instruction reached via a different VA — e.g. user
    // and supervisor mappings of shared kernel code).
    logpoint_t *lp = debug->logpoints;
    uint32_t phys_pc_page = 0;
    bool phys_pc_resolved = false;
    bool phys_pc_valid = false;
    while (lp != NULL) {
        if (lp->kind == LP_KIND_PC) {
            bool hit = (current_pc >= lp->addr && current_pc <= lp->end_addr);
            if (!hit && lp->start_phys_page <= lp->end_phys_page) {
                // Lazy-translate once across all logpoints with a phys range.
                if (!phys_pc_resolved) {
                    bool is_identity;
                    uint32_t phys_pc = debug_translate_address(current_pc, &is_identity, NULL, &phys_pc_valid);
                    phys_pc_page = phys_pc >> PAGE_SHIFT;
                    phys_pc_resolved = true;
                }
                if (phys_pc_valid && phys_pc_page >= lp->start_phys_page && phys_pc_page <= lp->end_phys_page)
                    hit = true;
            }
            if (hit) {
                lp->hit_count++;
                if (lp->message) {
                    char formatted[256];
                    format_logpoint_message(formatted, sizeof(formatted), lp->message, current_pc, 0, 0);
                    LOG_WITH(lp->category, lp->level, "logpoint $%08X: %s", current_pc, formatted);
                } else {
                    LOG_WITH(lp->category, lp->level, "logpoint hit at $%08X (hit count: %u)", current_pc,
                             lp->hit_count);
                }
            }
        }
        lp = lp->next;
    }

    if (debug->trace_buffer) {
        debug->trace_buffer[debug->trace_head] = cpu_get_pc(cpu);
        debug->trace_head = (debug->trace_head + 1) % debug->trace_buffer_size;
        if (debug->trace_head == debug->trace_tail)
            debug->trace_tail = (debug->trace_tail + 1) % debug->trace_buffer_size;
    }

    // Record PC in new trace entries buffer
    if (debug->trace_entries) {
        trace_add_pc_entry(debug, cpu_get_pc(cpu));
    }

    return stop;
}

// Delete a breakpoint at the specified address and space
// Returns true if breakpoint was found and deleted, false otherwise
bool delete_breakpoint(debug_t *debug, uint32_t addr, addr_space_t space) {
    breakpoint_t *prev = NULL;
    breakpoint_t *bp = debug->breakpoints;

    while (bp != NULL) {
        if (bp->addr == addr && bp->space == space) {
            // Found the breakpoint, remove it from the list
            if (prev == NULL) {
                debug->breakpoints = bp->next;
            } else {
                prev->next = bp->next;
            }
            if (bp->condition)
                free(bp->condition);
            free(bp);
            return true;
        }
        prev = bp;
        bp = bp->next;
    }
    return false;
}

// Delete breakpoint by id (index in the list)
static bool delete_breakpoint_by_id(debug_t *debug, int id) {
    breakpoint_t **pp = &debug->breakpoints;
    int i = 0;
    while (*pp) {
        if (i == id) {
            breakpoint_t *bp = *pp;
            *pp = bp->next;
            if (bp->condition)
                free(bp->condition);
            free(bp);
            return true;
        }
        pp = &(*pp)->next;
        i++;
    }
    return false;
}

// Delete all breakpoints
// Returns the number of breakpoints deleted
static int delete_all_breakpoints(debug_t *debug) {
    int count = 0;
    breakpoint_t *bp = debug->breakpoints;

    while (bp != NULL) {
        breakpoint_t *next = bp->next;
        if (bp->condition)
            free(bp->condition);
        free(bp);
        bp = next;
        count++;
    }
    debug->breakpoints = NULL;
    return count;
}

// List all breakpoints
static void list_breakpoints(debug_t *debug) {
    breakpoint_t *bp = debug->breakpoints;
    int count = 0;

    if (bp == NULL) {
        printf("No breakpoints set.\n");
        return;
    }

    printf("Breakpoints:\n");
    while (bp != NULL) {
        if (bp->space == ADDR_PHYSICAL)
            printf("  #%d: P:$%08X", count, (unsigned int)bp->addr);
        else
            printf("  #%d: $%08X", count, (unsigned int)bp->addr);
        if (bp->condition)
            printf("  if %s", bp->condition);
        printf("\n");
        bp = bp->next;
        count++;
    }
}

// Parse an address token, optionally followed by ".b"/".w"/".l" size.  Also
// honors the "start-end" and "start:length" forms used by PC-range logpoints.
// Returns false on invalid input.  *size_out is 0 when no size suffix was
// given (caller applies default).  *space_out receives the parsed address
// space (ADDR_LOGICAL by default, ADDR_PHYSICAL when the input had a P:
// prefix).
static bool parse_logpoint_target(const char *in, uint32_t *addr_out, uint32_t *end_addr_out, unsigned *size_out,
                                  addr_space_t *space_out) {
    char buf[64];
    strncpy(buf, in, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    *size_out = 0;

    // Strip optional .b/.w/.l (only when no ':' separator present — ':' is used
    // for length, and could appear with a size suffix too)
    char *dot = strrchr(buf, '.');
    if (dot && (dot[1] == 'b' || dot[1] == 'B') && dot[2] == '\0') {
        *size_out = 1;
        *dot = '\0';
    } else if (dot && (dot[1] == 'w' || dot[1] == 'W') && dot[2] == '\0') {
        *size_out = 2;
        *dot = '\0';
    } else if (dot && (dot[1] == 'l' || dot[1] == 'L') && dot[2] == '\0') {
        *size_out = 4;
        *dot = '\0';
    }

    // Skip L:/P: prefix for separator search
    int prefix_len = 0;
    if ((buf[0] == 'L' || buf[0] == 'l' || buf[0] == 'P' || buf[0] == 'p') && buf[1] == ':')
        prefix_len = 2;

    char *range_sep = strchr(buf + prefix_len, '-');
    char *length_sep = strchr(buf + prefix_len, ':');
    addr_space_t sp = ADDR_LOGICAL;

    if (range_sep) {
        *range_sep = '\0';
        if (!parse_address(buf, addr_out, &sp))
            return false;
        addr_space_t sp2;
        if (!parse_address(range_sep + 1, end_addr_out, &sp2))
            return false;
        if (*end_addr_out < *addr_out)
            return false;
    } else if (length_sep) {
        *length_sep = '\0';
        if (!parse_address(buf, addr_out, &sp))
            return false;
        uint32_t len;
        addr_space_t sp2;
        if (!parse_address(length_sep + 1, &len, &sp2))
            return false;
        if (len == 0)
            return false;
        *end_addr_out = *addr_out + len - 1;
    } else {
        if (!parse_address(buf, addr_out, &sp))
            return false;
        *end_addr_out = *addr_out;
    }
    *space_out = sp;
    return true;
}

/*
static void cmd_step(struct config* config, int n)
{
    debugger_t* debug = &config->debugger;

    debug->step = n > 0 ? n : 1;

    scheduler_run(config->scheduler);
}*/

// Check if tracing is active (for log capture hook)
int debug_trace_is_active(void) {
    if (!system_is_initialized())
        return 0;
    debug_t *debug = system_debug();
    if (!debug)
        return 0;
    return debug->trace_entries != NULL;
}

// Check if debug functionality is engaged (breakpoints, logpoints, or tracing)
bool debug_active(debug_t *debug) {
    if (!debug)
        return false;
    return debug->breakpoints != NULL || debug->logpoints != NULL || debug->trace_buffer != NULL;
}

// Capture a log message to the trace buffer
void debug_trace_capture_log(const char *line) {
    if (!system_is_initialized() || !line)
        return;
    debug_t *debug = system_debug();
    if (!debug)
        return;

    // Only capture if trace entries buffer is active
    if (!debug->trace_entries)
        return;

    // Allocate log buffer on first use
    if (!debug->trace_log_buffer) {
        debug->trace_log_buffer_size = 0x100000; // 1M log entries
        debug->trace_log_buffer = calloc(debug->trace_log_buffer_size, sizeof(trace_log_msg_t));
        if (!debug->trace_log_buffer)
            return;
        debug->trace_log_head = 0;
        debug->trace_log_count = 0;
    }

    // Store log message in log buffer
    uint32_t log_idx = debug->trace_log_head;

    // Free old message if overwriting
    if (debug->trace_log_buffer[log_idx].text) {
        free(debug->trace_log_buffer[log_idx].text);
    }

    // Strip trailing newline if present
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
        debug->trace_log_buffer[log_idx].text = strndup(line, len - 1);
    } else {
        debug->trace_log_buffer[log_idx].text = strdup(line);
    }

    // Advance log buffer head
    debug->trace_log_head = (debug->trace_log_head + 1) % debug->trace_log_buffer_size;
    debug->trace_log_count++;

    // Add trace entry referencing this log
    uint32_t entry_idx = debug->trace_entries_head;
    debug->trace_entries[entry_idx].type = TRACE_ENTRY_LOG;
    debug->trace_entries[entry_idx].value = log_idx;

    // Advance entries head
    debug->trace_entries_head = (debug->trace_entries_head + 1) % debug->trace_entries_size;
    if (debug->trace_entries_head == debug->trace_entries_tail) {
        debug->trace_entries_tail = (debug->trace_entries_tail + 1) % debug->trace_entries_size;
    }
}

// Helper to add a PC entry to the trace
static void trace_add_pc_entry(debug_t *debug, uint32_t pc) {
    if (!debug->trace_entries)
        return;

    uint32_t entry_idx = debug->trace_entries_head;
    debug->trace_entries[entry_idx].type = TRACE_ENTRY_PC;
    debug->trace_entries[entry_idx].value = pc;

    debug->trace_entries_head = (debug->trace_entries_head + 1) % debug->trace_entries_size;
    if (debug->trace_entries_head == debug->trace_entries_tail) {
        debug->trace_entries_tail = (debug->trace_entries_tail + 1) % debug->trace_entries_size;
    }
}

// Helper to free trace log buffer entries
static void trace_free_log_buffer(debug_t *debug) {
    if (debug->trace_log_buffer) {
        for (uint32_t i = 0; i < debug->trace_log_buffer_size; i++) {
            if (debug->trace_log_buffer[i].text) {
                free(debug->trace_log_buffer[i].text);
                debug->trace_log_buffer[i].text = NULL;
            }
        }
        free(debug->trace_log_buffer);
        debug->trace_log_buffer = NULL;
    }
    debug->trace_log_head = 0;
    debug->trace_log_count = 0;
}

// Helper to output trace entries to a file or stdout
static int trace_output_entries(debug_t *debug, FILE *fp) {
    int count = 0;
    uint32_t i = debug->trace_entries_tail;

    while (i != debug->trace_entries_head) {
        trace_entry_t *entry = &debug->trace_entries[i];

        if (entry->type == TRACE_ENTRY_PC) {
            // Disassemble and output the instruction
            char buf[160];
            debugger_disasm(buf, sizeof(buf), entry->value);
            if (fp) {
                fprintf(fp, "%s\n", buf);
            } else {
                printf("%s\n", buf);
            }
            count++;
        } else if (entry->type == TRACE_ENTRY_LOG) {
            // Output the log message with indentation
            uint32_t log_idx = entry->value;
            if (debug->trace_log_buffer && log_idx < debug->trace_log_buffer_size) {
                const char *text = debug->trace_log_buffer[log_idx].text;
                if (text) {
                    if (fp) {
                        fprintf(fp, "          LOG: %s\n", text);
                    } else {
                        printf("          LOG: %s\n", text);
                    }
                }
            }
        }

        i = (i + 1) % debug->trace_entries_size;
    }

    return count;
}

static uint64_t cmd_trace(int argc, char *argv[]) {
    debug_t *debug = system_debug();

    if (argc < 2) {
        printf("Usage: trace <start|stop|show [filename]>\n");
        printf("  trace show          - display trace to stdout\n");
        printf("  trace show <file>   - save trace to file\n");
        printf("  Log entries from enabled categories are captured and shown after their triggering instruction.\n");
        return 0;
    }

    const char *s = argv[1];

    if (strcmp(s, "start") == 0) {
        // Allocate legacy trace buffer (for backward compatibility)
        if (debug->trace_buffer == NULL) {
            debug->trace_buffer_size = 0x1000000;
            debug->trace_buffer = malloc(4 * debug->trace_buffer_size);
        }
        debug->trace_head = 0;
        debug->trace_tail = 0;
        debug->trace_size = 0;

        // Allocate new trace entries buffer
        if (debug->trace_entries == NULL) {
            debug->trace_entries_size = 0x2000000; // 32M entries (PC + log)
            debug->trace_entries = calloc(debug->trace_entries_size, sizeof(trace_entry_t));
        }
        debug->trace_entries_head = 0;
        debug->trace_entries_tail = 0;

        // Log buffer will be allocated on first log capture
        trace_free_log_buffer(debug);

        printf("Trace started (log capture enabled).\n");
    } else if (strcmp(s, "stop") == 0) {
        // Free legacy trace buffer
        free(debug->trace_buffer);
        debug->trace_buffer = NULL;

        // Free trace entries buffer
        free(debug->trace_entries);
        debug->trace_entries = NULL;
        debug->trace_entries_head = 0;
        debug->trace_entries_tail = 0;

        // Free log buffer
        trace_free_log_buffer(debug);

        printf("Trace stopped.\n");
    } else if (strcmp(s, "show") == 0) {
        // Use new trace entries if available, otherwise fall back to legacy
        if (debug->trace_entries) {
            if (argc >= 3) {
                // Save trace to file
                const char *filename = argv[2];
                FILE *fp = fopen(filename, "w");
                if (fp == NULL) {
                    printf("Error: Failed to open file '%s' for writing\n", filename);
                    return 0;
                }

                int count = trace_output_entries(debug, fp);
                fclose(fp);
                printf("Trace saved to '%s' (%d instruction entries)\n", filename, count);
            } else {
                // Display trace to stdout
                trace_output_entries(debug, NULL);
            }
        } else if (debug->trace_buffer) {
            // Legacy trace buffer display
            if (argc >= 3) {
                const char *filename = argv[2];
                FILE *fp = fopen(filename, "w");
                if (fp == NULL) {
                    printf("Error: Failed to open file '%s' for writing\n", filename);
                    return 0;
                }

                int count = 0;
                for (int i = debug->trace_tail; i != debug->trace_head; i = (i + 1) % debug->trace_buffer_size) {
                    char buf[160];
                    debugger_disasm(buf, sizeof(buf), debug->trace_buffer[i]);
                    fprintf(fp, "%s\n", buf);
                    count++;
                }

                fclose(fp);
                printf("Trace saved to '%s' (%d entries)\n", filename, count);
            } else {
                for (int i = debug->trace_tail; i != debug->trace_head; i = (i + 1) % debug->trace_buffer_size) {
                    char buf[160];
                    debugger_disasm(buf, sizeof(buf), debug->trace_buffer[i]);
                    printf("%s\n", buf);
                }
            }
        } else {
            printf("No trace data available. Use 'trace start' first.\n");
        }
    } else {
        printf("Invalid trace command: %s\n", s);
        printf("Usage: trace <start|stop|show [filename]>\n");
        printf("  trace show          - display trace to stdout\n");
        printf("  trace show <file>   - save trace to file\n");
    }

    return 0;
}

static uint64_t cmd_td(int argc, char *argv[]) {
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        printf("CPU not available\n");
        return 0;
    }

    // CPU/Addressing/MMU header line
    bool is_24bit = (g_address_mask == 0x00FFFFFF);
    if (g_mmu) {
        printf("CPU: 68030  Addressing: %s  MMU: %s", is_24bit ? "24-bit" : "32-bit",
               g_mmu->enabled ? "enabled" : "disabled");
        if (g_mmu->enabled)
            printf(" (TC=$%08X)", g_mmu->tc);
        printf("\n");
    } else {
        printf("CPU: 68000  Addressing: %s  MMU: none\n", is_24bit ? "24-bit" : "32-bit");
    }

    printf("D0 = $%08X    A0 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 0), (unsigned int)cpu_get_an(cpu, 0));
    printf("D1 = $%08X    A1 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 1), (unsigned int)cpu_get_an(cpu, 1));
    printf("D2 = $%08X    A2 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 2), (unsigned int)cpu_get_an(cpu, 2));
    printf("D3 = $%08X    A3 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 3), (unsigned int)cpu_get_an(cpu, 3));
    printf("D4 = $%08X    A4 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 4), (unsigned int)cpu_get_an(cpu, 4));
    printf("D5 = $%08X    A5 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 5), (unsigned int)cpu_get_an(cpu, 5));
    printf("D6 = $%08X    A6 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 6), (unsigned int)cpu_get_an(cpu, 6));
    printf("D7 = $%08X    A7 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 7), (unsigned int)cpu_get_an(cpu, 7));

    // PC, SR with decoded fields, USP/SSP (IMP-401, IMP-405)
    uint16_t sr = cpu_get_sr(cpu);
    int s_bit = (sr >> 13) & 1;
    int im = (sr >> 8) & 7;
    int t_bit = (sr >> 15) & 1;
    int x = (sr & cpu_ccr_x) ? 1 : 0;
    int n = (sr & cpu_ccr_n) ? 1 : 0;
    int z = (sr & cpu_ccr_z) ? 1 : 0;
    int v = (sr & cpu_ccr_v) ? 1 : 0;
    int cc = (sr & cpu_ccr_c) ? 1 : 0;
    printf("PC = $%08X    SR = $%04X  (S=%d, IM=%d, T=%d, XNZVC=%d%d%d%d%d)\n", (unsigned int)cpu_get_pc(cpu), sr,
           s_bit, im, t_bit, x, n, z, v, cc);
    printf("USP = $%08X   SSP = $%08X\n", (unsigned int)cpu_get_usp(cpu), (unsigned int)cpu_get_ssp(cpu));

    return 0;
}

// Helper function to convert string to uppercase (for case-insensitive comparisons)
static void str_to_upper(char *dest, const char *src) {
    while (*src) {
        *dest++ = (*src >= 'a' && *src <= 'z') ? (*src - 32) : *src;
        src++;
    }
    *dest = '\0';
}

static uint64_t cmd_set(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: set <register|address> <value>\n");
        printf("  Registers: D0-D7, A0-A7, PC, SP, SSP, USP, SR, CCR\n");
        printf("  Condition codes: C, V, Z, N, X\n");
        printf("  Memory: <address>.b|.w|.l <value>\n");
        return 0;
    }

    cpu_t *cpu = system_cpu();
    if (!cpu) {
        printf("CPU not available\n");
        return 0;
    }
    char target[64];
    str_to_upper(target, argv[1]);

    // Parse the value (support $ prefix for Motorola hex)
    const char *val_str = argv[2];
    if (*val_str == '$')
        val_str++;
    char *endptr;
    uint32_t value = (uint32_t)strtoul(val_str, &endptr,
                                       (*val_str == '0' && (*(val_str + 1) == 'x' || *(val_str + 1) == 'X')) ? 0 : 16);
    if (*endptr != '\0') {
        printf("Invalid value: %s\n", argv[2]);
        return 0;
    }

    // Data registers (D0-D7)
    if (target[0] == 'D' && target[1] >= '0' && target[1] <= '7' && target[2] == '\0') {
        int reg = target[1] - '0';
        cpu_set_dn(cpu, reg, value);
        printf("D%d = $%08X\n", reg, value);
        return 0;
    }

    // Address registers (A0-A7)
    if (target[0] == 'A' && target[1] >= '0' && target[1] <= '7' && target[2] == '\0') {
        int reg = target[1] - '0';
        cpu_set_an(cpu, reg, value);
        printf("A%d = $%08X\n", reg, value);
        return 0;
    }

    // Program Counter
    if (strcmp(target, "PC") == 0) {
        cpu_set_pc(cpu, value);
        printf("PC = $%08X\n", value);
        return 0;
    }

    // Stack Pointer (alias for A7)
    if (strcmp(target, "SP") == 0) {
        cpu_set_an(cpu, 7, value);
        printf("SP = $%08X\n", value);
        return 0;
    }

    // Supervisor Stack Pointer
    if (strcmp(target, "SSP") == 0) {
        cpu_set_ssp(cpu, value);
        printf("SSP = $%08X\n", value);
        return 0;
    }

    // User Stack Pointer
    if (strcmp(target, "USP") == 0) {
        cpu_set_usp(cpu, value);
        printf("USP = $%08X\n", value);
        return 0;
    }

    // Status Register
    if (strcmp(target, "SR") == 0) {
        cpu_set_sr(cpu, (uint16_t)value);
        printf("SR = $%04X\n", (uint16_t)value);
        return 0;
    }

    // Condition Code Register
    if (strcmp(target, "CCR") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        sr = (sr & ~cpu_ccr_mask) | (value & cpu_ccr_mask);
        cpu_set_sr(cpu, sr);
        printf("CCR = $%02X\n", (uint8_t)(value & cpu_ccr_mask));
        return 0;
    }

    // Individual condition code flags
    if (strcmp(target, "C") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        if (value & 1)
            sr |= cpu_ccr_c;
        else
            sr &= ~cpu_ccr_c;
        cpu_set_sr(cpu, sr);
        printf("C = %d\n", (value & 1));
        return 0;
    }

    if (strcmp(target, "V") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        if (value & 1)
            sr |= cpu_ccr_v;
        else
            sr &= ~cpu_ccr_v;
        cpu_set_sr(cpu, sr);
        printf("V = %d\n", (value & 1));
        return 0;
    }

    if (strcmp(target, "Z") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        if (value & 1)
            sr |= cpu_ccr_z;
        else
            sr &= ~cpu_ccr_z;
        cpu_set_sr(cpu, sr);
        printf("Z = %d\n", (value & 1));
        return 0;
    }

    if (strcmp(target, "N") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        if (value & 1)
            sr |= cpu_ccr_n;
        else
            sr &= ~cpu_ccr_n;
        cpu_set_sr(cpu, sr);
        printf("N = %d\n", (value & 1));
        return 0;
    }

    if (strcmp(target, "X") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        if (value & 1)
            sr |= cpu_ccr_x;
        else
            sr &= ~cpu_ccr_x;
        cpu_set_sr(cpu, sr);
        printf("X = %d\n", (value & 1));
        return 0;
    }

    // Master Stack Pointer (68030 only, IMP-406)
    if (strcmp(target, "MSP") == 0) {
        if (cpu->cpu_model != CPU_MODEL_68030) {
            printf("MSP is only available on 68030\n");
            return 0;
        }
        cpu->msp = value;
        printf("MSP = $%08X\n", value);
        return 0;
    }

    // FPU control registers (IMP-403)
    if (strcmp(target, "FPCR") == 0) {
        fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;
        if (!fpu) {
            printf("FPU not available\n");
            return 0;
        }
        fpu->fpcr = value;
        printf("FPCR = $%08X\n", value);
        return 0;
    }
    if (strcmp(target, "FPSR") == 0) {
        fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;
        if (!fpu) {
            printf("FPU not available\n");
            return 0;
        }
        fpu->fpsr = value;
        printf("FPSR = $%08X\n", value);
        return 0;
    }
    if (strcmp(target, "FPIAR") == 0) {
        fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;
        if (!fpu) {
            printf("FPU not available\n");
            return 0;
        }
        fpu->fpiar = value;
        printf("FPIAR = $%08X\n", value);
        return 0;
    }

    // Individual MMU registers (IMP-404)
    if (strcmp(target, "TC") == 0) {
        if (!g_mmu) {
            printf("MMU not present\n");
            return 0;
        }
        g_mmu->tc = value;
        printf("TC = $%08X\n", value);
        return 0;
    }
    if (strcmp(target, "TT0") == 0) {
        if (!g_mmu) {
            printf("MMU not present\n");
            return 0;
        }
        g_mmu->tt0 = value;
        printf("TT0 = $%08X\n", value);
        return 0;
    }
    if (strcmp(target, "TT1") == 0) {
        if (!g_mmu) {
            printf("MMU not present\n");
            return 0;
        }
        g_mmu->tt1 = value;
        printf("TT1 = $%08X\n", value);
        return 0;
    }

    // Memory addresses with size specifiers (.b, .w, .l)
    char *size_spec = strchr(target, '.');
    if (size_spec != NULL) {
        *size_spec = '\0';
        size_spec++;

        // Parse the address from target (supports $, 0x, and bare hex,
        // plus L: / P: prefixes for logical/physical address space).
        uint32_t addr;
        addr_space_t space;
        if (!parse_address(target, &addr, &space)) {
            printf("Invalid address: %s\n", target);
            return 0;
        }

        // Physical writes go through mmu_write_physical_*, which bypasses
        // the CPU SoA so the write reliably hits the targeted PA regardless
        // of whichever mode (user/supervisor) is currently active.  This is
        // what you want when poking a kernel-allocated structure that's
        // only mapped in one address space.
        if (space == ADDR_PHYSICAL) {
            bool ok = false;
            if (strcmp(size_spec, "B") == 0) {
                ok = mmu_write_physical_uint8(g_mmu, addr, (uint8_t)value);
                if (ok)
                    printf("P:[$%08X].b = $%02X\n", addr, (uint8_t)value);
            } else if (strcmp(size_spec, "W") == 0) {
                ok = mmu_write_physical_uint16(g_mmu, addr, (uint16_t)value);
                if (ok)
                    printf("P:[$%08X].w = $%04X\n", addr, (uint16_t)value);
            } else if (strcmp(size_spec, "L") == 0) {
                ok = mmu_write_physical_uint32(g_mmu, addr, value);
                if (ok)
                    printf("P:[$%08X].l = $%08X\n", addr, value);
            } else {
                printf("Invalid size specifier: .%s (use .b, .w, or .l)\n", size_spec);
                return 0;
            }
            if (!ok)
                printf("Physical write failed (PA $%08X unmapped or read-only)\n", addr);
            return 0;
        }

        if (strcmp(size_spec, "B") == 0) {
            memory_write_uint8(addr, (uint8_t)value);
            printf("[$%08X].b = $%02X\n", addr, (uint8_t)value);
            return 0;
        } else if (strcmp(size_spec, "W") == 0) {
            memory_write_uint16(addr, (uint16_t)value);
            printf("[$%08X].w = $%04X\n", addr, (uint16_t)value);
            return 0;
        } else if (strcmp(size_spec, "L") == 0) {
            memory_write_uint32(addr, value);
            printf("[$%08X].l = $%08X\n", addr, value);
            return 0;
        } else {
            printf("Invalid size specifier: .%s (use .b, .w, or .l)\n", size_spec);
            return 0;
        }
    }

    printf("Unknown target: %s\n", target);
    printf("Usage: set <register|address> <value>\n");
    printf("  Registers: D0-D7, A0-A7, PC, SP, SSP, USP, SR, CCR\n");
    printf("  FPU: FPCR, FPSR, FPIAR   MMU: TC, TT0, TT1   68030: MSP\n");
    printf("  Condition codes: C, V, Z, N, X\n");
    printf("  Memory: <address>.b|.w|.l <value>\n");
    printf("  Values are hex by default (42 = $42). Use 0d prefix for decimal (0d66).\n");
    return 0;
}

// ────────────────────────────────────────────────────────────────────────────
// Screenshot command - save emulated screen as PNG
// ────────────────────────────────────────────────────────────────────────────

// Screen dimensions (Macintosh Plus)
#define SCREEN_WIDTH  512
#define SCREEN_HEIGHT 342
#define FB_BYTES      (SCREEN_WIDTH * SCREEN_HEIGHT / 8)

// CRC32 table for PNG chunk checksums
static uint32_t crc32_table[256];
static int crc32_table_init = 0;

// Initialize CRC32 lookup table
static void init_crc32_table(void) {
    if (crc32_table_init)
        return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            if (c & 1)
                c = 0xedb88320 ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc32_table[n] = c;
    }
    crc32_table_init = 1;
}

// Write 32-bit big-endian value to buffer
static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xff;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;
    p[3] = v & 0xff;
}

// Write a PNG chunk to file
static int write_png_chunk(FILE *fp, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t header[8];
    // Write length (big-endian)
    write_be32(header, len);
    // Write type
    memcpy(header + 4, type, 4);
    if (fwrite(header, 1, 8, fp) != 8)
        return -1;

    // Write data (if any)
    if (len > 0 && data) {
        if (fwrite(data, 1, len, fp) != len)
            return -1;
    }

    // Compute CRC over type + data
    uint32_t crc = 0xffffffff;
    for (int i = 0; i < 4; i++) {
        crc = crc32_table[(crc ^ header[4 + i]) & 0xff] ^ (crc >> 8);
    }
    for (uint32_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    crc ^= 0xffffffff;

    // Write CRC (big-endian)
    uint8_t crc_buf[4];
    write_be32(crc_buf, crc);
    if (fwrite(crc_buf, 1, 4, fp) != 4)
        return -1;

    return 0;
}

// Adler-32 checksum for zlib
static uint32_t adler32(const uint8_t *data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// Calculate a simple checksum of the framebuffer for fast screen comparison
static uint32_t framebuffer_checksum(const uint8_t *fb) {
    uint32_t checksum = 0;
    for (int i = 0; i < FB_BYTES; i++) {
        checksum = checksum * 31 + fb[i];
    }
    return checksum;
}

// Calculate checksum for a region of the framebuffer (top, left, bottom, right)
// Region is specified in pixels; checksum operates on the 1-bit packed data
static uint32_t framebuffer_region_checksum(const uint8_t *fb, int top, int left, int bottom, int right) {
    uint32_t checksum = 0;
    int width = right - left;
    int height = bottom - top;
    (void)width;
    (void)height;

    // Process each row in the region
    for (int y = top; y < bottom; y++) {
        // Process each pixel in the row, packing into bytes
        for (int x = left; x < right; x++) {
            int byte_idx = y * (SCREEN_WIDTH / 8) + (x / 8);
            int bit_idx = 7 - (x % 8); // MSB first
            int bit = (fb[byte_idx] >> bit_idx) & 1;

            // Pack bits into a byte and add to checksum when we have 8 bits
            // or at end of row
            int rel_x = x - left;
            int byte_bit = 7 - (rel_x % 8);
            static uint8_t accum_byte = 0;
            if (byte_bit == 7)
                accum_byte = 0; // Start new byte
            if (bit)
                accum_byte |= (1 << byte_bit);
            if (byte_bit == 0 || x == right - 1) {
                checksum = checksum * 31 + accum_byte;
                accum_byte = 0;
            }
        }
    }
    return checksum;
}

// ────────────────────────────────────────────────────────────────────────────
// PNG reading for --match comparison
// ────────────────────────────────────────────────────────────────────────────

// Read 32-bit big-endian value from buffer
static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Simple inflate implementation for PNG stored blocks only
// Returns decompressed data or NULL on error
static uint8_t *inflate_stored_only(const uint8_t *data, size_t data_len, size_t *out_len) {
    if (data_len < 6)
        return NULL; // Need zlib header + at least one block

    // Skip zlib header (2 bytes)
    size_t pos = 2;

    // First pass: calculate total output size
    size_t total_size = 0;
    size_t scan_pos = pos;
    while (scan_pos < data_len - 4) { // Leave room for adler32
        uint8_t bfinal_btype = data[scan_pos++];
        int btype = (bfinal_btype >> 1) & 0x03;
        int bfinal = bfinal_btype & 0x01;

        if (btype != 0) {
            // Not a stored block - we only support stored blocks
            return NULL;
        }

        if (scan_pos + 4 > data_len)
            return NULL;
        uint16_t len = data[scan_pos] | ((uint16_t)data[scan_pos + 1] << 8);
        scan_pos += 4; // Skip len and nlen

        total_size += len;
        scan_pos += len;

        if (bfinal)
            break;
    }

    // Allocate output buffer
    uint8_t *output = malloc(total_size);
    if (!output)
        return NULL;

    // Second pass: extract data
    size_t out_pos = 0;
    while (pos < data_len - 4) {
        uint8_t bfinal_btype = data[pos++];
        int bfinal = bfinal_btype & 0x01;

        uint16_t len = data[pos] | ((uint16_t)data[pos + 1] << 8);
        pos += 4; // Skip len and nlen

        memcpy(output + out_pos, data + pos, len);
        out_pos += len;
        pos += len;

        if (bfinal)
            break;
    }

    *out_len = total_size;
    return output;
}

// Load a PNG file and extract framebuffer (1-bit packed, same format as emulator)
// Returns 0 on success, -1 on error
// Supports both stored-block PNGs (our format) and standard deflate PNGs
static int load_png_to_framebuffer(const char *filename, uint8_t *fb_out) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("Error: Cannot open '%s' for reading.\n", filename);
        return -1;
    }

    // Read entire file
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *file_data = malloc(file_size);
    if (!file_data) {
        fclose(fp);
        printf("Error: Out of memory.\n");
        return -1;
    }

    if (fread(file_data, 1, file_size, fp) != (size_t)file_size) {
        free(file_data);
        fclose(fp);
        printf("Error: Failed to read file.\n");
        return -1;
    }
    fclose(fp);

    // Check PNG signature
    static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (file_size < 8 || memcmp(file_data, png_sig, 8) != 0) {
        free(file_data);
        printf("Error: Not a valid PNG file.\n");
        return -1;
    }

    // Parse chunks to find IHDR and IDAT
    size_t pos = 8;
    int width = 0, height = 0, bit_depth = 0, color_type = 0;
    uint8_t *idat_data = NULL;
    size_t idat_len = 0;
    size_t idat_capacity = 0;

    while (pos + 12 <= (size_t)file_size) {
        uint32_t chunk_len = read_be32(file_data + pos);
        char chunk_type[5];
        memcpy(chunk_type, file_data + pos + 4, 4);
        chunk_type[4] = '\0';

        if (strcmp(chunk_type, "IHDR") == 0 && chunk_len >= 13) {
            width = read_be32(file_data + pos + 8);
            height = read_be32(file_data + pos + 12);
            bit_depth = file_data[pos + 16];
            color_type = file_data[pos + 17];
        } else if (strcmp(chunk_type, "IDAT") == 0) {
            // Append IDAT data
            if (idat_len + chunk_len > idat_capacity) {
                idat_capacity = (idat_len + chunk_len) * 2;
                uint8_t *new_idat = realloc(idat_data, idat_capacity);
                if (!new_idat) {
                    free(idat_data);
                    free(file_data);
                    printf("Error: Out of memory.\n");
                    return -1;
                }
                idat_data = new_idat;
            }
            memcpy(idat_data + idat_len, file_data + pos + 8, chunk_len);
            idat_len += chunk_len;
        } else if (strcmp(chunk_type, "IEND") == 0) {
            break;
        }

        pos += 12 + chunk_len; // length + type + data + crc
    }

    free(file_data);

    // Validate dimensions
    if (width != SCREEN_WIDTH || height != SCREEN_HEIGHT) {
        free(idat_data);
        printf("Error: PNG dimensions %dx%d don't match screen %dx%d.\n", width, height, SCREEN_WIDTH, SCREEN_HEIGHT);
        return -1;
    }

    if (!idat_data || idat_len == 0) {
        free(idat_data);
        printf("Error: No image data in PNG.\n");
        return -1;
    }

    // Decompress IDAT data (stored blocks only)
    size_t raw_size = 0;
    uint8_t *raw_data = inflate_stored_only(idat_data, idat_len, &raw_size);
    free(idat_data);

    if (!raw_data) {
        printf("Error: Failed to decompress PNG (only stored-block PNGs supported).\n");
        return -1;
    }

    // Calculate expected size based on color type
    size_t bytes_per_pixel;
    if (color_type == 6) {
        bytes_per_pixel = 4; // RGBA
    } else if (color_type == 2) {
        bytes_per_pixel = 3; // RGB
    } else if (color_type == 0) {
        bytes_per_pixel = 1; // Grayscale
    } else {
        free(raw_data);
        printf("Error: Unsupported PNG color type %d.\n", color_type);
        return -1;
    }

    size_t row_size = 1 + width * bytes_per_pixel; // 1 filter byte + pixel data
    size_t expected_size = row_size * height;

    if (raw_size != expected_size) {
        free(raw_data);
        printf("Error: PNG data size mismatch (got %zu, expected %zu).\n", raw_size, expected_size);
        return -1;
    }

    // Convert RGBA/RGB/Grayscale back to 1-bit packed framebuffer
    memset(fb_out, 0, FB_BYTES);
    for (int y = 0; y < height; y++) {
        uint8_t *row = raw_data + y * row_size + 1; // Skip filter byte
        for (int x = 0; x < width; x++) {
            // Get pixel brightness
            uint8_t v;
            if (color_type == 6) {
                v = row[x * 4]; // Use R channel from RGBA
            } else if (color_type == 2) {
                v = row[x * 3]; // Use R channel from RGB
            } else {
                v = row[x]; // Grayscale
            }

            // Convert to 1-bit: bright = white (0), dark = black (1)
            int bit = (v < 128) ? 1 : 0;

            // Pack into framebuffer
            int byte_idx = y * (SCREEN_WIDTH / 8) + (x / 8);
            int bit_idx = 7 - (x % 8); // MSB first
            if (bit) {
                fb_out[byte_idx] |= (1 << bit_idx);
            }
        }
    }

    free(raw_data);
    return 0;
}

// Compare current framebuffer with a reference PNG file
// Returns 0 if match, 1 if mismatch, -1 on error
static int match_framebuffer_with_png(const uint8_t *fb, const char *filename) {
    uint8_t *ref_fb = malloc(FB_BYTES);
    if (!ref_fb) {
        printf("Error: Out of memory.\n");
        return -1;
    }

    if (load_png_to_framebuffer(filename, ref_fb) < 0) {
        free(ref_fb);
        return -1;
    }

    // Compare framebuffers
    int match = (memcmp(fb, ref_fb, FB_BYTES) == 0);
    free(ref_fb);

    return match ? 0 : 1;
}

// Save framebuffer as PNG to the given file path
// Returns 0 on success, -1 on error
static int save_framebuffer_as_png(const uint8_t *fb, const char *filename) {
    // Initialize CRC table
    init_crc32_table();

    // Open output file
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("Error: Cannot open file '%s' for writing.\n", filename);
        return -1;
    }

    // PNG signature
    static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (fwrite(png_sig, 1, 8, fp) != 8)
        goto write_error;

    // IHDR chunk: width, height, bit depth, color type, compression, filter, interlace
    // bit depth = 8, color type = 6 (RGBA)
    uint8_t ihdr[13];
    write_be32(ihdr, SCREEN_WIDTH);
    write_be32(ihdr + 4, SCREEN_HEIGHT);
    ihdr[8] = 8; // bit depth (8 bits per channel)
    ihdr[9] = 6; // color type (RGBA)
    ihdr[10] = 0; // compression method (deflate)
    ihdr[11] = 0; // filter method
    ihdr[12] = 0; // interlace method (none)
    if (write_png_chunk(fp, "IHDR", ihdr, 13) < 0)
        goto write_error;

    // Prepare uncompressed image data with filter bytes
    // Each row: 1 filter byte (0 = none) + SCREEN_WIDTH * 4 bytes (RGBA)
    size_t row_size = 1 + SCREEN_WIDTH * 4;
    size_t raw_size = row_size * SCREEN_HEIGHT;
    uint8_t *raw_data = malloc(raw_size);
    if (!raw_data) {
        printf("Error: Out of memory.\n");
        fclose(fp);
        return -1;
    }

    // Convert 1-bit packed framebuffer to RGBA
    // In Mac framebuffer: 1 = black, 0 = white
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        uint8_t *row = raw_data + y * row_size;
        row[0] = 0; // filter byte: none
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            int byte_idx = y * (SCREEN_WIDTH / 8) + (x / 8);
            int bit_idx = 7 - (x % 8); // MSB first
            int bit = (fb[byte_idx] >> bit_idx) & 1;
            // bit=1 means black (0), bit=0 means white (255)
            uint8_t v = bit ? 0 : 255;
            uint8_t *pixel = row + 1 + x * 4;
            pixel[0] = v; // R
            pixel[1] = v; // G
            pixel[2] = v; // B
            pixel[3] = 255; // A (fully opaque)
        }
    }

    // Create "uncompressed" zlib stream using stored blocks
    // zlib header (2 bytes) + stored blocks + adler32 (4 bytes)
    // Each stored block: 1 byte header + 2 bytes len + 2 bytes nlen + data
    // Max block size is 65535 bytes

    // Calculate number of blocks needed
    size_t max_block = 65535;
    size_t num_blocks = (raw_size + max_block - 1) / max_block;
    // zlib overhead: 2 (header) + 5*num_blocks (block headers) + 4 (adler32)
    size_t zlib_size = 2 + raw_size + 5 * num_blocks + 4;

    uint8_t *zlib_data = malloc(zlib_size);
    if (!zlib_data) {
        printf("Error: Out of memory.\n");
        free(raw_data);
        fclose(fp);
        return -1;
    }

    size_t zpos = 0;
    // zlib header: CMF=0x78 (deflate, 32K window), FLG=0x01 (no dict, check bits)
    zlib_data[zpos++] = 0x78;
    zlib_data[zpos++] = 0x01;

    // Write stored blocks
    size_t remaining = raw_size;
    size_t src_pos = 0;
    while (remaining > 0) {
        size_t block_len = (remaining > max_block) ? max_block : remaining;
        int is_final = (remaining <= max_block) ? 1 : 0;
        // Block header: BFINAL (1 bit) + BTYPE (2 bits) = 0b000 or 0b001 for stored
        zlib_data[zpos++] = is_final ? 0x01 : 0x00;
        // LEN (2 bytes, little-endian)
        zlib_data[zpos++] = block_len & 0xff;
        zlib_data[zpos++] = (block_len >> 8) & 0xff;
        // NLEN (one's complement of LEN)
        zlib_data[zpos++] = (~block_len) & 0xff;
        zlib_data[zpos++] = ((~block_len) >> 8) & 0xff;
        // Data
        memcpy(zlib_data + zpos, raw_data + src_pos, block_len);
        zpos += block_len;
        src_pos += block_len;
        remaining -= block_len;
    }

    // Adler-32 checksum of uncompressed data (big-endian)
    uint32_t adler = adler32(raw_data, raw_size);
    write_be32(zlib_data + zpos, adler);
    zpos += 4;

    free(raw_data);

    // Write IDAT chunk
    if (write_png_chunk(fp, "IDAT", zlib_data, (uint32_t)zpos) < 0) {
        free(zlib_data);
        goto write_error;
    }
    free(zlib_data);

    // Write IEND chunk (empty)
    if (write_png_chunk(fp, "IEND", NULL, 0) < 0)
        goto write_error;

    fclose(fp);
    printf("Screenshot saved to '%s' (%dx%d).\n", filename, SCREEN_WIDTH, SCREEN_HEIGHT);
    return 0;

write_error:
    printf("Error: Failed to write to file '%s'.\n", filename);
    fclose(fp);
    return -1;
}

// Screenshot handler - save, checksum, match, or match-or-save
static void cmd_screenshot_handler(struct cmd_context *ctx, struct cmd_result *res) {
    // Check framebuffer availability first
    const uint8_t *fb = system_framebuffer();
    if (!fb) {
        cmd_err(res, "Framebuffer not available");
        return;
    }

    const char *subcmd = ctx->subcmd;

    // Default or "save": save screen as PNG file.  Require an explicit path so
    // typos like `screenshot save` (no argument) don't silently produce a file
    // named "save" in the current directory.
    if (subcmd == NULL || strcmp(subcmd, "save") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: screenshot [save] <filename.png>");
            return;
        }
        const char *filename = ctx->args[0].as_str;
        if (!filename || !*filename) {
            cmd_err(res, "screenshot: empty filename");
            return;
        }
        // Sanity-check the extension: the "save" token could end up here as
        // the filename if a user typed `screenshot save save`, and a missing
        // extension is almost certainly a mistake.  Accept .png/.PNG only.
        size_t fn_len = strlen(filename);
        bool has_png_ext = (fn_len >= 4) && (strcasecmp(filename + fn_len - 4, ".png") == 0);
        if (!has_png_ext) {
            cmd_err(res, "screenshot: path must end in .png (got '%s')", filename);
            return;
        }
        if (save_framebuffer_as_png(fb, filename) < 0) {
            cmd_err(res, "Failed to save screenshot to '%s'", filename);
            return;
        }
        cmd_ok(res);
        return;
    }

    // "checksum": compute screen checksum with optional region
    if (strcmp(subcmd, "checksum") == 0) {
        uint32_t checksum;
        if (ctx->args[0].present && ctx->args[1].present && ctx->args[2].present && ctx->args[3].present) {
            // Region specified: screenshot checksum top left bottom right
            int top = (int)ctx->args[0].as_int;
            int left = (int)ctx->args[1].as_int;
            int bottom = (int)ctx->args[2].as_int;
            int right = (int)ctx->args[3].as_int;
            // Validate bounds
            if (top < 0 || left < 0 || bottom <= top || right <= left || bottom > SCREEN_HEIGHT ||
                right > SCREEN_WIDTH) {
                cmd_err(res, "Invalid region bounds (0,0)-(%d,%d)", SCREEN_WIDTH, SCREEN_HEIGHT);
                return;
            }
            checksum = framebuffer_region_checksum(fb, top, left, bottom, right);
            cmd_printf(ctx, "Region checksum (%d,%d)-(%d,%d): 0x%08x\n", top, left, bottom, right, checksum);
        } else {
            // Full screen checksum
            checksum = framebuffer_checksum(fb);
            cmd_printf(ctx, "Screen checksum: 0x%08x\n", checksum);
        }
        cmd_int(res, checksum);
        return;
    }

    // "match": compare screen with reference PNG
    if (strcmp(subcmd, "match") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: screenshot match <reference.png>");
            return;
        }
        const char *ref_filename = ctx->args[0].as_str;
        int result = match_framebuffer_with_png(fb, ref_filename);
        if (result < 0) {
            cmd_printf(ctx, "MATCH FAILED: Error loading reference image.\n");
            cmd_int(res, 2);
            return;
        } else if (result == 0) {
            cmd_printf(ctx, "MATCH OK: Screen matches '%s'.\n", ref_filename);
            cmd_int(res, 0);
            return;
        } else {
            cmd_printf(ctx, "MATCH FAILED: Screen does not match '%s'.\n", ref_filename);
            cmd_int(res, 1);
            return;
        }
    }

    // "match-or-save": compare with reference, save actual on mismatch
    if (strcmp(subcmd, "match-or-save") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: screenshot match-or-save <reference.png> [actual.png]");
            return;
        }
        const char *ref_filename = ctx->args[0].as_str;
        int result = match_framebuffer_with_png(fb, ref_filename);
        if (result < 0) {
            cmd_printf(ctx, "MATCH FAILED: Error loading reference image.\n");
            // Save actual screenshot if output path provided
            if (ctx->args[1].present) {
                save_framebuffer_as_png(fb, ctx->args[1].as_str);
            }
            cmd_int(res, 2);
            return;
        } else if (result == 0) {
            cmd_printf(ctx, "MATCH OK: Screen matches '%s'.\n", ref_filename);
            cmd_int(res, 0);
            return;
        } else {
            // Mismatch: save actual screenshot if output path provided
            if (ctx->args[1].present) {
                save_framebuffer_as_png(fb, ctx->args[1].as_str);
                cmd_printf(ctx, "MATCH FAILED: Screen does not match '%s'. Saved actual to '%s'.\n", ref_filename,
                           ctx->args[1].as_str);
            } else {
                cmd_printf(ctx, "MATCH FAILED: Screen does not match '%s'.\n", ref_filename);
            }
            cmd_int(res, 1);
            return;
        }
    }

    cmd_err(res, "unknown subcommand: %s", subcmd);
}

// Address display mode command: addrmode [auto|expanded|collapsed]
static uint64_t cmd_addrmode(int argc, char *argv[]) {
    if (argc < 2) {
        // Show current mode
        const char *mode_str;
        switch (g_addr_display_mode) {
        case ADDR_DISPLAY_AUTO:
            mode_str = "auto";
            break;
        case ADDR_DISPLAY_EXPANDED:
            mode_str = "expanded";
            break;
        case ADDR_DISPLAY_COLLAPSED:
            mode_str = "collapsed";
            break;
        default:
            mode_str = "unknown";
            break;
        }
        printf("Address display: %s\n", mode_str);
        return 0;
    }

    if (strcmp(argv[1], "auto") == 0) {
        g_addr_display_mode = ADDR_DISPLAY_AUTO;
        printf("Address display: auto (collapsed when MMU off, expanded when MMU on)\n");
    } else if (strcmp(argv[1], "expanded") == 0) {
        g_addr_display_mode = ADDR_DISPLAY_EXPANDED;
        printf("Address display: expanded (always show L: and P:)\n");
    } else if (strcmp(argv[1], "collapsed") == 0) {
        g_addr_display_mode = ADDR_DISPLAY_COLLAPSED;
        printf("Address display: collapsed (omit P: when L = P)\n");
    } else {
        printf("Usage: addrmode [auto|expanded|collapsed]\n");
    }
    return 0;
}

// Translate command: translate <address> [--reverse]
//   Forward: logical → physical (default).
//   Reverse (--reverse): scan SoA to find logical pages that map to the given
//   physical address.  Cost is O(pages) but cheap enough for debugging.
static uint64_t cmd_translate(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: translate [--reverse] <address>\n");
        return 0;
    }

    // Parse flags
    bool reverse = false;
    int addr_idx = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--reverse") == 0 || strcmp(argv[i], "-r") == 0) {
            reverse = true;
        } else if (argv[i][0] != '-') {
            addr_idx = i;
        }
    }
    if (addr_idx >= argc) {
        printf("Usage: translate [--reverse] <address>\n");
        return 0;
    }

    uint32_t addr;
    addr_space_t space;
    if (!parse_address(argv[addr_idx], &addr, &space)) {
        printf("Invalid address: %s\n", argv[addr_idx]);
        return 0;
    }

    if (reverse) {
        // Reverse: which logical page(s) resolve to this physical address?
        if (!g_mmu || !g_mmu->enabled) {
            printf("MMU disabled — logical == physical == $%08X\n", addr);
            return 0;
        }
        uint32_t target_page = addr & ~(uint32_t)PAGE_MASK;
        int found = 0;
        // Walk SoA arrays.  A non-zero entry encodes (host_base - logical_base).
        // To translate back we rely on mmu_translate_debug since SoA lookups
        // reveal only host pointers.  Walk the address space in page strides
        // using mmu_translate_debug — slower but correct.
        // Under TC.SRE=1 the user and supervisor sides have separate roots;
        // scan both so user mappings aren't invisible.  When SRE=0 the two
        // sides share CRP, so a single pass suffices.
        bool sre_split = TC_SRE(g_mmu->tc) != 0;
        for (int side = 0; side < (sre_split ? 2 : 1); side++) {
            bool supervisor = (side == 0);
            for (int p = 0; p < g_page_count; p++) {
                uint32_t logical_page = (uint32_t)p << PAGE_SHIFT;
                uint32_t phys = mmu_translate_debug(g_mmu, logical_page, supervisor);
                if ((phys & ~(uint32_t)PAGE_MASK) == target_page) {
                    if (sre_split)
                        printf("  L:$%08X -> P:$%08X (%s)\n", logical_page, phys, supervisor ? "S" : "U");
                    else
                        printf("  L:$%08X -> P:$%08X\n", logical_page, phys);
                    if (++found >= 64) {
                        printf("  ... (stopped at 64 matches)\n");
                        goto reverse_done;
                    }
                }
            }
        }
    reverse_done:;
        if (found == 0)
            printf("No logical page maps to P:$%08X\n", addr);
        return 0;
    }

    if (space == ADDR_PHYSICAL) {
        printf("P:$%08X is already a physical address (hint: use --reverse)\n", addr);
        return 0;
    }

    bool is_identity = true;
    bool tt_hit = false;
    bool valid = true;
    uint32_t phys = debug_translate_address(addr, &is_identity, &tt_hit, &valid);

    if (!g_mmu || !g_mmu->enabled) {
        printf("L:$%08X -> P:$%08X  (no MMU)\n", addr, phys);
        return 0;
    }

    if (tt_hit) {
        printf("L:$%08X -> P:$%08X  (TT, transparent translation)\n", addr, phys);
    } else if (!valid) {
        printf("L:$%08X -> INVALID  (descriptor invalid)\n", addr);
    } else {
        printf("L:$%08X -> P:$%08X  (page table%s)\n", addr, phys, is_identity ? ", identity" : "");
    }
    return 0;
}

// ============================================================================
// FPU register dump (IMP-402)
// ============================================================================

// Convert float80 to approximate double for display
static double fp80_to_double(float80_reg_t f) {
    if (fp80_is_zero(f))
        return FP80_SIGN(f) ? -0.0 : 0.0;
    if (fp80_is_inf(f))
        return FP80_SIGN(f) ? -1.0 / 0.0 : 1.0 / 0.0;
    if (fp80_is_nan(f))
        return 0.0 / 0.0;
    int sign = FP80_SIGN(f);
    int exp = FP80_EXP(f) - 16383;
    double mant = (double)f.mantissa / (double)(1ULL << 63);
    double result = mant;
    // Apply exponent via repeated multiply (safe for reasonable range)
    if (exp > 0) {
        for (int i = 0; i < exp && i < 1023; i++)
            result *= 2.0;
    } else if (exp < 0) {
        for (int i = 0; i > exp && i > -1023; i--)
            result /= 2.0;
    }
    return sign ? -result : result;
}

static uint64_t cmd_fpregs(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    cpu_t *cpu = system_cpu();
    if (!cpu) {
        printf("CPU not available\n");
        return 0;
    }
    fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;
    if (!fpu) {
        printf("FPU not available\n");
        return 0;
    }

    for (int i = 0; i < 8; i++) {
        float80_reg_t fp = fpu->fp[i];
        double approx = fp80_to_double(fp);
        printf("FP%d = $%04X.%016llX  (%g)\n", i, fp.exponent, (unsigned long long)fp.mantissa, approx);
    }

    // FPCR decode: rounding mode and precision
    const char *rmode[] = {"RN", "RZ", "RM", "RP"};
    const char *rprec[] = {"Extended", "Single", "Double", "(reserved)"};
    uint32_t fpcr = fpu->fpcr;
    printf("FPCR  = $%08X  (%s, %s)\n", fpcr, rmode[(fpcr >> 4) & 3], rprec[(fpcr >> 6) & 3]);

    // FPSR decode: condition codes and accrued exceptions
    uint32_t fpsr = fpu->fpsr;
    printf("FPSR  = $%08X  [%c%c%c%c] Accrued:[%c%c%c%c%c]\n", fpsr, (fpsr & FPCC_N) ? 'N' : '-',
           (fpsr & FPCC_Z) ? 'Z' : '-', (fpsr & FPCC_I) ? 'I' : '-', (fpsr & FPCC_NAN) ? 'A' : '-',
           (fpsr & FPACC_IOP) ? 'I' : '-', (fpsr & FPACC_OVFL) ? 'O' : '-', (fpsr & FPACC_UNFL) ? 'U' : '-',
           (fpsr & FPACC_DZ) ? 'D' : '-', (fpsr & FPACC_INEX) ? 'X' : '-');

    printf("FPIAR = $%08X\n", fpu->fpiar);

    return 0;
}

// ============================================================================
// Configurable status line / prompt (IMP-308)
// ============================================================================

static int g_prompt_enabled = 1;

// Check if prompt/status line is enabled
int debug_prompt_enabled(void) {
    return g_prompt_enabled;
}

// Set prompt default at startup (e.g. from --no-prompt CLI flag).
// Persists across all subsequent client connections.
void debug_set_prompt_default(int enabled) {
    g_prompt_enabled = enabled ? 1 : 0;
}

static uint64_t cmd_prompt(int argc, char *argv[]) {
    if (argc < 2) {
        printf("prompt is %s\n", g_prompt_enabled ? "on" : "off");
        return g_prompt_enabled;
    }
    if (strcmp(argv[1], "on") == 0) {
        g_prompt_enabled = 1;
        printf("prompt on\n");
    } else if (strcmp(argv[1], "off") == 0) {
        g_prompt_enabled = 0;
        printf("prompt off\n");
    } else {
        printf("Usage: prompt [on|off]\n");
    }
    return 0;
}

// ============================================================================
// Temporary breakpoints (IMP-601)
// ============================================================================

// Temporary breakpoint list (auto-deleted on hit)
typedef struct tbreak_node {
    uint32_t addr;
    addr_space_t space;
    struct tbreak_node *next;
} tbreak_node_t;

static tbreak_node_t *tbreak_list = NULL;

// Set a temporary breakpoint (registers a real breakpoint + tracks it)
static breakpoint_t *set_temporary_breakpoint(debug_t *debug, uint32_t addr, addr_space_t space) {
    breakpoint_t *bp = set_breakpoint(debug, addr, space);
    if (!bp)
        return NULL;
    tbreak_node_t *node = malloc(sizeof(tbreak_node_t));
    if (!node)
        return bp;
    node->addr = addr;
    node->space = space;
    node->next = tbreak_list;
    tbreak_list = node;
    return bp;
}

// Check if a temporary breakpoint was hit. If so, remove it.
void debug_check_tbreak(debug_t *debug, uint32_t pc) {
    tbreak_node_t **pp = &tbreak_list;
    while (*pp) {
        tbreak_node_t *node = *pp;
        if (node->addr == pc) {
            delete_breakpoint(debug, node->addr, node->space);
            *pp = node->next;
            free(node);
            return;
        }
        pp = &node->next;
    }
}

// ============================================================================
// Logpoint management (IMP-604)
// ============================================================================

// Kind label for display
static const char *lp_kind_label(int kind) {
    switch (kind) {
    case LP_KIND_WRITE:
        return "WRITE";
    case LP_KIND_READ:
        return "READ";
    case LP_KIND_RW:
        return "RW";
    default:
        return "PC";
    }
}

// List all logpoints
static void list_logpoints(debug_t *debug) {
    if (!debug || !debug->logpoints) {
        printf("No logpoints set\n");
        return;
    }
    int count = 0;
    for (logpoint_t *lp = debug->logpoints; lp; lp = lp->next) {
        printf("  #%d  %-5s", count, lp_kind_label(lp->kind));
        if (lp->end_addr != lp->addr) {
            printf("  $%08X-$%08X", lp->addr, lp->end_addr);
        } else {
            printf("  $%08X          ", lp->addr);
        }
        if (lp->message)
            printf("  \"%s\"", lp->message);
        printf("  (hits: %u)\n", lp->hit_count);
        count++;
    }
    printf("%d logpoint(s)\n", count);
}

// Helper: free one logpoint node, releasing memory-logpoint page refcounts too
static void free_logpoint(logpoint_t *lp) {
    if (lp->kind != LP_KIND_PC) {
        if (lp->space == ADDR_LOGICAL) {
            uint32_t start_page = lp->addr >> PAGE_SHIFT;
            uint32_t end_page = lp->end_addr >> PAGE_SHIFT;
            memory_logpoint_uninstall(start_page, end_page);
        }
        // Physical range tracked separately; populated for both LOGICAL
        // (when MMU was enabled at install) and PHYSICAL logpoints.
        if (lp->end_phys_page >= lp->start_phys_page)
            memory_logpoint_uninstall_phys(lp->start_phys_page, lp->end_phys_page);
    }
    if (lp->message)
        free(lp->message);
    free(lp);
}

// Delete a logpoint at a specific address (matches the start address)
static int delete_logpoint(debug_t *debug, uint32_t addr) {
    logpoint_t **pp = &debug->logpoints;
    while (*pp) {
        if ((*pp)->addr == addr) {
            logpoint_t *lp = *pp;
            *pp = lp->next;
            free_logpoint(lp);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1; // not found
}

// Delete logpoint by id (index in the list)
static int delete_logpoint_by_id(debug_t *debug, int id) {
    logpoint_t **pp = &debug->logpoints;
    int i = 0;
    while (*pp) {
        if (i == id) {
            logpoint_t *lp = *pp;
            *pp = lp->next;
            free_logpoint(lp);
            return 0;
        }
        pp = &(*pp)->next;
        i++;
    }
    return -1;
}

// Delete all logpoints
static void delete_all_logpoints(debug_t *debug) {
    logpoint_t *lp = debug->logpoints;
    while (lp) {
        logpoint_t *next = lp->next;
        free_logpoint(lp);
        lp = next;
    }
    debug->logpoints = NULL;
}

// ============================================================================
// run-until command (IMP-504)
// ============================================================================

static uint64_t cmd_run_until(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: run-until <address>.b|.w|.l <op> <value> [timeout=N]\n");
        printf("  op: == != < > <= >=\n");
        printf("  Example: run-until $172.b == $80\n");
        return 0;
    }

    // Parse address with size specifier
    char target[64];
    str_to_upper(target, argv[1]);
    char *size_spec = strchr(target, '.');
    int size = 1; // default byte
    if (size_spec) {
        *size_spec = '\0';
        size_spec++;
        if (strcmp(size_spec, "W") == 0)
            size = 2;
        else if (strcmp(size_spec, "L") == 0)
            size = 4;
    }

    uint32_t addr;
    addr_space_t space;
    if (!parse_address(target, &addr, &space)) {
        printf("Invalid address: %s\n", argv[1]);
        return 0;
    }

    // Parse operator
    const char *op = argv[2];
    int op_type = -1; // 0:== 1:!= 2:< 3:> 4:<= 5:>=
    if (strcmp(op, "==") == 0)
        op_type = 0;
    else if (strcmp(op, "!=") == 0)
        op_type = 1;
    else if (strcmp(op, "<") == 0)
        op_type = 2;
    else if (strcmp(op, ">") == 0)
        op_type = 3;
    else if (strcmp(op, "<=") == 0)
        op_type = 4;
    else if (strcmp(op, ">=") == 0)
        op_type = 5;
    else {
        printf("Invalid operator: %s\n", op);
        return 0;
    }

    // Parse expected value
    const char *val_str = argv[3];
    if (*val_str == '$')
        val_str++;
    uint32_t expected = (uint32_t)strtoul(val_str, NULL, 16);

    // Parse optional timeout
    uint64_t timeout = 100000000; // default 100M instructions
    for (int i = 4; i < argc; i++) {
        if (strncmp(argv[i], "timeout=", 8) == 0) {
            timeout = strtoull(argv[i] + 8, NULL, 10);
        }
    }

    // Run scheduler in chunks, checking condition
    scheduler_t *sched = system_scheduler();
    if (!sched) {
        printf("Scheduler not available\n");
        return 0;
    }

    uint64_t start = cpu_instr_count();
    while (cpu_instr_count() - start < timeout) {
        scheduler_run_instructions(sched, 1000);
        scheduler_stop(sched);

        // Read current value
        uint32_t current;
        if (size == 1)
            current = memory_read_uint8(addr);
        else if (size == 2)
            current = memory_read_uint16(addr);
        else
            current = memory_read_uint32(addr);

        // Check condition
        int met = 0;
        switch (op_type) {
        case 0:
            met = (current == expected);
            break;
        case 1:
            met = (current != expected);
            break;
        case 2:
            met = (current < expected);
            break;
        case 3:
            met = (current > expected);
            break;
        case 4:
            met = (current <= expected);
            break;
        case 5:
            met = (current >= expected);
            break;
        }
        if (met) {
            printf("Condition met: [$%08X] = $%X after %llu instructions\n", addr, current,
                   (unsigned long long)(cpu_instr_count() - start));
            return 0;
        }
    }

    printf("Timeout after %llu instructions\n", (unsigned long long)timeout);
    return 1;
}

// ============================================================================
// Mac state summary (IMP-702)
// ============================================================================

static uint64_t cmd_mac_state(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    if (!system_memory()) {
        printf("System not initialized\n");
        return 0;
    }

    // Mouse state
    uint32_t addr_MTemp = debug_mac_lookup_global_address("MTemp");
    uint32_t addr_RawMouse = debug_mac_lookup_global_address("RawMouse");
    uint32_t addr_Mouse = debug_mac_lookup_global_address("Mouse");
    uint32_t addr_MBState = debug_mac_lookup_global_address("MBState");
    uint32_t addr_CrsrNew = debug_mac_lookup_global_address("CrsrNew");
    uint32_t addr_CrsrCouple = debug_mac_lookup_global_address("CrsrCouple");
    uint32_t addr_Ticks = debug_mac_lookup_global_address("Ticks");

    if (addr_Mouse) {
        int16_t mv = (int16_t)memory_read_uint16(addr_Mouse);
        int16_t mh = (int16_t)memory_read_uint16(addr_Mouse + 2);
        printf("Mouse:   pos=(%d,%d)", mv, mh);
    }
    if (addr_MBState) {
        uint8_t mb = memory_read_uint8(addr_MBState);
        printf("  button=%s  MBState=$%02X", (mb & 0x80) ? "UP" : "DOWN", mb);
    }
    printf("\n");

    if (addr_MTemp) {
        int16_t tv = (int16_t)memory_read_uint16(addr_MTemp);
        int16_t th = (int16_t)memory_read_uint16(addr_MTemp + 2);
        printf("Cursor:  MTemp=(%d,%d)", tv, th);
    }
    if (addr_CrsrNew) {
        printf("  CrsrNew=$%02X", memory_read_uint8(addr_CrsrNew));
    }
    if (addr_CrsrCouple) {
        printf("  CrsrCouple=$%02X", memory_read_uint8(addr_CrsrCouple));
    }
    printf("\n");

    if (addr_Ticks) {
        printf("Ticks:   %u\n", memory_read_uint32(addr_Ticks));
    }

    return 0;
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// ============================================================================
// Command Handlers
// ============================================================================

// --- print (replaces get, get-global, reg read) ---
static void cmd_print_handler(struct cmd_context *ctx, struct cmd_result *res) {
    // If no arg provided, delegate to raw_argv for backward compat
    if (!ctx->args[0].present) {
        cmd_err(res, "usage: print <register|global|address.size>");
        return;
    }

    struct resolved_symbol *sym = &ctx->args[0].as_sym;

    if (sym->kind == SYM_REGISTER) {
        // Special handling for FP data registers
        cpu_t *cpu = system_cpu();
        if (cpu && cpu->fpu && sym->size == 10) {
            fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;
            // Extract register number from name
            int reg = -1;
            if (sym->name[0] == 'F' && sym->name[1] == 'P' && sym->name[2] >= '0' && sym->name[2] <= '7')
                reg = sym->name[2] - '0';
            if (reg >= 0) {
                float80_reg_t fp = fpu->fp[reg];
                cmd_printf(ctx, "%s = $%04X.%016llX\n", sym->name, fp.exponent, (unsigned long long)fp.mantissa);
                cmd_ok(res);
                return;
            }
        }
        // Special handling for SR (show decoded flags)
        if (strcmp(sym->name, "SR") == 0) {
            uint16_t sr = (uint16_t)sym->value;
            int s_bit = (sr >> 13) & 1;
            int im = (sr >> 8) & 7;
            int t_bit = (sr >> 15) & 1;
            int x = (sr & 0x0010) ? 1 : 0, n = (sr & 0x0008) ? 1 : 0;
            int z = (sr & 0x0004) ? 1 : 0, v = (sr & 0x0002) ? 1 : 0;
            int cc = (sr & 0x0001) ? 1 : 0;
            cmd_printf(ctx, "SR = $%04X  (S=%d, IM=%d, T=%d, XNZVC=%d%d%d%d%d)\n", sr, s_bit, im, t_bit, x, n, z, v,
                       cc);
            cmd_int(res, sym->value);
            return;
        }
        // CRP/SRP (64-bit MMU registers)
        if (strcmp(sym->name, "CRP") == 0 && g_mmu) {
            cmd_printf(ctx, "CRP = $%08X_%08X\n", (uint32_t)(g_mmu->crp >> 32), (uint32_t)(g_mmu->crp & 0xFFFFFFFF));
            cmd_ok(res);
            return;
        }
        if (strcmp(sym->name, "SRP") == 0 && g_mmu) {
            cmd_printf(ctx, "SRP = $%08X_%08X\n", (uint32_t)(g_mmu->srp >> 32), (uint32_t)(g_mmu->srp & 0xFFFFFFFF));
            cmd_ok(res);
            return;
        }

        cmd_printf(ctx, "%s = $%0*X\n", sym->name, sym->size * 2, sym->value);
        cmd_int(res, sym->value);
        return;
    }

    if (sym->kind == SYM_MAC_GLOBAL) {
        cmd_printf(ctx, "%s ($%06X) = $%0*X\n", sym->name, sym->address, sym->size * 2, sym->value);
        cmd_int(res, sym->value);
        return;
    }

    // Special targets that aren't symbols
    const char *target_name = ctx->raw_argc >= 2 ? ctx->raw_argv[1] : NULL;
    if (target_name && (strcasecmp(target_name, "instr") == 0 || strcasecmp(target_name, "$instr") == 0)) {
        uint64_t count = cpu_instr_count();
        cmd_printf(ctx, "Instruction count = %llu\n", (unsigned long long)count);
        cmd_int(res, (int64_t)count);
        return;
    }

    // SYM_UNKNOWN: try as address.size (already resolved by parser)
    if (sym->address != 0 || sym->size != 0) {
        cmd_printf(ctx, "[$%08X].%c = $%0*X\n", sym->address, sym->size == 1 ? 'b' : (sym->size == 2 ? 'w' : 'l'),
                   sym->size * 2, sym->value);
        cmd_int(res, sym->value);
        return;
    }

    cmd_err(res, "unknown target: %s", ctx->raw_argv[1]);
}

// --- assert ---
// Compare a symbol (register or Mac global) against an expected value and fail
// the running script when they differ.  Useful for scripted integration tests.
// Usage: assert <target> <op> <value>
//   target : register (e.g. pc, d0, sr) or Mac global name
//   op     : == != < > <= >=
//   value  : numeric literal ($hex, 0xhex, or decimal) or another symbol
static void cmd_assert_handler(struct cmd_context *ctx, struct cmd_result *res) {
    // Require all three positional arguments.
    if (!ctx->args[0].present || !ctx->args[1].present || !ctx->args[2].present) {
        cmd_err(res, "usage: assert <target> <op> <value>  (op: == != < > <= >=)");
        return;
    }

    // LHS: resolved symbol (register or Mac global), parsed by ARG_SYMBOL.
    struct resolved_symbol *sym = &ctx->args[0].as_sym;
    if (sym->kind == SYM_UNKNOWN) {
        cmd_err(res, "assert: unknown target '%s'", ctx->raw_argv[1]);
        return;
    }
    uint64_t lhs = (uint64_t)sym->value;

    // Operator: one of == != < > <= >=
    const char *op = ctx->args[1].as_str;
    int op_type = -1;
    if (strcmp(op, "==") == 0)
        op_type = 0;
    else if (strcmp(op, "!=") == 0)
        op_type = 1;
    else if (strcmp(op, "<") == 0)
        op_type = 2;
    else if (strcmp(op, ">") == 0)
        op_type = 3;
    else if (strcmp(op, "<=") == 0)
        op_type = 4;
    else if (strcmp(op, ">=") == 0)
        op_type = 5;
    else {
        cmd_err(res, "assert: invalid operator '%s' (expected == != < > <= >=)", op);
        return;
    }

    // RHS: accept either a numeric literal or another resolvable symbol.
    const char *rhs_str = ctx->args[2].as_str;
    uint64_t rhs = 0;
    struct resolved_symbol rhs_sym;
    if (resolve_symbol(rhs_str[0] == '$' ? rhs_str + 1 : rhs_str, &rhs_sym) && rhs_sym.kind != SYM_UNKNOWN) {
        rhs = (uint64_t)rhs_sym.value;
    } else {
        // Numeric literal: strip leading '$' (hex) or accept 0x / decimal.
        const char *s = rhs_str;
        int base = 16; // default to hex (matches shell convention for addresses)
        if (*s == '$') {
            s++;
        } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
        } else {
            // Pure decimal only if it has no hex-only digits; otherwise assume hex.
            base = 10;
            for (const char *p = s; *p; p++) {
                if ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                    base = 16;
                    break;
                }
            }
        }
        char *endp = NULL;
        rhs = strtoull(s, &endp, base);
        if (!endp || *endp != '\0') {
            cmd_err(res, "assert: invalid value '%s'", rhs_str);
            return;
        }
    }

    // Evaluate the comparison.
    int met = 0;
    switch (op_type) {
    case 0:
        met = (lhs == rhs);
        break;
    case 1:
        met = (lhs != rhs);
        break;
    case 2:
        met = ((int64_t)lhs < (int64_t)rhs);
        break;
    case 3:
        met = ((int64_t)lhs > (int64_t)rhs);
        break;
    case 4:
        met = ((int64_t)lhs <= (int64_t)rhs);
        break;
    case 5:
        met = ((int64_t)lhs >= (int64_t)rhs);
        break;
    }

    // Report result; non-zero cmd_int on failure so script exit code reflects it.
    if (met) {
        cmd_printf(ctx, "ASSERT OK: %s = $%08llX %s $%08llX\n", sym->name, (unsigned long long)lhs, op,
                   (unsigned long long)rhs);
        cmd_int(res, 0);
    } else {
        cmd_printf(ctx, "ASSERT FAILED: %s = $%08llX, expected %s $%08llX\n", sym->name, (unsigned long long)lhs, op,
                   (unsigned long long)rhs);
        cmd_err(res, "assert failed: %s = $%08llX %s $%08llX is false", sym->name, (unsigned long long)lhs, op,
                (unsigned long long)rhs);
    }
}

// --- examine ---
static void cmd_examine_handler(struct cmd_context *ctx, struct cmd_result *res) {
    uint32_t addr;
    if (ctx->args[0].present) {
        addr = ctx->args[0].as_addr;
    } else {
        cmd_err(res, "usage: examine <address> [count]");
        return;
    }

    uint32_t nbytes = 64;
    if (ctx->args[1].present)
        nbytes = (uint32_t)ctx->args[1].as_int;
    if (nbytes == 0) {
        cmd_err(res, "byte count must be > 0");
        return;
    }
    if (nbytes > 512)
        nbytes = 512;

    for (uint32_t i = 0; i < nbytes; i += 16) {
        cmd_printf(ctx, "$%08X  ", addr + i);
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < nbytes)
                cmd_printf(ctx, "%02x ", memory_read_uint8(addr + i + j));
            else
                cmd_printf(ctx, "   ");
        }
        cmd_printf(ctx, " ");
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < nbytes) {
                uint8_t byte = memory_read_uint8(addr + i + j);
                cmd_printf(ctx, "%c", (byte >= 0x20 && byte <= 0x7e) ? byte : '.');
            }
        }
        cmd_printf(ctx, "\n");
    }
    cmd_ok(res);
}

// --- disasm ---
static void cmd_disasm_handler(struct cmd_context *ctx, struct cmd_result *res) {
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        cmd_err(res, "CPU not available");
        return;
    }

    uint32_t addr = ctx->args[0].present ? ctx->args[0].as_addr : cpu_get_pc(cpu);
    int count = ctx->args[1].present ? (int)ctx->args[1].as_int : 16;
    if (count <= 0)
        count = 16;

    for (int i = 0; i < count; i++) {
        char buf[160];
        int instr_len = debugger_disasm(buf, sizeof(buf), addr);
        cmd_printf(ctx, "%s\n", buf);
        addr += 2 * instr_len;
    }
    cmd_ok(res);
}

// --- continue (replaces run) ---
static void cmd_continue_handler(struct cmd_context *ctx, struct cmd_result *res) {
    scheduler_t *sched = system_scheduler();
    if (!sched) {
        cmd_err(res, "scheduler not available");
        return;
    }

    if (ctx->args[0].present) {
        // Counted execution: run exactly N instructions synchronously
        int64_t n = ctx->args[0].as_int;
        if (n <= 0) {
            cmd_err(res, "invalid instruction count");
            return;
        }
        scheduler_run_instructions(sched, (uint64_t)n);
    } else {
        // Unlimited execution: start async and return
        scheduler_start(sched);
    }
    cmd_ok(res);
}

// --- step ---
static void cmd_step_handler(struct cmd_context *ctx, struct cmd_result *res) {
    scheduler_t *sched = system_scheduler();
    if (!sched) {
        cmd_err(res, "scheduler not available");
        return;
    }

    int n = ctx->args[0].present ? (int)ctx->args[0].as_int : 1;
    if (n <= 0) {
        cmd_err(res, "invalid instruction count");
        return;
    }

    scheduler_run_instructions(sched, n);
    scheduler_stop(sched);
    cmd_ok(res);
}

// --- next (replaces so/step-over) ---
static void cmd_next_handler(struct cmd_context *ctx, struct cmd_result *res) {
    (void)ctx;
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        cmd_err(res, "CPU not available");
        return;
    }

    uint32_t pc = cpu_get_pc(cpu);
    char buf[160];
    int instr_words = debugger_disasm(buf, sizeof(buf), pc);

    uint16_t opcode = memory_read_uint16(pc);
    int is_call = 0;
    if ((opcode & 0xFF00) == 0x6100)
        is_call = 1; // BSR
    if ((opcode & 0xFFC0) == 0x4E80)
        is_call = 1; // JSR

    if (is_call) {
        uint32_t return_addr = pc + 2 * instr_words;
        debug_t *debug = system_debug();
        if (debug) {
            set_temporary_breakpoint(debug, return_addr, ADDR_LOGICAL);
            scheduler_t *sched = system_scheduler();
            if (sched)
                scheduler_start(sched);
        }
    } else {
        scheduler_t *sched = system_scheduler();
        if (sched)
            scheduler_run_instructions(sched, 1);
    }
    cmd_ok(res);
}

// --- finish ---
static void cmd_finish_handler(struct cmd_context *ctx, struct cmd_result *res) {
    (void)ctx;
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        cmd_err(res, "CPU not available");
        return;
    }

    uint32_t sp = cpu_get_an(cpu, 7);
    uint32_t return_addr = memory_read_uint32(sp);

    debug_t *debug = system_debug();
    if (!debug) {
        cmd_err(res, "debug not available");
        return;
    }

    cmd_printf(ctx, "Running until return to $%08X\n", return_addr);
    set_temporary_breakpoint(debug, return_addr, ADDR_LOGICAL);
    scheduler_t *sched = system_scheduler();
    if (sched)
        scheduler_start(sched);
    cmd_ok(res);
}

// --- until (replaces run-to) ---
static void cmd_until_handler(struct cmd_context *ctx, struct cmd_result *res) {
    if (!ctx->args[0].present) {
        cmd_err(res, "usage: until <address>");
        return;
    }
    uint32_t addr = ctx->args[0].as_addr;

    debug_t *debug = system_debug();
    if (!debug) {
        cmd_err(res, "debug not available");
        return;
    }

    set_temporary_breakpoint(debug, addr, ADDR_LOGICAL);
    scheduler_t *sched = system_scheduler();
    if (sched)
        scheduler_start(sched);
    cmd_ok(res);
}

// --- stop ---
static void cmd_stop_handler(struct cmd_context *ctx, struct cmd_result *res) {
    (void)ctx;
    scheduler_t *sched = system_scheduler();
    if (!sched) {
        cmd_err(res, "scheduler not available");
        return;
    }
    scheduler_stop(sched);
    cmd_ok(res);
}

// --- break (replaces br) ---
static void cmd_break_handler(struct cmd_context *ctx, struct cmd_result *res) {
    debug_t *debug = system_debug();
    if (!debug) {
        cmd_err(res, "debug not available");
        return;
    }

    const char *subcmd = ctx->subcmd;

    // Default or "set": set breakpoint at address (optionally "if <condition>")
    if (subcmd == NULL || strcmp(subcmd, "set") == 0) {
        // Find the first non-flag arg in raw_argv (skip "set" if present)
        int start = 1;
        if (subcmd && strcmp(subcmd, "set") == 0)
            start = 2;
        if (start >= ctx->raw_argc) {
            cmd_err(res, "usage: break <address> [if <condition>]");
            return;
        }
        uint32_t addr;
        addr_space_t sp;
        if (!parse_address(ctx->raw_argv[start], &addr, &sp)) {
            cmd_err(res, "invalid address: %s", ctx->raw_argv[start]);
            return;
        }
        breakpoint_t *bp = set_breakpoint(debug, addr, sp == ADDR_PHYSICAL ? ADDR_PHYSICAL : ADDR_LOGICAL);
        // Look for optional "if <expr...>" trailing tokens
        int ifpos = -1;
        for (int i = start + 1; i < ctx->raw_argc; i++) {
            if (strcasecmp(ctx->raw_argv[i], "if") == 0) {
                ifpos = i;
                break;
            }
        }
        if (ifpos > 0 && ifpos + 1 < ctx->raw_argc && bp) {
            // Join remaining tokens with spaces
            char expr[256];
            expr[0] = '\0';
            size_t off = 0;
            for (int i = ifpos + 1; i < ctx->raw_argc && off < sizeof(expr) - 2; i++) {
                if (off > 0)
                    expr[off++] = ' ';
                size_t n = strlen(ctx->raw_argv[i]);
                if (off + n >= sizeof(expr))
                    n = sizeof(expr) - 1 - off;
                memcpy(expr + off, ctx->raw_argv[i], n);
                off += n;
                expr[off] = '\0';
            }
            bp->condition = strdup(expr);
            cmd_printf(ctx, "Breakpoint set at $%08X  if %s\n", addr, expr);
        } else {
            cmd_printf(ctx, "Breakpoint set at $%08X.\n", addr);
        }
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "del") == 0) {
        if (ctx->raw_argc < 3) {
            cmd_err(res, "usage: break del <#id|address|all>");
            return;
        }
        const char *target = ctx->raw_argv[2];
        if (strcasecmp(target, "all") == 0) {
            int count = delete_all_breakpoints(debug);
            cmd_printf(ctx, "Deleted %d breakpoint(s).\n", count);
            cmd_ok(res);
            return;
        }
        if (target[0] == '#') {
            int id = atoi(target + 1);
            if (delete_breakpoint_by_id(debug, id))
                cmd_printf(ctx, "Deleted breakpoint #%d.\n", id);
            else
                cmd_printf(ctx, "No breakpoint with id #%d.\n", id);
            cmd_ok(res);
            return;
        }
        uint32_t addr;
        addr_space_t sp;
        if (!parse_address(target, &addr, &sp)) {
            cmd_err(res, "invalid address: %s", target);
            return;
        }
        if (delete_breakpoint(debug, addr, sp == ADDR_PHYSICAL ? ADDR_PHYSICAL : ADDR_LOGICAL))
            cmd_printf(ctx, "Deleted breakpoint at $%08X.\n", addr);
        else
            cmd_printf(ctx, "No breakpoint found at $%08X.\n", addr);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "list") == 0) {
        list_breakpoints(debug);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "clear") == 0) {
        int count = delete_all_breakpoints(debug);
        cmd_printf(ctx, "Deleted %d breakpoint(s).\n", count);
        cmd_ok(res);
        return;
    }

    cmd_err(res, "unknown subcommand: %s", subcmd);
}

// --- tbreak ---
static void cmd_tbreak_handler(struct cmd_context *ctx, struct cmd_result *res) {
    if (!ctx->args[0].present) {
        cmd_err(res, "usage: tbreak <address>");
        return;
    }
    uint32_t addr = ctx->args[0].as_addr;
    debug_t *debug = system_debug();
    if (!debug) {
        cmd_err(res, "debug not available");
        return;
    }
    set_temporary_breakpoint(debug, addr, ADDR_LOGICAL);
    cmd_printf(ctx, "Temporary breakpoint at $%08X\n", addr);
    cmd_ok(res);
}

// --- logpoint ---
static void cmd_logpoint_handler(struct cmd_context *ctx, struct cmd_result *res) {
    debug_t *debug = system_debug();
    if (!debug) {
        cmd_err(res, "debug not available");
        return;
    }

    // Bare invocation or "list": dump all logpoints
    if (ctx->raw_argc <= 1 || (ctx->subcmd && strcmp(ctx->subcmd, "list") == 0)) {
        list_logpoints(debug);
        cmd_ok(res);
        return;
    }

    // "clear": delete all logpoints
    if (ctx->subcmd && strcmp(ctx->subcmd, "clear") == 0) {
        delete_all_logpoints(debug);
        cmd_printf(ctx, "All logpoints deleted\n");
        cmd_ok(res);
        return;
    }

    // "del": delete by #id, address, or all
    if (ctx->subcmd && strcmp(ctx->subcmd, "del") == 0) {
        if (ctx->raw_argc < 3) {
            cmd_err(res, "usage: logpoint del <#id|address|all>");
            return;
        }
        const char *target = ctx->raw_argv[2];
        if (strcmp(target, "all") == 0) {
            delete_all_logpoints(debug);
            cmd_printf(ctx, "All logpoints deleted\n");
            cmd_ok(res);
            return;
        }
        if (target[0] == '#') {
            int id = atoi(target + 1);
            if (delete_logpoint_by_id(debug, id) == 0)
                cmd_printf(ctx, "Logpoint #%d deleted\n", id);
            else
                cmd_err(res, "No logpoint with id #%d", id);
            return;
        }
        uint32_t addr;
        addr_space_t sp;
        if (!parse_address(target, &addr, &sp)) {
            cmd_err(res, "Invalid address: %s", target);
            return;
        }
        if (delete_logpoint(debug, addr) == 0)
            cmd_printf(ctx, "Logpoint at $%08X deleted\n", addr);
        else
            cmd_err(res, "No logpoint at $%08X", addr);
        return;
    }

    // Otherwise: "set" (default subcommand) — set a logpoint.
    // Syntax:
    //   logpoint [set] [--write|--read|--rw] <addr[.size]> [msg] [key=val...]
    int arg_idx = 1;
    if (ctx->subcmd && strcmp(ctx->subcmd, "set") == 0)
        arg_idx = 2;

    // Pre-scan for kind flags so they can appear before or after the address.
    int kind = LP_KIND_PC;
    for (int i = arg_idx; i < ctx->raw_argc; i++) {
        const char *a = ctx->raw_argv[i];
        if (strcmp(a, "--write") == 0)
            kind = (kind == LP_KIND_READ) ? LP_KIND_RW : LP_KIND_WRITE;
        else if (strcmp(a, "--read") == 0)
            kind = (kind == LP_KIND_WRITE) ? LP_KIND_RW : LP_KIND_READ;
        else if (strcmp(a, "--rw") == 0)
            kind = LP_KIND_RW;
    }

    // Find first non-flag argument: this is the address
    int addr_arg = -1;
    for (int i = arg_idx; i < ctx->raw_argc; i++) {
        const char *a = ctx->raw_argv[i];
        if (a[0] == '-' && a[1] == '-')
            continue;
        addr_arg = i;
        break;
    }
    if (addr_arg < 0) {
        cmd_err(res, "usage: logpoint set [--write|--read] <addr[.b|.w|.l]> [msg]");
        return;
    }

    uint32_t addr, end_addr;
    unsigned size = 0;
    addr_space_t space = ADDR_LOGICAL;
    if (!parse_logpoint_target(ctx->raw_argv[addr_arg], &addr, &end_addr, &size, &space)) {
        cmd_err(res, "Invalid address: %s", ctx->raw_argv[addr_arg]);
        return;
    }
    // For memory logpoints with a size suffix and no explicit range, widen
    // the end to size-1 past the start so the hook matches all accesses
    // overlapping the address.
    if (kind != LP_KIND_PC && size > 0 && end_addr == addr)
        end_addr = addr + size - 1;

    // Parse remaining args (message + key=val), skipping flags and the address
    const char *message = NULL;
    const char *category_name = (kind == LP_KIND_PC) ? "logpoint" : "memory";
    int level = 0;
    for (int i = arg_idx; i < ctx->raw_argc; i++) {
        if (i == addr_arg)
            continue;
        char *a = ctx->raw_argv[i];
        if (a[0] == '-' && a[1] == '-')
            continue; // kind flag, already handled
        char *equals = strchr(a, '=');
        if (equals) {
            *equals = '\0';
            const char *key = a;
            const char *value = equals + 1;
            if (strcmp(key, "category") == 0)
                category_name = value;
            else if (strcmp(key, "level") == 0) {
                level = atoi(value);
                if (level < 0) {
                    cmd_err(res, "Invalid level: %s", value);
                    return;
                }
            } else if (strcmp(key, "value") == 0) {
                // Already parsed above; restore '=' and continue.
                *equals = '=';
                continue;
            } else {
                cmd_err(res, "Unknown parameter: %s", key);
                return;
            }
        } else if (!message) {
            message = a;
        } else {
            cmd_err(res, "Unexpected argument: %s", a);
            return;
        }
    }

    // Pre-scan for value=$X filter (memory logpoints only)
    bool have_value_filter = false;
    uint32_t value_filter = 0;
    for (int i = arg_idx; i < ctx->raw_argc; i++) {
        if (i == addr_arg)
            continue;
        char *a = ctx->raw_argv[i];
        if (strncmp(a, "value=", 6) == 0) {
            const char *vstr = a + 6;
            if (*vstr == '$')
                vstr++;
            char *endp = NULL;
            unsigned long v = strtoul(vstr, &endp, 16);
            if (!endp || *endp != '\0') {
                cmd_err(res, "Invalid value=: %s", a + 6);
                return;
            }
            value_filter = (uint32_t)v;
            have_value_filter = true;
        }
    }
    if (have_value_filter && kind == LP_KIND_PC) {
        cmd_err(res, "value= filter is only supported on memory logpoints");
        return;
    }

    log_category_t *category = log_get_category(category_name);
    if (!category)
        category = log_register_category(category_name);
    if (!category) {
        cmd_err(res, "Failed to register log category: %s", category_name);
        return;
    }

    logpoint_t *lp;
    if (kind == LP_KIND_PC)
        lp = set_logpoint(debug, addr, end_addr, category, level);
    else
        lp = set_memory_logpoint(debug, addr, end_addr, space, kind, category, level);
    if (!lp) {
        cmd_err(res, "Failed to set logpoint");
        return;
    }
    if (message)
        lp->message = strdup(message);
    if (have_value_filter) {
        lp->value_filter_active = true;
        lp->value_filter = value_filter;
    }

    const char *kstr = lp_kind_label(kind);
    if (addr == end_addr)
        cmd_printf(ctx, "logpoint %s set at $%08X (category: %s, level: %d)%s\n", kstr, addr, category_name, level,
                   have_value_filter ? " [value-filtered]" : "");
    else
        cmd_printf(ctx, "logpoint %s set at $%08X-$%08X (category: %s, level: %d)%s\n", kstr, addr, end_addr,
                   category_name, level, have_value_filter ? " [value-filtered]" : "");
    cmd_ok(res);
}

// `value-trap` — fast-path needle search.  Catches a specific (PA, size, value)
// write without forcing the page through the slow path (no per-page slowdown).
//   value-trap <PA> <size> <value>    — arm
//   value-trap off                    — disarm
//   value-trap                        — show status
// Size must be 1, 2, or 4.  PA is the literal physical address to match (no L:
// translation).  Value is the bytes the access carries (BE for >1-byte sizes).
static void cmd_value_trap_handler(struct cmd_context *ctx, struct cmd_result *res) {
    if (ctx->raw_argc <= 1) {
        if (g_value_trap_active)
            cmd_printf(ctx, "value-trap ARMED: pa=$%08X size=%u value=$%0*X (hits=%u)\n", g_value_trap_pa,
                       g_value_trap_size, (int)(g_value_trap_size * 2), g_value_trap_value, 0);
        else
            cmd_printf(ctx, "value-trap disarmed\n");
        cmd_ok(res);
        return;
    }
    if (strcasecmp(ctx->raw_argv[1], "off") == 0) {
        g_value_trap_active = 0;
        g_value_trap_hook = NULL;
        cmd_printf(ctx, "value-trap disarmed\n");
        cmd_ok(res);
        return;
    }
    if (ctx->raw_argc < 4) {
        cmd_err(res, "usage: value-trap <PA> <size:1|2|4> <value> | value-trap off");
        return;
    }
    uint32_t pa, size, value;
    addr_space_t sp;
    if (!parse_address(ctx->raw_argv[1], &pa, &sp)) {
        cmd_err(res, "invalid PA: %s", ctx->raw_argv[1]);
        return;
    }
    char *endp;
    size = (uint32_t)strtoul(ctx->raw_argv[2], &endp, 0);
    if (*endp != '\0' || (size != 1 && size != 2 && size != 4)) {
        cmd_err(res, "size must be 1, 2, or 4");
        return;
    }
    addr_space_t vsp;
    if (!parse_address(ctx->raw_argv[3], &value, &vsp)) {
        cmd_err(res, "invalid value: %s", ctx->raw_argv[3]);
        return;
    }
    g_value_trap_pa = pa;
    g_value_trap_size = size;
    g_value_trap_value = value;
    g_value_trap_hook = debug_value_trap_hook;
    g_value_trap_active = 1;
    cmd_printf(ctx, "value-trap armed: pa=$%08X size=%u value=$%0*X\n", pa, size, (int)(size * 2), value);
    cmd_ok(res);
}

// Walk the 68030 PMMU table tree for a single logical address, printing
// each level's descriptor.  Duplicates the logic of mmu.c:mmu_table_walk
// deliberately — the verbose traversal is a debug concern kept out of the
// hot path.  Uses CRP (supervisor=true) to match typical kernel accesses.
static void info_mmu_walk_one(struct cmd_context *ctx, uint32_t logical_addr, uint64_t root, const char *root_name) {
    uint32_t tc = g_mmu->tc;
    uint32_t is = TC_IS(tc);
    uint32_t ti[4] = {TC_TIA(tc), TC_TIB(tc), TC_TIC(tc), TC_TID(tc)};
    uint32_t root_upper = (uint32_t)(root >> 32);
    uint32_t root_lower = (uint32_t)(root & 0xFFFFFFFFu);
    uint32_t root_dt = root_upper & 3;
    cmd_printf(ctx, "%s = $%08X_%08X  root-DT=%u\n", root_name, root_upper, root_lower, root_dt);
    if (root_dt == DESC_DT_INVALID) {
        cmd_printf(ctx, "  root descriptor invalid — walk aborts\n");
        return;
    }

    bool long_desc = (root_dt == DESC_DT_TABLE8);
    uint32_t table_addr = root_lower & 0xFFFFFFF0u;
    uint32_t bit_pos = 32 - is;
    static const char *level_names[] = {"A", "B", "C", "D"};

    for (int level = 0; level < 4; level++) {
        uint32_t index_bits = ti[level];
        if (index_bits == 0)
            continue;
        bit_pos -= index_bits;
        uint32_t index = (logical_addr >> bit_pos) & ((1u << index_bits) - 1);
        uint32_t desc_addr = table_addr + index * (long_desc ? 8u : 4u);
        uint32_t desc_hi = mmu_read_physical_uint32(g_mmu, desc_addr);
        uint32_t desc_lo = long_desc ? mmu_read_physical_uint32(g_mmu, desc_addr + 4) : desc_hi;
        uint32_t dt = desc_hi & 3;
        const char *dt_name = (dt == 0) ? "INVALID" : (dt == 1) ? "PAGE" : (dt == 2) ? "TABLE4" : "TABLE8";

        if (long_desc)
            cmd_printf(ctx, "  L%s idx=$%X (%u bits)  desc@$%08X = $%08X_%08X  DT=%u(%s)", level_names[level], index,
                       index_bits, desc_addr, desc_hi, desc_lo, dt, dt_name);
        else
            cmd_printf(ctx, "  L%s idx=$%X (%u bits)  desc@$%08X = $%08X  DT=%u(%s)", level_names[level], index,
                       index_bits, desc_addr, desc_hi, dt, dt_name);

        if (dt == DESC_DT_INVALID) {
            cmd_printf(ctx, "  — walk terminates (page fault)\n");
            return;
        }
        if (dt == DESC_DT_PAGE) {
            uint32_t page_mask = (1u << bit_pos) - 1;
            uint32_t phys_base = desc_lo & ~page_mask & 0xFFFFFFFCu;
            uint32_t phys = phys_base | (logical_addr & page_mask);
            int wp = (desc_hi >> 2) & 1;
            int u = (desc_hi >> 3) & 1;
            int m = (desc_hi >> 4) & 1;
            int ci = (desc_hi >> 6) & 1;
            int s = long_desc ? ((desc_hi >> 8) & 1) : 0;
            cmd_printf(ctx, "  page_base=$%08X  [U=%d WP=%d M=%d CI=%d%s]  coverage=%u bytes\n", phys_base, u, wp, m,
                       ci, long_desc ? (s ? " S=1" : " S=0") : "", 1u << bit_pos);
            cmd_printf(ctx, "=> %s: L:$%08X -> P:$%08X\n", root_name, logical_addr, phys);
            return;
        }
        // Table descriptor (DT=TABLE4 or TABLE8): descend to next level.
        // Short-format table descriptors carry WP (bit 2) and U (bit 3) in
        // the low nibble, so strip bits 3:0 (mask $FFFFFFF0) rather than
        // bits 1:0 ($FFFFFFFC) which would mistake WP for an address bit.
        uint32_t next = desc_lo & 0xFFFFFFF0u;
        cmd_printf(ctx, "  next_table=$%08X\n", next);
        table_addr = next;
        long_desc = (dt == DESC_DT_TABLE8);
    }
    cmd_printf(ctx, "  walk exhausted levels without a page descriptor\n");
}

static void info_mmu_walk_impl(struct cmd_context *ctx, uint32_t logical_addr) {
    if (!g_mmu) {
        cmd_printf(ctx, "MMU not present\n");
        return;
    }
    if (!g_mmu->enabled) {
        cmd_printf(ctx, "MMU disabled — logical == physical == $%08X\n", logical_addr);
        return;
    }

    // Transparent translation match short-circuits the table walk.
    if (mmu_check_tt(g_mmu, logical_addr, false, true)) {
        cmd_printf(ctx, "L:$%08X hits transparent translation (identity)\n", logical_addr);
        cmd_printf(ctx, "  TT0 = $%08X%s\n", g_mmu->tt0, TT_ENABLE(g_mmu->tt0) ? "" : " (disabled)");
        cmd_printf(ctx, "  TT1 = $%08X%s\n", g_mmu->tt1, TT_ENABLE(g_mmu->tt1) ? "" : " (disabled)");
        cmd_printf(ctx, "=> L:$%08X -> P:$%08X\n", logical_addr, logical_addr);
        return;
    }

    uint32_t tc = g_mmu->tc;
    uint32_t is = TC_IS(tc);
    uint32_t ti[4] = {TC_TIA(tc), TC_TIB(tc), TC_TIC(tc), TC_TID(tc)};
    uint32_t ps = TC_PS(tc);
    cmd_printf(ctx, "TC=$%08X  IS=%u TIA=%u TIB=%u TIC=%u TID=%u PS=%u (%u-byte page)\n", tc, is, ti[0], ti[1], ti[2],
               ti[3], ps, 1u << ps);

    // When SRE is set the kernel has separate supervisor/user root tables;
    // show BOTH so a user-VA walk isn't silently resolved via the kernel's
    // identity-mapped SRP.
    if (TC_SRE(tc)) {
        cmd_printf(ctx, "-- SRP (supervisor root) --\n");
        info_mmu_walk_one(ctx, logical_addr, g_mmu->srp, "SRP");
        cmd_printf(ctx, "-- CRP (user/cpu root) --\n");
        info_mmu_walk_one(ctx, logical_addr, g_mmu->crp, "CRP");
    } else {
        info_mmu_walk_one(ctx, logical_addr, g_mmu->crp, "CRP");
    }
}

// Scan a logical address range and print contiguous mapped runs.
// Side-effect-free w.r.t. the software TLB (mmu_translate_debug doesn't touch
// SoA entries); saves/restores mmu->mmusr so PTEST state remains undisturbed.
static void info_mmu_map_impl(struct cmd_context *ctx, uint32_t start, uint32_t end) {
    if (!g_mmu) {
        cmd_printf(ctx, "MMU not present\n");
        return;
    }
    if (!g_mmu->enabled) {
        cmd_printf(ctx, "MMU disabled — logical == physical (identity)\n");
        return;
    }
    if (end < start) {
        cmd_printf(ctx, "end ($%08X) must be >= start ($%08X)\n", end, start);
        return;
    }

    uint32_t page_size = 1u << PAGE_SHIFT;
    uint16_t saved_mmusr = g_mmu->mmusr;

    // Walk via the current CPU mode rather than hardcoded supervisor so that
    // under TC.SRE=1 a user-space `info mmu-map` from a user-mode breakpoint
    // shows the user mappings, not the kernel's identity-mapped supervisor
    // view.  Use `info mmu-walk` (already SRE-aware) to inspect the other
    // side from the current mode.
    cpu_t *cpu = system_cpu();
    bool supervisor = cpu ? (cpu->supervisor != 0) : true;

    // Run state: when in_run, (run_start, run_phys_start, run_flags) describe
    // the contiguous mapping we're currently accumulating.
    bool in_run = false;
    uint32_t run_start = 0;
    uint32_t run_phys_start = 0;
    uint32_t last_logical = 0;
    uint32_t last_phys = 0;
    uint16_t run_flags = 0;
    const char *run_kind = "";
    int runs_printed = 0;
    const int run_limit = 512;

    // Align start down to page boundary; scan inclusive of end.
    uint32_t addr = start & ~(uint32_t)PAGE_MASK;
    uint64_t scan_end = (uint64_t)end;
    for (uint64_t a = addr; a <= scan_end; a += page_size) {
        uint32_t logical = (uint32_t)a;

        // Determine mapping status: TT match | valid walk | invalid.
        bool mapped = false;
        bool tt = false;
        uint32_t phys = logical;
        uint16_t flags = 0;
        if (mmu_check_tt(g_mmu, logical, false, supervisor)) {
            mapped = true;
            tt = true;
            phys = logical;
        } else {
            uint16_t mmusr = mmu_test_address(g_mmu, logical, false, supervisor, NULL);
            if (!(mmusr & MMUSR_I)) {
                mapped = true;
                phys = mmu_translate_debug(g_mmu, logical, supervisor);
                flags = mmusr & (MMUSR_W | MMUSR_S | MMUSR_M);
            }
        }

        const char *kind = tt ? "TT" : "PT";

        if (mapped) {
            if (!in_run) {
                in_run = true;
                run_start = logical;
                run_phys_start = phys;
                run_flags = flags;
                run_kind = kind;
            } else {
                // Continuity: physical must advance by one page and flags must match.
                bool contiguous = (phys == last_phys + page_size) && (flags == run_flags) && (kind == run_kind);
                if (!contiguous) {
                    // Close previous run, start fresh.
                    cmd_printf(ctx, "  L:$%08X-$%08X -> P:$%08X-$%08X  (%u KB, %s%s%s%s)\n", run_start,
                               last_logical + page_size - 1, run_phys_start, last_phys + page_size - 1,
                               (last_logical - run_start + page_size) >> 10, run_kind,
                               (run_flags & MMUSR_W) ? " WP" : "", (run_flags & MMUSR_S) ? " S" : "",
                               (run_flags & MMUSR_M) ? " M" : "");
                    if (++runs_printed >= run_limit) {
                        cmd_printf(ctx, "  ... (stopped at %d runs)\n", run_limit);
                        in_run = false;
                        break;
                    }
                    run_start = logical;
                    run_phys_start = phys;
                    run_flags = flags;
                    run_kind = kind;
                }
            }
            last_logical = logical;
            last_phys = phys;
        } else if (in_run) {
            // Hit invalid: close current run.
            cmd_printf(ctx, "  L:$%08X-$%08X -> P:$%08X-$%08X  (%u KB, %s%s%s%s)\n", run_start,
                       last_logical + page_size - 1, run_phys_start, last_phys + page_size - 1,
                       (last_logical - run_start + page_size) >> 10, run_kind, (run_flags & MMUSR_W) ? " WP" : "",
                       (run_flags & MMUSR_S) ? " S" : "", (run_flags & MMUSR_M) ? " M" : "");
            if (++runs_printed >= run_limit) {
                cmd_printf(ctx, "  ... (stopped at %d runs)\n", run_limit);
                in_run = false;
                break;
            }
            in_run = false;
        }

        // Guard against 32-bit wrap on the last page.
        if (logical == 0xFFFFF000u)
            break;
    }

    // Flush the final run if still open.
    if (in_run) {
        cmd_printf(ctx, "  L:$%08X-$%08X -> P:$%08X-$%08X  (%u KB, %s%s%s%s)\n", run_start,
                   last_logical + page_size - 1, run_phys_start, last_phys + page_size - 1,
                   (last_logical - run_start + page_size) >> 10, run_kind, (run_flags & MMUSR_W) ? " WP" : "",
                   (run_flags & MMUSR_S) ? " S" : "", (run_flags & MMUSR_M) ? " M" : "");
        runs_printed++;
    }
    if (runs_printed == 0)
        cmd_printf(ctx, "  (no mapped pages in L:$%08X-$%08X)\n", start, end);

    g_mmu->mmusr = saved_mmusr;
}

// --- info (subcommand hub) ---
static void cmd_info_handler(struct cmd_context *ctx, struct cmd_result *res) {
    const char *subcmd = ctx->subcmd;
    if (!subcmd) {
        cmd_err(res, "usage: info <regs|fpregs|mmu|break|logpoint|mac|process|events|schedule>");
        return;
    }

    if (strcmp(subcmd, "regs") == 0) {
        // Delegate to existing td implementation
        cmd_td(0, NULL);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "fpregs") == 0) {
        cmd_fpregs(0, NULL);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "mmu") == 0) {
        if (g_mmu) {
            cmd_printf(ctx, "MMU enabled=%d\n", g_mmu->enabled);
            cmd_printf(ctx, "TC  = $%08X\n", g_mmu->tc);
            cmd_printf(ctx, "CRP = $%08X_%08X\n", (uint32_t)(g_mmu->crp >> 32), (uint32_t)(g_mmu->crp & 0xFFFFFFFF));
            cmd_printf(ctx, "SRP = $%08X_%08X\n", (uint32_t)(g_mmu->srp >> 32), (uint32_t)(g_mmu->srp & 0xFFFFFFFF));
            cmd_printf(ctx, "TT0 = $%08X\n", g_mmu->tt0);
            cmd_printf(ctx, "TT1 = $%08X\n", g_mmu->tt1);
        } else {
            cmd_printf(ctx, "MMU not present\n");
        }
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "break") == 0) {
        debug_t *debug = system_debug();
        if (debug)
            list_breakpoints(debug);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "logpoint") == 0) {
        debug_t *debug = system_debug();
        if (debug)
            list_logpoints(debug);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "mac") == 0) {
        cmd_mac_state(0, NULL);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "process") == 0) {
        extern uint64_t cmd_process_info(int argc, char *argv[]);
        cmd_process_info(0, NULL);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "events") == 0) {
        // Dispatch the existing events command
        shell_dispatch("events");
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "schedule") == 0) {
        shell_dispatch("schedule");
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "exceptions") == 0) {
        int n = ctx->args[0].present ? (int)ctx->args[0].as_int : 32;
        exc_trace_dump(n);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "mmu-walk") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: info mmu-walk <logical-address>");
            return;
        }
        info_mmu_walk_impl(ctx, ctx->args[0].as_addr);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "mmu-map") == 0) {
        uint32_t start = ctx->args[0].present ? ctx->args[0].as_addr : 0x00000000u;
        uint32_t end = ctx->args[1].present ? ctx->args[1].as_addr : 0x1FFFFFFFu;
        info_mmu_map_impl(ctx, start, end);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "mmu-descriptors") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: info mmu-descriptors <address> [count]");
            return;
        }
        uint32_t addr = ctx->args[0].as_addr;
        int count = ctx->args[1].present ? (int)ctx->args[1].as_int : 4;
        if (count <= 0)
            count = 4;
        // Decode each longword as a PMMU descriptor.  DT=0 invalid, DT=1 page,
        // DT=2 short table, DT=3 long table.  For page descriptors we print
        // the physical base; for table descriptors we print the next-level
        // table address.
        for (int i = 0; i < count; i++) {
            uint32_t off = (uint32_t)(i * 4);
            uint32_t desc = memory_read_uint32(addr + off);
            uint32_t dt = desc & 3;
            const char *dt_name = (dt == 0) ? "INVALID" : (dt == 1) ? "PAGE" : (dt == 2) ? "TABLE4" : "TABLE8";
            cmd_printf(ctx, "$%08X  $%08X  DT=%u(%s)", addr + off, desc, dt, dt_name);
            if (dt == 1) {
                uint32_t phys = desc & ~0xFFu; // page descriptors keep flags in low 8 bits
                int wp = (desc >> 2) & 1;
                int u = (desc >> 3) & 1;
                int m = (desc >> 4) & 1;
                int ci = (desc >> 6) & 1;
                cmd_printf(ctx, "  phys=$%08X  [U=%d WP=%d M=%d CI=%d]", phys, u, wp, m, ci);
            } else if (dt == 2 || dt == 3) {
                uint32_t next = desc & ~0xFu; // table descriptors have DT/flags in low nibble
                cmd_printf(ctx, "  next=$%08X", next);
            }
            cmd_printf(ctx, "\n");
        }
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "phys-bytes") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: info phys-bytes <phys-address> [count]");
            return;
        }
        uint32_t addr = ctx->args[0].as_addr;
        int count = ctx->args[1].present ? (int)ctx->args[1].as_int : 16;
        if (count <= 0)
            count = 16;
        if (count > 256)
            count = 256;
        for (int i = 0; i < count; i += 16) {
            cmd_printf(ctx, "P:$%08X  ", addr + i);
            int row = (count - i < 16) ? (count - i) : 16;
            for (int j = 0; j < row; j++) {
                uint8_t b = g_mmu ? mmu_read_physical_uint8(g_mmu, addr + i + j) : 0;
                cmd_printf(ctx, "%02x ", b);
            }
            cmd_printf(ctx, "\n");
        }
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "soa") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: info soa <logical-address>");
            return;
        }
        uint32_t addr = ctx->args[0].as_addr;
        uint32_t page = (addr & ~(uint32_t)PAGE_MASK) >> PAGE_SHIFT;
        cmd_printf(ctx, "SoA for VA $%08X (page idx $%X):\n", addr & ~(uint32_t)PAGE_MASK, page);
        // Decode each SoA entry: entry = host_ptr - logical_page; so host_ptr = entry + logical_page
        // physical = host_ptr - mmu->physical_ram (assuming RAM mapping).
        uintptr_t lp = (uintptr_t)(addr & ~(uint32_t)PAGE_MASK);
        for (int k = 0; k < 4; k++) {
            const char *name[] = {"super_read", "super_write", "user_read", "user_write"};
            uintptr_t *arr = NULL;
            switch (k) {
            case 0:
                arr = g_supervisor_read;
                break;
            case 1:
                arr = g_supervisor_write;
                break;
            case 2:
                arr = g_user_read;
                break;
            case 3:
                arr = g_user_write;
                break;
            }
            if (!arr) {
                cmd_printf(ctx, "  %s: <NULL array>\n", name[k]);
                continue;
            }
            uintptr_t entry = arr[page];
            if (entry == 0) {
                cmd_printf(ctx, "  %-12s: 0 (slow path)\n", name[k]);
            } else {
                uintptr_t host_page_ptr = entry + lp;
                // Try to find matching physical page by checking RAM range
                uintptr_t ram_base = (uintptr_t)g_mmu->physical_ram;
                if (host_page_ptr >= ram_base && host_page_ptr < ram_base + g_mmu->physical_ram_size) {
                    uint32_t phys = (uint32_t)(host_page_ptr - ram_base);
                    cmd_printf(ctx, "  %-12s: entry=$%016lX  host=$%016lX  PA=$%08X\n", name[k], (unsigned long)entry,
                               (unsigned long)host_page_ptr, phys);
                } else {
                    cmd_printf(ctx, "  %-12s: entry=$%016lX  host=$%016lX  (non-RAM)\n", name[k], (unsigned long)entry,
                               (unsigned long)host_page_ptr);
                }
            }
        }
        cmd_ok(res);
        return;
    }

    cmd_err(res, "unknown info subcommand: %s", subcmd);
}

// ============================================================================
// Command Registration Tables
// ============================================================================

// --- print ---
static const char *print_aliases[] = {"p", NULL};
static const struct arg_spec print_args[] = {
    {"target", ARG_SYMBOL, "register, Mac global, or address.size"},
};

// --- assert ---
static const struct arg_spec assert_args[] = {
    {"target", ARG_SYMBOL, "register or Mac global to test"       },
    {"op",     ARG_STRING, "comparison operator (== != < > <= >=)"},
    {"value",  ARG_STRING, "expected value (numeric or symbol)"   },
};

// --- set ---
static const struct arg_spec set_cmd_args[] = {
    {"target", ARG_STRING, "register, flag, or address.size"},
    {"value",  ARG_STRING, "value to set"                   },
};

// --- examine ---
static const char *examine_aliases[] = {"x", NULL};
static const struct arg_spec examine_args[] = {
    {"address", ARG_ADDR,               "start address"                         },
    {"count",   ARG_INT | ARG_OPTIONAL, "bytes to display (default 64, max 512)"},
};

// --- disasm ---
static const char *disasm_aliases[] = {"dis", "d", NULL};
static const struct arg_spec disasm_args[] = {
    {"address", ARG_ADDR | ARG_OPTIONAL, "start address (default: $pc)"       },
    {"count",   ARG_INT | ARG_OPTIONAL,  "number of instructions (default 16)"},
};

// --- continue ---
// Note: "run" is kept as a separate simple command in scheduler.c.
// "run" to the scheduler's cmd_run.
static const char *continue_aliases[] = {"c", NULL};
static const struct arg_spec continue_args[] = {
    {"count", ARG_INT | ARG_OPTIONAL, "max instructions to execute"},
};

// --- step ---
static const char *step_aliases[] = {"s", "si", NULL};
static const struct arg_spec step_args[] = {
    {"count", ARG_INT | ARG_OPTIONAL, "number of instructions (default 1)"},
};

// --- next ---
static const char *next_aliases[] = {"n", "ni", NULL};

// --- finish ---
static const char *finish_aliases[] = {"fin", NULL};

// --- until ---
static const char *until_aliases[] = {"u", NULL};
static const struct arg_spec until_args[] = {
    {"address", ARG_ADDR, "target address"},
};

// --- stop ---
static const char *stop_aliases[] = {"halt", NULL};

// --- advance ---
static const struct arg_spec advance_args[] = {
    {"address", ARG_STRING, "address.size"                              },
    {"op",      ARG_STRING, "comparison operator (==, !=, <, >, <=, >=)"},
    {"value",   ARG_STRING, "expected value"                            },
};

// --- break ---
static const char *break_aliases[] = {"b", "br", NULL};
// Handler parses raw_argv directly to support "if <condition>" and #N syntax.
static const struct arg_spec break_set_args[] = {
    {"address",   ARG_STRING,              "breakpoint address"                                                   },
    {"condition", ARG_REST | ARG_OPTIONAL, "'if <expr>' — optional condition (mmu.enabled, cpu.supervisor, ...)"},
};
static const struct arg_spec break_del_args[] = {
    {"target", ARG_STRING | ARG_OPTIONAL, "#id, address, or 'all'"},
};
static const struct subcmd_spec break_subcmds[] = {
    {NULL,    NULL, break_set_args, 2, "set breakpoint at address"},
    {"set",   NULL, break_set_args, 2, "set breakpoint at address"},
    {"list",  NULL, NULL,           0, "list all breakpoints"     },
    {"del",   NULL, break_del_args, 1, "delete breakpoint(s)"     },
    {"clear", NULL, NULL,           0, "delete all breakpoints"   },
};

// --- tbreak ---
static const char *tbreak_aliases[] = {"tb", NULL};
static const struct arg_spec tbreak_args[] = {
    {"address", ARG_ADDR, "breakpoint address"},
};

// --- watch (alias for advance) ---
static const char *watch_aliases[] = {"w", NULL};

// --- logpoint ---
static const char *logpoint_aliases[] = {"lp", NULL};
// Handler drives off raw_argv (supports --write/--read flags, ranges, key=val
// options, and substitutions in the message).  The spec below is loose so the
// framework doesn't reject legitimate combinations.
static const struct arg_spec lp_set_args[] = {
    {"target",  ARG_STRING | ARG_OPTIONAL, "[--write|--read] <address[.b|.w|.l]>"           },
    {"message", ARG_REST | ARG_OPTIONAL,   "log message (supports $pc, $value, $cpu.d0 ...)"},
};
static const struct arg_spec lp_del_args[] = {
    {"target", ARG_STRING, "#id, address, or 'all'"},
};
static const struct subcmd_spec logpoint_subcmds[] = {
    {NULL,    NULL, lp_set_args, 2, "set logpoint"        },
    {"set",   NULL, lp_set_args, 2, "set logpoint"        },
    {"del",   NULL, lp_del_args, 1, "delete logpoint(s)"  },
    {"list",  NULL, NULL,        0, "list all logpoints"  },
    {"clear", NULL, NULL,        0, "delete all logpoints"},
};

// --- info ---
static const char *info_aliases[] = {"i", NULL};
static const char *info_regs_aliases[] = {"r", NULL};
static const char *info_fpregs_aliases[] = {"fp", NULL};
static const char *info_break_aliases[] = {"b", NULL};
static const char *info_logpoint_aliases[] = {"lp", NULL};
static const char *info_process_aliases[] = {"proc", NULL};
static const struct arg_spec info_exc_args[] = {
    {"count", ARG_INT | ARG_OPTIONAL, "number of entries to show (default 32, max 256)"},
};
static const struct arg_spec info_mmu_desc_args[] = {
    {"address", ARG_ADDR,               "descriptor table address"         },
    {"count",   ARG_INT | ARG_OPTIONAL, "number of descriptors (default 4)"},
};
static const struct arg_spec info_mmu_walk_args[] = {
    {"address", ARG_ADDR, "logical address to walk"},
};
static const struct arg_spec info_mmu_map_args[] = {
    {"start", ARG_ADDR | ARG_OPTIONAL, "scan start address (default $00000000)"         },
    {"end",   ARG_ADDR | ARG_OPTIONAL, "scan end address (inclusive, default $1FFFFFFF)"},
};
static const struct arg_spec info_phys_bytes_args[] = {
    {"address", ARG_ADDR,               "physical address"          },
    {"count",   ARG_INT | ARG_OPTIONAL, "bytes to dump (default 16)"},
};
static const struct arg_spec info_soa_args[] = {
    {"address", ARG_ADDR, "logical address (page granularity)"},
};
static const struct subcmd_spec info_subcmds[] = {
    {"regs",            info_regs_aliases,     NULL,                 0, "CPU register dump"                    },
    {"fpregs",          info_fpregs_aliases,   NULL,                 0, "FPU register dump"                    },
    {"mmu",             NULL,                  NULL,                 0, "MMU register dump"                    },
    {"mmu-descriptors", NULL,                  info_mmu_desc_args,   2, "decode MMU descriptors at address"    },
    {"mmu-walk",        NULL,                  info_mmu_walk_args,   1, "walk MMU tables for a logical address"},
    {"mmu-map",         NULL,                  info_mmu_map_args,    2, "list mapped logical ranges"           },
    {"phys-bytes",      NULL,                  info_phys_bytes_args, 2, "dump bytes from physical address"     },
    {"soa",             NULL,                  info_soa_args,        1, "show SoA TLB entries for a VA"        },
    {"break",           info_break_aliases,    NULL,                 0, "list all breakpoints"                 },
    {"logpoint",        info_logpoint_aliases, NULL,                 0, "list all logpoints"                   },
    {"mac",             NULL,                  NULL,                 0, "Mac OS state summary"                 },
    {"process",         info_process_aliases,  NULL,                 0, "current application info"             },
    {"events",          NULL,                  NULL,                 0, "scheduler event queue"                },
    {"schedule",        NULL,                  NULL,                 0, "scheduler mode and CPI"               },
    {"exceptions",      NULL,                  info_exc_args,        1, "dump CPU exception trace ring"        },
};

// --- translate ---
static const struct arg_spec translate_args[] = {
    {"address", ARG_STRING,                "logical address (or --reverse <physical>)"},
    {"flag",    ARG_STRING | ARG_OPTIONAL, "optional --reverse flag"                  },
};

// --- addrmode ---
static const char *addrmode_values[] = {"auto", "expanded", "collapsed", NULL};
static const struct arg_spec addrmode_args[] = {
    {"mode", ARG_ENUM | ARG_OPTIONAL, "display mode", addrmode_values},
};

// --- prompt ---
static const char *prompt_values[] = {"on", "off", NULL};
static const struct arg_spec prompt_args[] = {
    {"state", ARG_ENUM | ARG_OPTIONAL, "on or off", prompt_values},
};

// --- screenshot ---
static const struct arg_spec screenshot_save_args[] = {
    {"filename", ARG_PATH, "output PNG filename"},
};
static const struct arg_spec screenshot_checksum_args[] = {
    {"top",    ARG_INT | ARG_OPTIONAL, "region top"   },
    {"left",   ARG_INT | ARG_OPTIONAL, "region left"  },
    {"bottom", ARG_INT | ARG_OPTIONAL, "region bottom"},
    {"right",  ARG_INT | ARG_OPTIONAL, "region right" },
};
static const struct arg_spec screenshot_match_args[] = {
    {"reference", ARG_PATH, "reference PNG file"},
};
static const struct arg_spec screenshot_match_or_save_args[] = {
    {"reference", ARG_PATH,                "reference PNG file"     },
    {"actual",    ARG_PATH | ARG_OPTIONAL, "save actual on mismatch"},
};
static const struct subcmd_spec screenshot_subcmds[] = {
    {NULL,            NULL, screenshot_save_args,          1, "save screen as PNG"                      },
    {"save",          NULL, screenshot_save_args,          1, "save screen as PNG"                      },
    {"checksum",      NULL, screenshot_checksum_args,      4, "compute screen checksum"                 },
    {"match",         NULL, screenshot_match_args,         1, "compare with reference PNG"              },
    {"match-or-save", NULL, screenshot_match_or_save_args, 2, "compare with reference, save on mismatch"},
};

// --- trace ---
static const char *trace_action_values[] = {"start", "stop", NULL};
static const struct arg_spec trace_start_args[] = {
    {"action", ARG_STRING,              "start, stop, or show"    },
    {"file",   ARG_PATH | ARG_OPTIONAL, "output filename for show"},
};

// --- find ---
// Handler parses raw_argv directly: "bytes" consumes a variable number of hex
// tokens, and the trailing range/"all" slot is positional but optional.  The
// arg specs below are loose so the framework doesn't reject legitimate forms.
void cmd_find_handler(struct cmd_context *ctx, struct cmd_result *res);
static const char *find_aliases[] = {"f", NULL};
static const struct arg_spec find_str_args[] = {
    {"text", ARG_STRING,              "literal ASCII/UTF-8 text to search for"},
    {"rest", ARG_REST | ARG_OPTIONAL, "[range] [all]"                         },
};
static const struct arg_spec find_bytes_args[] = {
    {"bytes", ARG_REST, "2-digit hex tokens, optional [range] [all]"},
};
// find word / find long share the same shape: one numeric value then optional [range] [all].
static const struct arg_spec find_value_args[] = {
    {"value", ARG_STRING,              "numeric value ($hex, 0xhex, or bare hex)"},
    {"rest",  ARG_REST | ARG_OPTIONAL, "[range] [all]"                           },
};
static const struct subcmd_spec find_subcmds[] = {
    {"str",   NULL, find_str_args,   2, "find ASCII/UTF-8 text"             },
    {"bytes", NULL, find_bytes_args, 1, "find a byte sequence (2-digit hex)"},
    {"word",  NULL, find_value_args, 2, "find a 16-bit big-endian value"    },
    {"long",  NULL, find_value_args, 2, "find a 32-bit big-endian value"    },
};

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

debug_t *debug_init(void) {
    debug_t *debug = (debug_t *)malloc(sizeof(debug_t));
    if (!debug) {
        return NULL;
    }
    // Zero-initialize all fields to match behavior when struct was embedded
    memset(debug, 0, sizeof(debug_t));

    // Command registrations

    // Execution control
    register_command(&(struct cmd_reg){
        .name = "continue",
        .aliases = continue_aliases,
        .category = "Execution",
        .synopsis = "Resume execution",
        .fn = cmd_continue_handler,
        .args = continue_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "step",
        .aliases = step_aliases,
        .category = "Execution",
        .synopsis = "Step one (or N) instructions",
        .fn = cmd_step_handler,
        .args = step_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "next",
        .aliases = next_aliases,
        .category = "Execution",
        .synopsis = "Step over subroutine call",
        .fn = cmd_next_handler,
    });
    register_command(&(struct cmd_reg){
        .name = "finish",
        .aliases = finish_aliases,
        .category = "Execution",
        .synopsis = "Run until current subroutine returns",
        .fn = cmd_finish_handler,
    });
    register_command(&(struct cmd_reg){
        .name = "until",
        .aliases = until_aliases,
        .category = "Execution",
        .synopsis = "Run until address is reached",
        .fn = cmd_until_handler,
        .args = until_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "stop",
        .aliases = stop_aliases,
        .category = "Execution",
        .synopsis = "Stop execution immediately",
        .fn = cmd_stop_handler,
    });
    register_command(&(struct cmd_reg){
        .name = "advance",
        .category = "Execution",
        .synopsis = "Run until memory condition is met",
        .simple_fn = cmd_run_until,
        .args = advance_args,
        .nargs = 3,
    });

    // Breakpoints and watchpoints
    register_command(&(struct cmd_reg){
        .name = "break",
        .aliases = break_aliases,
        .category = "Breakpoints",
        .synopsis = "Manage breakpoints (set/del/list/clear)",
        .fn = cmd_break_handler,
        .subcmds = break_subcmds,
        .n_subcmds = 5,
    });
    register_command(&(struct cmd_reg){
        .name = "tbreak",
        .aliases = tbreak_aliases,
        .category = "Breakpoints",
        .synopsis = "Set temporary breakpoint (auto-deletes on hit)",
        .fn = cmd_tbreak_handler,
        .args = tbreak_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "watch",
        .aliases = watch_aliases,
        .category = "Breakpoints",
        .synopsis = "Set memory watchpoint",
        .simple_fn = cmd_run_until,
        .args = advance_args,
        .nargs = 3,
    });
    register_command(&(struct cmd_reg){
        .name = "logpoint",
        .aliases = logpoint_aliases,
        .category = "Breakpoints",
        .synopsis = "Manage logpoints (set/del/list/clear)",
        .fn = cmd_logpoint_handler,
        .subcmds = logpoint_subcmds,
        .n_subcmds = 5,
    });
    register_command(&(struct cmd_reg){
        .name = "value-trap",
        .category = "Breakpoints",
        .synopsis = "Catch a specific (PA, size, value) write on the fast path (no per-page slowdown)",
        .fn = cmd_value_trap_handler,
    });

    // Inspection
    register_command(&(struct cmd_reg){
        .name = "print",
        .aliases = print_aliases,
        .category = "Inspection",
        .synopsis = "Print a register, global, or memory value",
        .fn = cmd_print_handler,
        .args = print_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "assert",
        .category = "Inspection",
        .synopsis = "Assert that a register/global matches an expected value (fails script on mismatch)",
        .fn = cmd_assert_handler,
        .args = assert_args,
        .nargs = 3,
    });
    register_command(&(struct cmd_reg){
        .name = "examine",
        .aliases = examine_aliases,
        .category = "Inspection",
        .synopsis = "Examine raw memory (hex + ASCII)",
        .fn = cmd_examine_handler,
        .args = examine_args,
        .nargs = 2,
    });
    register_command(&(struct cmd_reg){
        .name = "disasm",
        .aliases = disasm_aliases,
        .category = "Inspection",
        .synopsis = "Disassemble instructions",
        .fn = cmd_disasm_handler,
        .args = disasm_args,
        .nargs = 2,
    });
    register_command(&(struct cmd_reg){
        .name = "set",
        .category = "Inspection",
        .synopsis = "Set register, flag, or memory value",
        .simple_fn = cmd_set,
        .args = set_cmd_args,
        .nargs = 2,
    });
    register_command(&(struct cmd_reg){
        .name = "info",
        .aliases = info_aliases,
        .category = "Inspection",
        .synopsis = "Show state (regs, fpregs, mmu, mmu-walk, mmu-map, break, mac, process, events, exceptions)",
        .fn = cmd_info_handler,
        .subcmds = info_subcmds,
        .n_subcmds = 13,
    });

    // Tracing
    register_command(&(struct cmd_reg){
        .name = "trace",
        .category = "Tracing",
        .synopsis = "Control instruction tracing (start/stop/show)",
        .simple_fn = cmd_trace,
        .args = trace_start_args,
        .nargs = 2,
    });

    // Display
    register_command(&(struct cmd_reg){
        .name = "translate",
        .category = "Display",
        .synopsis = "Show MMU address translation",
        .simple_fn = cmd_translate,
        .args = translate_args,
        .nargs = 2,
    });
    register_command(&(struct cmd_reg){
        .name = "addrmode",
        .category = "Display",
        .synopsis = "Set address display format",
        .simple_fn = cmd_addrmode,
        .args = addrmode_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "prompt",
        .category = "Display",
        .synopsis = "Toggle PC disassembly after steps",
        .simple_fn = cmd_prompt,
        .args = prompt_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "screenshot",
        .category = "Display",
        .synopsis = "Save screen snapshot or compute checksum",
        .fn = cmd_screenshot_handler,
        .subcmds = screenshot_subcmds,
        .n_subcmds = 5,
    });
    register_command(&(struct cmd_reg){
        .name = "find",
        .aliases = find_aliases,
        .category = "Inspection",
        .synopsis = "Search memory for a string, byte sequence, or numeric value (find str|bytes|word|long)",
        .fn = cmd_find_handler,
        .subcmds = find_subcmds,
        .n_subcmds = 4,
    });

    debug_mac_init();

    // Install memory-logpoint hook so the memory slow path can emit logs
    g_mem_logpoint_hook = debug_memory_logpoint_hook;

    return debug;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free all resources allocated by debug_init
void debug_cleanup(debug_t *debug) {
    if (!debug)
        return;

    // Free all breakpoints
    breakpoint_t *bp = debug->breakpoints;
    while (bp) {
        breakpoint_t *next = bp->next;
        free(bp);
        bp = next;
    }
    debug->breakpoints = NULL;

    // Free all logpoints (including their messages and memory-page refcounts)
    logpoint_t *lp = debug->logpoints;
    while (lp) {
        logpoint_t *next = lp->next;
        free_logpoint(lp);
        lp = next;
    }
    debug->logpoints = NULL;
    g_mem_logpoint_hook = NULL;

    // Free trace log buffer entries
    if (debug->trace_log_buffer) {
        for (uint32_t i = 0; i < debug->trace_log_buffer_size; i++) {
            if (debug->trace_log_buffer[i].text) {
                free(debug->trace_log_buffer[i].text);
            }
        }
        free(debug->trace_log_buffer);
        debug->trace_log_buffer = NULL;
    }

    // Free trace entries buffer
    if (debug->trace_entries) {
        free(debug->trace_entries);
        debug->trace_entries = NULL;
    }

    // Free trace buffer
    if (debug->trace_buffer) {
        free(debug->trace_buffer);
        debug->trace_buffer = NULL;
    }

    // Free the debug structure itself
    free(debug);
}

// ────────────────────────────────────────────────────────────────────────────
// Target instruction trace diagnostic output
// ────────────────────────────────────────────────────────────────────────────

// Print recent instruction trace for debugging
void debug_print_target_trace(void) {
    if (!system_is_initialized())
        return;
    debug_t *dbg = system_debug();
    if (!dbg || !dbg->trace_buffer) {
        return;
    }

    printf("\n=== Target 68K instruction trace (most recent last) ===\n");

    if (dbg->trace_head == dbg->trace_tail) {
        printf("(empty)\n");
        return;
    }

    int i;
    for (i = dbg->trace_tail; i != dbg->trace_head; i = (i + 1) % dbg->trace_buffer_size) {
        char buf[160];
        debugger_disasm(buf, sizeof(buf), dbg->trace_buffer[i]);
        printf("%s\n", buf);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Assertion failure handler (coordinates all diagnostic output)
// ────────────────────────────────────────────────────────────────────────────

// Main assertion failure handler - prints diagnostics and pauses execution
void gs_assert_fail(const char *expr, const char *file, int line, const char *func, const char *fmt, ...) {
    // Header
    printf("\n\n==================== ASSERT ====================\n");
    if (expr && *expr)
        printf("Assertion failed: (%s)\n", expr);
    else
        printf("Assertion failed\n");
    printf("at %s:%d in %s\n", file ? file : "<unknown>", line, func ? func : "<unknown>");

    // Optional message
    if (fmt) {
        printf("Message: ");
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }

    // Diagnostics - call each module's diagnostic function
    platform_print_host_callstack();
    debug_mac_print_target_backtrace();
    debug_mac_print_process_info_header();
    debug_print_target_trace();

    printf("================================================\n\n");

    bool paused = false;
    scheduler_t *sched = system_scheduler();
    if (sched) {
        if (scheduler_is_running(sched)) {
            scheduler_stop(sched);
            paused = true;
        }
    }

    if (paused) {
        printf("Emulation paused due to assertion; returning control to shell.\n");
    } else {
        printf("Assertion handled while scheduler idle; shell remains available.\n");
    }
    fflush(stdout);

    // Notify platform layer about assertion failure (e.g., for test integration)
    debug_t *debug = system_debug();
    if (debug && debug->assertion_callback) {
        debug->assertion_callback(expr, file, line, func);
    }
}
