# The PowerPC translation tier

Hot PowerPC code runs as WebAssembly generated at run time, compiled by
the host engine and called from the predecoded sprint loop.  It is a third
tier above the predecoded executor ([../predecode.md](../predecode.md)),
in the wasm builds only, behind `jit.enabled` (on by default there).  A translation
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
   - 3.8 Where the time goes, and what was tried
4. Integration
5. Testing
6. Tunables and counters

## 1. Responsibilities & design

### 1.1 What it is, and what it is not

It is a cache of compiled code for pages the predecoded tier already
runs, and it is entered only from the predecoded loop: with
`predecode.enabled = 0` nothing is translated, whatever `jit.enabled`
says.  Both switch at run time, mid-run; turning `jit.enabled` off drops
every translation (`ppc_jit_reset`).  It decides nothing about guest behaviour: every page that has a
translation still has its predecoded block, a translation can be dropped
at any time, and which tier ran an instruction is invisible to the guest.
The guest timeline is identical with and without it (§5).

It is not a tier for every build: native builds have no runtime code
generation (`jit.available` is 0 there, and setting `jit.enabled = 1`
fails with "no code generation in this build").  They do not pay for its
upkeep either: `PPD_JIT_TRY` and its no-progress latch, the
`g_ppc_user_wok` upkeep beside the user fills and the call to the tier's
write hook are compiled in under `__EMSCRIPTEN__` only (without that guard
the native bench gate measured +2.34 % host instructions on g3dt-boot
predecoded; with it +0.09 %).  The tunables can be set in every build, so
scripts run anywhere.  It is not a tier for every
instruction: it translates the shapes the predecoded loop specializes
(T0) minus branches to themselves (they take a slot instead of folding;
`isync` is a no-op there unless BAT writes are pending, when it leaves), plus a few the
predecoded loop leaves generic (T1) that it decodes from the raw word
(§3.3).  It does not run in little-endian mode, or with supervisor data
translation unless `jit.supervisor` is set (the default; and then not before the
supervisor-translated SoA exists: every access would take the slow
path), and it is never entered while an interrupt is deliverable.

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
instructions each (when a region's 255 chunks, `JIT_MAX_CHUNKS`, run out, everything is
assigned afresh).  Each chunk records a signature of what it was
translated from (its words, leaders and mappings), and a translation
regenerates only the chunks whose signature changed: new code costs a new
small function, not the retranslation of what was there.  A chunk whose
words and ownership are unchanged and that only gained leaders (new code
branching into the middle of its blocks) keeps its function too, unless
enough translations of the region have passed since its last one (an
interval doubling with each of its translations): the function is still
right, the new entries are only not translated yet (the interval starts
at `JIT_SPLIT_COMPILES`, 16 translations, and doubles up to 2^10 times
that).  A chunk whose signature did not change keeps its function and has
all its blocks revalidated, so a guest rewrite that stores the same words
(the DR emulator rewriting identical `bl` words) invalidates blocks but
costs no compile; a chunk that translated to nothing is remembered and not
retried until it changes.  The
optimizing compile of a function grows faster than linearly with its size
and its live locals (emu-core-bench measured 11 s in V8's TurboFan for a
660 KB function; here a 4096-instruction cap ran slower than 1024 and took
six times longer to optimize), so chunks are small and branches between
them chain (§3.6).

Why a region and not a page: on the real rows a page visit runs only 3.4
(pm6100) to 15.4 (G3) instructions, and 83-100 % of page-to-page
transitions stay inside one megabyte.  In the prototype (an emu68k-shaped
kernel), one function per page, chained pages and lazy-register variants
ran 144-234 MIPS against 291 predecoded; a chunked region ran 344-356.
Dropping a page's whole translation on any store into it lost 16× on the
DR-cache shape (13 MIPS); invalidating only the blocks that translated the
stored words (§3.7) kept parity.  Cutting the reachability walk at the
chunk cap never converged (a storm of about 65 K compiles), hence the
order: reachability over the whole region first, then whole blocks into
chunks.  Per member page and chunk, `maps[]` gives the chunk's
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
pthreads build), its function table, `ppc_jit_code_store` and `ppc_jit_tb`; the export
goes into the table (a freed slot, or a new one: `-sALLOW_TABLE_GROWTH`,
64 at a time; without the reuse the table grew on every recompile and
smc-burst ran at 101 MIPS instead of 569), and its index is the function
pointer the dispatcher calls.  After instantiation the dispatcher calls it once with a zero
budget on a scratch state, so the engine compiles it now and not at its
first real entry (V8 compiles wasm functions lazily, at their first call).

**Code size.**  That compile is V8's baseline tier at a fixed rate per
byte, about 20 MB/s whatever the function's size (measured on the Mac OS
X scenario, `docs/guide/BENCHMARKS.md`), and it is most of what a
translation costs: generating the module is a tenth of it.  So the
translator keeps bytes down where that costs no run time.  Each module
defines three store helpers after its function (8, 16 and 32 bits, on the
604 and 750): the fast path of a store and, in a user-mode region, its
slow path through `ppc_jit_code_store`, which inline was most of a
store's code.  A helper returns that function's status; the call site
leaves through its block's deopt tail at the store (not stored) or at the
next instruction (stored into translated code).  Loads stay inline: the
same treatment cost more at run time than their smaller code saved, as
did keeping registers in locals for the whole function (`jit.regs = 1`,
21 % fewer bytes, slower code).  On the Mac OS X scenario (cold / warm
round, ms): every access inline 5297 / 3528, every access through helpers
5582 / 3705, stores through helpers and loads inline (kept) 5017 / 3552,
loads through helpers and stores inline 5440 / 3668, `jit.regs = 1`
5272 / 3886.  Before the store helpers a translation averaged 93 bytes per
instruction (126 per access; blocks of 3.9 instructions, so per-block code
was a third of the bytes).  The access test has no T=1 term on the
604 and 750 (their mask is always zero).  `jit.trace = 5` prints the
bytes per guest instruction by what they are for: on the Mac OS X
scenario about 77, of which instruction bodies 43, block guards 12,
deopt tails 9, register loads 7, ways out 4.5, prologue and epilogue 2.

**Asynchronous compiles** (`jit.async`, the default except under Node).
`compile_region` hands each module to `WebAssembly.compile` and records
the chunk as pending (a token).  The promise resolves on the core thread
between its ticks — the browser build's main loop returns to the event
loop — and `ppc_jit_ready` installs the function if the chunk still
waits for that token (a chunk retranslated or dropped meanwhile drops
it).  At most `JIT_PENDING` (512) compiles are in flight; a slot
reused before its compile lands drops that compile.  A pending chunk's
code runs predecoded, and its entries are not counted.  Under Node a
script's `scheduler.run` never returns to the event loop, so the headless
builds and the differential compile synchronously (unless `jit.async` is
set explicitly).

What `jit.async` does *not* take off the core thread: the translation,
the instantiation, and the engine's real compile.  `WebAssembly.compile`
only validates (about 132 MB/s); V8 compiles each function lazily at its
first call, and that is `install_fn`'s zero-budget call in `ppc_jit_ready`,
on the core thread.  On the Mac OS X scenario the engine's compile is
about 85 % of the measured compile time (583 of 682 ms) and generating the
modules 59 ms.  In Chromium the scenario's 148 modules (7.6 MB) cost 246 ms
with the default flags, 962 ms with `--no-wasm-lazy-compilation` and
5762 ms compiled eagerly without dynamic tiering.  A worker that compiles
each module and makes its first call there, then hands the compiled
module over, would move that cost off the emulator thread; it is not
built.  So the only bound on what translation costs the emulator thread
is the governor's share of wall time (§3.1); there is no per-frame cap.

**Engine tiers.**  A fresh translation runs as V8's baseline (Liftoff)
code, at about the predecoded tier's speed; it gains once TurboFan has
optimized it (about 1.25-1.7× the predecoded tier), and a translation
replaced before that never gains.  This is also why the benchmarks time
the jit executor by wall clock, unpinned: the optimizing compiles run on
the engine's background threads (§5).

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
leads no block becomes an entry point after `JIT_ENTRY_HOT` (8) entries
(one entered rarely is not worth a translation).  The region is retranslated
when entry points were added, or invalidated blocks have been quiet for
`jit.quiet` entries, and the region's cooldown has passed: `jit.cooldown`
entries, doubling with every translation of the region up to
`JIT_COOL_MAX` (65536; `jit.cooldown = 0` means none).  An entry whose
runs keep ending within `JIT_POOR_RUN` (8) slots, `JIT_POOR_MAX` (32) more
often than not (a short loop around an untranslated instruction), is not
entered again before the next translation.

The tier is asked only at a relookup: a branch inside the page (the
predecoded `_IN` forms) never reaches `PPD_JIT_TRY`.  A loop entered in
the middle (after an interrupt, or a system call's return) therefore
stays predecoded until it leaves its page.

**The governor.**  Translating and compiling may take at most
`jit.budget` percent of the wall time since translation began (plus a
start allowance, `JIT_BUDGET_START_MS`, 50 ms); a translation due beyond
that is deferred by another cooldown.  Code that keeps changing — a boot — then costs at most that
share, and a working set that settles is translated soon after.  Which
translations exist is invisible to the guest, so a host-time decision is
safe; but what gets translated then depends on the host's speed, and the
translated share of identical guest work varies by a few points from run
to run (87.5-91 % on the Mac OS X warm round), which is benchmark noise.
A budget counted in guest instructions would be deterministic; it is not
built.  More budget does not pay inside an interaction: on the Mac OS X
scenario (5-round medians, cold / warm ms) 3 % gave 5258 / 3437, 10 %
5245 / 3676, 30 % 5474 / 3631, 100 % 5696 / 3613.  The extra code is
lukewarm, and without a limit whole regions recompile as pages join them
(an early run: 414 compiles for 35 regions).

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
`JIT_UNSTABLE_WRITES` (4) times between two translations of its region;
the 68K emulator toggles some of its own branches — is not translated from
then on: the walk continues after it, and the predecoded loop runs it.
Then it assigns words to chunks (§2.1); new words that continue a run of
a chunk join that chunk.  A word of another chunk, or one not reached,
ends a block (the translator's leader value 2): a block covers only words
its chunk owns, or a store into the others would not invalidate it.  Member words keep their classification
between translations; only changed words are classified again.

A member page whose blocks are invalidated after `JIT_CHURN_MAX` (1)
translation is code being written while it runs: it becomes volatile, is
not translated, and loses the tier's code-page mark.  It is tried again
after `JIT_VOLATILE_LOOKUPS` (2 M) dispatcher calls, an interval doubling
every time it turns volatile again, up to 2^12 times that.

The case that sets the limit at one is the PCI Power Macs' 68K emulator,
the DR (dynamic recompilation) emulator: its translation cache
(`0x69019000` up to `0x6903D060`, 144 KB) is rewritten in bursts -- new
translations at a bump pointer, `bl dispatcher` patched into `b target`
as it chains blocks, evicted blocks unlinked -- and flushed whole when it
fills.  On the System 7.6 desktop with Energy Saver in front that is
every 5 seconds (Energy Saver's `WaitNextEvent` sleep of 300 ticks runs
the Process Manager's `'scod'` code, whose translations overflow the
cache).  Retranslating the cache after each flush cost more than its
translations saved before the next one (Node and browser, measured
against a limit of 3 and against a policy that retranslated sooner); the
predecoded tier runs the cache and the tier the emulator's runtime and
the ROM (PM8500 desktop, 600 M instructions: 53 -> 60 MIPS in Node,
`main` 46; in the browser +0.4 % -> +6 % over the predecoded tier).
The PM6100's emulator, which does not translate, is unaffected.

A time-aware policy was tried and dropped: churn counted only for
invalidations within 4 M instructions of a translation, write and
unstable counts reset after a quiet spell, the cooldown doubled only for
back-to-back translations.  The translated share rose to 90-98 % (from
78-84 %), but the PM8500 desktop ran slower (Node 48-53 MIPS against
54-56; browser 43.7 against 46.3 predecoded): the DR cache is replaced
every 5 seconds, so its translations die before they pay back, although
while they live they run about 1.8× the predecoded tier.  (A 10 % budget
did not help, and `--no-liftoff` showed V8's tier-up was not the cause.)
DR blocks also end in `bl dispatcher`: about 2.3 M exits per 100 M
instructions.

### 3.3 Translation

`translate()` classifies the chunk's words with the predecoded tier's
classifier, plus the translator's own shapes (`jit_classify`): every
`bc` form the T0 ids leave out (linking, decrementing with a condition),
conditional, decrementing and linking `bclr`/`bcctr`, the X-form loads and
stores, the XO-form carrying, extended and OE add/subtract family, the
D-form accesses with rA = 0, `lmw`/`stmw` (one test for the whole transfer: word-aligned, inside one page, the page's SoA entry present -- anything else leaves at the instruction for the C forms), `mcrxr`, and `mfspr`/`mtspr` of the plain
supervisor SPRs (DSISR, DAR, SRR0/1, SPRG0-3: fields of `ppc_t` with no
side effect; in a user-mode region they raise a program interrupt, so
they stay in C there).  For a kernel that runs translated (Mac OS X):
`mfmsr`; `mtmsr` when only bits nothing translated depends on change
(EE, RI, FP, ME, FE0/FE1, PM) and no BAT write is pending -- any other
change, or POW, leaves for C, and setting EE with an interrupt pending
leaves after it so the predecoded loop takes the interrupt (the first
build tested that bit with a 0/1 flag, so the exit never fired and the
interrupt came late: the Mac OS X checkpoint compare caught it, and
switching instruction groups off one at a time isolated it); `mftb`
(TBL/TBU; not on the 601, which has no time base) through an imported
helper (`ppc_jit_tb`) that brings the scheduler's burndown up to the
instruction and reads `ppc_tb_now`; `lwarx`/`stwcx.` on the word-aligned fast path (the
reservation flag and `reserve_addr`, the EA, as C keeps them; unaligned
or missing accesses leave); the non-OE `divw`/`divwu` (zero for a zero
divisor and for `0x80000000 / -1`, as C), `mulhw`/`mulhwu`, `eqv` and
`sraw` (CA as `ppc_do_sraw`); `lwbrx`/`stwbrx`.  On the 604 and 750
(not the 601: its T=1 segments and boundary rules stay in C), floating
point: `lfs`/`lfsu`/`lfd`/`lfdu`/`stfs`/`stfsu`/`stfd` and their X forms
inline on the fast path with MSR[FP] set, MSR[LE] clear and the EA
word-aligned (what the 604 and 750 require; a doubleword need not be
8-aligned, and one crossing a page leaves) -- `lfs` widens exactly
(bitwise for inf/NaN, `ppc_f32_to_f64`), `stfs` is the bit selection and
leaves when it would denormalize, `stfd` is two words through the store
helper; `fadd`/`fsub`/`fmul` and the madd family, single and double, inline:
`ppc_softfp.c`'s host fast path step for step (round to nearest, the
operands, product and result inside its exponent envelope, a product of
two short significands, which is exact so no fma is needed -- wasm has
none) with FPSCR[XE] clear, so only FR, FI, XX (FX on its rise) and FPRF
change; any check that fails, and every other arithmetic or move with
Rc clear, and `fcmpu`/`fcmpo`, call an imported helper (`ppc_jit_fp`)
that runs the interpreter's own routines, so results and FPSCR are the
predecoded loop's either way; it declines (leave at the instruction)
when FP is unavailable or an FP exception is enabled, and a compare
returns its CR field for the translation's CR local.
Mode changes (`rfi`, `sc`, traps), DEC (its reads depend on time), the
segment registers, the FP record forms and FPSCR moves stay in C, as
does `dcbz`.  The kernel runs translated only
with `jit.supervisor` set, the default.  When it was introduced the
kernel was a net loss translated: on the Mac OS X 10.1 boot to the
Finder (Node, 6.1 G instructions) 89 % of instructions ran translated,
but the boot took 92 s against 86 s user-only and 82 s with the JIT off,
the kernel leaving translated code every 60-85 instructions in its
busiest stretches.  Since then (chains across regions, §3.6; the store
helpers; the 300 K threshold) the Mac OS X 10.0.3 boot to the settled
Finder (Node, 6.1 G instructions) takes 66-67 s with it, 77-79 s
user-only and 86-92 s with the JIT off.  What still leaves translated code most, per Mac OS X warm round (JIT
with supervisor code): `lfd` 264 K, `mfspr` (DEC, HID0) 241 K, `stfd`
120 K, `lfs` 104 K, `dcbz` 93 K, `rfi` 90 K, `mtsr` 59 K.  Translating
`lfd`/`lfdu`/`stfd`/`stfdu` alone was tried for Mac OS X and reverted:
with a doubleword-alignment guard one hot access deopted 190 K times a
round (Darwin aligns doubles in structures to 4); with the word alignment
the 604 and 750 actually require, there was no measurable gain (share
87.5-91 % against 89.2-89.6 %, 2950-3351 ms against 3013-3256).  Quake
III on the G3 is the other extreme: half its instructions ran in the
predecoded loop, leaving translated code at `lfs` (618 K exits in 10 s),
`lfd` (489 K), `stwbrx` (291 K), `stfd` and `lfsx`, and behind them
`fcmpo` (1.29 M).  With the floating point above translated, in a Q3DM0
browser checkpoint (WebGPU takeover, draws not rasterised so the
software GPU is not the limit) the share went from 49 % to 97 % and the
throughput from 38 to 77 MIPS, twice the frames.  Inlining the fast path
then took the C kernel from 16 % of the emulator thread to 6 % and the
helper call from 8 % to 3 %: 81 to 92 MIPS (alternated pairs, 79-83
against 87-98), 2.4 times the first throughput.  The checkpoint compare
(Node, from an in-game checkpoint, 3 G instructions, eager translation)
finds the guest state identical to the predecoded loop's.  What leaves
translated code most now is `dcbz` (167 K exits a second).  The way
to find what to translate next is `jit.trace = 3`, the untranslated exits
summed by opcode: `mcrxr` led on the PM6100 desktop (331 K exits per
150 M instructions), and translating it took the row from 68 to 79 MIPS.

It emits each block's instructions from the
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
exception — and looks the next pc up again.  Calling the slow path in
place instead, spilling every register at every access, matched deopt on
io-mixed but was 2-2.6× slower on dispatch-stress, from the extra code; a
later version would spill only the registers the block has written.

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
leaves right after it (an update form's rA updated first).  On the PM8500
desktop it cut the store deopts from 5.7 M to 56 K.

### 3.6 Fetch mapping and chaining

A region is entered only through the fetch window, for the mapping the
member was translated under.  Inside a function, a crossing into another
member page (a branch, or running off a page) re-checks, in
instruction-translation mode, that the fetch TLB maps the target page for
this privilege to the same host page; otherwise it leaves to the target.
In real mode the mapping is the memory map's, and every translation is
dropped when the memory map changes (`g_mem_map_generation`).

A transfer to an address the chunk has no block for goes to the
function's chain stub, which looks the address up at run time: the
target's region in `g_rdirect` (every region by mode and logical
megabyte, so a chain reaches the translations of any region of the same
MSR[IT]/MSR[PR] mode, not only its own), the member slot, the word's
owner chunk, that chunk's function and map, and (in
instruction-translation mode) the fetch TLB against the slot's mapping.
On a hit it stores CR/XER/LR/CTR and tail-calls the other function
(`return_call_indirect`) with the budget and the fold allowance;
otherwise it leaves.  The tables outlive the code that reads them: a
remapped member or a reset drops the region's functions first, and
regions themselves live until `ppc_jit_reset`, which clears `g_rdirect`.
On the Mac OS X scenario's warm round, chaining across regions cut the
exits to other regions from 995 K to 275 K (all exits from 3.05 M to
2.44 M; the rest are targets on pages that have not joined their region)
without a wall-time change measurable through the scenario's run-to-run
noise (about 7 %; two A/Bs of 5 and 7 runs disagreed in sign): an exit and
the entry after it cost less than the counts suggested.  Before it, the
3.05 M exits split into another region 995 K, the same chunk 928 K, no
block 713 K, another chunk 590 K and deopts 452 K.

### 3.7 Coherence

A member page carries the tier's own code-page mark (`MEM_CODE_JIT`), so
every store into it takes the slow path (or `ppc_jit_code_store`) and
reaches the predecode pool's invalidation hook, which tells the tier
first (`g_pd_tier_written_hook`).  The hook finds the page through
`memory_code_region_of`, because the RAM image is not 4 KB-aligned on the
host (an earlier `host & ~0xFFF` left DR-cache translations stale and made
the PM8500 diverge).  It runs before the store lands, so it cannot tell a
same-value store apart, and it counts `writes[]` for every word written.
The tier clears the valid flag of
exactly the blocks that translated the stored words: a store next to code
(a data word, a link field) invalidates nothing.  A stale block leaves at
its start; the region is retranslated once the writes have been quiet for
`jit.quiet` entries.  A pool reset resets the tier
(`g_pd_tier_reset_hook`), as do a memory map change and a model change.

### 3.8 Where the time goes, and what was tried

On the Mac OS X scenario's warm round (Node `--cpu-prof` with a
`--profiling-funcs` relink, JIT with supervisor code): `ppc_pd_run` 39 %,
translated code 26 %, `ppc_jit_enter` 6 %, and `ppc_set_sr`,
`htab_search`, `window_locate`, `region_entry`, `ppc_fetch_fill` and
`install_fn` 1-2 % each.  87-90 % of the instructions run translated, yet
the 10-13 % left to the predecoded tier cost more than all the translated
code (about 55 against 280 MIPS).  Of those 80 M interpreted instructions,
74 % are user code at `0x7xxxxxxx` and 7 % kernel vector code; they are
mostly `lwz`, `addi`, `or`, `stw` and branches (floating point is about
5 %).  Eleven pages run more than 1 M each interpreted, up to 13.8 K per
visit.  Why they are not translated: 66 % are on member pages at pcs that
lead no block, 22 % a tail of 1,145 pages under the threshold, 10 % poor
entries.  There are about 9 M `ppc_jit_enter` calls per round, more than
half of them declined.

Tried against that remainder, each measured on the warm round and
dropped:
- call return addresses as block leaders: the share unchanged (about
  90 %), the time within noise (the §3.2 rule stands);
- an untranslated instruction's exit handed back as one generic step, so
  the next instruction becomes an entry point: the share unchanged;
- looser regeneration throttles (the cooldown ceiling, the chunk-split
  gate): the share unchanged, the cold round worse;
- a **back-edge retry**, the predecoded loop asking the tier at a taken
  in-page backward branch: asked at every back edge it was about 5 %
  faster on Mac OS X but cost a relookup per iteration of every declined
  loop (pm8500-finder ×0.92, pm6100-68kemu ×0.85, g3dt-boot ×0.90); asked
  once per page visit the rows were neutral and the share rose a point,
  with no time gain;
- `jit.threshold` 100 K / 300 K / 1 M (cold / warm ms 5492 / 3885,
  5157 / 3722, 5340 / 3692): 300 K stays.

**Long sessions.**  A short window overstates the gain where the guest
keeps writing code.  On the PM8500 desktop, 600 M instructions after a
4.2 G boot, the tier gave +20 % over the predecoded tier when it was
introduced; per 100 M window it fell from 83 to 43 MIPS and the translated
share from 98 % to 53 %, with thousands of block invalidations per window.
The churn limit (§3.2) is what that measurement set.

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
| `src/platform/wasm/em_main.c` | the `perf` event's `engine` (`interpreter` / `predecoded` / `jit`), `jit_pct`, `jit_sup`, `jit_regions`, `jit_kb` and `ppc`, from `ppc_jit_status()` (`docs/guide/web.md`); the web UI's status bar shows it and switches the engine |

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
With `jit.supervisor` on by default, the differential's tier runs
translate supervisor code too.

**Mac OS X.**  The kernel tier has no differential row; each change to it
was checked by booting Mac OS X to a checkpoint at 3 G instructions with
the tier on (supervisor code included) and off, in the Node build with
`--jobs=inline --io=sync`, and comparing them: they must be identical but
for the disk image's file name.  Compare 2 GB checkpoints with a streaming
block-at-a-time compare (`cmp-checkpoints.py` holds both in memory); a
checkpoint is tied to its build, so every rebuilt binary needs a fresh
one, and native default I/O timing differs from Node's.  Build with
`Makefile.headless`; the wrong makefile silently tests an old binary.

**Benchmarks** (`docs/guide/BENCHMARKS.md`).  The bench rows time the jit
executor by wall clock (`--clock wall`: user CPU time includes V8's
background compiles) and unpinned: pinning metric B to one core starved
the engine's compile threads (g3dt-boot 41 MIPS pinned, 72 unpinned).
Run-to-run noise is 5-10 % (a 3-run A/B once showed a false 15 % gain),
so take medians of 5 or more, run configurations one after another (four
concurrent Node runs: 73 MIPS against 85 alone), and compare only within
one batch, since baselines drift between sessions.  `scenario.py` fails a
run whose guest instruction count differs.  An arm meant as "predecoded
only" must set `jit.enabled = 0`.  Row caveats: `pm6100-68kemu` is a
diskless SCSI-scan loop, the tier's best case (about 257 instructions per
call); a diskless pm8500 spends half its time in real-mode nanokernel
code.  The interactive scenario (`tests/bench/osx-interactive.script`:
menus, minimizing a window to the Dock and restoring it), not the boot,
decided `jit.supervisor`: the boot runs mostly once-only code.  Its
baseline before the kernel tier's later work (3-run medians, cold / warm
ms): native predecoded 3985 / 3473, Node predecoded 6362 / 5761, Node JIT
5695 / 4059, with supervisor code 5927 / 3886.  The browser rows are
`tests/e2e/web2-specs/perf-jit.spec.ts` and `perf-osx-interactive.spec.ts`.

## 6. Tunables and counters

The defaults (`enabled` and `supervisor` on) against the predecoded
executor, Node, metric B with `--clock wall`, unpinned: `pm8500-finder`
61 -> 94 MIPS, `pm6100-68kemu` 67 -> 93, `g3dt-boot` 72 -> 72, and the
Mac OS X 10.0.3 boot 86-92 s -> 66-67 s.  The one loss is the 7500's
Open Firmware (`pm7500-of`, 58 -> 50 MIPS): a few seconds at power-on.

| `jit.` | |
|---|---|
| `enabled` | 1 = run translations (wasm builds; the default there) |
| `available` | 1 when the build can generate code |
| `threshold` | instructions the predecoded tier runs in a page before it joins its region (300000: against 100000, 13 % faster on the PM6100 row, the PM8500 row unchanged within its noise, and better on the Mac OS X scenario) |
| `budget` | the share of wall time, in percent, translating may take (3; 100: no limit) |
| `cooldown` | entries between two translations of a region, doubling per translation (64) |
| `quiet` | entries after a code write before its region is retranslated (64) |
| `supervisor` | 1 = also translate supervisor code running with data translation on, once the supervisor-translated SoA exists (the default: the Mac OS X boot is about 15 % faster with it than user-only, §3.3) |
| `chunk_cap` | the most instructions one function translates (1024; 64..65536) |
| `async` | 1 = compile through `WebAssembly.compile` off the call path (§2.2; the default except under Node) |
| `regs` | 0 = guest GPRs in locals per block, 1 = per function; setting it drops every translation |
| `trace` | debugging: 1 every entry and exit; 2 a histogram of exit pcs, 3 prints it with the untranslated exits summed by opcode; 4 writes every module to the working directory; 5 the bytes per instruction, and per instruction by what they are for; 6 every region translation and what it regenerated; 7 where translation time goes; 8 every block invalidation; 9 every code-page write; 10 why `ppc_jit_code_store` declined; 11 collects per-word and per-writer statistics of the writes reaching translated pages (invalidating or not, value changed or not, CPU / translated code / DMA, and a timeline of invalidations), 12 prints them; 13 appends a snapshot of every translated page to `jitsnap.bin` |
| `regions`, `compiles`, `functions`, `bytes` | regions created, region translations, functions generated, bytes of WebAssembly |
| `calls`, `deopts`, `block_invals`, `compile_us`, `instructions` | entries into translated code, exits at a missed access, blocks invalidated by code writes, time spent translating and compiling, budget slots consumed inside translated code |
| `lookups`, `deferred` | calls into the dispatcher, translations the governor put off |
| `reset()` | drop every translation |

`ppc_jit_status()` reports on, supervisor, instructions run translated,
regions and bytes for the `perf` event (all zero in native builds).
