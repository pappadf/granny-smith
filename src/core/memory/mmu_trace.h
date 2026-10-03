// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// mmu_trace.h
// The debugger's view of one translation, shared by every MMU kind (the
// 68030 PMMU, the 68040, the PowerPC 601/604 and the Lisa's segment MMU).
//
// Each MMU's own translation code fills an mmu_xlate_t (the answer) and,
// when handed one, an mmu_trace_t (how it got there: one step per register,
// table level or page-table group consulted, in the order the hardware
// consults them).  The translation code records the trace itself, so the
// trace cannot drift from the translation it describes.  The object model
// turns both into the `translate`, `walk` and `map` results (debug.c).
//
// Header-only: the recording helpers are static inline so the memory and
// CPU code that fills a trace needs no extra translation unit, and a NULL
// trace costs the hot paths one predictable branch.

#ifndef MMU_TRACE_H
#define MMU_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One translation's answer.  `phys` is meaningful only when `valid`.
typedef struct mmu_xlate {
    uint32_t phys; // physical address
    bool valid; // a translation exists
    const char *via; // "identity", "tt", "page", "bat", "segment"
    const char *space; // the Lisa's physical space ("ram", "io", "rom", "mmureg"); NULL elsewhere
    const char *access; // what the queried privilege may do there: "rw", "ro" or "none"
    uint32_t span_bits; // log2 of the aligned region around the address that translates
                        // the same way (linear phys, same via/access, or likewise
                        // invalid); 0 = unknown, treat as one page.  Lets `map` skip
                        // whole blocks, segments and invalid table branches.
} mmu_xlate_t;

// Kinds of a step field's value.
typedef enum {
    MMU_TF_HEX, // unsigned, printed in hex (addresses, descriptor words)
    MMU_TF_UINT, // unsigned, printed in decimal (indexes, levels)
    MMU_TF_BOOL, // flag bit
    MMU_TF_STR, // static string (names, types, reasons)
} mmu_trace_kind_t;

// One named field of a step.  Keys and string values are static literals.
typedef struct mmu_trace_field {
    const char *key;
    mmu_trace_kind_t kind;
    uint32_t u; // HEX / UINT / BOOL
    const char *s; // STR
} mmu_trace_field_t;

#define MMU_TRACE_MAX_FIELDS 16
#define MMU_TRACE_MAX_STEPS  12

// One step of a translation: what was consulted (`step`), what it decided
// (`outcome`), and the fields read or derived on the way.
//
//   step     "tt" (68K transparent translation), "root" (68K root pointer),
//            "level" (68K table level), "bat" (PowerPC block translation),
//            "segment" (PowerPC segment register; the Lisa's segment
//            descriptor), "pteg" (PowerPC page-table entry group)
//   outcome  "miss"  this mechanism did not apply; the search went on
//            "next"  a table descriptor; the search followed it
//            "hit"   the translation was resolved here
//            "fault" the search stopped here without a translation
typedef struct mmu_trace_step {
    const char *step;
    const char *outcome;
    int n_fields;
    mmu_trace_field_t fields[MMU_TRACE_MAX_FIELDS];
} mmu_trace_step_t;

// The whole record.  Steps past MMU_TRACE_MAX_STEPS are dropped (no MMU
// here takes more than seven).
typedef struct mmu_trace {
    int n_steps;
    mmu_trace_step_t steps[MMU_TRACE_MAX_STEPS];
} mmu_trace_t;

// Start a step; NULL (so every helper below no-ops) without a trace or room.
static inline mmu_trace_step_t *mmu_trace_step(mmu_trace_t *t, const char *step) {
    if (!t || t->n_steps >= MMU_TRACE_MAX_STEPS)
        return NULL;
    mmu_trace_step_t *s = &t->steps[t->n_steps++];
    s->step = step;
    s->outcome = "miss";
    s->n_fields = 0;
    return s;
}

// Set the step's outcome.
static inline void mmu_trace_outcome(mmu_trace_step_t *s, const char *outcome) {
    if (s)
        s->outcome = outcome;
}

// Append one field (internal).
static inline void mmu_trace_put(mmu_trace_step_t *s, const char *key, mmu_trace_kind_t kind, uint32_t u,
                                 const char *str) {
    if (!s || s->n_fields >= MMU_TRACE_MAX_FIELDS)
        return;
    mmu_trace_field_t *f = &s->fields[s->n_fields++];
    f->key = key;
    f->kind = kind;
    f->u = u;
    f->s = str;
}

// Field helpers, one per kind.
static inline void mmu_trace_hex(mmu_trace_step_t *s, const char *key, uint32_t v) {
    mmu_trace_put(s, key, MMU_TF_HEX, v, NULL);
}
static inline void mmu_trace_uint(mmu_trace_step_t *s, const char *key, uint32_t v) {
    mmu_trace_put(s, key, MMU_TF_UINT, v, NULL);
}
static inline void mmu_trace_bool(mmu_trace_step_t *s, const char *key, bool v) {
    mmu_trace_put(s, key, MMU_TF_BOOL, v ? 1u : 0u, NULL);
}
static inline void mmu_trace_str(mmu_trace_step_t *s, const char *key, const char *v) {
    mmu_trace_put(s, key, MMU_TF_STR, 0, v);
}

// Fill an identity answer covering the whole address space (MMU off).
static inline void mmu_xlate_identity(mmu_xlate_t *x, uint32_t addr) {
    x->phys = addr;
    x->valid = true;
    x->via = "identity";
    x->space = NULL;
    x->access = "rw";
    x->span_bits = 32;
}

// The access string for a translation's protection, from the queried
// privilege's point of view.
static inline const char *mmu_access(bool readable, bool writable) {
    return !readable ? "none" : writable ? "rw" : "ro";
}

#endif // MMU_TRACE_H
