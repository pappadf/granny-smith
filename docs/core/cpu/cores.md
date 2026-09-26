# CPU cores — the module contract

How Granny Smith executes more than one CPU per machine: exactly one
**main CPU** that owns emulated time (the 68K today), plus any number of
**auxiliary cores** — peripheral processors that execute real guest code
but do not control time.  The first auxiliary core is the AV family's
DSP3210 (`src/core/cpu/dsp3210/`, wired by `src/machines/av/dsp.c`).

## Time model

The main CPU keeps exclusive ownership of emulated time:
`scheduler.cpu_cycles` advances only through main-CPU sprint accounting.
Auxiliary cores are clocked consumers of that timeline — they run in
**bursts** driven by an ordinary scheduler event:

```
ratio_x256 = round(aux_freq * 256 / (aux_cpi * main_freq))
budget     = (elapsed_main_cycles * ratio_x256 + carry) >> 8   // carry kept exact
aux_run(core, &budget)            // burn-down ABI, same shape as cpu_run_sprint
if (!aux_is_idle(core)) re-arm at +quantum; else park (zero cost)
```

Bursts live on the one event queue, so the guest timeline stays a pure
function of the frame-unit count — byte-determinism by construction
(scheduler.md §12).  Burst atomicity (the aux core runs its whole budget
while the main CPU is between sprints) is safe for every protocol in
scope: they are polled mailboxes and frame-cadence buffers with ≥10 ms
deadlines.  If a protocol ever needs finer interleaving, shrink that
core's quantum.

An idle core — held in reset, or parked in its `waiti`-equivalent with
nothing pending — has no scheduled event and costs zero.  Wake-ups (host
register writes, device interrupts) run through the glue, which re-arms
the burst at the next cycle and calls `cpu_reschedule()`.

## The main-CPU seam

The scheduler holds a four-entry `sched_cpu_if_t`
(`ctx`/`run_sprint`/`is_stopped`/`poll_interrupt`) instead of a
`struct cpu *`; `cpu_sched_if()` in cpu.c is the 68K adapter.  One
indirect call per sprint.  This is the seam a future main-CPU
architecture (PowerPC) plugs into without touching the scheduler.

## The core-module contract

A CPU core is a standard module (opaque struct, `_init`/`_reset`,
checkpoint as POD, object class) with these core-specific requirements:

| Requirement | Contract |
|---|---|
| Interpreter | big-switch decode, plain C, no JIT.  The shared decoder/disassembler template-macro pattern (the 68K's `cpu_decode.h` / `cpu_ops.h` model) is the house style — one guard-free decode tree included by both the emulator (execution `OP_` overloads) and the disassembler (sprintf `OP_` overloads), so the two cannot drift.  Follow it unless the ISA gives a concrete reason not to; the PPC core (`ppc_decode.h`) is the second instantiation of the pattern |
| Execution ABI | `void <arch>_run(<arch>_t *, uint32_t *instructions)` — burn-down counter; returns with it 0 (budget spent) or >0 (went idle) |
| Idle/reset | `<arch>_is_idle()`, `<arch>_reset(...)`, an interrupt-request entry point for external pins.  When guest code polls a pin's *level* (not just its latched request), the entry point must model both — e.g. `dsp3210_ext_pulse(s, vector, slots)` latches the request and asserts the live pin for `slots` of core time, and the status-register pin bits reflect the level, not the latch |
| **Bus access** | **injected at init** (the guest-physical hook pattern of `sonic.h`/`psc.h`).  The core never touches `g_active_*`, `g_page_table`, the MMU, or any sprint-timing global.  On-chip resources (internal RAM, MMIO) decode *inside* the core before the hooks are consulted |
| State | one POD struct, pointers last; checkpoint boundary before the first pointer; hook pointers re-planted on restore |
| **Disassembler** | mandatory, dependency-free, raw words + pc in / text out — linkable standalone (`tools/disasm --arch <name>`) |
| Object class | `machine.<name>` node with register attrs (hex), `instr_count`, `state` (`reset`/`running`/`idle`/`crashed`), methods `step(n)`, `disasm(addr, count)` (prints) and `frame(addr, count, before)` — the `debug_frame_build` map every CPU-like object answers, from a `cpu_debug_if_t` the glue fills in (`arch`, `get_pc`, `disasm`, `regs`, optionally `fpu`), so the web Debug view renders it with no per-core code.  **No `$` aliases** — those stay reserved for the main CPU |
| Logging | own `LOG_USE_CATEGORY_NAME("<arch>")` (in the glue; the core itself stays I/O-free) |
| Tests | unit suite under `tests/unit/suites/<arch>/` against a mock bus |

### The interpreter loop has exactly one exit

**Rule.** A sprint loop is left by one condition and one only — the burn-down
counter reaching zero. Anything that wants the loop to stop says so by
**setting `*instructions = 0`**, and then does its work either in the
instruction that triggered it or in the epilogue, which runs once per sprint
after the loop closes. No `break`, no second exit test, and above all **no
per-instruction inspection of state that only a rare path ever sets**.

**Why it is a contract and not a preference.** The loop body is the only code in
the emulator that runs tens of millions of times a second, and a conditional
there is not a rounding error. Measured on this tree, callgrind over a 25 M
instruction Plus boot: removing the two deferred-bus-error tests from the 68K
loops took **116.61 → 113.09 host instructions per emulated instruction, about
−3%**. A separate experiment adding a single perfectly-predicted `if` to the
68000 prologue cost **+3.05 host instructions per emulated instruction, +2.24%**
— and two thirds of that was second-order: the extra register pressure spilled
the opcode jump-table base. You cannot estimate this by reading the diff.

**The mechanism already exists.** `g_bus_error_instr_ptr` points at the live
burn-down counter; the memory slow paths, `lisa_mmu.c` and the exception
helpers all zero it through that pointer (28 sites). `OP_STOP_DATA` uses the
same idiom to halt, and the trace arming in `write_sr` uses it to end a sprint
so the next one begins with the new T1 — deliberately, instead of re-sampling
`cpu->trace` per instruction, which measured +2.17% on an SE/30 row.

**The shape for a new exception:**

```c
/* in the op that detects it */
cpu->my_exception_pending = 1;      /* or a global, if the memory layer raises it */
if (g_bus_error_instr_ptr)
    *g_bus_error_instr_ptr = 0;     /* the loop's own condition now ends it */

/* in CPU_DECODER_EPILOGUE, once per sprint, outside the loop */
if (__builtin_expect(cpu->my_exception_pending, 0)) { ... }
```

**`break` is not available at all.** The decode tree is a nested `switch`, and
every case already ends in `break;` — so a `break` inside an op macro binds to
the innermost *switch*, falls through to the outer switch's own `break`, and
runs on to the end of the loop body. It cannot leave the loop. That is why the
two ops which do leave early, `OP_UNDEFINED` and `VALIDATE_EA_030`, use
`continue`: `switch` captures `break` but not `continue`.

**`goto` to a per-exception label is a legitimate alternative**, and on its own
terms a better one than a flag. The label *is* the dispatch, so there is no
flag to store and no test outside the loop — only the detection `if` in the op,
which is irreducible whatever mechanism follows it. It also skips the rest of
the iteration (`pc += 2`, the burn-down decrement), which counter-zeroing
cannot do: zeroing lets the current iteration finish.

Two practicalities if you use it: the label must zero `*instructions` itself,
because the epilogue asserts the sprint spent its budget; and `goto` leaves the
loop's block scope, so an exception carrying a payload — a fault address — has
to put it in `cpu_t` rather than a local. Payload-free exceptions are where it
is cleanest.

**What does NOT settle any of this is the exit mechanism's own cost.** `break`,
`goto` and counter-zeroing differ only in what happens *after* the decision to
stop; the per-instruction cost is entirely in the **deciding**, and the loop
condition is evaluated every iteration regardless. The theoretical worry about
multiple exits inhibiting loop transforms is weak here — this loop is a 30k-
instruction switch full of calls and memory clobbers that no loop transform was
going to touch. If someone revisits it, measure register pressure rather than
control flow: that is what dominated every measurement behind this rule.

**If the faulting instruction must not complete**, the two are NOT
interchangeable — zeroing the counter lets the current iteration finish, so a
faulting fetch would go on to execute garbage. Even then, do not add a second
test: make a test that already exists carry the information. `ppc_run`'s fetch is the
worked example: it returns false for an ISI and the loop already tests that, so
a fetch bus error belongs in the same return value rather than in the extra
`if (g_bus_error_pending) break;` that sits beside it today.

**Known deviations**, all in the deferred-bus-error path and all measured
above. Removing them is its own piece of work:

| site | decoders | what it should become |
|---|---|---|
| `if (!g_bus_error_pending)` guarding the `cpu->ir` / `ir_pc` latch | 68000 | latch unconditionally; let the faulting path supply the pre-fault `ir` for the group-0 frame |
| `if (last_bus_error_pc != 0 && !supervisor && last_bus_error_pc != pc)` | all three 68K | clear the latch in the epilogue or at delivery, not per instruction |
| `if (g_bus_error_pending) break;` after the fetch | `ppc_run` | fold into `ppc_fetch`'s existing false return |

### Deferred bus errors: skip or retry

The emulator never aborts an instruction mid-execution. When a memory
access faults (unmapped page, failed MMU walk, device-signalled bus
error), the slow path latches `g_bus_error_pending` with the faulting
address, zeroes the burn-down counter through `g_bus_error_instr_ptr`,
and returns a default — `$FF` for reads, dropped writes. The faulting
instruction therefore **completes**, and the epilogue raises the
exception once per sprint. A real 68030 instead aborts the bus cycle,
saves full internal state in a Format $B frame, and restarts the
instruction from scratch after the handler's RTE (MC68030UM §7.2).

Because the instruction has already run, the saved PC in the exception
frame decides the semantics, and the two consumers want opposite ones:

| Frame | `saved_pc` | Semantics | Who needs it |
|---|---|---|---|
| Format `$A` | next instruction | **skip** — RTE resumes past the faulting access | ROM bus-error probes (NuBus slot detection): the ROM's handler deliberately advances past the probe |
| Format `$B` | faulting instruction | **retry** — RTE restarts the instruction | OS kernels with demand paging: the handler maps the page and expects the access to complete on restart |

The dispatch is `g_bus_error_is_pmmu` (`memory.h`), set by
`mmu_handle_fault` according to which path produced the fault: a PMMU
table-walk fault — invalid descriptor, supervisor-only, or
write-protected page — sets it true and takes
`exception_bus_error_retry()` (Format `$B`, `cpu->instruction_pc`);
every other bus timeout leaves it false and takes
`exception_bus_error()` (Format `$A`, skip).  Both epilogues (68030 and
68040) dispatch on exactly that flag.

**The retry path makes restart-cleanliness a per-op contract.**  Retry
re-executes the instruction from scratch, so every op that modifies
architectural state before a memory access must undo that state when
`g_bus_error_pending` is set — otherwise the restart runs with
already-incremented registers or committed stack moves.  The mechanism
is staging plus rollback macros (`_move_src_an_save`, `_cmpm_ay_save`,
…), now applied across MOVE src-An, CMPM, ADDX/SUBX, ABCD/SBCD, CAS,
PACK/UNPK, PUSH/POP (SP committed only if no bus error), and MOVEM
(`movem_to_register` stages register updates and bails before touching
registers on a fault).  A new op with a faultable memory access must do
the same.

The retry function detects a **double bus error** the way real hardware
does — not by comparing PCs (legitimate retry loops would false-trigger)
but by checking whether the frame push or vector read itself faulted; a
fault there halts like the real CPU's double-bus-error condition
(MC68030UM §8.3.3).  An earlier design keyed the retry/skip choice on
`VBR != 0` (kernel-installed vector table ⇒ retry); the PMMU-fault
discriminator superseded it because it separates the two cases by cause
rather than by heuristic.

This is a pragmatic reconstruction, not a faithful one: real hardware
aborts mid-instruction with saved pipeline state, while we complete the
instruction and undo its effects op by op.  The per-op staging covers
exactly the state the restart must not see; anything an op fails to
stage is a latent wrong-state bug that only a retried fault exposes.

### Known exception: the DSP3210 has two decoders

The AV families' DSP3210 is the one core that does **not** follow the shared
decode-tree rule above. `dsp3210.c` and `dsp3210_disasm.c` each carry their own
dispatch, and there is no `dsp3210_decode.h`.

The duplication is concrete, not notional: `dsp3210_disassemble` and
`exec_insn` carry **36 `case` labels each, in identical order**, behind the
same three-step preamble (`op6 = w >> 26`, then `opcode_is_illegal(op6)`, then
the `w >> 29` DA-class dispatch). `opcode_is_illegal` is **14 byte-identical
lines** in both, as are `bits()` and `sext16()`. Four helper pairs
(`dis_da`/`exec_da`, `dis_ca_alu_reg`/`exec_alu_reg`,
`dis_ca_alu_imm`/`exec_alu_imm`, `dis_ca_move`/`exec_move`) and several operand
tables are hand-synchronised. Nothing but the `dsp3210_disasm` unit suite stops
them drifting, and that suite checks agreement after the fact rather than by
construction.

**No ISA reason has been established.** The rule's escape hatch — "unless the
ISA gives a concrete reason not to" — requires one to be recorded, and this
file records none. Until someone examines whether the DA/format encodings
genuinely resist a shared tree, this is **debt, not a sanctioned exception**, and it is written down here so it
cannot be mistaken for one.

Why injected hooks and not the global fast path: aux cores are physical
bus masters (the main CPU's translated, mode-switched view would be
wrong under an MMU), the inline accessors charge I/O penalties against
the in-flight main sprint, private address spaces come free, and a core
then links against nothing — unit tests hand it a 16-line mock bus.

The injected-hook rule is an **auxiliary-core** rule.  A core serving as
the machine's *main* CPU (the 68K, or the PowerPC on the PDM and TNT
machines) is what the global fast path exists for: it
reads and writes through the same inline accessors, gets I/O penalties
charged to its own sprint, and owns the supervisor/user SoA switch.  A
main CPU likewise registers `machine.cpu` and the `$` register aliases —
both reserved for whichever core owns emulated time.

## Machine profile surface

`hw_profile_t.aux_cpus` (sentinel-terminated `struct aux_cpu_slot`
`{name, arch, freq}`) exports `capabilities.aux_cpus` from
`machine.profile` — the frontend and tests assert cores from data, never
from model names.

## When a device earns a core

**A device earns a real core when the guest can load code into it that
we do not control.**  Fixed-firmware devices (Egret, Cuda, COPS, IOPs,
the 8•24 GC) keep their verified behavioral models.  The DSP3210's whole
point is third-party `dspf` modules loaded at runtime — there is no
protocol surface to model; the protocol *is* "execute my program".
