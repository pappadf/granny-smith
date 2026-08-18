# PPC core — MPC601 main CPU

`src/core/cpu/ppc/` implements the PowerPC 601 as a **main CPU** — the first
non-68K architecture to own emulated time (proposal-powerpc-601-pdm.md).
The module is named `ppc`, not `ppc601`: the decode tree and register file
are architectural 32-bit PowerPC with 601-specific behavior (POWER
holdovers, MQ, RTC-instead-of-timebase, 601 BAT format, HID SPRs, 601-only
vectors) behind `cpu_model` discrimination.  Nothing beyond the 601 is
implemented.

Source of truth: Motorola/IBM, *PowerPC 601 RISC Microprocessor User's
Manual*, 1995 (MPC601UM/AD) — cited per chapter/table in the code.

## Files — the shared decoder/disassembler pattern

The module follows the house decode-template pattern (the 68K's
`cpu_decode.h` model, proposal-heterogeneous-multi-cpu.md §3.3.1): one
decode tree shared by the emulator and the disassembler, so the two cannot
drift out of sync and each cross-checks the other.

| File | Contents |
|---|---|
| `ppc.h` | public surface: `ppc_t` (opaque), init/reset/run, sched-if and debug-if adapters, external-interrupt line |
| `ppc_internal.h` | the `ppc_t` state (POD, pointers last), MSR/XER/vector constants, field macros, shared inline helpers |
| `ppc.c` | lifecycle, checkpoint, exception machinery, SPR file, `machine.cpu` object class, `$` aliases |
| `ppc_decode.h` | **the shared decode tree** — an include-guard-free template configured via `PPC_DECODER_*` macros with one `OP_`-prefixed leaf per instruction; carries the validity rules (reserved fields, BO forms, strict `sc`) so both includers agree by construction |
| `ppc_ops.h` | the emulator's overloads: factored bodies (carry/overflow, compares, branch conditions, alignment) + the one-liner `OP_` table |
| `ppc_run.c` | multi-statement instruction bodies (branches, divides, strings), the `ppc_execute` instantiation, sprint loop |
| `ppc_mmu.c` | the 601 MMU front end (Phase D): T=1 segments, 601 BATs, hashed page table search, the translation caches and their invalidation (see below) |
| `ppc_fpu.c` | FP bodies: single↔double conversions, compares, the FPSCR-instruction write rules, and the Phase-E arithmetic wrappers (writeback/CR1/precise-trap delivery) |
| `ppc_softfp.c/.h` | **the FPU arithmetic kernel** (Phase E): integer-only IEEE 754 with the full FPSCR status model — pure functions of (operands, FPSCR), dependency-free (the `ppc_disasm` precedent) |
| `ppc_disasm.c/.h` | the second instantiation of the same tree with sprintf-style `ASM(…)` overloads; dependency-free (`tools/disasm --arch ppc`) |

One consequence of the shared tree: invalid forms (reserved fields set,
invalid BO encodings) take the illegal-instruction program exception in the
interpreter, exactly where the disassembler prints `.long` — a legitimate
deterministic choice for what the manual calls boundedly-undefined forms.

## Main-CPU status (vs the aux-core contract)

Per the cores.md main-vs-aux rule, this core uses the **global fast-path
memory system** (`memory_read_uint32` etc.), owns the supervisor/user SoA
switch (`ppc_update_active_maps`), and registers `machine.cpu` plus the
`$pc $lr $ctr $cr $msr $xer $r0..$r31` aliases.  The sprint ABI mirrors
the 68K decoders: burn-down counter, `g_bus_error_instr_ptr` fault-exit,
deferred fault delivered in the epilogue as a machine check (601UM §5.4.2,
the TEA path).

### The (PR, DT)-keyed SoA discipline

Unlike the 68K cores (whose supervisor/user arrays are both filled by the
bus-side MMU), the 601 core splits the two array pairs by ROLE:

- the **supervisor arrays always hold the machine's eager physical
  identity view** (filled by the family's page layout, e.g. `pdm_fill_page`);
- the **user arrays belong to the MMU front end**, which fills them with
  LOGICAL page mappings as translated user-mode accesses succeed.

`ppc_update_active_maps` therefore selects the user arrays only when
**both** MSR[PR] and MSR[DT] are set; every other mode combination runs on
the identity view, with supervisor translated accesses (the nanokernel's
MemRetry paths) rewritten to physical addresses per access through a small
translation TLB in `ppc_mmu.c`.  This is what makes exception entry and
`rfi` free: the dominant mode transition (user-translated 68k world ↔
untranslated kernel handler) is a pointer swap, not an invalidation.
`g_user_soa_reserved` (memory.h) tells the generic identity-restore paths
in memory.c to keep their hands off the user arrays.

### MMU (Phase D)

`ppc_mmu.c` implements 601UM Chapter 6 with the 601's own quirks:

- **Order**: the segment's T bit decides first — a T=1 segment PREVAILS
  over any matching BAT (Table 6-10, opposite of the later architecture) —
  then the 4 unified 601-format BATs, then the hashed page table.
- **T=1 segments translate data accesses even with MSR[DT]=0** (§6.5.2) —
  how HWInit reaches RAM and I/O "untranslated", and why the SR5-toggle
  flash-probe aliasing works.  A per-CPU `sr_t_mask` keeps that check off
  the translation-off fast path.  BUID $07F is memory-forced
  (PA = SR[28-31] ‖ EA[4-31], protection bypassed); other BUIDs model the
  failed bus reply as the 601-only $00A00 exception (SRR0 = following
  instruction, DSISR unchanged), with atomics taking a DSI (DSISR bit 5)
  and FP load/stores the alignment exception per §6.10.
- **HTAB search** per §6.9/Figure 6-19: 19-bit hash, primary + secondary
  PTEG, 8 PTEs each.  R is set even on protection violations (§6.8.4);
  C only on permitted stores — and both are written back to the in-RAM
  PTE, which the PDM nanokernel depends on (it harvests C bits from
  evicted PTEs to maintain the 68k page-descriptor Modified flags).
- **Exact fault images**: DSI DSISR bit 1 not-found / 4 protection /
  6 store, DAR = EA; ISI SRR1 = $40200000 for an HTAB miss (bits 1 AND
  10 — the mask the nanokernel's InstStorageInt tests), $08000000 for
  protection, and NO bits for a T=1 fetch (Table 6-3 footnote).
  Translation runs before any register writeback, so a faulting update
  form leaves rA untouched.
- **Invalidation**: `mtsr`/`mtsrin`/BAT/SDR1 writes invalidate ONLY on a
  value change — the nanokernel reloads identical values wholesale on
  every space touch (SetSpace), and change-triggered invalidation makes
  that free.  `tlbie` invalidates by congruence class (EA[13-19] mod 128,
  Figure 6-15) — exactly the granularity the kernel's FlushTLB loop
  assumes.  `dcbz` takes the alignment exception on W=1/I=1 mappings and
  is a no-op in non-memory-forced T=1 segments.
- **Caches**: the user-SoA fills (tracked, so invalidation is
  proportional to fills), a 256-entry direct-mapped translation TLB for
  the non-SoA paths, and a fetch window + 64-entry fetch TLB holding
  instruction-page host pointers.  All of it lives in file statics — NOT
  in the checkpointed `ppc_t` blob (host pointers in the stream would
  break checkpoint byte-determinism) — and rebuilds lazily after restore.
- **Debug surface**: `machine.cpu.mmu.translate(ea)` and
  `machine.cpu.mmu.peek(ea, size)` are side-effect-free reads through the
  current translation; the debug-if `translate` hook feeds `debug.mac`, so
  the 68k world's logical memory reads normally on PDM.  Limitation:
  logical-address memory logpoints on translated pages degrade (the slow
  path sees the physical address) — use physical logpoints on PDM.

## Implemented (Phase B)

- Full integer ISA including the POWER-architecture holdovers the 601
  retains (`abs clcs div divs doz dozi lscbx maskg maskir mul nabs rlmi
  rrib sle sleq sliq slliq sllq slq sraiq sraq sre srea sreq sriq srliq
  srlq srq`) and the MQ register.
- Exception model: vectors, SRR0/SRR1 per the chapter-5 register-settings
  tables, MSR mutation (EE/PR/FP/FE0/SE/FE1/IT/DT cleared; ME/EP kept),
  `rfi`, `sc`, program (illegal/privileged/trap), FP-unavailable,
  level-sensitive external interrupt, alignment per §5.4.6 (page-crossing
  rules under MSR[DT], 256 MB rules otherwise, string/multiple and lwarx
  cases) with the Table 5-13 DSISR encoding.
- SPR file with the 601 asymmetries: RTC read SPR 4/5 vs write 20/21,
  the POWER user-level DEC read (SPR 6), `mftb` illegal, invalid SPRs as
  no-ops, privilege gating.
- FPR file with load/store conversion in deterministic code (NaN payloads
  never pass through host FP arithmetic — WASM byte-determinism), FP
  moves/compares/FPSCR ops.

## FPU arithmetic (Phase E) — `ppc_softfp.c`

The datapath is an **integer-only IEEE 754 kernel** ("software floating
point"): significand arithmetic, rounding decisions, and every FPSCR
status bit are computed in integer code, so results and status images are
byte-identical on native and WASM hosts *by construction* rather than by
auditing host-FP corner cases.  (This deliberately goes one step past the
proposal §3.6 wording — "host doubles for the arithmetic" — because the
FR/FI/OX/UX flags and the directed rounding modes need exact knowledge of
the infinitely precise result anyway; once that machinery exists, host
doubles are redundant as the implementation and become the test *oracle*
instead: `tests/unit/suites/ppc_fpu` compares ~290k randomized cases
against host IEEE arithmetic, and its corpus digest is diffed between the
native and emcc/node builds by `make wasm-check`.)

Shape: `sf_unpack` → exact significand arithmetic (`sf_add`/`sf_mul`/
`sf_div`/`sf_madd` — the madd family carries the architecture's full
106-bit fused intermediate, 601UM §2.5.1.1) → `sf_round_pack`, which owns
denormalization, tininess-before-rounding, the per-mode disabled-overflow
results, and the ±1536/±192 trap-enabled exponent wraps (§5.4.7.4/5).
Alignment uses the Berkeley jamming discipline (≥10 guard bits under the
round position make the jam bit provably harmless).  The instruction
surface applies the §5.4.7 action lists: NaN selection frA→frB→frC with
high-bit quieting, VE/ZE suppression (frD and FPRF untouched, FR/FI
cleared), FX only on 0→1 transitions, FEX/VX always derived and never
writable, and the FEX & MSR[FE0|FE1] program exception delivered
precisely (SRR0 = the causing instruction, SRR1[11]; the 601 ORs FE0/FE1).
`fctiw[z]` stores the 601's `$FFF80000` high word and leaves FPRF
untouched (architecturally undefined — deterministic choice).  Corner
cases whose full RTL lived in the manual's absent Appendix F are marked
AUTHORITY-PENDING in the kernel and the suite: frsp/single-op NaN payload
truncation, fctiw's rounded-result VXCVI boundary, and the FR
magnitude-increment reading (§11 acquisition item).

`machine.cpu.fpu` exposes `fpscr` and `fpr0..fpr31` (raw 64-bit
patterns); its registration is what flips the machine-capabilities `fpu`
bit for the PDM profiles.

## Implemented (Phase C — with the PDM family)

- **RTC/DEC time derivation** (`ppc_bind_time`): RTCU/RTCL/DEC derive from
  `scheduler_cpu_cycles` at exactly 7.8336 MHz-equivalent via the reduced
  rational 7,833,600/freq — the dossier's hard constraint (HWInit measures
  the CPU clock against DEC and snaps within ±1/1024; the derivation must
  be exact over any interval, not on average).  RTCL advances 128 units
  per tick and rolls into RTCU at 10⁹; DEC decrements 128/tick, with the
  sign-transition latched by a scheduler event (`ppc.dec`) and taken when
  MSR[EE] allows.  Unbound (unit tests) the SPRs are static state.
- **Translation**: superseded by the Phase-D MMU front end — see "MMU
  (Phase D)" above.  The 601-format BAT layout (BLPI/PBN/BSM, V+BSM in
  the LOWER register, WIM/Ks/Ku/PP in the upper — NOT the later
  architecture's layout) and the T=1 memory-forced segments landed here.
- **601 branch folding**: `b`/`bc`/`bclr`/`bcctr` retire in zero sprint
  slots (they issue to the branch unit in parallel on real silicon) —
  required for HWInit's timed 8-addi+`bdnz` loop to measure CPI 1.0
  exactly.  Two exclusions: a branch to itself burns a slot (a `b .` spin
  cannot stall the sprint), and the last budget slot never folds, so
  single-step and PC breakpoints observe branch targets.  Consequence:
  timed guest measurements are only exact in free-running execution — an
  active debugger single-steps and suppresses folding.

## Verification

- `tests/unit/suites/ppc_vectors/` — the [powerpc-test](https://github.com/pappadf/powerpc-test)
  smoke tier (`third-party/powerpc-test`) replayed through its own
  reference runner with this core as the runner's `custom` backend: 206
  encodings, 1153 vectors, four randomized replays each.  Inputs are
  SPARSE, so every unlisted register is randomized on every replay and the
  read set and write set are checked for free — an instruction that
  consults a register it should not, or writes one it should not, fails
  almost surely.  Vectors assert conformance to the powerpc-sail formal
  model, not to silicon; the eight defects the first replay found here,
  and the six it found upstream, are adjudicated against the 601UM in
  gs-docs `notes/2026-08-18-powerpc-test-vector-disagreements.md`.
- `tests/unit/suites/ppc/` — directed semantics tests from the chapter-10
  RTL, written before the corpus above existed and still the place where a
  manual-cited rule gets pinned (`test_conformance_regressions` restates
  every rule the vectors caught).  Executed words are cross-checked against
  the disassembler so encoder typos cannot agree with decoder typos.
- `tests/unit/suites/ppc_mmu/` — the Phase-D proof list: 601 BAT
  protection keys, T=1 with DT off and the SR-toggle alias, primary and
  secondary HTAB search with R/C write-back (PTEG addresses computed
  independently from Figure 6-19), exact DSI/ISI images, the abandoned
  update-form rule, the (PR,DT) SoA discipline, tlbie congruence classes,
  mtsr change-triggered invalidation, dcbz W/I, and the $00A00 contract.
- `tests/integration/pdm-rom-ladder/` — the shipping ROM as test program;
  high-water rung **L20** (the gray desktop through the Ariel scanout, with
  the AWACS chime, the exact VIA tick rate and the 68k emulator dispatching
  real 68k ROM code all asserted below it).
- `tests/unit/suites/ppc_disasm/` — vectors cross-validated against
  `powerpc-linux-gnu-objdump -b binary -m powerpc:601 -EB` (the RE
  workflow's oracle) over a directed sweep, a 10k-word fuzz corpus, and
  128 KB of the shipping PDM ROM's HWInit/nanokernel region, modulo
  objdump's simplified-mnemonic aliases and its POWER fallback spellings
  for invalid forms.  objdump accepts some 603+ instructions (`fsel`,
  `tlbld`) under `-m powerpc:601`; this decoder correctly rejects them.
