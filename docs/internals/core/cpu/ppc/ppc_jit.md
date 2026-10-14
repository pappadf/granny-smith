# The PowerPC translation tier

Hot PowerPC code runs as WebAssembly generated at run time, compiled by
the host engine and called from the predecoded sprint loop.  It is a third
tier above the predecoded executor ([../predecode.md](../predecode.md)),
in the wasm builds only, behind `jit.enabled` (default 0).  A translation
runs only where its result is the predecoded tier's; for anything else it
leaves, with the guest state committed, and the predecoded loop goes on.

Source: `src/core/cpu/ppc/ppc_jit.[ch]` (the byte writer, the translator,
the tier manager and the `jit` node), the `PPD_JIT_TRY` hook in
`ppc_pd_run.h`, `ppc_pd_classify_word` in `ppc_run.c`, `g_ppc_user_wok` in
`ppc_mmu.c`, the owner bits of the code-page marks in
`src/core/memory/memory.[ch]` and the tier hooks of
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
   - 3.6 Fetch mapping and chaining
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
instruction: it translates the shapes the predecoded loop specializes
(T0) minus branches to themselves (they take a slot instead of folding;
`isync` is a no-op there unless BAT writes are pending, when it leaves), plus a few the
predecoded loop leaves generic (T1) that it decodes from the raw word
(§3.3).  It does not run in little-endian mode or with supervisor data
translation (every access takes the slow path there), and it is never
entered while an interrupt is deliverable.

### 1.2 Design rules

- **Leave, never decide.**  An instruction outside the set, a memory
  access that misses the fast path, a block the budget cannot fit: the
  translation leaves at that instruction with everything before it
  committed, and the predecoded loop runs it.  The JIT raises no
  exception, computes no DSISR and walks no page table.  The one call it
  makes into C, `ppc_jit_code_store`, completes a store only in the case
  where the switch core would complete it through a live write entry
  (§3.5).
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
**members** (`jit_slot_t`, one per page) are the pages the predecoded tier
has run `jit.threshold` instructions in, each recorded with the host page it was
translated from (`host_adjust`, the fetch window's) and a snapshot of its
words.  The run-time **entry points** seen on a member (`extra[]`, counted
up to `JIT_ENTRY_HOT`) seed the translation.

The region's reachable code is split into **chunks** (`jit_chunk_t`), one
generated function each.  A reached word keeps its chunk (`owner[]`) from
one translation to the next; code reached for the first time goes into
new chunks, whole blocks in address order, at most `jit.chunk_cap`
instructions each (when a region's 255 chunks run out, everything is
assigned afresh).  Each chunk records a signature of what it was
translated from (its words, leaders and mappings), and a translation
regenerates only the chunks whose signature changed: new code costs a new
small function, not the retranslation of what was there.  A chunk whose
words and ownership are unchanged and that only gained leaders (new code
branching into the middle of its blocks) keeps its function too, unless
enough translations of the region have passed since its last one (an
interval doubling with each of its translations): the function is still
right, the new entries are only not translated yet.  The
optimizing compile of a function grows faster than linearly with its size
and its live locals (emu-core-bench measured 11 s in V8's TurboFan for a
660 KB function; here a 4096-instruction cap ran slower than 1024 and took
six times longer to optimize), so chunks are small and branches between
them chain (§3.6).  Per member page and chunk, `maps[]` gives the chunk's
block at each word that leads one; per member word, `owner[]` and `cov[]`
give the chunk and block that translated it.

### 2.2 The generated function

```
(param p budget entry folds) (result budget)
  ;; prologue: CR/XER/LR/CTR, the SoA tables, the address mask, the 601
  ;;           T=1 mask (MSR[DT] clear only)
  block $out
    block $chain
      loop $loop
        block $b(n-1) ... block $b0
          local.get $idx
          br_table $b0 .. $b(n-1) $out
        end   ;; block 0: guard, its GPRs into locals, body, way on
        ...
      end
    end       ;; the chain stub: tail-call another chunk's function (§3.6)
  end
  ;; epilogue: CR/XER/LR/CTR, pc, the fold allowance; return the budget
```

A block loads the GPRs its instructions name into their locals after its
guard and stores the ones it may write on every way out (`jit.regs = 0`),
so live ranges stay inside a block and entering or leaving a function
costs only the registers of the block it passes through.  `jit.regs = 1`
keeps them in locals for the whole function instead (prologue, `$out` and
the chain stub).  A block with loads or stores wraps its body in `$deopt`:
an access that misses the fast path sets the instruction's index and
branches there, and one tail per block gives the slots back and leaves.

One module per function, importing the emulator's memory (shared in the
pthreads build), its function table and `ppc_jit_code_store`; the export
goes into the table (a freed slot, or a new one: `-sALLOW_TABLE_GROWTH`,
64 at a time), and its index is the function pointer the dispatcher
calls.  After instantiation the dispatcher calls it once with a zero
budget on a scratch state, so the engine compiles it now and not at its
first real entry (V8 compiles wasm functions lazily, at their first call).

**Asynchronous compiles** (`jit.async`, the default except under Node).
`compile_region` hands each module to `WebAssembly.compile` and records
the chunk as pending (a token).  The promise resolves on the core thread
between its ticks — the browser build's main loop returns to the event
loop — and `ppc_jit_ready` installs the function if the chunk still
waits for that token (a chunk retranslated or dropped meanwhile drops
it).  A pending chunk's code runs predecoded, and its entries are not
counted.  Under Node a script's `scheduler.run` never returns to the
event loop, so the headless builds and the differential compile
synchronously.

## 3. Behaviour & algorithms

### 3.1 Entry and promotion

`PPD_JIT_TRY` runs where the predecoded loop looks a page up, when the
fetch window maps p->pc's page: at the start of `relookup`, and after the
window was refilled there (after a page change, and after every generic
step: about one lookup per three instructions on firmware code).  Two
caches keep the declined lookups cheap, checked inline: the page the
dispatcher last declined (`g_ppc_jit_cold`: the hook adds the budget spent
since to its heat until the heat reaches the threshold) and a
direct-mapped set of declined member words (`g_ppc_jit_skip`: no block
there yet, an entry that runs too short, a volatile page), cleared by
every translation and every 4096 dispatcher calls.

`ppc_jit_enter` finds the region and the member; when the member's fetch
mapping is the window's and its chunk has a valid block at the pc, it
calls the chunk's function.  Otherwise it counts: a page becomes a member
once the predecoded tier has run `jit.threshold` instructions in it (the
budget spent between a declined lookup and the next one); a word that
leads no block becomes an entry point after `JIT_ENTRY_HOT` entries (one
entered rarely is not worth a translation).  The region is retranslated
when entry points were added, or invalidated blocks have been quiet for
`jit.quiet` entries, and the region's cooldown has passed: `jit.cooldown`
entries, doubling with every translation of the region up to
`JIT_COOL_MAX`.  An entry whose runs keep ending within `JIT_POOR_RUN`
slots (a short loop around an untranslated instruction) is not entered
again before the next translation.

**The governor.**  Translating and compiling may take at most
`jit.budget` percent of the wall time since translation began (plus a
start allowance); a translation due beyond that is deferred by another
cooldown.  Code that keeps changing — a boot — then costs at most that
share, and a working set that settles is translated soon after.  Which
translations exist is invisible to the guest, so a host-time decision is
safe.

An exit that made no progress (the entry block cannot fit the budget, or
was invalidated) is not retried before the predecoded loop has run an
instruction (`jit_skip_pc`).

### 3.2 Reachability and chunks

`compile_region` walks the direct control flow from the entry points over
the member pages: a block leader is an entry point, a branch target, or
the instruction after a conditional branch, as far as the predecoded tier
has run it: a word whose predecoded entry was never decoded never executed
(the pool decodes at dispatch; a peek at its cache, no lookup), and code
that never ran is not translated.  The continuation after a
call, after an untranslated instruction and off the end of a page is not
followed: the 68K emulator keeps operands after its calls, and data
translated as code turns every store to it into an invalidation.  Those
continuations become leaders once execution enters them (§3.1).  A word
already translated counts as run (its predecoded entry may never be
decoded again).  A word the guest keeps rewriting — stored into
`JIT_UNSTABLE_WRITES` times between two translations of its region; the
68K emulator toggles some of its own branches — is not translated from
then on: the walk continues after it, and the predecoded loop runs it.
Then it assigns words to chunks (§2.1); new words that continue a run of
a chunk join that chunk.  A word of another chunk, or one not reached,
ends a block (the translator's leader value 2): a block covers only words
its chunk owns, or a store into the others would not invalidate it.  Member words keep their classification
between translations; only changed words are classified again.

A member page whose blocks are invalidated again after `JIT_CHURN_MAX`
translations is code being written while it runs (the 68K dynamic
recompiler's cache, patched as it links blocks): it becomes volatile, is
not translated, and loses the tier's code-page mark.  It is tried again
after `JIT_VOLATILE_LOOKUPS` dispatcher calls, an interval doubling every
time it turns volatile again.

### 3.3 Translation

`translate()` classifies the chunk's words with the predecoded tier's
classifier, plus the translator's own shapes (`jit_classify`): every
`bc` form the T0 ids leave out (linking, decrementing with a condition),
conditional, decrementing and linking `bclr`/`bcctr`, the X-form loads and
stores, the XO-form carrying, extended and OE add/subtract family, the
D-form accesses with rA = 0, `mcrxr`, and `mfspr`/`mtspr` of the plain
supervisor SPRs (DSISR, DAR, SRR0/1, SPRG0-3: fields of `ppc_t` with no
side effect; in a user-mode region they raise a program interrupt, so
they stay in C there).  Mode changes (`rfi`, `sc`, traps), the
time-dependent SPRs (DEC, TB) and the reservation pair stay in C.  It emits each block's instructions from the
entry fields: registers as wasm locals, immediates and masks as
constants, CR0 records and compares with XER[SO], XER[CA] and XER[OV] as
bits of the XER local, the 601's `mtxer` mask.  A branch to a block of
the chunk is `idx = b; br $loop`; `blr` and `bctr` look the target up in
the chunk's maps (`jt_ref_t refs[]`); anything else goes to the chain stub.
Blocks lay out in address order inside one loop; a block falls into the
next when it starts right after it.

### 3.4 Budget and folding

The predecoded loop's rule: every instruction takes a budget slot, except
a branch, which folds (takes none, uses one of the sprint's fold
allowance) unless it is on the sprint's last slot or branches to itself.
A block of n instructions ending in a branch costs n - 1 slots and one
fold.  Its guard requires `budget > n` (and a fold left): at least two
slots remain when the branch retires, so it always folds, and a block
never takes the sprint's last slot.  An indirect branch that turns out to
branch to itself trades the fold back for a slot at run time.  So the
slots a translated block consumes are exactly the predecoded loop's,
events land on the same instructions, and the tail of every sprint (less
than one block) runs predecoded.

### 3.5 Memory accesses and exits

A load or store is inlined when the active SoA table has an entry for its
page, it is outside a 601 T=1 segment (MSR[DT] clear), and it does not
cross a page (a big-endian misaligned access within a page is an ordinary
access on both models: `ppc_check_align_scalar`, the 604's scalar gate).  The masked address, the entry, the big-endian
access.  Otherwise the function leaves **at** the access
(`PPC_JIT_EXIT_DEOPT`): it gives back the block's unspent slots and fold,
stores the registers, and the predecoded loop runs that one instruction
on the generic step — the slow path, a device, a DSI, an alignment
exception — and looks the next pc up again.

One miss is common enough to complete in place: a store into a code page,
whose write entry the code-page marks took away (the 68K emulator's own
variables sit next to its code).  `ppc_jit_code_store` completes it when
the active tables are the user tables, the page's read entry is set and
`g_ppc_user_wok` (the write entry the fill that planted it would have
planted, kept in step with the user fills by `ppc_mmu.c`) equals it, the
page is still a code page, and on the 604 and 750 only: the coherence
hooks, then the bytes.  When the page's user entries are gone it asks the
translation TLB (`ppc_mmu_user_store_host`), as `ppc_dxlate_slow` would
without a walk.  When the store invalidated translated code the function
leaves right after it (an update form's rA updated first).

### 3.6 Fetch mapping and chaining

A region is entered only through the fetch window, for the mapping the
member was translated under.  Inside a function, a crossing into another
member page (a branch, or running off a page) re-checks, in
instruction-translation mode, that the fetch TLB maps the target page for
this privilege to the same host page; otherwise it leaves to the target.
In real mode the mapping is the memory map's, and every translation is
dropped when the memory map changes (`g_mem_map_generation`).

A transfer to an address the chunk has no block for goes to the
function's chain stub, which looks the address up in the region's own
tables at run time: the member slot, the word's owner chunk, that chunk's
function and map, and (in instruction-translation mode) the fetch TLB
against the slot's mapping.  On a hit it stores CR/XER/LR/CTR and
tail-calls the other function (`return_call_indirect`) with the budget and
the fold allowance; otherwise it leaves.  The tables outlive the code that
reads them: a remapped member or a reset drops the region's functions
first.

### 3.7 Coherence

A member page carries the tier's own code-page mark (`MEM_CODE_JIT`), so
every store into it takes the slow path (or `ppc_jit_code_store`) and
reaches the predecode pool's invalidation hook, which tells the tier
first (`g_pd_tier_written_hook`).  The tier clears the valid flag of
exactly the blocks that translated the stored words: a store next to code
(a data word, a link field) invalidates nothing.  A stale block leaves at
its start; the region is retranslated once the writes have been quiet for
`jit.quiet` entries.  A pool reset resets the tier
(`g_pd_tier_reset_hook`), as do a memory map change and a model change.

## 4. Integration

| Where | What |
|---|---|
| `ppc_pd_run.h` | `PPD_JIT_TRY` at relookup; a deopt continues at `t2_step` |
| `ppc_run.c` | `ppc_pd_classify_word`: the classifier, for the translator |
| `ppc_mmu.c` | `g_ppc_user_wok`, beside the user SoA fills |
| `predecode.[ch]` | `g_pd_tier_written_hook`, `g_pd_tier_reset_hook` |
| `memory.[ch]` | code-page marks per owner (`MEM_CODE_PREDECODE`, `MEM_CODE_JIT`) |
| `object/root.c` | `ppc_jit_object_install` (the `jit` node, the hooks) |
| `Makefile`, `Makefile.headless` | `-sALLOW_TABLE_GROWTH` in the wasm builds |

The generated code needs wasm tail calls (Chrome 112, Firefox 121,
Safari 18.2, Node 20).

## 5. Testing

The predecode differential (`tests/integration/predecode-diff`) runs each
PowerPC row a third time with the tier on, eagerly (`jit.threshold = 1`,
`jit.cooldown = 16`, `jit.budget = 100`), when the build has it (`jit.available`), and
compares the checkpoints with the switch core's.  Build the Node wasm
build (`make -f Makefile.headless TARGET=node`) and run the test with it;
the build is single-threaded, so it needs `--jobs=inline --io=sync`.

Two runs compare only when they split the run into the same
`scheduler.run` calls: a branch folds or not by where the sprint ends.

## 6. Tunables and counters

| `jit.` | |
|---|---|
| `enabled` | 1 = run translations (wasm builds) |
| `available` | 1 when the build can generate code |
| `threshold` | instructions the predecoded tier runs in a page before it joins its region (100000) |
| `budget` | the share of wall time, in percent, translating may take (3; 100: no limit) |
| `cooldown` | entries between two translations of a region, doubling per translation (64) |
| `quiet` | entries after a code write before its region is retranslated (64) |
| `chunk_cap` | the most instructions one function translates (1024) |
| `regs` | 0 = guest GPRs in locals per block, 1 = per function |
| `trace` | debugging: 1 every entry and exit; 2 a histogram of exit pcs, 3 prints it with the untranslated exits summed by opcode; 4 writes every module to the working directory; 5 the bytes per instruction; 6 every region translation and what it regenerated; 7 where translation time goes; 8 every block invalidation; 9 every code-page write; 10 why `ppc_jit_code_store` declined; 11 collects per-word and per-writer statistics of the writes reaching translated pages (invalidating or not, value changed or not, CPU / translated code / DMA, and a timeline of invalidations), 12 prints them; 13 appends a snapshot of every translated page to `jitsnap.bin` |
| `regions`, `compiles`, `functions`, `bytes` | regions created, region translations, functions generated, bytes of WebAssembly |
| `calls`, `deopts`, `block_invals`, `compile_us`, `instructions` | entries into translated code, exits at a missed access, blocks invalidated by code writes, time spent translating and compiling, budget slots consumed inside translated code |
| `lookups`, `deferred` | calls into the dispatcher, translations the governor put off |
| `reset()` | drop every translation |
