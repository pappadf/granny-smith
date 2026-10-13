// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc_jit.h
// The PowerPC translation tier: hot guest code becomes WebAssembly at run
// time, compiled by the host engine and called from the predecoded sprint
// loop (docs/internals/core/cpu/ppc/ppc_jit.md).  The unit is a region: the
// pages of one megabyte of logical address space that were entered,
// translated together (in chunks of at most jit.chunk_cap instructions, one
// function each), so that branches between them stay inside.  A translation
// runs only where its result is the predecoded tier's: anything it does not
// handle -- an instruction outside the specialized set, a memory access
// that misses the fast path, the sprint's last block -- leaves it, with the
// state committed, for the predecoded loop.
//
// Only the wasm build generates code.  The translator itself is portable
// (it produces bytes); native builds report jit.available = 0.

#ifndef PPC_JIT_H
#define PPC_JIT_H

#include <stdbool.h>
#include <stdint.h>

struct ppc;

// Why a translation returned (g_ppc_jit_exit):
//   PPC_JIT_EXIT_PC     continue at p->pc (a branch out, an instruction it
//                       does not translate, a block the budget cannot fit);
//   PPC_JIT_EXIT_DEOPT  the instruction at p->pc missed the memory fast path:
//                       the predecoded loop runs it, everything before it is
//                       committed.
enum { PPC_JIT_EXIT_PC = 0, PPC_JIT_EXIT_DEOPT = 1 };

// Set at run time by the shell (`jit.enabled`); never in native builds.
extern bool g_ppc_jit_on;

// Try to run translated code at p->pc, whose page the fetch window maps.
// Returns false when nothing ran (no translation for this entry; it may
// have been counted towards one).  Otherwise the translation ran: the
// budget and folds_left are updated and *exit says how it ended.
bool ppc_jit_enter(struct ppc *p, uint32_t *instructions, uint64_t *folds_left, int *exit);

// The predecode pool's invalidation hook calls this first: a store or a
// host-side writer changed [host, host + len).
void ppc_jit_code_written(const uint8_t *host, uint32_t len);

// Drop every translation (pool reset, memory map change, model change,
// checkpoint restore).
void ppc_jit_reset(void);

// The `jit` node (object root).
void ppc_jit_object_install(void);

#endif // PPC_JIT_H
