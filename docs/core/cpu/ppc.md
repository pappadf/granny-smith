# PPC core — MPC601/MPC604 main CPU

`src/core/cpu/ppc/` implements the PowerPC 601 and 604 as a **main CPU** —
the first non-68K architecture to own emulated time
(proposal-powerpc-601-pdm.md).  The module is named `ppc`, not `ppc601`:
the decode tree and register file are architectural 32-bit PowerPC with
model-specific behavior behind `cpu_model` discrimination, exactly the way
`cpu.c` discriminates 68000/030/040.  Two models exist —
`CPU_MODEL_PPC601` (the PDM machines) and `CPU_MODEL_PPC604` (Phase A of
the TNT proposal; see "The 604 model" below).  `ppc_init` takes the model;
`ppc_reset` preserves it.

Sources of truth: Motorola/IBM, *PowerPC 601 RISC Microprocessor User's
Manual*, 1995 (MPC601UM/AD) for the 601; *PowerPC 604 RISC Microprocessor
User's Manual*, 1994 (MPC604UM/AD) and *PowerPC Microprocessor Family:
The Programming Environments* (MPCFPE32B, "PEM") for the 604 — cited per
chapter/table in the code.

## Files — the shared decoder/disassembler pattern

The module follows the house decode-template pattern (the 68K's
`cpu_decode.h` model, proposal-multi-cpu.md §3.3.1): one
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
  6 store, DAR = EA; ISI SRR1 = $40000000 for an HTAB miss (bit 1 only
  — the nanokernel's InstStorageInt masks $40200000, but with
  `andis.`/`beq`, so either bit satisfies it, while Copland's
  GetFaultInformation counts bit 10 among its hard-error bits
  $10200000 and would panic on a plain page fault), $08000000 for
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

## The 604 model (TNT proposal Phase A)

`CPU_MODEL_PPC604` is a bounded delta over the shared machinery — decoder
template, softfloat kernel, exception plumbing, sched-if/debug-if, SoA
discipline all carry over untouched.  It is dead code on every shipping
machine until the TNT family lands (the DSP3210/601 precedent).  The
deltas, each keyed on `cpu_model`:

- **Holdover rejection**: every POWER holdover, MQ (SPR 0), the RTC SPRs
  (4/5/20/21), the POWER DEC read (SPR 6) and HID1 take the illegal
  program exception.  The decode tree stays model-blind; the leaves
  decide (`M601()`/`M604()` guards in `ppc_ops.h`).
- **Time**: TBL/TBU replace the RTC — read via `mftb`/`mftbu` (xo 371,
  user-readable), written via SPR 284/285 (supervisor), stored in the
  same `rtcu/rtcl` slots at their rebase instant.  DEC decrements once
  per timebase tick.  `ppc_bind_time(p, s, freq_hz, tick_hz)` takes the
  tick rate explicitly: 7,833,600 on the 601 (PDM), bus/4 on the 604.
- **MSR**: adds POW (accepted as a no-op idle hint), BE, PM, RI.
  Exception entry keeps ME/EP/PM and clears the rest; `rfi` restores
  MSR[16-31] only (POW survives).  ILE/LE stay unimplemented on both
  models (big-endian Macs).
- **MMU**: the ARCHITECTED BAT format (BEPI/BL/Vs/Vp upper,
  BRPN/WIMG/PP lower; blocks 128 KB–256 MB; PP-only protection) with
  four SPLIT pairs each way — fetch consults the IBATs (`batu/batl`),
  data the DBATs (`dbatu/dbatl`, SPR 536-543).  BATs take precedence
  over the segment; T=1 is consulted only after a BAT miss, and MSR[DR]=0
  is true real addressing mode (no segment consult — `sr_t_mask` stays
  zero).  The HTAB search is byte-identical to the 601's.  `tlbie`
  invalidates by EA[14-19] (64 classes, 604UM §5.4.3.2); `tlbsync` is a
  supervisor no-op (invalidation is synchronous here).
- **Fault images**: ISI HTAB miss sets SRR1 bit 1 alone ($40000000);
  direct-store fetches set SRR1[3]; direct-store data accesses take a DSI
  with DSISR bit 0 (bit 5 for lwarx/stwcx./eciwx/ecowx) — there is no
  $00A00 vector.  The trace vector is $00D00 and $00F00 is the (never
  firing) performance monitor; neither model delivers trace today
  (MSR[SE]/[BE] are storage-only).  A TEA machine check sets SRR1[13],
  zeroes SRR1[30], and clears MSR[ME] (604UM Table 4-8).
- **Alignment** (604UM §4.5.6): only FP loads/stores, lmw/stmw,
  lwarx/stwcx. and eciwx/ecowx require word alignment; every other
  misaligned access is split by hardware — `ppc_scalar_gate` performs a
  translated page-crossing access byte-wise, since the two pages need not
  be physically adjacent.  `dcbz` additionally faults with the data cache
  disabled (HID0[17] clear — the HRESET state) or locked (HID0[19]).
  The string rules stay 601-shaped on both models (implementation-
  specific per the PEM; ladder-observable).
- **SPR file**: undefined SPR numbers trap (the 604 fully decodes the
  field — 604UM §4.5.7) where the 601 no-ops; MMCR0/PMC1/PMC2/SIA/SDA are
  supervisor read-zero/write-ignore stubs; PIR/DABR/IABR/HID0 are
  store-and-readback.  Reset state: MSR = $00000040 (HRESET sets only
  IP), PVR = $00040103 (the kernel keys on the $0004 half; the revision
  is a chosen constant), HID0 = 0.
- **Optional FP** (PEM pages): `fsel` (no FPSCR effects; −0 counts as
  ≥ 0, NaN selects frB), `stfiwx`, and the estimates `fres`/`frsqrte`.
  The estimates are implementation-defined within architected envelopes
  (2⁻⁸ / 2⁻⁵ relative); this model's documented constants are the
  correctly-rounded single quotient 1.0/frB (through the integer div
  kernel) and the round-to-nearest of a 62-bit-truncated integer-sqrt
  reciprocal (relative error < 2⁻⁶¹).  Both report only their architected
  FPSCR effects — never XX; FR/FI ("undefined") read cleared.  `fsqrt`
  remains illegal: the 604 does not implement it.
- **Superscalar timing is not modeled**: the 604 keeps the sprint/CPI-1.0
  model and 601-style branch folding (TNT proposal §4.2), for the same
  determinism-and-measurement reasons as the 601.
- **Object model**: `machine.cpu` adds `dbat0u..dbat3l` and `tbu`/`tbl`
  (aliases of the rtcu/rtcl storage); the members are static per class,
  so they exist — inert — on the 601 too.
- **Disassembler**: the union of both models decodes; `is_power` flags the
  601-only encodings, `is_604` the 604-only ones, and
  `ppc_disassemble_model(word, addr, 601|604, out)` applies one model's
  validity view (`.long` for the other model's exclusives — matching the
  runtime trap and `objdump -m powerpc:<model>`).  `tools/disasm` grows
  `--arch ppc604` (`ppc`/`ppc601` keep the 601 view).

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
  The tier replays TWICE: once as the 601 (every file), once as the 604
  with the model-divergent mnemonics filtered (POWER holdovers,
  mfspr/mtspr/mtmsr/rfi, the word-alignment classes, dcbz) — the shared
  ISA must replay bit-identically under both models.
- `tests/unit/suites/ppc/` — directed semantics tests from the chapter-10
  RTL, written before the corpus above existed and still the place where a
  manual-cited rule gets pinned (`test_conformance_regressions` restates
  every rule the vectors caught).  Executed words are cross-checked against
  the disassembler so encoder typos cannot agree with decoder typos.
  A `test_604_*` matrix covers the model deltas: reset state, the
  holdover-rejection table, MSR bits, timebase reads/writes/privilege,
  the split-BAT SPR file and PM stubs, the word-alignment classes, the
  TEA machine-check image, and the optional-FP group (with the 601-side
  rejections).
- `tests/unit/suites/ppc_mmu/` — the Phase-D proof list: 601 BAT
  protection keys, T=1 with DT off and the SR-toggle alias, primary and
  secondary HTAB search with R/C write-back (PTEG addresses computed
  independently from Figure 6-19), exact DSI/ISI images, the abandoned
  update-form rule, the (PR,DT) SoA discipline, tlbie congruence classes,
  mtsr change-triggered invalidation, dcbz W/I, and the $00A00 contract.
  The `test_604_*` section proves the split I/D BAT files (and the
  architected format's BL/Vs/Vp/PP fields), BAT-before-segment ordering,
  real-mode segment blindness, the direct-store DSI/ISI images, the
  hardware-split page-crossing access over discontiguous translations,
  per-class tlbie, and the 604 dcbz rules.
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
