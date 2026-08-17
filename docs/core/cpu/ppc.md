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

## Files

| File | Contents |
|---|---|
| `ppc.h` | public surface: `ppc_t` (opaque), init/reset/run, sched-if and debug-if adapters, external-interrupt line |
| `ppc_internal.h` | the `ppc_t` state (POD, pointers last), MSR/XER/vector constants, shared inline helpers |
| `ppc.c` | lifecycle, checkpoint, exception machinery, SPR file, `machine.cpu` object class, `$` aliases |
| `ppc_run.c` | the interpreter: primary-opcode switch → extended switches (19, 31), sprint loop |
| `ppc_ops.h` | instruction-body helpers: carry/overflow, compares, branch conditions, alignment rules |
| `ppc_fpu.c` | FP surface: single↔double conversions, moves, compares, FPSCR access; arithmetic lands in Phase E |
| `ppc_disasm.c/.h` | dependency-free disassembler (`tools/disasm --arch ppc`) |

## Main-CPU status (vs the aux-core contract)

Per the cores.md main-vs-aux rule, this core uses the **global fast-path
memory system** (`memory_read_uint32` etc.), owns the supervisor/user SoA
switch on every MSR[PR] transition (`ppc_update_active_maps`), and
registers `machine.cpu` plus the `$pc $lr $ctr $cr $msr $xer $r0..$r31`
aliases.  The sprint ABI mirrors the 68K decoders: burn-down counter,
`g_bus_error_instr_ptr` fault-exit, deferred fault delivered in the
epilogue as a machine check (601UM §5.4.2, the TEA path).

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
  moves/compares/FPSCR ops.  **FP arithmetic raises a loud illegal until
  Phase E**; RTC/DEC time derivation and the MMU front end land with the
  PDM family (Phases C/D).

## Verification

- `tests/unit/suites/ppc/` — directed semantics tests from the chapter-10
  RTL (no public 601 test corpus exists; see proposal §7).  Executed words
  are cross-checked against the disassembler so encoder typos cannot agree
  with decoder typos.
- `tests/unit/suites/ppc_disasm/` — vectors cross-validated against
  `powerpc-linux-gnu-objdump -b binary -m powerpc:601 -EB` (the RE
  workflow's oracle) over a directed sweep, a 10k-word fuzz corpus, and
  128 KB of the shipping PDM ROM's HWInit/nanokernel region, modulo
  objdump's simplified-mnemonic aliases and its POWER fallback spellings
  for invalid forms.  objdump accepts some 603+ instructions (`fsel`,
  `tlbld`) under `-m powerpc:601`; this decoder correctly rejects them.
