// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug.c
// Debugging, breakpoints, logpoints, and trace functionality.

// ============================================================================
// Includes
// ============================================================================

#include "debug.h"

#include "addr_format.h"
#include "alias.h"
#include "cmd_io.h"
#include "cmd_parse.h"
#include "cmd_symbol.h"
#include "cmd_types.h"
#include "common.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "debug_mac.h"
#include "expr.h"
#include "fpu.h"
#include "gs_classes.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "object.h"
#include "scheduler.h"
#include "shell.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

// Forward declarations — class descriptors are at the bottom of the file but
// debug_init / debug_cleanup reference them.
extern const class_desc_t debug_class;
extern const class_desc_t bp_collection_class;
extern const class_desc_t lp_collection_class;
extern const class_desc_t debug_mac_class;
extern const class_desc_t debug_mac_globals_class;

// Mac low-memory globals table (defined in mac_globals_data.c). Used by
// debug.mac.globals.{read,write,address,list}.
extern struct {
    const char *name;
    uint32_t address;
    int size;
    const char *description;
} mac_global_vars[];
extern const size_t mac_global_vars_count;

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

    // Hit counter — exposed via debug.breakpoints[N].hit_count.
    uint32_t hit_count;

    // Sparse stable id and the per-entry object_t that backs
    // `debug.breakpoints[id]`. The object is owned by this breakpoint;
    // freeing it fires invalidators on any held nodes.
    int id;
    struct object *entry_object;

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

    // Sparse stable id and the per-entry object_t. Same shape as the
    // breakpoint struct above.
    int id;
    struct object *entry_object;

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

    breakpoint_t *bp = calloc(1, sizeof(breakpoint_t));

    if (bp == NULL)
        return NULL;

    bp->addr = addr;
    bp->space = space;
    bp->condition = NULL;
    bp->hit_count = 0;
    bp->id = debug->next_breakpoint_id++;
    // The entry object is created lazily by gs_classes the first time
    // someone resolves debug.breakpoints[id]; we just hold the slot.
    bp->entry_object = gs_classes_make_breakpoint_object(bp);

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
    lp->id = debug->next_logpoint_id++;
    lp->entry_object = gs_classes_make_logpoint_object(lp);

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
    lp->id = debug->next_logpoint_id++;
    lp->entry_object = gs_classes_make_logpoint_object(lp);
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

// Adapter for expr_alias_fn — alias_lookup's signature differs because
// it also returns the alias kind via an out-pointer.
static const char *alias_lookup_for_expr(void *ud, const char *name) {
    (void)ud;
    return alias_lookup(name, NULL);
}

// Expand a logpoint message via the unified ${...} interpolator. Sets
// up the `lp` synthetic context with addr/value/size, then calls
// expr_interpolate_string against the object root with the alias
// resolver bound. Outside ${...} the message is literal — a bare `$`
// is just a dollar sign. Use ${cpu.pc}, ${lp.value},
// ${memory.peek.l(<addr>)}, ${memory.read_cstring(<addr>)}.
// Per-fire bindings exposed during logpoint message expansion. The
// three event-intrinsic values (`value`, `addr`, `size`) live nowhere
// else — there is no persistent object holding "the byte that just got
// written to 0x4000". The deferred interpolator queries this binding
// table for bare identifiers; everything else (registers, memory,
// peripherals) resolves through the normal object tree at fire time.
typedef struct {
    uint32_t addr;
    uint32_t value;
    unsigned size;
} lp_bindings_t;

static value_t lp_binding(void *ud, const char *name) {
    const lp_bindings_t *lp = (const lp_bindings_t *)ud;
    if (strcmp(name, "value") == 0) {
        // Mask the value to its declared width so 1/2-byte writes don't
        // print as full 32-bit words.
        uint64_t mask = (lp->size >= 4) ? 0xFFFFFFFFu : ((1u << (lp->size * 8)) - 1u);
        int w = (lp->size == 1) ? 1 : (lp->size == 2) ? 2 : 4;
        value_t v = val_uint((uint8_t)w, lp->value & mask);
        v.flags |= VAL_HEX;
        return v;
    }
    if (strcmp(name, "addr") == 0) {
        value_t v = val_uint(4, lp->addr);
        v.flags |= VAL_HEX;
        return v;
    }
    if (strcmp(name, "size") == 0)
        return val_uint(1, lp->size);
    return val_none();
}

static void format_logpoint_message(char *buf, size_t buf_size, const char *msg, uint32_t addr, uint32_t value,
                                    unsigned size) {
    if (!msg) {
        buf[0] = '\0';
        return;
    }
    lp_bindings_t lp = {.addr = addr, .value = value, .size = size};
    expr_ctx_t ctx = {
        .root = object_root(),
        .alias = alias_lookup_for_expr,
        .alias_ud = NULL,
        .binding = lp_binding,
        .binding_ud = &lp,
    };
    value_t v = expr_interpolate_string(msg, &ctx);

    const char *s = (v.kind == V_STRING && v.s) ? v.s : "";
    size_t n = strlen(s);
    if (n >= buf_size)
        n = buf_size - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    value_free(&v);
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

extern int cpu_disasm(uint16_t *instr, char *buf);
extern int disasm_68000(uint16_t *instr, char *buf);

// Forward declarations for trace functions
static void trace_add_pc_entry(debug_t *debug, uint32_t pc);

// Forward declarations for logpoint management (IMP-604)
void list_logpoints(debug_t *debug);
static int delete_logpoint(debug_t *debug, uint32_t addr);
int delete_all_logpoints(debug_t *debug);

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
                bp->hit_count++;
                if (bp->space == ADDR_PHYSICAL) {
                    printf("breakpoint hit at P:$%08X (PC=$%08X)\n", bp->addr, current_pc);
                } else {
                    printf("breakpoint hit at $%08X\n", bp->addr);
                }
                // Remember this PC to skip it next time we check
                debug->last_breakpoint_pc = current_pc;
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

// Helper: free one breakpoint, releasing its per-entry object_t (which
// fires invalidators on any held node_t). Used by every code path that
// removes a breakpoint so the invalidation discipline is uniform.
static void free_breakpoint(breakpoint_t *bp) {
    if (!bp)
        return;
    if (bp->entry_object) {
        object_delete(bp->entry_object);
        bp->entry_object = NULL;
    }
    if (bp->condition)
        free(bp->condition);
    free(bp);
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
            free_breakpoint(bp);
            return true;
        }
        prev = bp;
        bp = bp->next;
    }
    return false;
}

// Delete breakpoint by sparse stable id (proposal §2.1). Walks the list
// matching `bp->id` rather than the position-in-list — positions shift
// when other entries are removed, ids do not.
static bool delete_breakpoint_by_id(debug_t *debug, int id) {
    breakpoint_t **pp = &debug->breakpoints;
    while (*pp) {
        if ((*pp)->id == id) {
            breakpoint_t *bp = *pp;
            *pp = bp->next;
            free_breakpoint(bp);
            return true;
        }
        pp = &(*pp)->next;
    }
    return false;
}

// Delete all breakpoints
// Returns the number of breakpoints deleted
int delete_all_breakpoints(debug_t *debug) {
    int count = 0;
    breakpoint_t *bp = debug->breakpoints;

    while (bp != NULL) {
        breakpoint_t *next = bp->next;
        free_breakpoint(bp);
        bp = next;
        count++;
    }
    debug->breakpoints = NULL;
    return count;
}

// List all breakpoints
void list_breakpoints(debug_t *debug) {
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

// Helper function to convert string to uppercase (for case-insensitive comparisons)
static void str_to_upper(char *dest, const char *src) {
    while (*src) {
        *dest++ = (*src >= 'a' && *src <= 'z') ? (*src - 32) : *src;
        src++;
    }
    *dest = '\0';
}

uint64_t cmd_set(int argc, char *argv[]) {
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
uint32_t framebuffer_checksum(const uint8_t *fb) {
    uint32_t checksum = 0;
    for (int i = 0; i < FB_BYTES; i++) {
        checksum = checksum * 31 + fb[i];
    }
    return checksum;
}

// Calculate checksum for a region of the framebuffer (top, left, bottom, right)
// Region is specified in pixels; checksum operates on the 1-bit packed data
uint32_t framebuffer_region_checksum(const uint8_t *fb, int top, int left, int bottom, int right) {
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
int match_framebuffer_with_png(const uint8_t *fb, const char *filename) {
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
int save_framebuffer_as_png(const uint8_t *fb, const char *filename) {
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
void list_logpoints(debug_t *debug) {
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
    if (lp->entry_object) {
        object_delete(lp->entry_object);
        lp->entry_object = NULL;
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

// Delete logpoint by sparse stable id (proposal §2.1). Same shape as
// delete_breakpoint_by_id — match `lp->id`, not list position.
static int delete_logpoint_by_id(debug_t *debug, int id) {
    logpoint_t **pp = &debug->logpoints;
    while (*pp) {
        if ((*pp)->id == id) {
            logpoint_t *lp = *pp;
            *pp = lp->next;
            free_logpoint(lp);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

// Delete all logpoints; returns the number deleted (matches the
// breakpoint counterpart so typed wrappers can report the count).
int delete_all_logpoints(debug_t *debug) {
    int count = 0;
    logpoint_t *lp = debug ? debug->logpoints : NULL;
    while (lp) {
        logpoint_t *next = lp->next;
        free_logpoint(lp);
        lp = next;
        count++;
    }
    if (debug)
        debug->logpoints = NULL;
    return count;
}

// ============================================================================
// Mac state summary (IMP-702)
// ============================================================================

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
// Truthiness for the new predicate form. The shell's $(...) expansion
// has already converted the result to a string before reaching us; we
// match the proposal §2.5 truthiness rules on that string.
bool predicate_is_truthy(const char *s) {
    if (!s || !*s)
        return false;
    if (strcmp(s, "false") == 0 || strcmp(s, "0") == 0)
        return false;
    if (strcmp(s, "<error") == 0)
        return false; // formatted error tail
    if (strncmp(s, "<error:", 7) == 0)
        return false;
    return true;
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
        // Tokens of the form `<known-key>=<value>` are recognised as
        // named parameters. Anything else — including a message like
        // "pc=${cpu.pc}" that happens to contain `=` — is treated as
        // the message body. ${...} interpolation in a message naturally
        // contains `key=value` text patterns that pre-existing logic
        // would have mis-parsed.
        char *equals = strchr(a, '=');
        bool is_named = false;
        const char *key = NULL, *value = NULL;
        if (equals) {
            size_t klen = (size_t)(equals - a);
            if ((klen == 8 && memcmp(a, "category", 8) == 0) || (klen == 5 && memcmp(a, "level", 5) == 0) ||
                (klen == 5 && memcmp(a, "value", 5) == 0)) {
                is_named = true;
                *equals = '\0';
                key = a;
                value = equals + 1;
            }
        }
        if (is_named) {
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

// ============================================================================
// Command Registration Tables
// ============================================================================

// --- print ---
static const struct arg_spec print_args[] = {
    {"target", ARG_SYMBOL, "register, Mac global, or address.size"},
};

// --- examine ---
static const struct arg_spec examine_args[] = {
    {"address", ARG_ADDR,               "start address"                         },
    {"count",   ARG_INT | ARG_OPTIONAL, "bytes to display (default 64, max 512)"},
};

// --- logpoint ---
// Handler drives off raw_argv (supports --write/--read flags, ranges, key=val
// options, and substitutions in the message).  The spec below is loose so the
// framework doesn't reject legitimate combinations.
static const struct arg_spec lp_set_args[] = {
    {"target",  ARG_STRING | ARG_OPTIONAL, "[--write|--read] <address[.b|.w|.l]>"                             },
    {"message", ARG_REST | ARG_OPTIONAL,   "log message (${cpu.pc}, ${lp.value}, ${lp.addr}, ${...:08x}, ...)"},
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

// --- find ---
// Handler parses raw_argv directly: "bytes" consumes a variable number of hex
// tokens, and the trailing range/"all" slot is positional but optional.  The
// arg specs below are loose so the framework doesn't reject legitimate forms.
void cmd_find_handler(struct cmd_context *ctx, struct cmd_result *res);
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
// Object-model accessors and id-based collection helpers
// ============================================================================
//
// These implement the `debug.{breakpoints,logpoints}` indexed-child
// surface declared in debug.h. Walks are O(N) over the linked list;
// fanout is bounded by user-set entries (~tens) so this is fine.

int debug_alloc_breakpoint_id(debug_t *debug) {
    return debug ? debug->next_breakpoint_id++ : -1;
}
int debug_alloc_logpoint_id(debug_t *debug) {
    return debug ? debug->next_logpoint_id++ : -1;
}

breakpoint_t *debug_breakpoint_by_id(debug_t *debug, int id) {
    if (!debug)
        return NULL;
    for (breakpoint_t *bp = debug->breakpoints; bp; bp = bp->next)
        if (bp->id == id)
            return bp;
    return NULL;
}

logpoint_t *debug_logpoint_by_id(debug_t *debug, int id) {
    if (!debug)
        return NULL;
    for (logpoint_t *lp = debug->logpoints; lp; lp = lp->next)
        if (lp->id == id)
            return lp;
    return NULL;
}

int debug_breakpoint_count(debug_t *debug) {
    int n = 0;
    if (!debug)
        return 0;
    for (breakpoint_t *bp = debug->breakpoints; bp; bp = bp->next)
        n++;
    return n;
}

int debug_logpoint_count(debug_t *debug) {
    int n = 0;
    if (!debug)
        return 0;
    for (logpoint_t *lp = debug->logpoints; lp; lp = lp->next)
        n++;
    return n;
}

// next(prev) — return the smallest live id strictly greater than `prev`
// (or any live id if `prev<0`). Linear scan because the list is in
// add-order, not id order: callers iterating via next() get a stable
// ascending sequence even when entries were removed and re-added.
int debug_breakpoint_next_id(debug_t *debug, int prev_id) {
    if (!debug)
        return -1;
    int best = -1;
    for (breakpoint_t *bp = debug->breakpoints; bp; bp = bp->next) {
        if (bp->id <= prev_id)
            continue;
        if (best < 0 || bp->id < best)
            best = bp->id;
    }
    return best;
}

int debug_logpoint_next_id(debug_t *debug, int prev_id) {
    if (!debug)
        return -1;
    int best = -1;
    for (logpoint_t *lp = debug->logpoints; lp; lp = lp->next) {
        if (lp->id <= prev_id)
            continue;
        if (best < 0 || lp->id < best)
            best = lp->id;
    }
    return best;
}

bool debug_remove_breakpoint(debug_t *debug, int id) {
    if (!debug)
        return false;
    return delete_breakpoint_by_id(debug, id);
}

bool debug_remove_logpoint(debug_t *debug, int id) {
    if (!debug)
        return false;
    return delete_logpoint_by_id(debug, id) == 0;
}

uint32_t breakpoint_get_addr(const breakpoint_t *bp) {
    return bp ? bp->addr : 0;
}
int breakpoint_get_space(const breakpoint_t *bp) {
    return bp ? (bp->space == ADDR_PHYSICAL ? 1 : 0) : 0;
}
const char *breakpoint_get_condition(const breakpoint_t *bp) {
    return bp ? bp->condition : NULL;
}
uint32_t breakpoint_get_hit_count(const breakpoint_t *bp) {
    return bp ? bp->hit_count : 0;
}
int breakpoint_get_id(const breakpoint_t *bp) {
    return bp ? bp->id : -1;
}
struct object *breakpoint_get_entry_object(const breakpoint_t *bp) {
    return bp ? bp->entry_object : NULL;
}
void breakpoint_set_condition(breakpoint_t *bp, const char *expr) {
    if (!bp)
        return;
    char *copy = (expr && *expr) ? strdup(expr) : NULL;
    if (bp->condition)
        free(bp->condition);
    bp->condition = copy;
}

uint32_t logpoint_get_addr(const logpoint_t *lp) {
    return lp ? lp->addr : 0;
}
uint32_t logpoint_get_end_addr(const logpoint_t *lp) {
    return lp ? lp->end_addr : 0;
}
int logpoint_get_kind(const logpoint_t *lp) {
    return lp ? lp->kind : 0;
}
int logpoint_get_level(const logpoint_t *lp) {
    return lp ? lp->level : 0;
}
const char *logpoint_get_category_name(const logpoint_t *lp) {
    if (!lp || !lp->category)
        return NULL;
    return log_category_name(lp->category);
}
const char *logpoint_get_message(const logpoint_t *lp) {
    return lp ? lp->message : NULL;
}
uint32_t logpoint_get_hit_count(const logpoint_t *lp) {
    return lp ? lp->hit_count : 0;
}
int logpoint_get_id(const logpoint_t *lp) {
    return lp ? lp->id : -1;
}
struct object *logpoint_get_entry_object(const logpoint_t *lp) {
    return lp ? lp->entry_object : NULL;
}

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

    // Phase 5c — legacy debugger / inspection / display shell command
    // registrations retired. The typed object-model bridge surfaces
    // every operation; cmd_*_handler bodies remain for the
    // shell_*_argv apply functions used by the typed wrappers.

    debug_mac_init();

    // Install memory-logpoint hook so the memory slow path can emit logs
    g_mem_logpoint_hook = debug_memory_logpoint_hook;

    // Object-tree binding — instance_data on the debug node and its
    // collection / mac children is the debug_t* itself.
    debug->object = object_new(&debug_class, debug, "debug");
    if (debug->object) {
        object_attach(object_root(), debug->object);
        debug->bp_collection_object = object_new(&bp_collection_class, debug, "breakpoints");
        if (debug->bp_collection_object)
            object_attach(debug->object, debug->bp_collection_object);
        debug->lp_collection_object = object_new(&lp_collection_class, debug, "logpoints");
        if (debug->lp_collection_object)
            object_attach(debug->object, debug->lp_collection_object);
        debug->mac_object = object_new(&debug_mac_class, debug, "mac");
        if (debug->mac_object) {
            object_attach(debug->object, debug->mac_object);
            debug->mac_globals_object = object_new(&debug_mac_globals_class, debug, "globals");
            if (debug->mac_globals_object)
                object_attach(debug->mac_object, debug->mac_globals_object);
        }
    }

    return debug;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free all resources allocated by debug_init
void debug_cleanup(debug_t *debug) {
    if (!debug)
        return;

    // Tear down object-tree nodes before any of the underlying storage
    // is freed (entry objects fired by object_delete reference the
    // breakpoint_t / logpoint_t state). Children first, then root.
    if (debug->mac_globals_object) {
        object_detach(debug->mac_globals_object);
        object_delete(debug->mac_globals_object);
        debug->mac_globals_object = NULL;
    }
    if (debug->mac_object) {
        object_detach(debug->mac_object);
        object_delete(debug->mac_object);
        debug->mac_object = NULL;
    }
    if (debug->lp_collection_object) {
        object_detach(debug->lp_collection_object);
        object_delete(debug->lp_collection_object);
        debug->lp_collection_object = NULL;
    }
    if (debug->bp_collection_object) {
        object_detach(debug->bp_collection_object);
        object_delete(debug->bp_collection_object);
        debug->bp_collection_object = NULL;
    }
    if (debug->object) {
        object_detach(debug->object);
        object_delete(debug->object);
        debug->object = NULL;
    }

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

// ===== argv-driven entry points for the typed object-model bridge =====
// These bypass shell_dispatch / find_cmd: the typed wrappers tokenize
// their spec strings via `tokenize` and call into these helpers directly.

// Run a cmd_fn handler with a fresh ctx/result/io. Returns the integer
// from cmd_int (when set), 0 on RES_OK, -1 on parse error or RES_ERR.
static int64_t run_handler(cmd_fn fn, const struct cmd_reg *reg, int argc, char **argv) {
    struct cmd_io io;
    init_cmd_io(&io);
    struct cmd_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = io.out_stream;
    ctx.err = io.err_stream;
    struct cmd_result res;
    memset(&res, 0, sizeof(res));
    res.type = RES_OK;
    if (cmd_parse_args(argc, argv, reg, &ctx, &res))
        fn(&ctx, &res);
    finalize_cmd_io(&io, &res);
    if (res.type == RES_ERR) {
        if (res.as_str)
            fprintf(stderr, "%s\n", res.as_str);
        return -1;
    }
    if (res.type == RES_INT)
        return res.as_int;
    return 0;
}

int64_t shell_print_argv(int argc, char **argv) {
    static const struct cmd_reg reg = {
        .name = "print",
        .fn = cmd_print_handler,
        .args = print_args,
        .nargs = 1,
    };
    return run_handler(cmd_print_handler, &reg, argc, argv);
}

int shell_examine_argv(int argc, char **argv) {
    static const struct cmd_reg reg = {
        .name = "examine",
        .fn = cmd_examine_handler,
        .args = examine_args,
        .nargs = 2,
    };
    return (int)run_handler(cmd_examine_handler, &reg, argc, argv);
}

int shell_logpoint_argv(int argc, char **argv) {
    static const struct cmd_reg reg = {
        .name = "logpoint",
        .fn = cmd_logpoint_handler,
        .subcmds = logpoint_subcmds,
        .n_subcmds = 5,
    };
    return (int)run_handler(cmd_logpoint_handler, &reg, argc, argv);
}

int shell_find_argv(int argc, char **argv) {
    static const struct cmd_reg reg = {
        .name = "find",
        .fn = cmd_find_handler,
        .subcmds = find_subcmds,
        .n_subcmds = 4,
    };
    return (int)run_handler(cmd_find_handler, &reg, argc, argv);
}

// === Object-model class descriptors =========================================
//
// `debug.breakpoints` / `debug.logpoints` are indexed children
// with sparse stable indices. Each entry is its own object_t whose
// instance_data is the underlying breakpoint_t / logpoint_t. The entry
// classes carry the per-entry attributes (addr, condition, hit_count,
// …) and a `remove()` method.

// --- breakpoint entry class -------------------------------------------------

static breakpoint_t *bp_from(struct object *self) {
    return (breakpoint_t *)object_data(self);
}

static value_t bp_attr_addr(struct object *self, const member_t *m) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint detached");
    value_t v = val_uint(4, breakpoint_get_addr(bp));
    v.flags |= VAL_HEX;
    return v;
}

static value_t bp_attr_space(struct object *self, const member_t *m) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint detached");
    static const char *const names[] = {"logical", "physical"};
    int idx = breakpoint_get_space(bp);
    return val_enum(idx, names, 2);
}

static value_t bp_attr_condition(struct object *self, const member_t *m) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint detached");
    const char *c = breakpoint_get_condition(bp);
    return val_str(c ? c : "");
}

static value_t bp_attr_condition_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp) {
        value_free(&in);
        return val_err("breakpoint detached");
    }
    if (in.kind != V_STRING && in.kind != V_NONE) {
        value_free(&in);
        return val_err("condition must be a string");
    }
    breakpoint_set_condition(bp, (in.kind == V_STRING) ? in.s : NULL);
    value_free(&in);
    return val_none();
}

static value_t bp_attr_hit_count(struct object *self, const member_t *m) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint detached");
    return val_uint(4, breakpoint_get_hit_count(bp));
}

static value_t bp_attr_id(struct object *self, const member_t *m) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint detached");
    return val_int(breakpoint_get_id(bp));
}

static value_t bp_method_remove(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint already removed");
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("debugger not initialised");
    int id = breakpoint_get_id(bp);
    if (!debug_remove_breakpoint(debug, id))
        return val_err("breakpoint #%d not found", id);
    return val_none();
}

static const member_t bp_entry_members[] = {
    {.kind = M_ATTR,
     .name = "addr",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = bp_attr_addr, .set = NULL}},
    {.kind = M_ATTR, .name = "space", .flags = VAL_RO, .attr = {.type = V_ENUM, .get = bp_attr_space, .set = NULL}},
    {.kind = M_ATTR,
     .name = "condition",
     .flags = 0,
     .attr = {.type = V_STRING, .get = bp_attr_condition, .set = bp_attr_condition_set}},
    {.kind = M_ATTR,
     .name = "hit_count",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = bp_attr_hit_count, .set = NULL}},
    {.kind = M_ATTR, .name = "id", .flags = VAL_RO, .attr = {.type = V_INT, .get = bp_attr_id, .set = NULL}},
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Remove this breakpoint",
     .flags = 0,
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = bp_method_remove}},
};

const class_desc_t breakpoint_entry_class = {
    .name = "breakpoint",
    .members = bp_entry_members,
    .n_members = sizeof(bp_entry_members) / sizeof(bp_entry_members[0]),
};

struct object *gs_classes_make_breakpoint_object(struct breakpoint *bp) {
    if (!bp)
        return NULL;
    return object_new(&breakpoint_entry_class, bp, NULL);
}

// --- logpoint entry class ---------------------------------------------------

static logpoint_t *lp_from(struct object *self) {
    return (logpoint_t *)object_data(self);
}

static value_t lpe_attr_addr(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    value_t v = val_uint(4, logpoint_get_addr(lp));
    v.flags |= VAL_HEX;
    return v;
}
static value_t lpe_attr_end_addr(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    value_t v = val_uint(4, logpoint_get_end_addr(lp));
    v.flags |= VAL_HEX;
    return v;
}
static value_t lpe_attr_kind(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    static const char *const names[] = {"pc", "write", "read", "rw"};
    int idx = logpoint_get_kind(lp);
    if (idx < 0 || idx > 3)
        idx = 0;
    return val_enum(idx, names, 4);
}
static value_t lpe_attr_level(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    return val_int(logpoint_get_level(lp));
}
static value_t lpe_attr_category(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    const char *n = logpoint_get_category_name(lp);
    return val_str(n ? n : "");
}
static value_t lpe_attr_message(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    const char *s = logpoint_get_message(lp);
    return val_str(s ? s : "");
}
static value_t lpe_attr_hit_count(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    return val_uint(4, logpoint_get_hit_count(lp));
}
static value_t lpe_attr_id(struct object *self, const member_t *m) {
    (void)m;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint detached");
    return val_int(logpoint_get_id(lp));
}
static value_t lpe_method_remove(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    logpoint_t *lp = lp_from(self);
    if (!lp)
        return val_err("logpoint already removed");
    debug_t *debug = system_debug();
    if (!debug)
        return val_err("debugger not initialised");
    int id = logpoint_get_id(lp);
    if (!debug_remove_logpoint(debug, id))
        return val_err("logpoint #%d not found", id);
    return val_none();
}

static const member_t lp_entry_members[] = {
    {.kind = M_ATTR,
     .name = "addr",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = lpe_attr_addr, .set = NULL}},
    {.kind = M_ATTR,
     .name = "end_addr",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = lpe_attr_end_addr, .set = NULL}},
    {.kind = M_ATTR, .name = "kind", .flags = VAL_RO, .attr = {.type = V_ENUM, .get = lpe_attr_kind, .set = NULL}},
    {.kind = M_ATTR, .name = "level", .flags = VAL_RO, .attr = {.type = V_INT, .get = lpe_attr_level, .set = NULL}},
    {.kind = M_ATTR,
     .name = "category",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = lpe_attr_category, .set = NULL}},
    {.kind = M_ATTR,
     .name = "message",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = lpe_attr_message, .set = NULL}},
    {.kind = M_ATTR,
     .name = "hit_count",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = lpe_attr_hit_count, .set = NULL}},
    {.kind = M_ATTR, .name = "id", .flags = VAL_RO, .attr = {.type = V_INT, .get = lpe_attr_id, .set = NULL}},
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Remove this logpoint",
     .flags = 0,
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = lpe_method_remove}},
};

const class_desc_t logpoint_entry_class = {
    .name = "logpoint",
    .members = lp_entry_members,
    .n_members = sizeof(lp_entry_members) / sizeof(lp_entry_members[0]),
};

struct object *gs_classes_make_logpoint_object(struct logpoint *lp) {
    if (!lp)
        return NULL;
    return object_new(&logpoint_entry_class, lp, NULL);
}

// --- collection objects -----------------------------------------------------
//
// `debug.breakpoints` is a real object_t* attached to `debug` at
// debug_init time. Its instance_data is the debug_t* itself, so a
// single helper recovers it whether you're holding the debug node
// or one of its collection children. The collection class declares:
//   - method members (add, clear) for the legacy mutation API;
//   - one indexed M_CHILD member that exposes per-entry objects, so
//     `debug.breakpoints.0` and `[0]` both resolve via the integer-
//     segment rule in node_child (object.c).

static debug_t *debug_from(struct object *self) {
    return (debug_t *)object_data(self);
}

// Forward-declared because the indexed-child member descriptors below
// need it but the entry classes are already defined above.
static struct object *bp_entries_get(struct object *self, int index);
static int bp_entries_count(struct object *self);
static int bp_entries_next(struct object *self, int prev_index);
static struct object *lp_entries_get(struct object *self, int index);
static int lp_entries_count(struct object *self);
static int lp_entries_next(struct object *self, int prev_index);

static struct object *bp_entries_get(struct object *self, int index) {
    breakpoint_t *bp = debug_breakpoint_by_id(debug_from(self), index);
    return bp ? breakpoint_get_entry_object(bp) : NULL;
}
static int bp_entries_count(struct object *self) {
    return debug_breakpoint_count(debug_from(self));
}
static int bp_entries_next(struct object *self, int prev_index) {
    return debug_breakpoint_next_id(debug_from(self), prev_index);
}

static struct object *lp_entries_get(struct object *self, int index) {
    logpoint_t *lp = debug_logpoint_by_id(debug_from(self), index);
    return lp ? logpoint_get_entry_object(lp) : NULL;
}
static int lp_entries_count(struct object *self) {
    return debug_logpoint_count(debug_from(self));
}
static int lp_entries_next(struct object *self, int prev_index) {
    return debug_logpoint_next_id(debug_from(self), prev_index);
}

static value_t bp_method_add(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    debug_t *debug = debug_from(self);
    if (!debug)
        return val_err("debugger not initialised");
    if (argc < 1)
        return val_err("breakpoints.add: expected (addr, [condition], [space])");
    bool ok = true;
    uint64_t addr = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("breakpoints.add: addr is not numeric");
    // Optional `space` (3rd arg): "logical" (default) or "physical".
    // Physical-space breakpoints are only meaningful on the 68030 with
    // the MMU active; on the Plus the two address spaces coincide.
    addr_space_t space = ADDR_LOGICAL;
    if (argc >= 3 && argv[2].kind == V_STRING && argv[2].s && *argv[2].s) {
        if (strcmp(argv[2].s, "physical") == 0)
            space = ADDR_PHYSICAL;
        else if (strcmp(argv[2].s, "logical") != 0)
            return val_err("breakpoints.add: space must be \"logical\" or \"physical\"");
    }
    breakpoint_t *bp = set_breakpoint(debug, (uint32_t)addr, space);
    if (!bp)
        return val_err("breakpoints.add: allocation failed");
    if (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s)
        breakpoint_set_condition(bp, argv[1].s);
    return val_obj(breakpoint_get_entry_object(bp));
}

static value_t bp_method_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = debug_from(self);
    if (!debug)
        return val_err("debugger not initialised");
    int id;
    while ((id = debug_breakpoint_next_id(debug, -1)) >= 0)
        debug_remove_breakpoint(debug, id);
    return val_none();
}

static value_t bp_method_list(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = debug_from(self);
    if (!debug)
        return val_err("debugger not initialised");
    list_breakpoints(debug);
    return val_none();
}

static value_t lp_method_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = debug_from(self);
    if (!debug)
        return val_err("debugger not initialised");
    int id;
    while ((id = debug_logpoint_next_id(debug, -1)) >= 0)
        debug_remove_logpoint(debug, id);
    return val_none();
}

static value_t lp_method_list(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = debug_from(self);
    if (!debug)
        return val_err("debugger not initialised");
    list_logpoints(debug);
    return val_none();
}

// `debug.logpoints.add(spec)` — install a logpoint from the legacy spec
// string (e.g. `--write 0x000016A.l "Ticks bumped pc=${cpu.pc} ..." level=5`).
// Forwarded to shell_logpoint_argv which carries the rich-parser command body.
static value_t lp_method_add(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("logpoints.add: expected (spec_string)");
    char line[2048];
    int n = snprintf(line, sizeof(line), "logpoint %s", argv[0].s ? argv[0].s : "");
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("logpoints.add: argument too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("logpoints.add: empty spec");
    return val_bool(shell_logpoint_argv(targc, targv) == 0);
}

static const arg_decl_t bp_add_args[] = {
    {.name = "addr",      .kind = V_UINT,   .flags = VAL_HEX,          .doc = "address"                              },
    {.name = "condition", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "optional condition string"            },
    {.name = "space",     .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "\"logical\" (default) or \"physical\""},
};

static const arg_decl_t lp_add_args[] = {
    {.name = "spec",
     .kind = V_STRING,
     .doc = "Legacy logpoint spec string (e.g. `--write 0x000016A.l \"text\" level=5`)"},
};

static const member_t bp_collection_members[] = {
    {.kind = M_METHOD,
     .name = "add",
     .doc = "Add a breakpoint (logical-space by default; pass space=\"physical\" for the 68030 PMMU path)",
     .method = {.args = bp_add_args, .nargs = 3, .result = V_OBJECT, .fn = bp_method_add}},
    {.kind = M_METHOD,
     .name = "clear",
     .doc = "Remove every breakpoint",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = bp_method_clear}},
    {.kind = M_METHOD,
     .name = "list",
     .doc = "Print the breakpoint table",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = bp_method_list}},
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &breakpoint_entry_class,
               .indexed = true,
               .get = bp_entries_get,
               .count = bp_entries_count,
               .next = bp_entries_next,
               .lookup = NULL}},
};

const class_desc_t bp_collection_class = {
    .name = "breakpoints",
    .members = bp_collection_members,
    .n_members = sizeof(bp_collection_members) / sizeof(bp_collection_members[0]),
};

static const member_t lp_collection_members[] = {
    {.kind = M_METHOD,
     .name = "add",
     .doc = "Install a logpoint from a spec string",
     .method = {.args = lp_add_args, .nargs = 1, .result = V_BOOL, .fn = lp_method_add}},
    {.kind = M_METHOD,
     .name = "clear",
     .doc = "Remove every logpoint",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = lp_method_clear}},
    {.kind = M_METHOD,
     .name = "list",
     .doc = "Print the logpoint table",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = lp_method_list}},
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &logpoint_entry_class,
               .indexed = true,
               .get = lp_entries_get,
               .count = lp_entries_count,
               .next = lp_entries_next,
               .lookup = NULL}},
};

const class_desc_t lp_collection_class = {
    .name = "logpoints",
    .members = lp_collection_members,
    .n_members = sizeof(lp_collection_members) / sizeof(lp_collection_members[0]),
};

// `debug.log(category, level_or_spec)` — adjust per-subsystem log level.
// The second arg accepts either an integer level or a full named-arg spec
// string (e.g. `"level=5 file=/tmp/foo.txt stdout=off ts=on"`); spec
// strings are tokenised and forwarded to cmd_log directly.
static value_t debug_method_log(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING)
        return val_err("debug.log: expected (category, level | spec)");
    char line[512];
    int n;
    if (argv[1].kind == V_STRING) {
        n = snprintf(line, sizeof(line), "log %s %s", argv[0].s, argv[1].s ? argv[1].s : "");
    } else {
        bool ok = false;
        int64_t level = val_as_i64(&argv[1], &ok);
        if (!ok)
            return val_err("debug.log: second arg must be integer level or spec string");
        n = snprintf(line, sizeof(line), "log %s %lld", argv[0].s, (long long)level);
    }
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("debug.log: argument too long");
    char *targv[32];
    int targc = tokenize(line, targv, 32);
    if (targc <= 0)
        return val_err("debug.log: empty spec");
    return val_bool(cmd_log(targc, targv) == 0);
}

static const arg_decl_t debug_log_args[] = {
    {.name = "category", .kind = V_STRING, .doc = "Subsystem name (memory, logpoint, ...)"            },
    {.name = "level",    .kind = V_NONE,   .doc = "Integer level (0..5) or full named-arg spec string"},
};

// `debug.disasm([count])` — disassemble `count` instructions forward from
// the current PC (default 16). Lives under debug.* (not cpu.*) because it's
// a pure observation operation: same family as debug.breakpoints,
// debug.logpoints, debug.mac.* — debugger affordances that *use* the CPU's
// encoding knowledge but aren't themselves part of running the CPU.
static value_t debug_method_disasm(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t count = 16;
    if (argc >= 1) {
        bool ok = false;
        count = val_as_i64(&argv[0], &ok);
        if (!ok)
            return val_err("debug.disasm: count must be integer");
        if (count <= 0)
            count = 16;
    }
    cpu_t *cpu = system_cpu();
    if (!cpu)
        return val_err("debug.disasm: CPU not initialised");
    uint32_t addr = cpu_get_pc(cpu);
    char buf[160];
    for (int i = 0; i < (int)count; i++) {
        int instr_len = debugger_disasm(buf, sizeof(buf), addr);
        printf("%s\n", buf);
        addr += 2 * instr_len;
    }
    return val_bool(true);
}

static const arg_decl_t debug_disasm_args[] = {
    {.name = "count", .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "Number of instructions (default 16)"},
};

// `debug.step([n])` — single-step n instructions (default 1) and stop.
// Wraps the scheduler's run-N-then-stop pattern in one call so debug
// scripts don't have to chain `scheduler.run(n)` + `scheduler.stop`.
static value_t debug_method_step(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t count = 1;
    if (argc >= 1) {
        bool ok = false;
        count = val_as_i64(&argv[0], &ok);
        if (!ok)
            return val_err("debug.step: count must be integer");
    }
    if (count <= 0)
        return val_err("debug.step: count must be positive");
    scheduler_t *s = system_scheduler();
    if (!s)
        return val_err("debug.step: scheduler not initialised");
    scheduler_run_instructions(s, (int)count);
    scheduler_stop(s);
    return val_bool(true);
}

static const arg_decl_t debug_step_args[] = {
    {.name = "count", .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "Number of instructions (default 1)"},
};

static const member_t debug_members[] = {
    {.kind = M_METHOD,
     .name = "log",
     .doc = "Set per-subsystem log level (or pass a full spec string)",
     .method = {.args = debug_log_args, .nargs = 2, .result = V_BOOL, .fn = debug_method_log}      },
    {.kind = M_METHOD,
     .name = "disasm",
     .doc = "Disassemble forward from PC (default 16 instructions)",
     .method = {.args = debug_disasm_args, .nargs = 1, .result = V_BOOL, .fn = debug_method_disasm}},
    {.kind = M_METHOD,
     .name = "step",
     .doc = "Single-step N instructions and stop (default 1)",
     .method = {.args = debug_step_args, .nargs = 1, .result = V_BOOL, .fn = debug_method_step}    },
};

const class_desc_t debug_class = {
    .name = "debug",
    .members = debug_members,
    .n_members = sizeof(debug_members) / sizeof(debug_members[0]),
};

// === debug.mac.globals — Mac low-memory globals access ======================
//
// `mac_global_vars[]` (defined in mac_globals_data.c) names ~471 fixed
// addresses in the Mac low-memory area, each with a size (1/2/4/N) and
// a description. Rather than auto-expand into 471 attributes, we
// expose them through a small method surface:
//
//   debug.mac.globals.read(name)        — read by name; returns uint
//                                          for size 1/2/4, bytes for N
//   debug.mac.globals.write(name, val)  — write a 1/2/4-byte global
//   debug.mac.globals.address(name)     — return the static address
//   debug.mac.globals.list()            — list of known names
//
// Lookup is O(N) over the table; this is a debugging-only surface.

static int mac_global_lookup(const char *name) {
    if (!name)
        return -1;
    for (size_t i = 0; i < mac_global_vars_count; i++) {
        if (mac_global_vars[i].name && strcmp(mac_global_vars[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

static value_t method_mac_globals_read(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("debug.mac.globals.read: expected (name)");
    int idx = mac_global_lookup(argv[0].s);
    if (idx < 0)
        return val_err("debug.mac.globals.read: unknown global '%s'", argv[0].s);
    uint32_t addr = mac_global_vars[idx].address;
    int sz = mac_global_vars[idx].size;
    switch (sz) {
    case 1: {
        value_t v = val_uint(1, memory_read_uint8(addr));
        v.flags |= VAL_HEX;
        return v;
    }
    case 2: {
        value_t v = val_uint(2, memory_read_uint16(addr));
        v.flags |= VAL_HEX;
        return v;
    }
    case 4: {
        value_t v = val_uint(4, memory_read_uint32(addr));
        v.flags |= VAL_HEX;
        return v;
    }
    default: {
        if (sz <= 0 || sz > 256)
            return val_err("debug.mac.globals.read: unexpected entry size %d", sz);
        uint8_t buf[256];
        for (int i = 0; i < sz; i++)
            buf[i] = memory_read_uint8(addr + (uint32_t)i);
        return val_bytes(buf, (size_t)sz);
    }
    }
}

static value_t method_mac_globals_write(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("debug.mac.globals.write: expected (name, value)");
    int idx = mac_global_lookup(argv[0].s);
    if (idx < 0)
        return val_err("debug.mac.globals.write: unknown global '%s'", argv[0].s);
    bool ok = true;
    uint64_t v = val_as_u64(&argv[1], &ok);
    if (!ok)
        return val_err("debug.mac.globals.write: value is not numeric");
    uint32_t addr = mac_global_vars[idx].address;
    int sz = mac_global_vars[idx].size;
    switch (sz) {
    case 1:
        memory_write_uint8(addr, (uint8_t)v);
        break;
    case 2:
        memory_write_uint16(addr, (uint16_t)v);
        break;
    case 4:
        memory_write_uint32(addr, (uint32_t)v);
        break;
    default:
        return val_err("debug.mac.globals.write: '%s' is %d bytes (only 1/2/4 supported)", argv[0].s, sz);
    }
    return val_none();
}

static value_t method_mac_globals_address(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("debug.mac.globals.address: expected (name)");
    int idx = mac_global_lookup(argv[0].s);
    if (idx < 0)
        return val_err("debug.mac.globals.address: unknown global '%s'", argv[0].s);
    value_t v = val_uint(4, mac_global_vars[idx].address);
    v.flags |= VAL_HEX;
    return v;
}

static value_t method_mac_globals_list(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    // Build a deduplicated list of names (the table has a few historical
    // duplicates such as TimeSCSIDB; the legacy resolver kept first-found).
    value_t *items = (value_t *)calloc(mac_global_vars_count, sizeof(value_t));
    if (!items)
        return val_err("debug.mac.globals.list: out of memory");
    size_t out = 0;
    for (size_t i = 0; i < mac_global_vars_count; i++) {
        const char *nm = mac_global_vars[i].name;
        if (!nm)
            continue;
        bool dup = false;
        for (size_t j = 0; j < out; j++) {
            if (items[j].kind == V_STRING && items[j].s && strcmp(items[j].s, nm) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        items[out++] = val_str(nm);
    }
    return val_list(items, out);
}

static const arg_decl_t mac_globals_name_arg[] = {
    {.name = "name", .kind = V_STRING, .doc = "Mac low-memory global symbol (e.g. \"Ticks\")"},
};
static const arg_decl_t mac_globals_write_args[] = {
    {.name = "name", .kind = V_STRING, .doc = "Mac low-memory global symbol"},
    {.name = "value", .kind = V_UINT, .flags = VAL_HEX, .doc = "value to write"},
};

static const member_t debug_mac_globals_members[] = {
    {.kind = M_METHOD,
     .name = "read",
     .doc = "Read a Mac low-memory global by name (uint for 1/2/4-byte; bytes for larger)",
     .method = {.args = mac_globals_name_arg, .nargs = 1, .result = V_UINT, .fn = method_mac_globals_read}   },
    {.kind = M_METHOD,
     .name = "write",
     .doc = "Write a 1/2/4-byte Mac low-memory global by name",
     .method = {.args = mac_globals_write_args, .nargs = 2, .result = V_NONE, .fn = method_mac_globals_write}},
    {.kind = M_METHOD,
     .name = "address",
     .doc = "Return the address of a named Mac low-memory global",
     .method = {.args = mac_globals_name_arg, .nargs = 1, .result = V_UINT, .fn = method_mac_globals_address}},
    {.kind = M_METHOD,
     .name = "list",
     .doc = "List all known Mac low-memory global names",
     .method = {.args = NULL, .nargs = 0, .result = V_LIST, .fn = method_mac_globals_list}                   },
};

const class_desc_t debug_mac_globals_class = {
    .name = "globals",
    .members = debug_mac_globals_members,
    .n_members = sizeof(debug_mac_globals_members) / sizeof(debug_mac_globals_members[0]),
};

// === debug.mac — Mac-specific debugging utilities ===========================
//
// Holds `globals` as a child and exposes lookups for atrap names. More
// Mac-specific facets (process info, target backtrace, …) belong here
// in time; for now this covers the typed-bridge needs.

static value_t method_mac_atrap(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("debug.mac.atrap: expected (opcode)");
    bool ok = true;
    uint64_t op = val_as_u64(&argv[0], &ok);
    if (!ok)
        return val_err("debug.mac.atrap: opcode must be numeric");
    return val_str(macos_atrap_name((uint16_t)op));
}

static const arg_decl_t mac_atrap_args[] = {
    {.name = "opcode", .kind = V_UINT, .flags = VAL_HEX, .doc = "A-trap opcode (e.g. 0xA86E)"},
};

static const member_t debug_mac_members[] = {
    {.kind = M_METHOD,
     .name = "atrap",
     .doc = "Resolve an A-trap opcode to its symbolic name",
     .method = {.args = mac_atrap_args, .nargs = 1, .result = V_STRING, .fn = method_mac_atrap}},
};

const class_desc_t debug_mac_class = {
    .name = "mac",
    .members = debug_mac_members,
    .n_members = sizeof(debug_mac_members) / sizeof(debug_mac_members[0]),
};

// --- screen ---------------------------------------------------------------
//
// Wraps the legacy `screenshot` subcommand family. Each method
// delegates to the framebuffer logic in this module.

static value_t screen_method_save(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("screen.save: expected (path)");
    const char *path = argv[0].s;
    if (!path || !*path)
        return val_err("screen.save: empty path");
    size_t n = strlen(path);
    if (n < 4 || strcasecmp(path + n - 4, ".png") != 0)
        return val_err("screen.save: path must end in .png (got '%s')", path);
    const uint8_t *fb = system_framebuffer();
    if (!fb)
        return val_err("screen.save: framebuffer not available");
    if (save_framebuffer_as_png(fb, path) < 0)
        return val_err("screen.save: failed to save '%s'", path);
    return val_bool(true);
}

// `screen.match(reference)` — bitwise compare the framebuffer against
// a reference PNG.  A mismatch is a HARD failure: returns val_err so
// the path-form dispatcher (and hence the headless script runner)
// propagates non-zero out, failing the integration test.  Use
// `screen.match_or_save` for the non-fatal diagnostic flow.
static value_t screen_method_match(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("screen.match: expected (reference_path)");
    const char *ref = argv[0].s ? argv[0].s : "";
    const uint8_t *fb = system_framebuffer();
    if (!fb)
        return val_err("screen.match: framebuffer not available");
    int result = match_framebuffer_with_png(fb, ref);
    if (result < 0) {
        printf("MATCH FAILED: Error loading reference image '%s'.\n", ref);
        return val_err("screen.match: cannot load reference '%s'", ref);
    }
    if (result == 0) {
        printf("MATCH OK: Screen matches '%s'.\n", ref);
        return val_bool(true);
    }
    printf("MATCH FAILED: Screen does not match '%s'.\n", ref);
    return val_err("screen.match: screen does not match '%s'", ref);
}

static value_t screen_method_match_or_save(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING)
        return val_err("screen.match_or_save: expected (reference_path, [actual_path])");
    const char *ref = argv[0].s ? argv[0].s : "";
    const char *actual = (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    const uint8_t *fb = system_framebuffer();
    if (!fb)
        return val_err("screen.match_or_save: framebuffer not available");
    int result = match_framebuffer_with_png(fb, ref);
    if (result < 0) {
        printf("MATCH FAILED: Error loading reference image.\n");
        if (actual)
            save_framebuffer_as_png(fb, actual);
        return val_bool(false);
    }
    if (result == 0) {
        printf("MATCH OK: Screen matches '%s'.\n", ref);
        return val_bool(true);
    }
    if (actual) {
        save_framebuffer_as_png(fb, actual);
        printf("MATCH FAILED: Screen does not match '%s'. Saved actual to '%s'.\n", ref, actual);
    } else {
        printf("MATCH FAILED: Screen does not match '%s'.\n", ref);
    }
    return val_bool(false);
}

static value_t screen_method_checksum(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const uint8_t *fb = system_framebuffer();
    if (!fb)
        return val_err("screen.checksum: framebuffer not available");
    if (argc == 0)
        return val_int((int64_t)(int32_t)framebuffer_checksum(fb));
    if (argc < 4)
        return val_err("screen.checksum: expected (top, left, bottom, right) or no args");
    bool ok = true;
    int64_t t = val_as_i64(&argv[0], &ok);
    int64_t l = val_as_i64(&argv[1], &ok);
    int64_t b = val_as_i64(&argv[2], &ok);
    int64_t r = val_as_i64(&argv[3], &ok);
    if (!ok)
        return val_err("screen.checksum: region args must be integers");
    if (t < 0 || l < 0 || b <= t || r <= l || b > DEBUG_SCREEN_HEIGHT || r > DEBUG_SCREEN_WIDTH)
        return val_err("screen.checksum: invalid region bounds (0,0)-(%d,%d)", DEBUG_SCREEN_WIDTH, DEBUG_SCREEN_HEIGHT);
    return val_int((int64_t)(int32_t)framebuffer_region_checksum(fb, (int)t, (int)l, (int)b, (int)r));
}

static const arg_decl_t screen_save_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Output PNG path (must end in .png)"},
};
static const arg_decl_t screen_match_args[] = {
    {.name = "reference", .kind = V_STRING, .doc = "Reference PNG path"},
};
static const arg_decl_t screen_match_or_save_args[] = {
    {.name = "reference", .kind = V_STRING, .doc = "Reference PNG path"},
    {.name = "actual", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Path to write current screen on miss"},
};
static const arg_decl_t screen_checksum_args[] = {
    {.name = "top",    .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "Region top edge"   },
    {.name = "left",   .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "Region left edge"  },
    {.name = "bottom", .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "Region bottom edge"},
    {.name = "right",  .kind = V_INT, .flags = OBJ_ARG_OPTIONAL, .doc = "Region right edge" },
};

static const member_t screen_members[] = {
    {.kind = M_METHOD,
     .name = "save",
     .doc = "Save the current framebuffer to a PNG file",
     .method = {.args = screen_save_args, .nargs = 1, .result = V_BOOL, .fn = screen_method_save}                  },
    {.kind = M_METHOD,
     .name = "match",
     .doc = "Compare the framebuffer against a reference PNG (true if identical)",
     .method = {.args = screen_match_args, .nargs = 1, .result = V_BOOL, .fn = screen_method_match}                },
    {.kind = M_METHOD,
     .name = "match_or_save",
     .doc = "Like `match`, but also write the current screen to `actual` on mismatch",
     .method = {.args = screen_match_or_save_args, .nargs = 2, .result = V_BOOL, .fn = screen_method_match_or_save}},
    {.kind = M_METHOD,
     .name = "checksum",
     .doc = "Polynomial hash of the framebuffer (full screen or top/left/bottom/right region)",
     .method = {.args = screen_checksum_args, .nargs = 4, .result = V_INT, .fn = screen_method_checksum}           },
};

const class_desc_t screen_class = {
    .name = "screen",
    .members = screen_members,
    .n_members = sizeof(screen_members) / sizeof(screen_members[0]),
};

// === Process-singleton lifecycle ============================================
//
// `screen` is a stateless facade — checksum/save read the framebuffer
// from whatever machine is currently booted. Register once at
// shell_init.

static struct object *s_screen_object = NULL;

void screen_class_register(void) {
    if (s_screen_object)
        return;
    s_screen_object = object_new(&screen_class, NULL, "screen");
    if (s_screen_object)
        object_attach(object_root(), s_screen_object);
}

void screen_class_unregister(void) {
    if (s_screen_object) {
        object_detach(s_screen_object);
        object_delete(s_screen_object);
        s_screen_object = NULL;
    }
}
