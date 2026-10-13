# The PowerPC translation tier

Hot PowerPC code runs as WebAssembly generated at run time, compiled by
the host engine and called from the predecoded sprint loop.  It is a third
tier above the predecoded executor ([../predecode.md](../predecode.md)),
in the wasm builds only, behind `jit.enabled` (default 0).  A translation
runs only where its result is the predecoded tier's; for anything else it
leaves, with the guest state committed, and the predecoded loop goes on.

Source: `src/core/cpu/ppc/ppc_jit.[ch]` (the byte writer, the translator,
the tier manager and the `jit` node), the `PPD_JIT_TRY` hook in
`ppc_pd_run.h`, `ppc_pd_classify_word` in `ppc_run.c`, the owner bits of
the code-page marks in `src/core/memory/memory.[ch]` and the tier hooks of
`src/core/cpu/predecode.[ch]`.

## Contents

1. Responsibilities & design
   - 1.1 What it is, and what it is not
   - 1.2 Design rules
2. Key types
   - 2.1 Regions, members and chunks
   - 2.2 The generated function
3. Behaviour & algorithms
   - 3.1 Entry and promotion
   - 3.2 Reachability and chunks
   - 3.3 Translation
   - 3.4 Budget and folding
   - 3.5 Memory accesses and exits
   - 3.6 Fetch mapping
   - 3.7 Coherence
4. Integration
5. Testing
6. Tunables and counters

## 1. Responsibilities & design

### 1.1 What it is, and what it is not

It is a cache of compiled code for pages the predecoded tier already
runs.  It decides nothing about guest behaviour: every page that has a
translation still has its predecoded block, a translation can be dropped
at any time, and which tier ran an instruction is invisible to the guest.
The guest timeline is identical with and without it (§5).

It is not a tier for every build: native builds have no runtime code
generation (`jit.available` is 0 there).  It is not a tier for every
instruction: it translates exactly the shapes the predecoded loop
specializes (T0), minus `isync` (its context synchronization stays in C)
and branches to themselves (they take a slot instead of folding).  It
does not run in little-endian mode or with supervisor data translation
(every access takes the slow path there), and it is never entered while
an interrupt is deliverable.

### 1.2 Design rules

- **Leave, never decide.**  An instruction outside the set, a memory
  access that misses the fast path, a block the budget cannot fit: the
  translation leaves at that instruction with everything before it
  committed, and the predecoded loop runs it.  The JIT raises no
  exception, computes no DSISR, walks no page table, calls no slow path.
- **The predecoded loop owns the sprint.**  A block runs only with a slot
  to spare after it, so `*instructions` reaches zero, and every sprint
  ends, in the predecoded loop (§3.4).
- **Nothing the guest can observe changes during a call.**  Interrupt
  inputs, MSR, the MMU and the code a translation runs can change only in
  places a translation leaves first.

## 2. Key types

### 2.1 Regions, members and chunks

A **region** (`jit_region_t`) is one megabyte of logical address space
under one fetch mode: the key is the base, MSR[IT] and MSR[PR].  Its
**members** (`jit_slot_t`, one per page) are the pages entered often
enough (`jit.threshold`), each recorded with the host page it was
translated from (`host_adjust`, the fetch window's) and a snapshot of its
words.  The run-time **entry points** seen on a member (`extra[]`) seed the
translation.

The region's reachable code is split into **chunks** (`jit_chunk_t`) of
at most `jit.chunk_cap` instructions, one generated function each.  The
optimizing compile of a function grows faster than linearly with its size
(emu-core-bench measured 11 s in V8's TurboFan for a 660 KB function), so
the cap keeps every function's optimizing compile short.  Per member page
and chunk, `maps[]` gives the chunk's block at each word that leads one;
per member word, `owner[]` and `cov[]` give the chunk and block that
translated it.

### 2.2 The generated function

```
(param p budget entry folds) (result budget)
  ;; prologue: the GPRs the chunk names, CR/XER/LR/CTR, the SoA tables,
  ;;           the address mask, the 601 T=1 mask (MSR[DT] clear only)
  block $out
    loop $loop
      block $b(n-1) ... block $b0
        local.get $idx
        br_table $b0 .. $b(n-1) $out
      end   ;; block 0's code: guard, instructions, branch or fall through
      ...
    end
  end
  ;; epilogue: those registers, pc, the fold allowance; return the budget
```

One module per function, importing the emulator's memory (shared in the
pthreads build); the export goes into the emulator's function table (a
freed slot, or a new one: `-sALLOW_TABLE_GROWTH`), and its index is the
function pointer the dispatcher calls.  After instantiation the
dispatcher calls it once with a zero budget on a scratch state, so the
engine compiles it now and not at its first real entry (V8 compiles wasm
functions lazily, at their first call).

## 3. Behaviour & algorithms

### 3.1 Entry and promotion

`PPD_JIT_TRY` runs where the predecoded loop looks a page up, when the
fetch window maps p->pc's page: at the start of `relookup`, and after the
window was refilled there.  `ppc_jit_enter` finds the region and the
member; when the member's fetch mapping is the window's and its chunk has
a valid block at the pc, it calls the chunk's function.  Otherwise it
counts the entry: a page becomes a member after `jit.threshold` entries;
an entry at a word that leads no block becomes an entry point; the region
is retranslated when entry points were added and `jit.cooldown` entries
passed since the last translation, or when invalidated blocks have been
quiet for `jit.quiet` entries.

An exit that made no progress (the entry block cannot fit the budget, or
was invalidated) is not retried before the predecoded loop has run an
instruction (`jit_skip_pc`).

### 3.2 Reachability and chunks

`compile_region` walks the direct control flow from the entry points over
the member pages: a block leader is an entry point, a branch target, the
instruction after a conditional branch or a call, the instruction after
an untranslated one (the predecoded loop runs it, then re-enters), or the
first word of the next page when code runs off a page.  Then it assigns
whole blocks, in address order, to chunks of at most the cap.  A leader
owned by another chunk still ends a block (the translator's leader value
2), so a branch into another chunk lands on a block start there: the
partition does not depend on the order in which entries were seen.

### 3.3 Translation

`translate()` classifies every member word with the predecoded tier's
classifier and emits each block's instructions from the entry fields:
registers as wasm locals, immediates and masks as constants, CR0 records
and compares with XER[SO], XER[CA] as a bit of the XER local, the 601's
`mtxer` mask.  A branch to a block of the chunk is `idx = b; br $loop`;
`blr` and `bctr` look the target up in the chunk's maps
(`jt_ref_t refs[]`, read by the code); anything else leaves with the pc.
Blocks lay out in address order inside one loop; a block falls into the
next when it starts right after it.

### 3.4 Budget and folding

The predecoded loop's rule: every instruction takes a budget slot, except
a branch, which folds (takes none, uses one of the sprint's fold
allowance) unless it is on the sprint's last slot or branches to itself.
A block of n instructions ending in a branch costs n - 1 slots and one
fold.  Its guard requires `budget > n` (and a fold left): at least two
slots remain when the branch retires, so it always folds, and a block
never takes the sprint's last slot.  `blr`/`bctr` to themselves trade the
fold back for a slot at run time.  So the slots a translated block
consumes are exactly the predecoded loop's, events land on the same
instructions, and the tail of every sprint (less than one block) runs
predecoded.

### 3.5 Memory accesses and exits

A load or store is inlined when it is naturally aligned, outside a 601
T=1 segment (MSR[DT] clear), and the active SoA table has an entry for its
page: the masked address, the entry, the big-endian access.  Otherwise
the function leaves **at** the access (`PPC_JIT_EXIT_DEOPT`): it gives back
the block's unspent slots and fold, stores the registers, and the
predecoded loop runs that one instruction on the generic step — the
slow path, a device, a DSI, an alignment exception, a store into a code
page — and looks the next pc up again.

### 3.6 Fetch mapping

A region is entered only through the fetch window, for the mapping the
member was translated under.  Inside a function, a crossing into another
member page (a branch, or running off a page) re-checks, in
instruction-translation mode, that the fetch TLB maps the target page for
this privilege to the same host page; otherwise it leaves to the target.
In real mode the mapping is the memory map's, and every translation is
dropped when the memory map changes (`g_mem_map_generation`).

### 3.7 Coherence

A member page carries the tier's own code-page mark (`MEM_CODE_JIT`), so
every store into it takes the slow path and reaches the predecode pool's
invalidation hook, which tells the tier first (`g_pd_tier_written_hook`).
The tier clears the valid flag of exactly the blocks that translated the
stored words: a store next to code (a data word, a link field) invalidates
nothing.  A stale block leaves at its start; the region is retranslated
once the writes have been quiet for `jit.quiet` entries.  A store from
inside a translation into a code page misses the fast path (the page has
no write entry), so it leaves first.  A pool reset resets the tier
(`g_pd_tier_reset_hook`), as do a memory map change and a model change.

## 4. Integration

| Where | What |
|---|---|
| `ppc_pd_run.h` | `PPD_JIT_TRY` at relookup; a deopt continues at `t2_step` |
| `ppc_run.c` | `ppc_pd_classify_word`: the classifier, for the translator |
| `predecode.[ch]` | `g_pd_tier_written_hook`, `g_pd_tier_reset_hook` |
| `memory.[ch]` | code-page marks per owner (`MEM_CODE_PREDECODE`, `MEM_CODE_JIT`) |
| `object/root.c` | `ppc_jit_object_install` (the `jit` node, the hooks) |
| `Makefile`, `Makefile.headless` | `-sALLOW_TABLE_GROWTH` in the wasm builds |

## 5. Testing

The predecode differential (`tests/integration/predecode-diff`) runs each
PowerPC row a third time with the tier on, eagerly (`jit.threshold = 1`,
`jit.cooldown = 16`), when the build has it (`jit.available`), and
compares the checkpoints with the switch core's.  Build the Node wasm
build (`make -f Makefile.headless TARGET=node`) and run the test with it;
the build is single-threaded, so it needs `--jobs=inline --io=sync`.

## 6. Tunables and counters

| `jit.` | |
|---|---|
| `enabled` | 1 = run translations (wasm builds) |
| `available` | 1 when the build can generate code |
| `threshold` | entries into a page before it joins its region (16) |
| `cooldown` | entries between two translations of a region (64) |
| `quiet` | entries after a code write before its region is retranslated (64) |
| `chunk_cap` | the most instructions one function translates (4096) |
| `regions`, `compiles`, `functions`, `bytes` | regions created, region translations, functions generated, bytes of WebAssembly |
| `calls`, `deopts`, `block_invals`, `compile_us` | entries into translated code, exits at a missed access, blocks invalidated by code writes, time spent translating and compiling |
| `reset()` | drop every translation |
