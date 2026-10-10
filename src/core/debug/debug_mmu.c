// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug_mmu.c
// The shared half of machine.cpu.mmu's inspection methods (debug_mmu.h):
// the result shapes of translate / walk / map, the map sweep, and the
// argument tables, over each MMU's own side-effect-free translation.

#include "debug_mmu.h"

#include <string.h>

// Put an unsigned hex field (addresses, descriptor words).
void debug_mmu_put_hex(value_map_builder_t *b, const char *key, uint32_t v) {
    value_t x = val_uint(4, v);
    x.flags |= VFLAG_HEX;
    val_map_put(b, key, x);
}

// Put a flag field.
void debug_mmu_put_bool(value_map_builder_t *b, const char *key, bool v) {
    val_map_put(b, key, val_bool(v));
}

// The fields translate, walk and each map run share: phys (valid only),
// valid, via, access (valid only) and space (the Lisa).
static void put_xlate_fields(value_map_builder_t *b, const mmu_xlate_t *x) {
    if (x->valid)
        debug_mmu_put_hex(b, "phys", x->phys);
    val_map_put(b, "valid", val_bool(x->valid));
    val_map_put(b, "via", val_str(x->via ? x->via : "page"));
    if (x->valid && x->access)
        val_map_put(b, "access", val_str(x->access));
    if (x->valid && x->space)
        val_map_put(b, "space", val_str(x->space));
}

value_t debug_mmu_xlate_result(const mmu_xlate_t *x) {
    value_map_builder_t *b = val_map_new();
    put_xlate_fields(b, x);
    return val_map_finish(b);
}

// One trace step as a map: {step, outcome, ...fields in recording order}.
static value_t step_value(const mmu_trace_step_t *s) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "step", val_str(s->step));
    val_map_put(b, "outcome", val_str(s->outcome));
    for (int i = 0; i < s->n_fields; i++) {
        const mmu_trace_field_t *f = &s->fields[i];
        switch (f->kind) {
        case MMU_TF_HEX:
            debug_mmu_put_hex(b, f->key, f->u);
            break;
        case MMU_TF_UINT:
            val_map_put(b, f->key, val_uint(4, f->u));
            break;
        case MMU_TF_BOOL:
            val_map_put(b, f->key, val_bool(f->u != 0));
            break;
        case MMU_TF_STR:
            val_map_put(b, f->key, val_str(f->s ? f->s : ""));
            break;
        }
    }
    return val_map_finish(b);
}

value_t debug_mmu_walk_result(const mmu_xlate_t *x, const mmu_trace_t *trace) {
    value_map_builder_t *b = val_map_new();
    put_xlate_fields(b, x);
    value_t *items = NULL;
    size_t len = 0, cap = 0;
    for (int i = 0; trace && i < trace->n_steps; i++)
        val_list_push(&items, &len, &cap, step_value(&trace->steps[i]));
    val_map_put(b, "steps", val_list(items, len));
    return val_map_finish(b);
}

void debug_mmu_parse_mode(int argc, const value_t *argv, int idx, bool default_supervisor, bool *supervisor,
                          bool *fetch) {
    *supervisor = (argc > idx && argv[idx].kind == VK_BOOL) ? argv[idx].b : default_supervisor;
    *fetch = argc > idx + 1 && argv[idx + 1].kind == VK_BOOL && argv[idx + 1].b;
}

value_t debug_mmu_translate(debug_mmu_xlate_fn fn, void *ctx, bool default_supervisor, int argc, const value_t *argv) {
    bool sup, fetch;
    debug_mmu_parse_mode(argc, argv, 1, default_supervisor, &sup, &fetch);
    mmu_xlate_t x = {0};
    fn(ctx, (uint32_t)argv[0].u, sup, fetch, &x, NULL);
    return debug_mmu_xlate_result(&x);
}

value_t debug_mmu_walk(debug_mmu_xlate_fn fn, void *ctx, bool default_supervisor, int argc, const value_t *argv) {
    bool sup, fetch;
    debug_mmu_parse_mode(argc, argv, 1, default_supervisor, &sup, &fetch);
    mmu_xlate_t x = {0};
    mmu_trace_t trace;
    trace.n_steps = 0;
    fn(ctx, (uint32_t)argv[0].u, sup, fetch, &x, &trace);
    return debug_mmu_walk_result(&x, &trace);
}

// A run being accumulated by the map sweep.
typedef struct {
    uint64_t start, end; // [start, end) logical
    uint32_t phys; // physical address of `start`
    mmu_xlate_t x; // the run's via/access/space
} map_run_t;

// The run as a list item.
static value_t run_value(const map_run_t *r) {
    value_map_builder_t *b = val_map_new();
    debug_mmu_put_hex(b, "start", (uint32_t)r->start);
    value_t size = val_uint(8, r->end - r->start);
    size.flags |= VFLAG_HEX;
    val_map_put(b, "size", size);
    debug_mmu_put_hex(b, "phys", r->phys);
    val_map_put(b, "via", val_str(r->x.via ? r->x.via : "page"));
    if (r->x.access)
        val_map_put(b, "access", val_str(r->x.access));
    if (r->x.space)
        val_map_put(b, "space", val_str(r->x.space));
    return val_map_finish(b);
}

// Same string, NULL-tolerant.
static bool same_str(const char *a, const char *b) {
    return a == b || (a && b && strcmp(a, b) == 0);
}

// Default map limit: enough for any real table layout, small enough that a
// fragmented address space cannot produce an unbounded answer.
static const value_t k_map_limit_default = {.kind = VK_UINT, .u = 512};

value_t debug_mmu_map(debug_mmu_xlate_fn fn, void *ctx, bool default_supervisor, uint64_t space_end, uint32_t page_bits,
                      int argc, const value_t *argv) {
    uint64_t start = (argc > 0 && argv[0].kind == VK_UINT) ? argv[0].u : 0;
    uint64_t end = (argc > 1 && argv[1].kind == VK_UINT) ? argv[1].u : space_end;
    bool sup, fetch;
    debug_mmu_parse_mode(argc, argv, 2, default_supervisor, &sup, &fetch);
    uint64_t limit = (argc > 4 && argv[4].kind == VK_UINT) ? argv[4].u : k_map_limit_default.u;
    if (end > space_end)
        end = space_end;
    if (start >= end)
        return val_err("map: start must be below end");
    if (limit == 0)
        return val_err("map: limit must be at least 1");

    value_t *items = NULL;
    size_t len = 0, cap = 0;
    map_run_t run;
    bool have_run = false;
    uint64_t addr = start;
    while (addr < end) {
        mmu_xlate_t x = {0};
        fn(ctx, (uint32_t)addr, sup, fetch, &x, NULL);

        // The region this answer covers: its declared uniform span, else one page.
        uint32_t bits = x.span_bits ? x.span_bits : page_bits;
        uint64_t block = (bits >= 32) ? (1ull << 32) : (1ull << bits);
        uint64_t seg_end = (addr & ~(block - 1)) + block;
        if (seg_end > end)
            seg_end = end;

        if (x.valid) {
            bool extends = have_run && run.end == addr && same_str(run.x.via, x.via) &&
                           same_str(run.x.access, x.access) && same_str(run.x.space, x.space) &&
                           (uint32_t)(run.phys + (uint32_t)(addr - run.start)) == x.phys;
            if (extends) {
                run.end = seg_end;
            } else {
                if (have_run) {
                    if (len >= limit)
                        break; // full: the next run would be one too many
                    val_list_push(&items, &len, &cap, run_value(&run));
                }
                run.start = addr;
                run.end = seg_end;
                run.phys = x.phys;
                run.x = x;
                have_run = true;
            }
        } else if (have_run) {
            if (len >= limit)
                break;
            val_list_push(&items, &len, &cap, run_value(&run));
            have_run = false;
        }
        addr = seg_end;
    }
    if (have_run && len < limit)
        val_list_push(&items, &len, &cap, run_value(&run));
    return val_list(items, len);
}

const value_t debug_mmu_desc_count_default = {.kind = VK_UINT, .u = 1};

uint32_t debug_mmu_desc_count(int argc, const value_t *argv) {
    uint64_t n = (argc > 1 && argv[1].kind == VK_UINT) ? argv[1].u : 1;
    if (n < 1)
        n = 1;
    return n > DEBUG_MMU_DESC_MAX ? DEBUG_MMU_DESC_MAX : (uint32_t)n;
}

const arg_decl_t debug_mmu_xlate_args[DEBUG_MMU_XLATE_NARGS] = {
    {.name = "addr", .kind = VK_UINT, .presentation_flags = VFLAG_HEX, .doc = "logical (effective) address"},
    {.name = "supervisor",
     .kind = VK_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "translate for supervisor (true) or user (false)",
     .default_doc = "the CPU's current state"},
    {.name = "fetch",
     .kind = VK_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "an instruction fetch (the 68040's ITT registers, the PowerPC's IBATs and MSR[IT]) rather than a data "
            "access"},
};

const arg_decl_t debug_mmu_map_args[DEBUG_MMU_MAP_NARGS] = {
    {.name = "start",
     .kind = VK_UINT,
     .presentation_flags = VFLAG_HEX,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "first logical address",
     .default_doc = "0"},
    {.name = "end",
     .kind = VK_UINT,
     .presentation_flags = VFLAG_HEX,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "end of the range (exclusive)",
     .default_doc = "the end of the address space"},
    {.name = "supervisor",
     .kind = VK_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "map the supervisor (true) or user (false) view",
     .default_doc = "the CPU's current state"},
    {.name = "fetch",
     .kind = VK_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "the instruction-fetch view rather than the data view"},
    {.name = "limit",
     .kind = VK_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_map_limit_default,
     .doc = "most runs to list; continue from the last run's end for more"},
};
