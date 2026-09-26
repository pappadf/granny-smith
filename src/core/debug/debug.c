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
#include "common.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "crc32.h"
#include "debug_mac.h"
#include "display.h"
#include "expr.h"
#include "fpu.h"
#include "inflate.h"
#include "log.h"
#include "log_categories.h"
#include "memory.h"
#include "mmu.h"
#include "nubus.h"
#include "object.h"
#include "pci.h"
#include "root.h"
#include "scheduler.h"
#include "shell.h"
#include "shell_var.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

// Forward declarations — class descriptors are at the bottom of the file but
// debug_init / debug_cleanup reference them.
static const class_desc_t debug_class;
static const class_desc_t bp_collection_class;
static const class_desc_t lp_collection_class;
static const class_desc_t debug_mac_class;
static const class_desc_t debug_mac_globals_class;

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

    // A disabled breakpoint stays in the list but never stops (or counts).
    // Stored inverted so calloc's zero is "enabled".
    bool disabled;

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

    // For PC logpoints, the install-time physical *address* range (not page —
    // a page-granular compare fires on every instruction sharing the page,
    // which made PC logpoints unusable).  end_phys < start_phys means "no
    // physical range was recorded"; the logical compare is then the only test.
    uint32_t start_phys;
    uint32_t end_phys;

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
    return memory_debug_read_uint16(addr);
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
    // The entry object is created lazily by the root install path the first time
    // someone resolves debug.breakpoints[id]; we just hold the slot.
    bp->entry_object = gs_classes_make_breakpoint_object(bp);

    // add bp to a linked list
    bp->next = debug->breakpoints;
    debug->breakpoints = bp;

    debug->active = true;

    return bp;
}

// `$name` resolver for debugger-side expression contexts: straight
// through to the shell's scoped binding store (+ alias table).
static value_t debug_shell_binding(void *ud, const char *name) {
    (void)ud;
    return shell_binding_get(name);
}

// Evaluate a breakpoint condition expression using the full ${...}
// expression grammar (see src/core/object/expr.c).  Supports anything
// expr_eval supports — paths (cpu.pc, cpu.d0), method calls
// (memory.peek.l(0x1201D420)), arithmetic/bitwise/comparison/logical
// operators, ternary, etc.  Examples:
//   cpu.pc == 0x40802A14
//   cpu.d0 == 0
//   memory.peek.l(0x1201D420) == 0xE000
//   cpu.supervisor && cpu.pc >= 0x10000000
//
// Unknown / parse-failed / V_ERROR expressions evaluate to true so a
// typo doesn't silently swallow hits.
static bool eval_breakpoint_condition(const char *expr) {
    if (!expr || !*expr)
        return true;
    while (*expr == ' ' || *expr == '\t')
        expr++;
    if (!*expr)
        return true;

    expr_ctx_t ctx = {
        .root = object_root(),
        .binding = debug_shell_binding, // $name → shell bindings/aliases
        .binding_ud = NULL,
    };

    value_t v = expr_eval(expr, &ctx);
    bool result;
    switch (v.kind) {
    case V_BOOL:
        result = v.b;
        break;
    case V_INT:
        result = (v.i != 0);
        break;
    case V_UINT:
        result = (v.u != 0);
        break;
    case V_FLOAT:
        result = (v.f != 0.0);
        break;
    case V_STRING:
        result = (v.s && *v.s);
        break;
    case V_NONE:
        result = false;
        break;
    case V_ERROR:
        // Parse / resolution error — safer default is to fire so the
        // user notices the typo rather than missing the breakpoint.
        result = true;
        break;
    default:
        result = true;
        break;
    }
    value_free(&v);
    return result;
}

// Set a logpoint at the specified address range (end_addr == addr for single address)
logpoint_t *set_logpoint(debug_t *debug, uint32_t addr, uint32_t end_addr, log_category_t *category, int level) {

    logpoint_t *lp = calloc(1, sizeof(logpoint_t));

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
    // physical address range so the per-instruction check can fire when the
    // same physical instruction is executed via an aliased VA — same intent as
    // de9bde3 for memory logpoints, but address-exact: comparing pages made a
    // single-address logpoint fire on every instruction sharing its page.
    // When MMU is off or translation fails, leave the range "empty"
    // (end < start) and fall back to the logical match.
    lp->start_phys_page = 1;
    lp->end_phys_page = 0;
    lp->start_phys = 1;
    lp->end_phys = 0;
    if (g_mmu && g_mmu->enabled) {
        bool is_identity, valid;
        uint32_t phys_start = debug_translate_address(addr, &is_identity, NULL, &valid);
        if (valid) {
            uint32_t phys_end = debug_translate_address(end_addr, &is_identity, NULL, &valid);
            if (valid) {
                lp->start_phys = phys_start;
                lp->end_phys = phys_end;
                if (lp->end_phys < lp->start_phys) {
                    uint32_t tmp = lp->start_phys;
                    lp->start_phys = lp->end_phys;
                    lp->end_phys = tmp;
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
    // calloc, and the no-range sentinel set explicitly below, matching
    // set_logpoint.  This used to malloc and then assign 14 of the 16 fields
    // by hand, leaving start_phys/end_phys as heap garbage.  Inert today only
    // because the single reader is guarded by `lp->kind == LP_KIND_PC` -- a
    // coincidence of the current control flow, not an invariant anyone stated,
    // and any future pass that iterates all logpoints (a unified hit test, an
    // entries column, a checkpoint of the list) reads uninitialised memory.
    // MSan and valgrind flag it now.
    logpoint_t *lp = calloc(1, sizeof(logpoint_t));
    if (!lp)
        return NULL;
    lp->start_phys = 1; // start > end == "no physical range", as set_logpoint
    lp->end_phys = 0;
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
            bool supervisor = debug_cpu_is_supervisor();
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
        } else if (g_mem_logical_xlate) {
            // PPC-translated machine (no g_mmu): watch the physical pages
            // behind the 68k-world mapping too, so aliases and physical-
            // address routes (DMA, supervisor identity) stay off the fast
            // path.  translate_mac resolves the user data context whatever
            // the stop context (debug.h).
            const cpu_debug_if_t *dif = system_cpu_debug_if();
            if (dif && dif->translate_mac) {
                bool ok_start = false, ok_end = false;
                uint32_t phys_start = dif->translate_mac(dif->ctx, addr, &ok_start) >> PAGE_SHIFT;
                uint32_t phys_end = dif->translate_mac(dif->ctx, end_addr, &ok_end) >> PAGE_SHIFT;
                if (ok_start && ok_end) {
                    if (phys_end < phys_start) {
                        uint32_t tmp = phys_start;
                        phys_start = phys_end;
                        phys_end = tmp;
                    }
                    memory_logpoint_install_phys(phys_start, phys_end);
                    lp->start_phys_page = phys_start;
                    lp->end_phys_page = phys_end;
                }
            }
        }
    } else {
        // Physical-space logpoint: only the physical array is bumped.
        memory_logpoint_install_phys(start_page, end_page);
        lp->start_phys_page = start_page;
        lp->end_phys_page = end_page;
    }
    return lp;
}

// Expand a logpoint message template at fire time.
// The message is a stored (raw) interpolating-string body: `${expr}`
// splices any expression, `$name` splices a binding. The three
// event-intrinsic values (`$value`, `$addr`, `$size`) live nowhere
// else — there is no persistent object holding "the byte that just got
// written to 0x4000" — so they are pushed as per-fire bindings, layered
// in front of the shell's regular binding store.
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
    // Everything else falls through to the shell's binding store
    // (scoped `let` bindings, then the alias table as V_REF).
    return shell_binding_get(name);
}

// Render a logpoint's `message=` template.
//
// The template is stored raw and re-interpolated on every fire, so each hit
// runs interp_walk -> expr_eval -> object_resolve with a malloc per `${...}`
// body.  That looks like a performance defect, bad enough to make a write
// logpoint on a hot page "effectively unusable".
//
// MEASURED before restructuring.
// A Plus, a write logpoint over 0x0000-0xFFFF, 20M cycles, 71,650 fires in
// every run, output discarded, best of three:
//
//     no message=            455 ms
//     plain message=         468 ms   (+13 ms; no ${} so it short-circuits)
//     two-expression ${}=    529 ms   (+61 ms over plain)
//
// So the re-parse costs about 0.85 us per fire, roughly 13% -- on top of a
// path that already costs 6.4 us per fire for the slow-path routing the
// logpoint itself forces.  The defect is real; the severity is not.  The
// template is not what makes a hot-page logpoint expensive, and pre-splitting
// it into a {literal, expr-body} chunk list would add cached state and its
// invalidation to recover a small fraction of an already-slow debugging path.
//
// Deliberately not restructured.  If that changes, the number to beat is
// above.
static void format_logpoint_message(char *buf, size_t buf_size, const char *msg, uint32_t addr, uint32_t value,
                                    unsigned size) {
    if (!msg) {
        buf[0] = '\0';
        return;
    }
    lp_bindings_t lp = {.addr = addr, .value = value, .size = size};
    expr_ctx_t ctx = {
        .root = object_root(),
        .binding = lp_binding,
        .binding_ud = &lp,
    };
    value_t v = expr_interpolate_body(msg, &ctx);

    const char *s = (v.kind == V_STRING && v.s) ? v.s : (v.kind == V_ERROR && v.err) ? v.err : "";
    size_t n = strlen(s);
    if (n >= buf_size)
        n = buf_size - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    value_free(&v);
}

// ============================================================================
// Exception trace ring
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
    e->arch = EXC_ARCH_M68K; // this entry point serves the 68K exception paths
    s_exc_trace_head = (s_exc_trace_head + 1) % EXC_TRACE_RING_SIZE;
    s_exc_trace_count++;

    // Stream to the log pipeline if the exceptions category is enabled.
    // The LOG_WITH macro short-circuits when level > threshold, so this adds
    // only a single memory load + branch when streaming is off.
    log_category_t *cat = exc_trace_get_category();
    // Line 1010 ($028) is the OS call interface, so decode the trap word at the
    // faulting PC: with the category enabled the stream then reads as a Toolbox
    // /OS call trace instead of a wall of identical vector numbers.  Only done
    // when streaming is on — the read is a debug-path memory access.
    const char *trap_name = "";
    if (vector == 0x028 && log_get_level(cat) >= 1)
        trap_name = macos_atrap_name(memory_debug_read_uint16(faulting_pc));
    LOG_WITH(cat, 1, "[EXC] vec=$%03X %s fmt=$%X rw=%s addr=$%08X pc=$%08X saved_pc=$%08X sr=$%04X vbr=$%08X%s", vector,
             trap_name, format_frame, rw ? "R" : "W", fault_addr, faulting_pc, saved_pc, sr, vbr,
             double_fault_kind ? "  [DOUBLE FAULT]" : "");
}

// Dump the exception trace ring buffer (most recent EXC_TRACE_RING_SIZE entries)
// to stdout, oldest first.  Filters routine vectors when filter is non-zero:
//   filter=0: print everything in ring
//   filter=1: skip TRAP #0..15 ($080-$0BC), Line 1010 ($028), Line 1111 ($02C)
//             and interrupt autovectors ($060-$07C)
void debug_exc_trace_dump(int filter) {
    uint32_t total = (uint32_t)s_exc_trace_count;
    uint32_t count = total < EXC_TRACE_RING_SIZE ? total : EXC_TRACE_RING_SIZE;
    printf("=== Exception trace ring (%u total events, showing %u%s) ===\n", total, count,
           filter ? ", routine traps/IRQs filtered" : "");
    if (count == 0) {
        printf("(empty)\n");
        return;
    }
    // Walk from oldest to newest. With s_exc_trace_count <= ring size, oldest is at idx 0.
    // Otherwise oldest is at (s_exc_trace_head) which is the next-write slot (= oldest live).
    uint32_t start_idx = (total <= EXC_TRACE_RING_SIZE) ? 0 : (s_exc_trace_head % EXC_TRACE_RING_SIZE);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start_idx + i) % EXC_TRACE_RING_SIZE;
        exc_trace_entry_t *e = &s_exc_trace_ring[idx];
        if (filter) {
            // Filter routine: TRAP #N ($080-$0BC), Line 1010/1111 ($028, $02C),
            // interrupt autovectors ($060-$07C), trace ($024)
            if ((e->vector >= 0x080 && e->vector <= 0x0BC) || e->vector == 0x028 || e->vector == 0x02C ||
                e->vector == 0x024 || (e->vector >= 0x060 && e->vector <= 0x07C))
                continue;
        }
        // Per-arch line format: the 68K entry prints as always; a PPC
        // entry carries MSR in the vbr slot, the vector offset in
        // format_frame, and DAR in fault_addr.
        if (e->arch == EXC_ARCH_PPC) {
            printf("[%llu] vec=$%05X rw=%s dar=$%08X pc=$%08X srr0=$%08X msr=$%08X%s\n", (unsigned long long)e->ts,
                   e->format_frame, e->rw ? "R" : "W", e->fault_addr, e->faulting_pc, e->saved_pc, e->vbr,
                   e->double_fault_kind ? "  [DOUBLE FAULT]" : "");
        } else {
            printf("[%llu] vec=$%03X fmt=$%X rw=%s addr=$%08X pc=$%08X saved_pc=$%08X sr=$%04X vbr=$%08X%s\n",
                   (unsigned long long)e->ts, e->vector, e->format_frame, e->rw ? "R" : "W", e->fault_addr,
                   e->faulting_pc, e->saved_pc, e->sr, e->vbr, e->double_fault_kind ? "  [DOUBLE FAULT]" : "");
        }
    }
}

// The debug_t whose construction installed g_mem_logpoint_hook; only its
// teardown clears the hook (debug_init / debug_delete).
static debug_t *g_mem_hook_owner = NULL;

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
                if (g_mmu && g_mmu->enabled) {
                    phys_addr = mmu_translate_debug(g_mmu, addr, supervisor);
                } else if (g_mem_logical_xlate && g_mem_logpoint_page_count &&
                           g_mem_logpoint_page_count[addr >> PAGE_SHIFT]) {
                    // PPC keep-logical route: a logically-watched page
                    // arrives with its logical address — translate it for
                    // the physical compare.  Unwatched pages arrive
                    // already-physical and compare as-is.
                    bool ok;
                    uint32_t pa = g_mem_logical_xlate(addr, &ok);
                    if (ok)
                        phys_addr = pa;
                }
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
            const cpu_debug_if_t *dif = system_cpu_debug_if();
            uint32_t pc = dif ? dif->get_pc(dif->ctx) : 0;
            LOG_WITH(lp->category, lp->level, "logpoint %s $%08X.%c value=$%0*X pc=$%08X", is_write ? "WRITE" : "READ",
                     addr,
                     (size == 1)   ? 'b'
                     : (size == 2) ? 'w'
                                   : 'l',
                     (int)(size * 2), value, pc);
        }
    }
}

// Forward declarations for trace functions
static void trace_add_pc_entry(debug_t *debug, uint32_t pc);

// Forward declarations for logpoint management (IMP-604)
void list_logpoints(debug_t *debug);
int delete_all_logpoints(debug_t *debug);

// Disassemble one instruction at pc through the main-CPU debug interface,
// splitting the core's "mnemonic\toperands" text.  Returns bytes consumed
// (so callers advance the address arch-neutrally); 2 as a safe fallback
// when no machine/core is up.
// The split text's buffers: the longest mnemonic the 68K decoder produces is
// a 32-character trap name (_CaseAndMarkSensitiveEqualString, measured over
// every opcode), which a 31-character cap used to truncate.
#define DISASM_MNEMONIC_MAX 48
#define DISASM_OPERANDS_MAX 80

static int disasm_at(uint32_t pc, char mnemonic[DISASM_MNEMONIC_MAX], char operands[DISASM_OPERANDS_MAX]) {
    char buf[100];
    int i, n;

    const cpu_debug_if_t *dif = system_cpu_debug_if();
    if (!dif) {
        buf[0] = '\0';
        n = 2;
    } else {
        n = dif->disasm(dif->ctx, pc, buf, sizeof(buf));
    }

    if (strlen(buf) == 0) {
        snprintf(mnemonic, DISASM_MNEMONIC_MAX, "ILLEGAL");
        operands[0] = '\0';
    } else {
        // Bounded so there is always room for the NUL even if the core's
        // disasm ever returns an opcode without a tab separator.
        for (i = 0; i < DISASM_MNEMONIC_MAX - 1 && buf[i] != '\0' && buf[i] != '\t'; i++)
            mnemonic[i] = buf[i];
        mnemonic[i] = '\0';
        if (buf[i] == '\t')
            snprintf(operands, DISASM_OPERANDS_MAX, "%s", buf + i + 1);
        else
            operands[0] = '\0';
    }

    return n;
}

// Disassemble instruction at addr, write to buf, return instruction length in bytes
int debugger_disasm(char *buf, size_t buf_size, uint32_t addr) {
    char mnemonic[DISASM_MNEMONIC_MAX], operands[DISASM_OPERANDS_MAX];

    int n = disasm_at(addr, mnemonic, operands);

    // Format with address prefix.  Use snprintf to bound output: addr_str is
    // up to 39 chars, mnemonic up to 47, operands up to 79 — worst case
    // ~160 bytes, larger than the 100-byte caller buffers historically used.
    // dc19792 enlarged the inner operands buffer but missed callers; this
    // bounds the final write so a complex full-extension-word instruction
    // can't overflow.
    char addr_str[40];
    format_address_pair(addr_str, sizeof(addr_str), addr);
    if (buf_size > 0)
        snprintf(buf, buf_size, "%s  %04x  %-10s%-12s", addr_str, (int)cpu_get_uint16(addr), mnemonic, operands);

    return n;
}

// Disassemble instruction at current PC, write to buf (bounded by buf_size)
void debugger_disasm_pc(char *buf, size_t buf_size) {
    if (!buf || buf_size == 0)
        return;
    const cpu_debug_if_t *dif = system_cpu_debug_if();
    if (!dif) {
        buf[0] = '\0';
        return;
    }
    debugger_disasm(buf, buf_size, dif->get_pc(dif->ctx));
}

// Check if execution should break and trace current instruction
int debug_break_and_trace(void) {
    debug_t *debug = system_debug();
    const cpu_debug_if_t *dif = system_cpu_debug_if();
    if (!debug || !dif)
        return false;
    bool stop = false;
    uint32_t current_pc = dif->get_pc(dif->ctx);

    // If we have a last_breakpoint_pc set, this means we need to skip checking
    // for breakpoints at that specific PC address one time (to allow resuming execution)
    if (debug->last_breakpoint_pc != 0 && current_pc == debug->last_breakpoint_pc) {
        // Clear the flag after skipping once
        debug->last_breakpoint_pc = 0;
    } else {
        // Check for breakpoints at current PC
        breakpoint_t *bp = debug->breakpoints;
        while (bp != NULL) {
            // A disabled breakpoint is kept but ignored.
            if (bp->disabled) {
                bp = bp->next;
                continue;
            }
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
    // Match on either the logical PC (install-time VA) or the exact physical
    // address (catches the same physical instruction reached via a different
    // VA — e.g. user and supervisor mappings of shared kernel code).
    logpoint_t *lp = debug->logpoints;
    uint32_t phys_pc_addr = 0;
    bool phys_pc_resolved = false;
    bool phys_pc_valid = false;
    while (lp != NULL) {
        if (lp->kind == LP_KIND_PC) {
            bool hit = (current_pc >= lp->addr && current_pc <= lp->end_addr);
            if (!hit && lp->start_phys <= lp->end_phys) {
                // Lazy-translate once across all logpoints with a phys range.
                if (!phys_pc_resolved) {
                    bool is_identity;
                    phys_pc_addr = debug_translate_address(current_pc, &is_identity, NULL, &phys_pc_valid);
                    phys_pc_resolved = true;
                }
                if (phys_pc_valid && phys_pc_addr >= lp->start_phys && phys_pc_addr <= lp->end_phys)
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
        // Standard ring buffer: advance tail past the slot we're about to
        // clobber BEFORE the write, so the just-written entry survives the
        // wrap.  Previous order (write then check-and-advance-tail) lost
        // the newest entry the moment the buffer first filled.
        int next_head = (debug->trace_head + 1) % debug->trace_buffer_size;
        if (next_head == debug->trace_tail)
            debug->trace_tail = (debug->trace_tail + 1) % debug->trace_buffer_size;
        debug->trace_buffer[debug->trace_head] = current_pc;
        debug->trace_head = next_head;
    }

    // Record PC in new trace entries buffer
    if (debug->trace_entries) {
        trace_add_pc_entry(debug, current_pc);
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

// Delete breakpoint by sparse stable id. Walks the list
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
        if (bp->disabled)
            printf("  (disabled)");
        printf("\n");
        bp = bp->next;
        count++;
    }
}

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

// ────────────────────────────────────────────────────────────────────────────
// Screenshot command - save emulated screen as PNG
// ────────────────────────────────────────────────────────────────────────────
//
// Pixel-format support: 1/2/4/8 bpp indexed (through the display's CLUT),
// 16-bit 555/565 and 32-bit XRGB; any other format is a hard error.

// Write 32-bit big-endian value to buffer
// Write a PNG chunk to file
static int write_png_chunk(FILE *fp, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t header[8];
    // Write length (big-endian)
    WR_BE32(header, len);
    // Write type
    memcpy(header + 4, type, 4);
    if (fwrite(header, 1, 8, fp) != 8)
        return -1;

    // Write data (if any)
    if (len > 0 && data) {
        if (fwrite(data, 1, len, fp) != len)
            return -1;
    }

    // CRC over type + data
    uint32_t crc = gs_crc32(gs_crc32(0, header + 4, 4), data, len > 0 && data ? len : 0);

    // Write CRC (big-endian)
    uint8_t crc_buf[4];
    WR_BE32(crc_buf, crc);
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

// ===== Minimal DEFLATE compressor (LZ77 + fixed Huffman) ====================
// Screenshots are committed to the repo as reference images, so they are worth
// compressing: a 640x480 framebuffer is 1.2 MB of stored blocks but ~12 KB
// deflated.  Only the writer is new — the PNG reader already inflates, so
// references written either way keep matching.

#define DEFLATE_WINDOW    32768
#define DEFLATE_MIN_MATCH 3
#define DEFLATE_MAX_MATCH 258
#define DEFLATE_HASH_BITS 15
#define DEFLATE_HASH_SIZE (1 << DEFLATE_HASH_BITS)
#define DEFLATE_MAX_CHAIN 64

// The RFC 1951 §3.2.5 length/distance tables the writer needs are shared
// with the decoder and now live in inflate.c (declared in inflate.h).

// LSB-first bit writer over a caller-sized buffer.
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    uint32_t acc; // pending bits, lowest bit written first
    int nbits;
} deflate_bw_t;

// Append the low `n` bits of `v`, least-significant bit first.
static void deflate_put(deflate_bw_t *w, uint32_t v, int n) {
    if (n <= 0)
        return;
    w->acc |= (v & ((1u << n) - 1)) << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        if (w->pos < w->cap)
            w->buf[w->pos] = (uint8_t)(w->acc & 0xff);
        w->pos++;
        w->acc >>= 8;
        w->nbits -= 8;
    }
}

// Append a Huffman code: the code's most-significant bit goes out first.
static void deflate_put_code(deflate_bw_t *w, uint32_t code, int n) {
    uint32_t reversed = 0;
    for (int i = 0; i < n; i++)
        reversed |= ((code >> i) & 1u) << (n - 1 - i);
    deflate_put(w, reversed, n);
}

// Emit one literal/length symbol in the fixed Huffman code (RFC 1951 §3.2.6).
static void deflate_put_symbol(deflate_bw_t *w, unsigned sym) {
    if (sym < 144)
        deflate_put_code(w, 0x30 + sym, 8);
    else if (sym < 256)
        deflate_put_code(w, 0x190 + (sym - 144), 9);
    else if (sym < 280)
        deflate_put_code(w, sym - 256, 7);
    else
        deflate_put_code(w, 0xc0 + (sym - 280), 8);
}

// Hash three bytes into the match-chain head table.
static uint32_t deflate_hash3(const uint8_t *p) {
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2]) & (DEFLATE_HASH_SIZE - 1);
}

// Deflate `src` into a complete zlib stream (2-byte header, one fixed-Huffman
// block, adler32 trailer).  Returns a malloc'd buffer, or NULL on OOM.
static uint8_t *zlib_compress(const uint8_t *src, size_t len, size_t *out_len) {
    // Fixed Huffman codes an incompressible byte in at most 9 bits, so 1.25x
    // plus a small constant can never overflow.
    size_t cap = len + len / 4 + 64;
    uint8_t *out = malloc(cap);
    int32_t *head = malloc(DEFLATE_HASH_SIZE * sizeof(int32_t));
    int32_t *prev = malloc((len ? len : 1) * sizeof(int32_t));
    if (!out || !head || !prev) {
        free(out);
        free(head);
        free(prev);
        return NULL;
    }
    for (size_t i = 0; i < DEFLATE_HASH_SIZE; i++)
        head[i] = -1;

    deflate_bw_t w = {out, cap, 0, 0, 0};
    // zlib header: CM=8 / CINFO=7 (32K window), FLEVEL=2, FCHECK making the
    // 16-bit value a multiple of 31.
    out[w.pos++] = 0x78;
    out[w.pos++] = 0x9c;
    deflate_put(&w, 1, 1); // BFINAL — one block covers the whole stream
    deflate_put(&w, 1, 2); // BTYPE = 01, fixed Huffman

    size_t pos = 0;
    while (pos < len) {
        size_t best_len = 0, best_dist = 0;
        if (pos + DEFLATE_MIN_MATCH <= len) {
            uint32_t h = deflate_hash3(src + pos);
            // Walk the chain of earlier positions with the same hash, newest
            // first, keeping the longest match inside the 32K window.
            int32_t cand = head[h];
            for (int chain = DEFLATE_MAX_CHAIN; cand >= 0 && chain > 0; chain--) {
                size_t dist = pos - (size_t)cand;
                if (dist == 0 || dist > DEFLATE_WINDOW)
                    break;
                size_t max_len = len - pos;
                if (max_len > DEFLATE_MAX_MATCH)
                    max_len = DEFLATE_MAX_MATCH;
                size_t l = 0;
                while (l < max_len && src[(size_t)cand + l] == src[pos + l])
                    l++;
                if (l > best_len) {
                    best_len = l;
                    best_dist = dist;
                    if (l >= DEFLATE_MAX_MATCH)
                        break;
                }
                cand = prev[cand];
            }
            prev[pos] = head[h];
            head[h] = (int32_t)pos;
        }

        if (best_len >= DEFLATE_MIN_MATCH) {
            int lc = 28;
            while (lc > 0 && best_len < deflate_len_base[lc])
                lc--;
            deflate_put_symbol(&w, 257 + (unsigned)lc);
            deflate_put(&w, (uint32_t)(best_len - deflate_len_base[lc]), deflate_len_extra[lc]);
            int dc = 29;
            while (dc > 0 && best_dist < deflate_dist_base[dc])
                dc--;
            deflate_put_code(&w, (uint32_t)dc, 5);
            deflate_put(&w, (uint32_t)(best_dist - deflate_dist_base[dc]), deflate_dist_extra[dc]);
            // Index the bytes the match covered so later matches can find them.
            for (size_t k = 1; k < best_len; k++) {
                if (pos + k + DEFLATE_MIN_MATCH > len)
                    break;
                uint32_t h2 = deflate_hash3(src + pos + k);
                prev[pos + k] = head[h2];
                head[h2] = (int32_t)(pos + k);
            }
            pos += best_len;
        } else {
            deflate_put_symbol(&w, src[pos]);
            pos++;
        }
    }
    deflate_put_symbol(&w, 256); // end of block
    if (w.nbits > 0)
        deflate_put(&w, 0, 8 - w.nbits); // pad the final byte

    free(head);
    free(prev);

    uint32_t adler = adler32(src, len);
    if (w.pos + 4 > cap) { // cannot happen with the 1.25x bound; fail loudly
        free(out);
        return NULL;
    }
    WR_BE32(out + w.pos, adler);
    w.pos += 4;

    *out_len = w.pos;
    return out;
}

// Calculate a simple checksum of the framebuffer for fast screen comparison.
// v1 hashes every byte of `bits`; CLUT inclusion (so palette-only changes
// hash differently) lands with the indexed/direct format expansion.
uint32_t framebuffer_checksum(const display_t *d) {
    if (!d || !d->bits)
        return 0;
    const size_t fb_bytes = (size_t)d->stride * d->height;
    uint32_t checksum = 0;
    for (size_t i = 0; i < fb_bytes; i++) {
        checksum = checksum * 31 + d->bits[i];
    }
    return checksum;
}

// Calculate checksum for a region of the framebuffer (top, left, bottom, right)
// Region is specified in pixels.

uint32_t framebuffer_region_checksum(const display_t *d, int top, int left, int bottom, int right) {
    if (!d || !d->bits)
        return 0;
    // Colour displays: hash the bytes backing each row's span.  Before this,
    // every format except 1 bpp returned 0 — silently, so a caller comparing
    // two regions on an 8 bpp machine compared 0 with 0 and could never see
    // a difference.  That is how av-speech-recognition.spec.ts came to
    // "prove" a Finder window had not opened when it had never been able to
    // tell.  1 bpp keeps its exact bit-packing walk below so existing
    // baselines are unchanged.
    if (d->format != PIXEL_1BPP_MSB) {
        // display.h owns bits-per-pixel.  This was a third
        // copy of that switch, with a 0 return meaning "not a format I can
        // walk"; display_bpp answers for every format in the enum and faults
        // on anything else, so the sentinel had nothing left to signal.
        unsigned bpp = display_bpp(d->format);
        // Byte span of the row segment, rounded outwards for sub-byte
        // formats (a partial byte still changes when the region does).
        size_t first = ((size_t)left * bpp) / 8;
        size_t last = (((size_t)right * bpp) + 7) / 8; // exclusive
        uint32_t checksum = 0;
        for (int y = top; y < bottom; y++) {
            const uint8_t *row = d->bits + (size_t)y * d->stride;
            for (size_t i = first; i < last; i++)
                checksum = checksum * 31 + row[i];
        }
        return checksum;
    }
    const uint8_t *fb = d->bits;
    const uint32_t stride = d->stride;
    uint32_t checksum = 0;

    // Process each row in the region.  The byte accumulator must reset at
    // each row boundary (independent of `left % 8`), so we initialise it
    // here rather than relying on byte_bit==7 inside the loop — that path
    // fires only when `left` is byte-aligned and would leak bits across
    // rows otherwise.
    for (int y = top; y < bottom; y++) {
        uint8_t accum_byte = 0;
        // Process each pixel in the row, packing into bytes
        for (int x = left; x < right; x++) {
            int byte_idx = y * (int)stride + (x / 8);
            int bit_idx = 7 - (x % 8); // MSB first
            int bit = (fb[byte_idx] >> bit_idx) & 1;

            int rel_x = x - left;
            int byte_bit = 7 - (rel_x % 8);
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
// Convert one row of a live framebuffer to packed RGBA (no PNG filter
// byte).  Pulled out of save_framebuffer_as_png so screen.match can
// compare any pixel format against an RGBA reference PNG without
// going through a temp PNG round-trip.
//
// `out_rgba` must point to at least `width * 4` bytes.
static void framebuffer_row_to_rgba(const display_t *d, int y, uint8_t *out_rgba) {
    // The conversion itself is display.h's (display_row_to_rgba): it was
    // written twice in this file, byte for byte, and the PNG writer below is
    // the other copy.
    display_row_to_rgba(d, (uint32_t)y, out_rgba);
}

// Load a PNG file and decode it to packed RGBA (8 bits per channel, 4
// bytes per pixel, no filter bytes).  Used by screen.match's
// RGBA-vs-RGBA comparison path; works with any PNG color type that
// save_framebuffer_as_png emits (RGBA) or that older 1bpp tooling
// might have produced (grayscale, RGB).  `out_rgba` must be at least
// `expected_width * expected_height * 4` bytes.  Returns 0 / -1.
static int load_png_to_rgba(const char *filename, int expected_width, int expected_height, uint8_t *out_rgba) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("Error: Cannot open '%s' for reading.\n", filename);
        return -1;
    }
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

    static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (file_size < 8 || memcmp(file_data, png_sig, 8) != 0) {
        free(file_data);
        printf("Error: Not a valid PNG file.\n");
        return -1;
    }

    size_t pos = 8;
    int width = 0, height = 0, bit_depth = 0, color_type = 0;
    uint8_t *idat_data = NULL;
    size_t idat_len = 0;
    size_t idat_capacity = 0;
    while (pos + 12 <= (size_t)file_size) {
        uint32_t chunk_len = RD_BE32(file_data + pos);
        char chunk_type[5];
        memcpy(chunk_type, file_data + pos + 4, 4);
        chunk_type[4] = '\0';
        if (strcmp(chunk_type, "IHDR") == 0 && chunk_len >= 13) {
            width = RD_BE32(file_data + pos + 8);
            height = RD_BE32(file_data + pos + 12);
            bit_depth = file_data[pos + 16];
            color_type = file_data[pos + 17];
        } else if (strcmp(chunk_type, "IDAT") == 0) {
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
        pos += 12 + chunk_len;
    }
    free(file_data);

    if (width != expected_width || height != expected_height) {
        free(idat_data);
        printf("Error: PNG dimensions %dx%d don't match screen %dx%d.\n", width, height, expected_width,
               expected_height);
        return -1;
    }
    if (!idat_data || idat_len == 0) {
        free(idat_data);
        printf("Error: No image data in PNG.\n");
        return -1;
    }
    if (bit_depth != 8) {
        free(idat_data);
        printf("Error: PNG bit depth %d unsupported (only 8).\n", bit_depth);
        return -1;
    }

    size_t raw_size = 0;
    // At most 8-bit RGBA plus a filter byte per row, at dimensions already
    // checked against the screen.
    size_t raw_max = (size_t)height * (1 + (size_t)width * 4);
    uint8_t *raw_data = inflate_zlib_alloc(idat_data, idat_len, raw_max, &raw_size);
    free(idat_data);
    if (!raw_data) {
        printf("Error: Failed to decompress PNG..\n");
        return -1;
    }

    size_t bpp_in;
    if (color_type == 6)
        bpp_in = 4; // RGBA
    else if (color_type == 2)
        bpp_in = 3; // RGB
    else if (color_type == 0)
        bpp_in = 1; // Grayscale
    else {
        free(raw_data);
        printf("Error: Unsupported PNG color type %d.\n", color_type);
        return -1;
    }
    size_t row_size = 1 + (size_t)width * bpp_in;
    if (raw_size != row_size * (size_t)height) {
        free(raw_data);
        printf("Error: PNG data size mismatch (got %zu, expected %zu).\n", raw_size, row_size * (size_t)height);
        return -1;
    }

    for (int y = 0; y < height; y++) {
        const uint8_t *row = raw_data + (size_t)y * row_size + 1; // skip filter byte
        uint8_t *out_row = out_rgba + (size_t)y * (size_t)width * 4;
        for (int x = 0; x < width; x++) {
            uint8_t r, g, b, a;
            if (color_type == 6) {
                r = row[x * 4 + 0];
                g = row[x * 4 + 1];
                b = row[x * 4 + 2];
                a = row[x * 4 + 3];
            } else if (color_type == 2) {
                r = row[x * 3 + 0];
                g = row[x * 3 + 1];
                b = row[x * 3 + 2];
                a = 255;
            } else { // grayscale
                r = g = b = row[x];
                a = 255;
            }
            out_row[x * 4 + 0] = r;
            out_row[x * 4 + 1] = g;
            out_row[x * 4 + 2] = b;
            out_row[x * 4 + 3] = a;
        }
    }
    free(raw_data);
    return 0;
}

// Public wrapper over load_png_to_rgba (debug.h): lets subsystems outside
// debug.c (machine.videoin.load) reuse the golden PNG decoder.
int debug_load_png_rgba(const char *filename, int width, int height, uint8_t *out_rgba) {
    return load_png_to_rgba(filename, width, height, out_rgba);
}

// Compare current framebuffer with a reference PNG file.
// Returns 0 if match, 1 if mismatch, -1 on error.
//
// Compares in RGBA space (8 bits per channel, 4 bytes per pixel) so
// every pixel format save_framebuffer_as_png supports — 1/2/4/8 bpp
// indexed, 16-bit 5-5-5, and 32-bit XRGB — also matches.  Indexed
// formats need the live CLUT to be populated; if the framebuffer is
// indexed and the CLUT is empty (e.g. screenshot before the OS has
// uploaded a palette) the live conversion will see clut_len == 0 and
// the colour bytes will be zero, which won't match a real reference
// — that's intentional, the test should screenshot post-CLUT-load.
//
// `exclude_rects` points to `n_rects` consecutive {top, left, bottom,
// right} quads (half-open).  Those pixels are zeroed in BOTH the live and
// reference buffers before the compare, so any phase-dependent content
// there is ignored.  Callers must validate the bounds; out-of-range values
// are clamped defensively here.
//
// More than one region because a single frame can carry more than one
// phase-dependent field, and a bounding box over both would blank whatever
// sits between them.  The About This Macintosh rows are the case that
// forced it: the menu-bar clock in the top right AND the Finder's "Largest
// Unused Block" figure lower down, with "Total Memory" -- an assertion the
// row exists to make -- sitting between the two.
int match_framebuffer_with_png(const display_t *d, const char *filename, const int *exclude_rects, int n_rects) {
    if (!d || !d->bits) {
        printf("Error: No active display.\n");
        return -1;
    }
    switch (d->format) {
    case PIXEL_1BPP_MSB:
    case PIXEL_2BPP_MSB:
    case PIXEL_4BPP_MSB:
    case PIXEL_8BPP:
    case PIXEL_16BPP_555:
    case PIXEL_16BPP_565:
    case PIXEL_32BPP_XRGB:
        break;
    default:
        printf("Error: PNG match: unsupported pixel format %d.\n", (int)d->format);
        return -1;
    }
    if ((d->format == PIXEL_2BPP_MSB || d->format == PIXEL_4BPP_MSB || d->format == PIXEL_8BPP) &&
        (!d->clut || d->clut_len == 0)) {
        printf("Error: PNG match: indexed format with no CLUT.\n");
        return -1;
    }

    const size_t rgba_bytes = (size_t)d->width * (size_t)d->height * 4;
    uint8_t *fb_rgba = malloc(rgba_bytes);
    uint8_t *ref_rgba = malloc(rgba_bytes);
    if (!fb_rgba || !ref_rgba) {
        free(fb_rgba);
        free(ref_rgba);
        printf("Error: Out of memory.\n");
        return -1;
    }
    for (uint32_t y = 0; y < d->height; y++)
        framebuffer_row_to_rgba(d, (int)y, fb_rgba + (size_t)y * d->width * 4);
    if (load_png_to_rgba(filename, (int)d->width, (int)d->height, ref_rgba) < 0) {
        free(fb_rgba);
        free(ref_rgba);
        return -1;
    }
    // Mask the excluded region (if any) in both buffers so phase-dependent
    // pixels there don't affect the compare.  Clamp to the framebuffer.
    for (int r = 0; exclude_rects && r < n_rects; r++) {
        const int *q = exclude_rects + r * 4;
        int top = q[0], left = q[1], bottom = q[2], right = q[3];
        if (top < 0)
            top = 0;
        if (left < 0)
            left = 0;
        if (bottom > (int)d->height)
            bottom = (int)d->height;
        if (right > (int)d->width)
            right = (int)d->width;
        for (int y = top; y < bottom; y++) {
            size_t off = ((size_t)y * d->width + left) * 4;
            size_t span = (size_t)(right - left) * 4;
            memset(fb_rgba + off, 0, span);
            memset(ref_rgba + off, 0, span);
        }
    }
    int match = (memcmp(fb_rgba, ref_rgba, rgba_bytes) == 0);
    free(fb_rgba);
    free(ref_rgba);
    return match ? 0 : 1;
}

// Save framebuffer as PNG to the given file path.
// Emits an 8-bit RGBA truecolour PNG whatever the display's pixel format;
// indexed formats are decoded through the display's CLUT.
int save_framebuffer_as_png(const display_t *d, const char *filename) {
    if (!d || !d->bits) {
        printf("Error: No active display.\n");
        return -1;
    }
    switch (d->format) {
    case PIXEL_1BPP_MSB:
    case PIXEL_2BPP_MSB:
    case PIXEL_4BPP_MSB:
    case PIXEL_8BPP:
    case PIXEL_16BPP_555:
    case PIXEL_16BPP_565:
    case PIXEL_32BPP_XRGB:
        break;
    default:
        printf("Error: PNG save: unsupported pixel format %d.\n", (int)d->format);
        return -1;
    }
    if ((d->format == PIXEL_2BPP_MSB || d->format == PIXEL_4BPP_MSB || d->format == PIXEL_8BPP) &&
        (!d->clut || d->clut_len == 0)) {
        printf("Error: PNG save: indexed format with no CLUT.\n");
        return -1;
    }
    const int width = (int)d->width;
    const int height = (int)d->height;

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
    WR_BE32(ihdr, (uint32_t)width);
    WR_BE32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8; // bit depth (8 bits per channel)
    ihdr[9] = 6; // color type (RGBA)
    ihdr[10] = 0; // compression method (deflate)
    ihdr[11] = 0; // filter method
    ihdr[12] = 0; // interlace method (none)
    if (write_png_chunk(fp, "IHDR", ihdr, 13) < 0)
        goto write_error;

    // Prepare uncompressed image data with filter bytes
    // Each row: 1 filter byte (0 = none) + width * 4 bytes (RGBA)
    size_t row_size = 1 + (size_t)width * 4;
    size_t raw_size = row_size * (size_t)height;
    uint8_t *raw_data = malloc(raw_size);
    if (!raw_data) {
        printf("Error: Out of memory.\n");
        fclose(fp);
        return -1;
    }

    // Convert framebuffer to 8-bit RGBA, one row at a time.  The per-pixel
    // decode is display.h's, shared with framebuffer_row_to_rgba above and
    // with the object model -- this was a second, byte-identical copy of that
    // switch.  `row + 1` steps past the PNG filter byte.
    for (int y = 0; y < height; y++) {
        uint8_t *row = raw_data + y * row_size;
        row[0] = 0; // filter byte: none
        display_row_to_rgba(d, (uint32_t)y, row + 1);
    }

    // Deflate the filtered scanlines into a zlib stream.  A 640x480 screen is
    // 1.2 MB raw and about 12 KB compressed, which is what makes committing a
    // reference image per test milestone reasonable.
    size_t zpos = 0;
    uint8_t *zlib_data = zlib_compress(raw_data, raw_size, &zpos);
    free(raw_data);
    if (!zlib_data) {
        printf("Error: Out of memory.\n");
        fclose(fp);
        return -1;
    }

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
    printf("Screenshot saved to '%s' (%dx%d).\n", filename, width, height);
    return 0;

write_error:
    printf("Error: Failed to write to file '%s'.\n", filename);
    fclose(fp);
    return -1;
}

// ============================================================================
// FPU register dump (IMP-402)
// ============================================================================

// ============================================================================
// Configurable status line / prompt (IMP-308)
// ============================================================================

static bool g_prompt_enabled = true;

// Check if prompt/status line is enabled
bool debug_prompt_enabled(void) {
    return g_prompt_enabled;
}

// Set prompt default at startup (e.g. from --no-prompt CLI flag).
// Persists across all subsequent client connections.
void debug_set_prompt_default(bool enabled) {
    g_prompt_enabled = enabled;
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
    if (!lp)
        return;
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

// Delete logpoint by sparse stable id. Same shape as
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
    debug_t *debug = (debug_t *)calloc(1, sizeof(debug_t));
    if (!debug) {
        return NULL;
    }

    debug_mac_init();

    // Install memory-logpoint hook so the memory slow path can emit logs.
    // The hook is process-global; this instance owns it until another is
    // constructed (see debug_delete).
    g_mem_logpoint_hook = debug_memory_logpoint_hook;
    g_mem_hook_owner = debug;

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

    // Free all breakpoints — via free_breakpoint() so the entry object
    // and condition string go with them (the same discipline every other
    // breakpoint-removal path follows).
    breakpoint_t *bp = debug->breakpoints;
    while (bp) {
        breakpoint_t *next = bp->next;
        free_breakpoint(bp);
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
    // The hook is process-global but installed and cleared by cfg-scoped
    // construction and teardown, and the documented reload order is
    // system_create(new) THEN system_destroy(old).  Clearing unconditionally
    // therefore removed the hook the NEW machine had just installed: its
    // logpoints stayed registered -- pages forced onto the slow path, the
    // fast-path cost paid -- with nothing to call, so none of them ever fired
    // and nothing reported it.  root.c guards exactly this shape with
    // g_installed_cfg (root.c:295-309); this is the same guard, keyed on the
    // owning instance.  It used to compare the handler instead, which every
    // instance installs, so the guard was always true and a checkpoint.load
    // still cleared the new machine's hook: memory logpoints never fired
    // after a load, even ones added afterwards (#172).
    if (g_mem_hook_owner == debug) {
        g_mem_logpoint_hook = NULL;
        g_mem_hook_owner = NULL;
    }

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

static debug_failure_hook_fn g_failure_hook;

void debug_set_failure_hook(debug_failure_hook_fn fn) {
    g_failure_hook = fn;
}

// Shared tail of gs_assert_fail and gs_unimplemented_fail: dump what the host
// and the guest were doing, stop the machine, and hand the shell back.  Neither
// aborts -- a stopped machine with a message on it is worth more than a dead
// process, and in the browser it is the difference between a diagnosable page
// and a blank one.
static void diagnose_and_halt(const char *kind, const char *expr, const char *file, int line, const char *func) {
    platform_print_host_callstack();
    debug_mac_print_target_backtrace();
    debug_mac_print_process_info_header();
    debug_print_target_trace();

    printf("================================================\n\n");

    bool paused = false;
    scheduler_t *sched = system_scheduler();
    if (sched && scheduler_is_running(sched)) {
        scheduler_stop(sched);
        paused = true;
    }

    if (paused)
        printf("Emulation paused (%s); returning control to shell.\n", kind);
    else
        printf("Handled %s while scheduler idle; shell remains available.\n", kind);
    fflush(stdout);

    // Notify the platform layer (the browser tells its test harness; headless
    // fails the run).
    if (g_failure_hook)
        g_failure_hook(kind, expr, file, line, func);
}

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

    diagnose_and_halt("assertion", expr, file, line, func);
}

// The unimplemented-function handler.  Same diagnostics and the same halt --
// what differs is the claim being made, so the banner says so and nothing here
// is compiled out by GS_FAST (see GS_UNIMPLEMENTED in common.h for why a
// release build is exactly where this one matters).
//
// The banner goes to stderr, unbuffered: if a platform's failure hook
// aborts -- the unit harness does -- a buffered stdout banner is lost at the
// moment it was written for.
void gs_unimplemented_fail(const char *file, int line, const char *func, const char *fmt, ...) {
    fprintf(stderr, "\n\n============= UNIMPLEMENTED =============\n");
    if (fmt) {
        fprintf(stderr, "  ");
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  at %s:%d in %s()\n", file ? file : "<unknown>", line, func ? func : "<unknown>");
    fprintf(stderr, "\n  The guest's request is legal for the hardware being emulated.\n");
    fprintf(stderr, "  This is a gap in Granny Smith, not a fault in the guest.\n");
    fflush(stderr);

    diagnose_and_halt("unimplemented function", NULL, file, line, func);
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

// The address-space enumeration, declared ONCE.
//
// It was a V_ENUM on the read side (bp.space, and lpe.kind's sibling) and a
// V_STRING plus a hand-rolled strcmp on the two method ARGUMENTS, in this same
// file.  So reads were typed and writes were not: an unrecognised string
// silently meant "logical", nothing could complete the values, and
// object-model.md explicitly lists enum membership as something bodies must
// not re-check.
static const char *const debug_space_values[] = {"logical", "physical", NULL};
#define DEBUG_SPACE_COUNT 2

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
    int idx = breakpoint_get_space(bp);
    return val_enum(idx, debug_space_values, DEBUG_SPACE_COUNT);
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
    // Empty string clears the condition.
    breakpoint_set_condition(bp, (in.s && *in.s) ? in.s : NULL);
    value_free(&in);
    return val_none();
}

// Read `enabled`.
static value_t bp_attr_enabled(struct object *self, const member_t *m) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp)
        return val_err("breakpoint detached");
    return val_bool(!bp->disabled);
}

// Write `enabled`: false keeps the breakpoint but stops it firing.
static value_t bp_attr_enabled_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    breakpoint_t *bp = bp_from(self);
    if (!bp) {
        value_free(&in);
        return val_err("breakpoint detached");
    }
    bp->disabled = !val_as_bool(&in);
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
     .flags = VAL_RO,
     .doc = "Address this breakpoint watches, in the space named by `space`",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = bp_attr_addr, .set = NULL}},
    {.kind = M_ATTR,
     .name = "space",
     .flags = VAL_RO,
     .doc = "\"logical\" or \"physical\" — which address `addr` is in (they coincide with the MMU off)",
     .attr = {.type = V_ENUM, .get = bp_attr_space, .set = NULL}                              },
    {.kind = M_ATTR,
     .name = "condition",
     .flags = 0,
     .doc = "Expression that must evaluate true for the breakpoint to stop; empty = always stop",
     .attr = {.type = V_STRING, .get = bp_attr_condition, .set = bp_attr_condition_set}       },
    {.kind = M_ATTR,
     .name = "enabled",
     .flags = 0,
     .doc = "False keeps the breakpoint listed but stops it firing",
     .attr = {.type = V_BOOL, .get = bp_attr_enabled, .set = bp_attr_enabled_set}             },
    {.kind = M_ATTR,
     .name = "hit_count",
     .flags = VAL_RO,
     .doc = "Times this breakpoint has fired since it was added",
     .attr = {.type = V_UINT, .get = bp_attr_hit_count, .set = NULL}                          },
    {.kind = M_ATTR,
     .name = "id",
     .flags = VAL_RO,
     .doc = "Stable identifier; survives the removal of other breakpoints (indices do not)",
     .attr = {.type = V_INT, .get = bp_attr_id, .set = NULL}                                  },
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Remove this breakpoint",
     .flags = 0,
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = bp_method_remove}           },
};

static const class_desc_t breakpoint_entry_class = {
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
     .flags = VAL_RO,
     .doc = "First address of the watched range",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = lpe_attr_addr, .set = NULL}    },
    {.kind = M_ATTR,
     .name = "end_addr",
     .flags = VAL_RO,
     .doc = "Last address of the watched range, inclusive; equals `addr` for a single address",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = lpe_attr_end_addr, .set = NULL}},
    {.kind = M_ATTR,
     .name = "kind",
     .flags = VAL_RO,
     .doc = "What triggers it: \"pc\" on execution, or \"read\"/\"write\"/\"rw\" on a data access",
     .attr = {.type = V_ENUM, .get = lpe_attr_kind, .set = NULL}                                   },
    {.kind = M_ATTR,
     .name = "level",
     .flags = VAL_RO,
     .doc = "Log level each fire is emitted at",
     .attr = {.type = V_INT, .get = lpe_attr_level, .set = NULL}                                   },
    {.kind = M_ATTR,
     .name = "category",
     .flags = VAL_RO,
     .doc = "Log category each fire is emitted under",
     .attr = {.type = V_STRING, .get = lpe_attr_category, .set = NULL}                             },
    {.kind = M_ATTR,
     .name = "message",
     .flags = VAL_RO,
     .doc = "Fire-time template; $value/$addr/$size bind per fire. Empty = the default one-line report",
     .attr = {.type = V_STRING, .get = lpe_attr_message, .set = NULL}                              },
    {.kind = M_ATTR,
     .name = "hit_count",
     .flags = VAL_RO,
     .doc = "Times this logpoint has fired since it was added",
     .attr = {.type = V_UINT, .get = lpe_attr_hit_count, .set = NULL}                              },
    {.kind = M_ATTR,
     .name = "id",
     .flags = VAL_RO,
     .doc = "Stable identifier; survives the removal of other logpoints (indices do not)",
     .attr = {.type = V_INT, .get = lpe_attr_id, .set = NULL}                                      },
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Remove this logpoint",
     .flags = 0,
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = lpe_method_remove}               },
};

static const class_desc_t logpoint_entry_class = {
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
static int bp_entries_next(struct object *self, int prev_index);
static struct object *lp_entries_get(struct object *self, int index);
static int lp_entries_next(struct object *self, int prev_index);

static struct object *bp_entries_get(struct object *self, int index) {
    breakpoint_t *bp = debug_breakpoint_by_id(debug_from(self), index);
    return bp ? breakpoint_get_entry_object(bp) : NULL;
}
static int bp_entries_next(struct object *self, int prev_index) {
    return debug_breakpoint_next_id(debug_from(self), prev_index);
}

static struct object *lp_entries_get(struct object *self, int index) {
    logpoint_t *lp = debug_logpoint_by_id(debug_from(self), index);
    return lp ? logpoint_get_entry_object(lp) : NULL;
}
static int lp_entries_next(struct object *self, int prev_index) {
    return debug_logpoint_next_id(debug_from(self), prev_index);
}

static value_t bp_method_add(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    debug_t *debug = debug_from(self);
    if (!debug)
        return val_err("debugger not initialised");
    uint64_t addr = argv[0].u;
    // Optional `space` (3rd arg): "logical" (default) or "physical".
    // Physical-space breakpoints are only meaningful on the 68030 with
    // the MMU active; on the Plus the two address spaces coincide.
    //
    // Read the enum index, not `.s`: on a V_ENUM the string pointer shares
    // storage with `enm`, so the old `argv[2].s && *argv[2].s` test
    // dereferenced an index as a pointer.  It never fired only because the
    // slot had no default and so was unreachable by name at all.
    addr_space_t space = (argc >= 3 && argv[2].kind == V_ENUM && argv[2].enm.idx == 1) ? ADDR_PHYSICAL : ADDR_LOGICAL;
    // One breakpoint per (address, space): a second add returns the existing
    // entry (a new condition, if given, replaces its old one) instead of
    // stacking a duplicate that would fire twice and need removing twice.
    breakpoint_t *bp = debug->breakpoints;
    while (bp && !(bp->addr == (uint32_t)addr && bp->space == space))
        bp = bp->next;
    if (!bp)
        bp = set_breakpoint(debug, (uint32_t)addr, space);
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
    delete_all_breakpoints(debug);
    return val_none();
}

static value_t lp_method_clear(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    debug_t *debug = debug_from(self);
    if (!debug)
        return val_err("debugger not initialised");
    delete_all_logpoints(debug);
    return val_none();
}

// `debug.logpoints.add` — typed named-argument surface:
//   debug.logpoints.add addr=0x16A width=l mode=write level=5
//       message="Ticks pc=${machine.cpu.pc:08x} val=${$value:08x}"
// `message` is a template slot: stored raw, evaluated per fire
// with `$value`/`$addr`/`$size` bindings in scope. Returns the created
// entry object, like breakpoints.add.
static value_t lp_method_add(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    debug_t *debug = debug_from(self);
    if (!debug)
        return val_err("debugger not initialised");

    // argv: 0 addr, 1 mode, 2 width, 3 end, 4 message, 5 level,
    //       6 category, 7 value, 8 space
    uint32_t addr = (uint32_t)argv[0].u;

    const char *mode = (argc > 1 && argv[1].kind == V_STRING && argv[1].s) ? argv[1].s : "pc";
    int kind;
    if (strcmp(mode, "pc") == 0)
        kind = LP_KIND_PC;
    else if (strcmp(mode, "read") == 0)
        kind = LP_KIND_READ;
    else if (strcmp(mode, "write") == 0)
        kind = LP_KIND_WRITE;
    else if (strcmp(mode, "rw") == 0)
        kind = LP_KIND_RW;
    else
        return val_err("logpoints.add: mode must be pc, read, write, or rw");

    unsigned size = 0;
    if (argc > 2 && argv[2].kind == V_STRING && argv[2].s && argv[2].s[0]) {
        const char *w = argv[2].s;
        if (strcmp(w, "b") == 0)
            size = 1;
        else if (strcmp(w, "w") == 0)
            size = 2;
        else if (strcmp(w, "l") == 0)
            size = 4;
        else
            return val_err("logpoints.add: width must be b, w, or l");
    }

    // The slot is V_UINT, so validate_slot has already coerced anything the
    // caller passed; V_NONE means it passed nothing (obj_arg_unset).
    uint32_t end_addr = addr;
    if (argc > 3 && argv[3].kind == V_UINT)
        end_addr = (uint32_t)argv[3].u;
    // Memory logpoints with a width and no explicit range widen to
    // cover every access overlapping the address.
    if (kind != LP_KIND_PC && size > 0 && end_addr == addr)
        end_addr = addr + size - 1;

    const char *message = (argc > 4 && argv[4].kind == V_STRING && argv[4].s && argv[4].s[0]) ? argv[4].s : NULL;

    int level = 0;
    if (argc > 5) {
        bool ok = false;
        int64_t lv = val_as_i64(&argv[5], &ok);
        if (ok)
            level = (int)lv;
        if (level < 0)
            return val_err("logpoints.add: level must be >= 0");
    }

    const char *category_name = (argc > 6 && argv[6].kind == V_STRING && argv[6].s && argv[6].s[0])
                                    ? argv[6].s
                                    : (kind == LP_KIND_PC ? "logpoint" : "memory");

    bool have_value_filter = false;
    uint32_t value_filter = 0;
    if (argc > 7 && argv[7].kind == V_UINT) {
        have_value_filter = true;
        value_filter = (uint32_t)argv[7].u;
    }
    if (have_value_filter && kind == LP_KIND_PC)
        return val_err("logpoints.add: value filter is only supported on memory logpoints");

    // The slot is V_ENUM against debug_space_values, so the index is the
    // answer -- validate_slot rejected anything that is not in the table.
    addr_space_t space = (argc > 8 && argv[8].kind == V_ENUM && argv[8].enm.idx == 1) ? ADDR_PHYSICAL : ADDR_LOGICAL;

    log_category_t *category = log_get_category(category_name);
    if (!category)
        category = log_register_category(category_name);
    if (!category)
        return val_err("logpoints.add: cannot register category '%s'", category_name);

    logpoint_t *lp;
    if (kind == LP_KIND_PC)
        lp = set_logpoint(debug, addr, end_addr, category, level);
    else
        lp = set_memory_logpoint(debug, addr, end_addr, space, kind, category, level);
    if (!lp)
        return val_err("logpoints.add: allocation failed");
    if (message)
        lp->message = strdup(message);
    if (have_value_filter) {
        lp->value_filter_active = true;
        lp->value_filter = value_filter;
    }
    return val_obj(logpoint_get_entry_object(lp));
}

// "logical" — the default for every space= argument below.
static const value_t def_space_logical = {
    .kind = V_ENUM, .enm = {.idx = 0, .table = debug_space_values, .n_table = DEBUG_SPACE_COUNT}
};

static const arg_decl_t bp_add_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "address"},
    {.name = "condition",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &obj_arg_unset,
     .doc = "optional condition string"},
    {.name = "space",
     .kind = V_ENUM,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .enum_values = debug_space_values,
     .default_value = &def_space_logical,
     .doc = "\"logical\" (default) or \"physical\""},
};

// Interior optional slots need defaults so the named-argument binder's
// V_NONE holes fill instead of erroring (see node_validate_args).
static const value_t lp_def_mode = {.kind = V_STRING, .s = (char *)"pc"};
static const value_t lp_def_width = {.kind = V_STRING, .s = (char *)""};
static const value_t lp_def_message = {.kind = V_STRING, .s = (char *)""};
static const value_t lp_def_level = {.kind = V_INT, .i = 0};
static const value_t lp_def_category = {.kind = V_STRING, .s = (char *)""};

static const arg_decl_t lp_add_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "address (or range start)"},
    {.name = "mode",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &lp_def_mode,
     .doc = "pc (default), read, write, or rw"},
    {.name = "width",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &lp_def_width,
     .doc = "access width for memory modes: b, w, or l"},
    {.name = "end",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_HEX,
     .default_value = &obj_arg_unset,
     .doc = "range end address, inclusive"},
    {.name = "message",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_TEMPLATE,
     .default_value = &lp_def_message,
     .doc = "fire-time template; $value/$addr/$size bind per fire"},
    {.name = "level",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &lp_def_level,
     .doc = "log level (default 0)"},
    {.name = "category",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &lp_def_category,
     .doc = "log category (default: logpoint / memory)"},
    {.name = "value",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .presentation_flags = VAL_HEX,
     .default_value = &obj_arg_unset,
     .doc = "only fire when the accessed value matches (memory modes)"},
    {.name = "space",
     .kind = V_ENUM,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .enum_values = debug_space_values,
     .default_value = &def_space_logical,
     .doc = "\"logical\" (default) or \"physical\""},
};

static const member_t bp_collection_members[] = {
    {.kind = M_METHOD,
     .name = "add",
     .doc = "Add a breakpoint (logical-space by default; pass space=\"physical\" for the 68030 PMMU path); "
            "adding an address that already has one returns that entry", .method = {.args = bp_add_args, .nargs = 3, .result = V_OBJECT, .fn = bp_method_add}},
    {.kind = M_METHOD,
     .name = "clear",
     .doc = "Remove every breakpoint",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = bp_method_clear}},
    // `list` retired: read `entries` — the REPL renders
    // an object list as a table.
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &breakpoint_entry_class,
               .indexed = true,
               .get = bp_entries_get,
               .next = bp_entries_next,
               .lookup = NULL}},
};

static const class_desc_t bp_collection_class = {
    .name = "breakpoints",
    .members = bp_collection_members,
    .n_members = sizeof(bp_collection_members) / sizeof(bp_collection_members[0]),
};

static const member_t lp_collection_members[] = {
    {.kind = M_METHOD,
     .name = "add",
     .doc = "Install a logpoint (named args; message= is a fire-time template)",
     .method = {.args = lp_add_args, .nargs = 9, .result = V_OBJECT, .fn = lp_method_add}},
    {.kind = M_METHOD,
     .name = "clear",
     .doc = "Remove every logpoint",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = lp_method_clear}},
    // `list` retired: read `entries`.
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &logpoint_entry_class,
               .indexed = true,
               .get = lp_entries_get,
               .next = lp_entries_next,
               .lookup = NULL}},
};

static const class_desc_t lp_collection_class = {
    .name = "logpoints",
    .members = lp_collection_members,
    .n_members = sizeof(lp_collection_members) / sizeof(lp_collection_members[0]),
};

// `debug.log(category, level=, stdout=, file=, ts=, pc=)` — per-subsystem
// logging, with real named arguments.
//
// The second slot used to be declared V_NONE -- no type at all -- and accept
// either an integer or a spec string like "level=5 file=tmp/foo.txt
// stdout=off ts=on", which the body then parsed itself with strtok_r.  So the
// framework validated nothing (it had been told nothing to validate),
// completion could offer neither the keys nor their values, and the method
// carried its own boolean vocabulary and its own error wording.
// docs/core/shell/object-model.md ("Library conventions") says in as many
// words that named arguments exist to retire exactly this: "no flag
// grammars inside strings".
//
// `debug.log(cat)` with nothing else prints the category's current settings,
// which is what the bare form always did.
static value_t debug_method_log(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    if (argv[0].kind != V_ENUM || !argv[0].enm.table)
        return val_err("debug.log: category is required");
    const char *category = argv[0].enm.table[argv[0].enm.idx];

    bool touched = false;

    if (argv[1].kind != V_NONE) {
        bool ok = false;
        int64_t level = val_as_i64(&argv[1], &ok);
        if (!ok || level < 0)
            return val_err("debug.log: level must be a non-negative integer");
        if (log_set_category_level(category, (int)level) != 0)
            return val_err("debug.log: cannot set level on '%s'", category);
        touched = true;
    }
    if (argv[2].kind == V_BOOL) {
        log_set_category_stdout(category, argv[2].b);
        touched = true;
    }
    if (argv[3].kind == V_STRING && argv[3].s) {
        if (log_set_category_file(category, argv[3].s) != 0)
            return val_err("debug.log: cannot open log file '%s'", argv[3].s);
        touched = true;
    }
    if (argv[4].kind == V_BOOL) {
        log_set_category_timestamp(category, argv[4].b);
        touched = true;
    }
    if (argv[5].kind == V_BOOL) {
        log_set_category_show_pc(category, argv[5].b);
        touched = true;
    }

    log_print_category(category);
    (void)touched;
    return val_bool(true);
}

// log_foreach_category callback: put "<category>" → <level> into the map.
static void debug_log_level_map_cb(const log_category_t *cat, void *ud) {
    value_map_builder_t *b = (value_map_builder_t *)ud;
    val_map_put(b, log_category_name(cat), val_int(log_get_level(cat)));
}

// `debug.log_levels()` — every registered category and its current level, as a
// map {<category>: <level>, ...}.  The Logs view's level editor reads
// this to populate its list (the categories register lazily, so the set grows
// as subsystems first log; a freshly booted machine has registered its own).
static value_t debug_method_log_levels(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    value_map_builder_t *b = val_map_new();
    log_foreach_category(debug_log_level_map_cb, b);
    return val_map_finish(b);
}

// Every category the manifest declares, as an enum table, so the framework
// rejects a typo and completion can offer all 62 names.
static const char *const debug_log_category_values[] = {
#define X(n, lvl, desc) n,
    GS_LOG_CATEGORIES(X)
#undef X
        NULL};

// "Not supplied", as a default.
//
// An optional slot with no default_value cannot be a HOLE before a later
// given slot -- node_validate_args says so directly: "argc truncation only
// works at the tail".  So `debug.log(cpu, level=3, ts=on)` would fail on
// `file`, which sits between them.  A V_NONE default is filled in and skips
// validation, which is precisely "the caller did not mention this one" and is
// what the body below tests for.

static const arg_decl_t debug_log_args[] = {
    {.name = "category", .kind = V_ENUM, .enum_values = debug_log_category_values, .doc = "Subsystem to configure"},
    {.name = "level",
     .default_value = &obj_arg_unset,
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Verbosity; 0 silences level-1-and-up sites"},
    {.name = "stdout",
     .default_value = &obj_arg_unset,
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Emit to stdout"},
    {.name = "file",
     .default_value = &obj_arg_unset,
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Append to this path; \"off\" closes it"},
    {.name = "ts",
     .default_value = &obj_arg_unset,
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Stamp each line with a timestamp"},
    {.name = "pc",
     .default_value = &obj_arg_unset,
     .kind = V_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Stamp each line with the guest PC"},
};

// `debug.exceptions([filter])` — dump the always-on 256-entry exception ring.
// filter=0 prints everything; filter=1 skips routine traps (A/F-line, TRAP #N,
// IRQ autovectors, trace) so fatal exceptions (bus error/addr error/illegal/
// privilege) stand out.
static const arg_decl_t debug_exceptions_args[] = {
    {.name = "filter",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "0 = print all (default); 1 = filter out routine traps/IRQs"},
};
extern void debug_exc_trace_dump(int filter);
static value_t debug_method_exceptions(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int filter = (argc >= 1) ? (int)argv[0].i : 0;
    debug_exc_trace_dump(filter);
    return val_bool(true);
}

// `debug.disasm([addr], [count])` — disassemble forward.
//   debug.disasm                    PC, 16 instructions
//   debug.disasm <count>            PC, <count> instructions
//   debug.disasm <addr> <count>     <addr>, <count> instructions
// Lives under debug.* (not cpu.*) because it's a pure observation
// operation: same family as debug.breakpoints, debug.logpoints,
// debug.mac.* — debugger affordances that *use* the CPU's encoding
// knowledge but aren't themselves part of running the CPU.
// Prints `count` disassembled instructions to stdout (one line each)
// and returns V_BOOL so `${debug.disasm(5)}` is a truthy single-token
// predicate for shell-script asserts. The web2 UI does NOT consume
// this method's return value — it pulls a structured snapshot via
// `debug.frame` instead — so this can keep the historical
// printf-+-val_bool shape that scripts depend on.
static value_t debug_method_disasm(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const cpu_debug_if_t *dif = system_cpu_debug_if();
    if (!dif)
        return val_err("debug.disasm: CPU not initialised");

    // Either slot may be absent (V_NONE), so read by kind rather than argc:
    // `disasm(20)` is a count, `disasm(0x400000, 20)` is address + count, and
    // `disasm(count=20)` -- which used to fail with "missing argument
    // 'addr_or_count'" -- is a count from the PC.
    uint32_t addr = dif->get_pc(dif->ctx);
    int64_t count = 16;
    bool have_first = argc >= 1 && argv[0].kind == V_INT;
    bool have_second = argc >= 2 && argv[1].kind == V_INT;
    if (have_first && have_second) {
        addr = (uint32_t)argv[0].i;
        count = argv[1].i;
    } else if (have_first) {
        count = argv[0].i;
    } else if (have_second) {
        count = argv[1].i;
    }
    if (count <= 0)
        count = 16;
    if (count > 256)
        count = 256;

    char buf[160];
    for (int i = 0; i < (int)count; i++) {
        int instr_len = debugger_disasm(buf, sizeof(buf), addr); // returns bytes
        printf("%s\n", buf);
        addr += (uint32_t)instr_len;
    }
    return val_bool(true);
}

static const arg_decl_t debug_disasm_args[] = {
    {.name = "addr_or_count",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &obj_arg_unset,
     .doc = "With one arg: instruction count (from PC, default 16). With two args: start address."},
    {.name = "count",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Number of instructions when addr is given as the first argument."},
};

// Choose where a disassembly window starts so that `before` rows precede
// the PC and one row lands exactly on it.  68K instructions are 2-20 bytes,
// so decoding forward from an arbitrary earlier address can step over the
// PC; this tries even start offsets behind the PC and keeps the first that
// re-synchronises on it after exactly `before` instructions (else the one
// with the most rows that still lands on it, else the PC itself).  On PPC
// every instruction is 4 bytes and the first aligned candidate wins.
static uint32_t frame_anchor(const cpu_debug_if_t *dif, uint32_t pc, int before) {
    char buf[128];
    uint32_t best = pc;
    int best_rows = 0;
    for (uint32_t back = 2; back <= (uint32_t)before * 20; back += 2) {
        uint32_t pos = pc - back;
        int rows = 0;
        while (pos < pc && rows <= before) {
            int n = dif->disasm(dif->ctx, pos, buf, sizeof(buf));
            pos += (uint32_t)(n > 0 ? n : 2);
            rows++;
        }
        if (pos != pc || rows > before)
            continue; // stepped over the PC, or too far back
        if (rows == before)
            return pc - back; // exactly `before` rows of context
        if (rows > best_rows) {
            best_rows = rows;
            best = pc - back;
        }
    }
    return best;
}

value_t debug_translation_result(uint32_t phys, bool valid, const char *via) {
    value_map_builder_t *b = val_map_new();
    if (valid) {
        value_t p = val_uint(4, phys);
        p.flags |= VAL_HEX;
        val_map_put(b, "phys", p);
    }
    val_map_put(b, "valid", val_bool(valid));
    val_map_put(b, "via", val_str(via));
    return val_map_finish(b);
}

bool debug_parse_space(int argc, const value_t *argv, int idx, bool *physical) {
    *physical = false;
    if (argc <= idx || argv[idx].kind == V_NONE)
        return true; // omitted: logical
    if (argv[idx].kind != V_STRING || !argv[idx].s)
        return false;
    if (strcmp(argv[idx].s, "logical") == 0)
        return true;
    if (strcmp(argv[idx].s, "physical") == 0) {
        *physical = true;
        return true;
    }
    return false;
}

// Split one disassembled instruction (the cpu_debug_if_t `disasm` text) into
// the row's mnemonic and operands: at the tab, or -- for an algebraic
// syntax with no mnemonic column, like the DSP3210's -- all of it as the
// mnemonic.  An empty text is an illegal encoding.
static void frame_split_disasm(const char *buf, char *mnem, size_t mnem_len, char *ops, size_t ops_len) {
    if (!buf[0]) {
        snprintf(mnem, mnem_len, "ILLEGAL");
        ops[0] = '\0';
        return;
    }
    const char *tab = strchr(buf, '\t');
    if (!tab) {
        snprintf(mnem, mnem_len, "%s", buf);
        ops[0] = '\0';
        return;
    }
    snprintf(mnem, mnem_len, "%.*s", (int)(tab - buf), buf);
    snprintf(ops, ops_len, "%s", tab + 1);
}

// The frame of one CPU-like core -- `debug.frame`, `machine.cpu.frame` and
// an auxiliary core's `frame` (machine.dsp) all answer this, so the Debug
// view renders any of them with one component.  Bundles the register file,
// a disassembly window and per-row translation into one map, one bridge
// round-trip.  Everything architecture-specific comes from the core's
// cpu_debug_if_t (its register names, its FPU, its instruction-side
// translation), so it works unchanged on a 68000, a 68030/040, a PowerPC
// 601/604 and the DSP3210; before, debug.frame read the 68K cpu_t and failed
// on every PowerPC machine, and the DSP had no frame at all.
//
// Output (a V_MAP):
//   {
//     "arch": "m68k" | "ppc" | "dsp3210",
//     "pc":   <int>,
//     "regs": { name: <int>, ... },     // the core's own names: d0..a7/pc/sr/usp/ssp,
//                                       // r0..r31/pc/lr/ctr/cr/xer/msr/srr0/srr1[/mq],
//                                       // or the DSP's r1..r22/pc/ps/emr/pcw/dauc/ctr
//     "rows": [ { "addr", "phys" (int|null), "valid", "mnem", "ops" }, ... ],
//     "fpu":  { ... }                   // only when the core has one:
//                                       // 68K {fp:[{hex,val}]x8, fpcr, fpsr, fpiar}
//                                       // PPC {fpr:[{hex,val}]x32, fpscr}
//                                       // DSP {a:[{hex,val}]x4} (the DAU accumulators)
//   }
//
// With no `addr`, the window starts at the PC, or `before` instructions
// ahead of it (re-synchronised so a row always lands on the PC).  Use named
// arguments from JS: a single positional argument is `addr`, not `count`.
// Address values are plain integers so the JS side needs no conversion.
value_t debug_frame_build(const cpu_debug_if_t *dif, const char *who, int argc, const value_t *argv) {
    if (!dif || !dif->get_pc || !dif->regs || !dif->disasm)
        return val_err("%s: CPU not initialised", who);

    uint32_t pc = dif->get_pc(dif->ctx);
    bool have_addr = argc >= 1 && argv[0].kind == V_INT;
    int64_t count = (argc >= 2 && argv[1].kind == V_INT) ? argv[1].i : 32;
    int64_t before = (argc >= 3 && argv[2].kind == V_INT) ? argv[2].i : 0;
    if (count <= 0)
        count = 32;
    if (count > 256)
        count = 256;
    if (before < 0)
        before = 0;
    if (before >= count)
        before = count - 1;
    uint32_t addr = have_addr ? (uint32_t)argv[0].i : (before ? frame_anchor(dif, pc, (int)before) : pc);

    value_map_builder_t *b = val_map_new();
    val_map_put(b, "arch", val_str(dif->arch ? dif->arch : "unknown"));
    val_map_put(b, "pc", val_int((int64_t)pc));

    value_map_builder_t *regs = val_map_new();
    dif->regs(dif->ctx, regs);
    val_map_put(b, "regs", val_map_finish(regs));

    // Disasm rows + per-row translation, instruction side (the IBATs on PPC);
    // a core with no translation at all addresses physical memory directly.
    uint32_t (*xlate)(void *, uint32_t, bool *) = dif->translate_code ? dif->translate_code : dif->translate;
    value_t *rows = NULL;
    size_t n_rows = 0, cap_rows = 0;
    char buf[128], mnem[100], ops[100];
    for (int i = 0; i < (int)count; i++) {
        value_map_builder_t *row = val_map_new();
        val_map_put(row, "addr", val_int((int64_t)addr));
        bool valid = true;
        uint32_t phys = xlate ? xlate(dif->ctx, addr, &valid) : addr;
        val_map_put(row, "phys", valid ? val_int((int64_t)phys) : val_none());
        val_map_put(row, "valid", val_bool(valid));
        buf[0] = '\0';
        int n = dif->disasm(dif->ctx, addr, buf, sizeof(buf)); // bytes consumed
        frame_split_disasm(buf, mnem, sizeof(mnem), ops, sizeof(ops));
        val_map_put(row, "mnem", val_str(mnem));
        val_map_put(row, "ops", val_str(ops));
        val_list_push(&rows, &n_rows, &cap_rows, val_map_finish(row));
        addr += (uint32_t)(n > 0 ? n : 2);
    }
    val_map_put(b, "rows", val_list(rows, n_rows));

    // FPU block — only when the core has one.
    if (dif->fpu) {
        value_map_builder_t *fb = val_map_new();
        if (dif->fpu(dif->ctx, fb))
            val_map_put(b, "fpu", val_map_finish(fb));
        else {
            value_t unused = val_map_finish(fb);
            value_free(&unused);
        }
    }

    return val_map_finish(b);
}

// `debug.frame([addr], [count], [before])` — the main CPU's frame; the same
// as `machine.cpu.frame`, kept under debug for the tools that call it.
static value_t debug_method_frame(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    return debug_frame_build(system_cpu_debug_if(), "debug.frame", argc, argv);
}

// The frame's default row count: a named `before` must be reachable past it.
static const value_t k_frame_count32 = {.kind = V_INT, .i = 32};

const arg_decl_t debug_frame_args[DEBUG_FRAME_NARGS] = {
    {.name = "addr",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &obj_arg_unset,
     .doc = "Start address (default PC)"},
    {.name = "count",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_frame_count32,
     .doc = "Number of rows (default 32)"},
    {.name = "before",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Rows to show ahead of the PC when addr is omitted (default 0)"},
};

// `debug.step([n])` — single-step n instructions (default 1) and stop.
// Wraps the scheduler's run-N-then-stop pattern in one call so debug
// scripts don't have to chain `scheduler.run(n)` + `scheduler.stop`.
static value_t debug_method_step(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t count = (argc >= 1) ? argv[0].i : 1;
    if (count <= 0)
        return val_err("debug.step: count must be positive");
    scheduler_t *s = system_scheduler();
    if (!s)
        return val_err("debug.step: scheduler not initialised");
    // Arm the same instruction budget `scheduler.run N` does and drive it
    // through scheduler_run_frame -- the loop the headless pump runs -- so
    // stepped time pulses VBL and consumes frame_cycles_left exactly as
    // running does.  It used to call scheduler_run_instructions, which
    // advanced cpu_cycles with the VBL line dead: N stepped instructions did
    // not match N run ones.  Still synchronous, so `while cond { debug.step 1 }`
    // works in the browser terminal too, where nothing pumps between
    // statements; returning at once would need an asynchronous step, which
    // does not exist yet.
    if (!scheduler_run_with_budget(s, (uint64_t)count))
        return val_err("debug.step: instruction count too large");
    while (scheduler_is_running(s))
        scheduler_run_frame(s, global_emulator);
    return val_bool(true);
}

static const arg_decl_t debug_step_args[] = {
    {.name = "count", .kind = V_INT, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Number of instructions (default 1)"},
};

static const member_t debug_members[] = {
    {.kind = M_METHOD,
     .name = "log",
     .doc = "Configure a log category: debug.log(cat, level=, stdout=, file=, ts=, pc=)",
     .method = {.args = debug_log_args, .nargs = 6, .result = V_BOOL, .fn = debug_method_log}                                                                                                                },
    {.kind = M_METHOD,
     .name = "disasm",
     .doc = "Disassemble forward. `disasm` from PC, `disasm <count>` from PC, `disasm <addr> <count>` from addr.",
     .method = {.args = debug_disasm_args, .nargs = 2, .result = V_BOOL, .fn = debug_method_disasm}                                                                                                          },
    {.kind = M_METHOD,
     .name = "frame",
     .doc = "The CPU's debug frame: registers, disassembly, per-row translation (= machine.cpu.frame)",
     .method = {.args = debug_frame_args, .nargs = DEBUG_FRAME_NARGS, .result = V_MAP, .fn = debug_method_frame}                                                                                             },
    {.kind = M_METHOD,
     .name = "step",
     .doc = "Run N instructions (default 1) and stop, through the frame loop exactly as scheduler.run N does "
            "(VBL and timers keep running)",                                                                       .method = {.args = debug_step_args, .nargs = 1, .result = V_BOOL, .fn = debug_method_step}},
    {.kind = M_METHOD,
     .name = "exceptions",
     .doc = "Dump the 256-entry exception trace ring (always-on). Optional filter=1 hides routine traps/IRQs.",
     .method = {.args = debug_exceptions_args, .nargs = 1, .result = V_BOOL, .fn = debug_method_exceptions}                                                                                                  },
    {.kind = M_METHOD,
     .name = "log_levels",
     .doc = "Every registered log category and its level as a map {<cat>: <level>}.",
     .method = {.args = NULL, .nargs = 0, .result = V_MAP, .fn = debug_method_log_levels}                                                                                                                    },
};

static const class_desc_t debug_class = {
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
    (void)argc;
    int idx = mac_global_lookup(argv[0].s);
    if (idx < 0)
        return val_err("debug.mac.globals.read: unknown global '%s'", argv[0].s);
    // Globals live in the mac world's logical space (identity on 68K
    // machines; the user-data view on PDM — debug_mac_xlate).
    uint32_t addr = mac_global_vars[idx].address;
    int sz = mac_global_vars[idx].size;
    switch (sz) {
    case 1: {
        value_t v = val_uint(1, memory_debug_read_uint8(debug_mac_xlate(addr)));
        v.flags |= VAL_HEX;
        return v;
    }
    case 2: {
        value_t v = val_uint(2, memory_debug_read_uint16(debug_mac_xlate(addr)));
        v.flags |= VAL_HEX;
        return v;
    }
    case 4: {
        value_t v = val_uint(4, memory_debug_read_uint32(debug_mac_xlate(addr)));
        v.flags |= VAL_HEX;
        return v;
    }
    default: {
        // Wider entries (KeyMap 16, EventQueue 10, FileVars 184, …) come
        // back as a byte buffer; the method's result slot is declared
        // V_ANY precisely so this branch is legal (issue #106).
        uint8_t buf[256];
        if (sz <= 0)
            return val_err("debug.mac.globals.read: '%s' is a region marker with no size — "
                           "use debug.mac.globals.address(\"%s\") and read memory directly",
                           argv[0].s, argv[0].s);
        if (sz > (int)sizeof(buf))
            return val_err("debug.mac.globals.read: '%s' is %d bytes (max %zu)", argv[0].s, sz, sizeof(buf));
        for (int i = 0; i < sz; i++)
            buf[i] = memory_debug_read_uint8(debug_mac_xlate(addr + (uint32_t)i));
        return val_bytes(buf, (size_t)sz);
    }
    }
}

static value_t method_mac_globals_write(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    int idx = mac_global_lookup(argv[0].s);
    if (idx < 0)
        return val_err("debug.mac.globals.write: unknown global '%s'", argv[0].s);
    uint64_t v = argv[1].u;
    uint32_t addr = mac_global_vars[idx].address;
    int sz = mac_global_vars[idx].size;
    // On a mac-world-translated machine (PDM) the resolved address is
    // physical, so the write must take the debug path; 68K machines keep
    // the historical live-CPU write.
    const cpu_debug_if_t *dif = system_cpu_debug_if();
    bool xl = dif && dif->translate_mac;
    switch (sz) {
    case 1:
        if (xl)
            memory_debug_write_uint8(debug_mac_xlate(addr), (uint8_t)v);
        else
            memory_write_uint8(addr, (uint8_t)v);
        break;
    case 2:
        if (xl)
            memory_debug_write_uint16(debug_mac_xlate(addr), (uint16_t)v);
        else
            memory_write_uint16(addr, (uint16_t)v);
        break;
    case 4:
        if (xl)
            memory_debug_write_uint32(debug_mac_xlate(addr), (uint32_t)v);
        else
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
    (void)argc;
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
    {.name = "value", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "value to write"},
};

static const member_t debug_mac_globals_members[] = {
    {.kind = M_METHOD,
     .name = "read",
     .doc = "Read a Mac low-memory global by name (uint for 1/2/4-byte; bytes for larger)",
     // V_ANY, not V_UINT: the result kind follows the entry's width — 49
     // of the 471 globals are wider than 4 bytes and read as V_BYTES.
     .method = {.args = mac_globals_name_arg, .nargs = 1, .result = V_ANY, .fn = method_mac_globals_read}    },
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

static const class_desc_t debug_mac_globals_class = {
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
    (void)argc;
    return val_str(macos_atrap_name((uint16_t)argv[0].u));
}

static const arg_decl_t mac_atrap_args[] = {
    {.name = "opcode", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "A-trap opcode (e.g. 0xA86E)"},
};

static const member_t debug_mac_members[] = {
    {.kind = M_METHOD,
     .name = "atrap",
     .doc = "Resolve an A-trap opcode to its symbolic name",
     .method = {.args = mac_atrap_args, .nargs = 1, .result = V_STRING, .fn = method_mac_atrap}},
};

static const class_desc_t debug_mac_class = {
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
    (void)argc;
    const char *path = argv[0].s;
    if (!*path)
        return val_err("screen.save: empty path");
    size_t n = strlen(path);
    if (n < 4 || strcasecmp(path + n - 4, ".png") != 0)
        return val_err("screen.save: path must end in .png (got '%s')", path);
    const display_t *d = system_display_synced();
    if (!d || !d->bits)
        return val_err("screen.save: framebuffer not available");
    if (save_framebuffer_as_png(d, path) < 0)
        return val_err("screen.save: failed to save '%s'", path);
    return val_bool(true);
}

// `screen.match(reference)` — bitwise compare the framebuffer against
// a reference PNG.  A mismatch is a HARD failure: returns val_err so
// the path-form dispatcher (and hence the headless script runner)
// propagates non-zero out, failing the integration test.  Use
// `screen.match_or_save` for the non-fatal diagnostic flow.
// Parse the optional exclude rectangle shared by `screen.match` and
// `screen.matches`: either just the reference (whole-screen compare) or the
// reference plus all four region edges (top, left, bottom, right) — reject
// anything in between.  Returns NULL through *err_out on a bad call.
// Parse 0, 1 or 2 exclude rectangles from the optional arguments and write
// them into `rect` (4 ints each).  Returns the count, or -1 on error.
static int screen_parse_exclude_rects(const char *who, const display_t *d, int argc, const value_t *argv, int rect[8],
                                      value_t *err_out) {
    if (argc != 1 && argc != 5 && argc != 9) {
        *err_out = val_err("%s: expected (reference), (reference, top, left, bottom, right) or the same with a "
                           "second rectangle appended",
                           who);
        return -1;
    }
    int n = (argc - 1) / 4;
    for (int r = 0; r < n; r++) {
        int base = 1 + r * 4;
        int top = (int)argv[base].i, left = (int)argv[base + 1].i;
        int bottom = (int)argv[base + 2].i, right = (int)argv[base + 3].i;
        if (top < 0 || left < 0 || bottom <= top || right <= left || bottom > (int)d->height || right > (int)d->width) {
            *err_out = val_err("%s: invalid exclude region %d for (0,0)-(%u,%u)", who, r + 1, d->width, d->height);
            return -1;
        }
        rect[r * 4] = top;
        rect[r * 4 + 1] = left;
        rect[r * 4 + 2] = bottom;
        rect[r * 4 + 3] = right;
    }
    return n;
}

static value_t screen_method_match(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *ref = argv[0].s;
    const display_t *d = system_display_synced();
    if (!d || !d->bits)
        return val_err("screen.match: framebuffer not available");
    int rect[8];
    value_t err = val_none();
    int n_rects = screen_parse_exclude_rects("screen.match", d, argc, argv, rect, &err);
    if (n_rects < 0)
        return err;
    int result = match_framebuffer_with_png(d, ref, n_rects ? rect : NULL, n_rects);
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

// `screen.matches(reference[, top, left, bottom, right])` — the non-fatal
// twin of `screen.match`: same comparison (including the optional exclude
// rectangle), V_BOOL result, no abort, no diff artifacts, and no per-call
// output (it is a polling primitive — wait_match() in the integration
// library calls it every few million cycles).  An unreadable reference is
// still a hard error: polling against a missing golden would spin to the
// ceiling and report a timeout instead of the actual mistake.
static value_t screen_method_matches(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *ref = argv[0].s;
    const display_t *d = system_display_synced();
    if (!d || !d->bits)
        return val_err("screen.matches: framebuffer not available");
    int rect[8];
    value_t err = val_none();
    int n_rects = screen_parse_exclude_rects("screen.matches", d, argc, argv, rect, &err);
    if (n_rects < 0)
        return err;
    int result = match_framebuffer_with_png(d, ref, n_rects ? rect : NULL, n_rects);
    if (result < 0)
        return val_err("screen.matches: cannot load reference '%s'", ref);
    return val_bool(result == 0);
}

static value_t screen_method_match_or_save(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *ref = argv[0].s;
    const char *actual = (argc >= 2 && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    const display_t *d = system_display_synced();
    if (!d || !d->bits)
        return val_err("screen.match_or_save: framebuffer not available");
    int result = match_framebuffer_with_png(d, ref, NULL, 0);
    if (result < 0) {
        printf("MATCH FAILED: Error loading reference image.\n");
        if (actual)
            save_framebuffer_as_png(d, actual);
        return val_bool(false);
    }
    if (result == 0) {
        printf("MATCH OK: Screen matches '%s'.\n", ref);
        return val_bool(true);
    }
    if (actual) {
        save_framebuffer_as_png(d, actual);
        printf("MATCH FAILED: Screen does not match '%s'. Saved actual to '%s'.\n", ref, actual);
    } else {
        printf("MATCH FAILED: Screen does not match '%s'.\n", ref);
    }
    return val_bool(false);
}

static value_t screen_method_checksum(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const display_t *d = system_display_synced();
    if (!d || !d->bits)
        return val_err("screen.checksum: framebuffer not available");
    if (argc == 0)
        return val_int((int64_t)(int32_t)framebuffer_checksum(d));
    if (argc < 4)
        return val_err("screen.checksum: expected (top, left, bottom, right) or no args");
    int64_t t = argv[0].i, l = argv[1].i, b = argv[2].i, r = argv[3].i;
    if (t < 0 || l < 0 || b <= t || r <= l || b > (int64_t)d->height || r > (int64_t)d->width)
        return val_err("screen.checksum: invalid region bounds (0,0)-(%u,%u)", d->width, d->height);
    return val_int((int64_t)(int32_t)framebuffer_region_checksum(d, (int)t, (int)l, (int)b, (int)r));
}

// `screen.width` / `screen.height` — read the active display's pixel
// dimensions.  Lets e2e and integration tests stop hardcoding 512/342 so
// they keep working when a IIcx/IIx with a 640x480 NuBus card boots.
static value_t screen_attr_width(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const display_t *d = system_display();
    return val_int(d ? (int64_t)d->width : 0);
}

static value_t screen_attr_height(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const display_t *d = system_display();
    return val_int(d ? (int64_t)d->height : 0);
}

// `screen.depth` — bits per pixel of the active display, whichever device
// is driving it (built-in video or a NuBus card).  Per-card depth is
// reachable as `nubus.slot[N].card.framebuffer.depth`, but the coverage
// records the integration suites emit need the depth of the screen that
// is actually live, without first knowing which device owns it.
static value_t screen_attr_depth(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const display_t *d = system_display();
    // A fourth copy of the bits-per-pixel switch lived here -- and this is the
    // one every integration row asserts on (`machine.screen.depth`), so it is
    // the copy that had to stay right while the others drifted.  display.h
    // owns it now.
    return val_int(d ? (int)display_bpp(d->format) : 0);
}

// `screen.par_w` / `screen.par_h` — the active display's pixel aspect ratio
// (one display pixel's width:height in host units; see display.h).  1:1 is
// square (every Mac); the Lisa 2's 720x364 raster reports 2:3 so the frontend
// can stretch it vertically.  0 in the descriptor normalizes to 1 here.
static value_t screen_attr_par_w(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const display_t *d = system_display();
    return val_int(d && d->par_w ? (int64_t)d->par_w : 1);
}

static value_t screen_attr_par_h(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const display_t *d = system_display();
    return val_int(d && d->par_h ? (int64_t)d->par_h : 1);
}

// `screen.source` — a non-owning reference edge to the active NuBus card's
// framebuffer node (machine.screen.source → reference →
// machine.nubus.slot[N].card.framebuffer).  Re-resolved on each access via
// nubus_active_framebuffer_object(), so a card swap or machine teardown can
// never leave it dangling (a held pointer plus an invalidator is the hot-path
// pattern, and it applies to per-frame rendering, which uses
// nubus_primary_display() directly — not this navigational link).  NULL (no
// source) on builtin-video machines.
// `machine.screen.source` — a reference edge to whichever framebuffer node
// is currently driving the display.  A seated PCI display card wins over a
// NuBus one: on the machines that have both, the PCI card is the primary
// display (pci_primary_display walks the slot table in declared order).
// Screen CAPTURE does not depend on this — it reads the substrate's
// display_t — so this is a debugging convenience, not a correctness path.
static struct object *screen_source_lookup(struct object *self, const char *name) {
    (void)self;
    (void)name;
    struct object *pci_fb = pci_active_framebuffer_object();
    return pci_fb ? pci_fb : nubus_active_framebuffer_object();
}

static const arg_decl_t screen_save_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Output PNG path (must end in .png)"},
};
static const arg_decl_t screen_match_args[] = {
    {.name = "reference", .kind = V_STRING, .doc = "Reference PNG path"},
    {.name = "top",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Exclude-region top edge"},
    {.name = "left",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Exclude-region left edge"},
    {.name = "bottom",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Exclude-region bottom edge"},
    {.name = "right",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Exclude-region right edge"},
    {.name = "top2",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Second exclude-region top edge"},
    {.name = "left2",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Second exclude-region left edge"},
    {.name = "bottom2",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Second exclude-region bottom edge"},
    {.name = "right2",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Second exclude-region right edge"},
};
static const arg_decl_t screen_matches_args[] = {
    {.name = "reference", .kind = V_STRING, .doc = "Reference PNG path"},
    {.name = "top",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Exclude-region top edge"},
    {.name = "left",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Exclude-region left edge"},
    {.name = "bottom",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Exclude-region bottom edge"},
    {.name = "right",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Exclude-region right edge"},
    {.name = "top2",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Second exclude-region top edge"},
    {.name = "left2",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Second exclude-region left edge"},
    {.name = "bottom2",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Second exclude-region bottom edge"},
    {.name = "right2",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Second exclude-region right edge"},
};
static const arg_decl_t screen_match_or_save_args[] = {
    {.name = "reference", .kind = V_STRING, .doc = "Reference PNG path"},
    {.name = "actual",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Path to write current screen on miss"},
};
static const arg_decl_t screen_checksum_args[] = {
    {.name = "top",    .kind = V_INT, .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED, .doc = "Region top edge" },
    {.name = "left",   .kind = V_INT, .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED, .doc = "Region left edge"},
    {.name = "bottom",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Region bottom edge"                                                                                       },
    {.name = "right",
     .kind = V_INT,
     .validation_flags = OBJ_ARG_OPTIONAL | OBJ_ARG_GROUPED,
     .doc = "Region right edge"                                                                                        },
};
// Stride and format: every per-card framebuffer node has had these, and the
// generic `screen` node -- the one node that exists on EVERY machine,
// including the ones with built-in video and no card node at all -- did not.
static value_t screen_attr_stride(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const display_t *d = system_display();
    return val_uint(4, d ? d->stride : 0);
}
static value_t screen_attr_format(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    const display_t *d = system_display();
    return val_str(d ? display_format_name(d->format) : "");
}

static const member_t screen_members[] = {
    {.kind = M_ATTR,
     .name = "width",
     .flags = VAL_RO,
     .doc = "Active display width in pixels",
     .attr = {.type = V_INT, .get = screen_attr_width, .set = NULL}},
    {.kind = M_ATTR,
     .name = "height",
     .flags = VAL_RO,
     .doc = "Active display height in pixels",
     .attr = {.type = V_INT, .get = screen_attr_height, .set = NULL}},
    {.kind = M_ATTR,
     .name = "depth",
     .flags = VAL_RO,
     .doc = "Bits per pixel of the active display (1/2/4/8/16/32; 0 if unknown)",
     .attr = {.type = V_INT, .get = screen_attr_depth, .set = NULL}},
    {.kind = M_ATTR,
     .name = "stride",
     .flags = VAL_RO,
     .doc = "Row stride in bytes (rowBytes) of the active display",
     .attr = {.type = V_UINT, .get = screen_attr_stride, .set = NULL}},
    {.kind = M_ATTR,
     .name = "format",
     .flags = VAL_RO,
     .doc = "Pixel encoding of the active display",
     .attr = {.type = V_STRING, .get = screen_attr_format, .set = NULL}},
    {.kind = M_ATTR,
     .name = "par_w",
     .flags = VAL_RO,
     .doc = "Pixel aspect ratio numerator (display pixel width; 1 = square)",
     .attr = {.type = V_INT, .get = screen_attr_par_w, .set = NULL}},
    {.kind = M_ATTR,
     .name = "par_h",
     .flags = VAL_RO,
     .doc = "Pixel aspect ratio denominator (display pixel height; 1 = square)",
     .attr = {.type = V_INT, .get = screen_attr_par_h, .set = NULL}},
    {.kind = M_METHOD,
     .name = "save",
     .doc = "Save the current framebuffer to a PNG file",
     .method = {.args = screen_save_args, .nargs = 1, .result = V_BOOL, .fn = screen_method_save}},
    {.kind = M_METHOD,
     .name = "match",
     .doc = "Compare the framebuffer against a reference PNG (true if identical); optional "
            "(top, left, bottom, right) excludes a region from the compare", .method = {.args = screen_match_args, .nargs = 9, .result = V_BOOL, .fn = screen_method_match}},
    {.kind = M_METHOD,
     .name = "matches",
     .doc = "Non-fatal `match`: true/false without aborting, artifacts, or output (polling primitive); optional "
            "(top, left, bottom, right) excludes a region from the compare", .method = {.args = screen_matches_args, .nargs = 9, .result = V_BOOL, .fn = screen_method_matches}},
    {.kind = M_METHOD,
     .name = "match_or_save",
     .doc = "Like `match`, but also write the current screen to `actual` on mismatch",
     .method = {.args = screen_match_or_save_args, .nargs = 2, .result = V_BOOL, .fn = screen_method_match_or_save}},
    {.kind = M_METHOD,
     .name = "checksum",
     .doc = "Polynomial hash of the framebuffer (full screen or top/left/bottom/right region)",
     .method = {.args = screen_checksum_args, .nargs = 4, .result = V_INT, .fn = screen_method_checksum}},
    {.kind = M_CHILD,
     .name = "source",
     .doc = "Reference to the active card's framebuffer node driving this screen",
     .label = "Source",
     .child = {.cls = NULL, .reference = true, .lookup = screen_source_lookup}},
};

static const class_desc_t screen_class = {
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
    if (s_screen_object) {
        object_set_label(s_screen_object, "Screen");
        object_set_order(s_screen_object, 120);
        object_attach(machine_object(), s_screen_object);
    }
}
