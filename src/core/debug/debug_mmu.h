// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug_mmu.h
// The inspection methods every MMU kind's `machine.cpu.mmu` node answers the
// same way -- translate, walk, map and descriptor -- on the 68030 PMMU, the
// 68040, the PowerPC 601/604 and the Lisa's segment MMU.  Each MMU supplies
// one side-effect-free translation function (mmu_xlate_t + an optional
// mmu_trace_t, mmu_trace.h); this module turns its answers into the shared
// result shapes, so a debugger needs no per-architecture code to read them:
//
//   translate(addr, [supervisor], [fetch])
//       {phys, valid, via, access, space?}
//   walk(addr, [supervisor], [fetch])
//       translate's map plus `steps`: [{step, outcome, ...fields}, ...]
//   map([start], [end], [supervisor], [fetch], [limit])
//       [{start, size, phys, via, access, space?}, ...]  -- mapped runs
//   descriptor(addr, [count], [format])
//       [{addr, desc, type, ...fields}, ...]  -- raw descriptors decoded
//
// `phys` is absent when the translation is invalid.  `access` is what the
// queried privilege may do there ("rw", "ro", "none").  See mmu_trace.h for
// the step vocabulary.

#ifndef DEBUG_MMU_H
#define DEBUG_MMU_H

#include "mmu_trace.h"
#include "object.h"
#include "value.h"

#include <stdbool.h>
#include <stdint.h>

// One MMU's debugger translation: fill *out for `addr` as seen by the given
// privilege and access kind, and record the steps into `trace` when it is
// non-NULL.  Must not change machine state.
typedef void (*debug_mmu_xlate_fn)(void *ctx, uint32_t addr, bool supervisor, bool fetch, mmu_xlate_t *out,
                                   mmu_trace_t *trace);

// translate's result map for one answer.
value_t debug_mmu_xlate_result(const mmu_xlate_t *x);

// walk's result map: translate's, plus the trace's steps.
value_t debug_mmu_walk_result(const mmu_xlate_t *x, const mmu_trace_t *trace);

// Read the (supervisor, fetch) arguments at argv[idx], argv[idx+1]; an omitted
// `supervisor` takes `default_supervisor` (the CPU's current state).
void debug_mmu_parse_mode(int argc, const value_t *argv, int idx, bool default_supervisor, bool *supervisor,
                          bool *fetch);

// The translate/walk method bodies, given the MMU's translation function.
// argv is (addr, [supervisor], [fetch]).
value_t debug_mmu_translate(debug_mmu_xlate_fn fn, void *ctx, bool default_supervisor, int argc, const value_t *argv);
value_t debug_mmu_walk(debug_mmu_xlate_fn fn, void *ctx, bool default_supervisor, int argc, const value_t *argv);

// The map method body: sweep [start, end) -- `space_end` (exclusive, up to
// 2^32) when `end` is omitted -- at `page_bits` granularity (skipping any
// region an answer's span_bits declares uniform), and list the maximal runs
// that translate linearly with the same via/access/space.  argv is
// ([start], [end], [supervisor], [fetch], [limit]).
value_t debug_mmu_map(debug_mmu_xlate_fn fn, void *ctx, bool default_supervisor, uint64_t space_end, uint32_t page_bits,
                      int argc, const value_t *argv);

// The shared argument tables (translate and walk share one).  The
// DECLS macros are the same entries as initialisers, so an MMU that takes
// one more trailing argument (the Lisa's `context`) can build its own table
// without restating these.
#define DEBUG_MMU_XLATE_ARG_DECLS                                                                                      \
    {.name = "addr", .kind = VK_UINT, .presentation_flags = VFLAG_HEX, .doc = "logical (effective) address"},          \
        {.name = "supervisor",                                                                                         \
         .kind = VK_BOOL,                                                                                              \
         .validation_flags = OBJ_ARG_OPTIONAL,                                                                         \
         .doc = "translate for supervisor (true) or user (false)",                                                     \
         .default_doc = "the CPU's current state"},                                                                    \
    {                                                                                                                  \
        .name = "fetch", .kind = VK_BOOL, .validation_flags = OBJ_ARG_OPTIONAL,                                        \
        .doc = "an instruction fetch (the 68040's ITT registers, the PowerPC's IBATs and MSR[IT]) rather than a "      \
               "data access"                                                                                           \
    }
#define DEBUG_MMU_MAP_ARG_DECLS                                                                                        \
    {.name = "start",                                                                                                  \
     .kind = VK_UINT,                                                                                                  \
     .presentation_flags = VFLAG_HEX,                                                                                  \
     .validation_flags = OBJ_ARG_OPTIONAL,                                                                             \
     .doc = "first logical address",                                                                                   \
     .default_doc = "0"},                                                                                              \
        {.name = "end",                                                                                                \
         .kind = VK_UINT,                                                                                              \
         .presentation_flags = VFLAG_HEX,                                                                              \
         .validation_flags = OBJ_ARG_OPTIONAL,                                                                         \
         .doc = "end of the range (exclusive)",                                                                        \
         .default_doc = "the end of the address space"},                                                               \
        {.name = "supervisor",                                                                                         \
         .kind = VK_BOOL,                                                                                              \
         .validation_flags = OBJ_ARG_OPTIONAL,                                                                         \
         .doc = "map the supervisor (true) or user (false) view",                                                      \
         .default_doc = "the CPU's current state"},                                                                    \
        {.name = "fetch",                                                                                              \
         .kind = VK_BOOL,                                                                                              \
         .validation_flags = OBJ_ARG_OPTIONAL,                                                                         \
         .doc = "the instruction-fetch view rather than the data view"},                                               \
    {                                                                                                                  \
        .name = "limit", .kind = VK_UINT, .validation_flags = OBJ_ARG_OPTIONAL,                                        \
        .default_value = &debug_mmu_map_limit_default,                                                                 \
        .doc = "most runs to list; continue from the last run's end for more"                                          \
    }
#define DEBUG_MMU_XLATE_NARGS 3
extern const arg_decl_t debug_mmu_xlate_args[DEBUG_MMU_XLATE_NARGS];
#define DEBUG_MMU_MAP_NARGS 5
extern const arg_decl_t debug_mmu_map_args[DEBUG_MMU_MAP_NARGS];
// map's default `limit` (512 runs).
extern const value_t debug_mmu_map_limit_default;

// descriptor's (addr, [count]) leading arguments; each MMU appends its own
// `format` enum.  `count` defaults to 1 and is capped at DEBUG_MMU_DESC_MAX.
#define DEBUG_MMU_DESC_MAX 256
extern const value_t debug_mmu_desc_count_default;
uint32_t debug_mmu_desc_count(int argc, const value_t *argv);

// One peek read: `size` (1, 2 or 4) bytes at `addr`, big-endian, through the
// MMU unless `physical`.  Returns val_none() with *out set, or the error to
// hand back (an address with no translation, a refused space).
typedef value_t (*debug_mmu_read_fn)(void *ctx, uint32_t addr, unsigned size, bool physical, uint32_t *out);

// The peek method body: argv is (addr, [size], [space]); the space is checked
// before the size, every error starts "peek:", and the result is a plain
// VK_UINT of `size` bytes on every MMU kind.
value_t debug_mmu_peek(debug_mmu_read_fn fn, void *ctx, int argc, const value_t *argv);

// peek's (addr, size, space) table; `addr_doc` and `space_doc` are what
// differ between MMU kinds, so each kind keeps its own table built from this.
#define DEBUG_MMU_PEEK_NARGS 3
// The same entries without the outer braces, for a kind that appends an
// argument of its own (the Lisa's `context`).
#define DEBUG_MMU_PEEK_ARG_DECLS(addr_doc, space_doc)                                                                  \
    {.name = "addr", .kind = VK_UINT, .presentation_flags = VFLAG_HEX, .doc = (addr_doc)},                             \
        {.name = "size",                                                                                               \
         .kind = VK_UINT,                                                                                              \
         .validation_flags = OBJ_ARG_OPTIONAL,                                                                         \
         .default_value = &debug_mmu_peek_size_default,                                                                \
         .doc = "1, 2 or 4 bytes"},                                                                                    \
    {                                                                                                                  \
        .name = "space", .kind = VK_ENUM, .enum_values = debug_space_values, .validation_flags = OBJ_ARG_OPTIONAL,     \
        .doc = (space_doc), .default_doc = "logical"                                                                   \
    }
#define DEBUG_MMU_PEEK_ARGS(addr_doc, space_doc) {DEBUG_MMU_PEEK_ARG_DECLS(addr_doc, space_doc)}

// peek's default size (4).
extern const value_t debug_mmu_peek_size_default;

// Helpers for descriptor decoders: put a hex / bool field into a map.
void debug_mmu_put_hex(value_map_builder_t *b, const char *key, uint32_t v);
void debug_mmu_put_bool(value_map_builder_t *b, const char *key, bool v);

#endif // DEBUG_MMU_H
