# CPU cores — the module contract

How Granny Smith executes more than one CPU per machine: exactly one
**main CPU** that owns emulated time (the 68K today), plus any number of
**auxiliary cores** — peripheral processors that execute real guest code
but do not control time.  The first auxiliary core is the AV family's
DSP3210 (`src/core/cpu/dsp3210/`, wired by `src/machines/av/dsp.c`).
Design:
executed by `proposal-dsp3210-plaintalk.md`.

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
| Interpreter | big-switch decode, plain C, no JIT.  The shared decoder/disassembler template-macro pattern (the 68K's `cpu_decode.h` / `cpu_ops.h` model, spelled out in proposal-heterogeneous-multi-cpu.md §3.3.1) is the house style — one guard-free decode tree included by both the emulator (execution `OP_` overloads) and the disassembler (sprintf `OP_` overloads), so the two cannot drift.  Follow it unless the ISA gives a concrete reason not to; the PPC core (`ppc_decode.h`) is the second instantiation of the pattern |
| Execution ABI | `void <arch>_run(<arch>_t *, uint32_t *instructions)` — burn-down counter; returns with it 0 (budget spent) or >0 (went idle) |
| Idle/reset | `<arch>_is_idle()`, `<arch>_reset(...)`, an interrupt-request entry point for external pins.  When guest code polls a pin's *level* (not just its latched request), the entry point must model both — e.g. `dsp3210_ext_pulse(s, vector, slots)` latches the request and asserts the live pin for `slots` of core time, and the status-register pin bits reflect the level, not the latch |
| **Bus access** | **injected at init** (the guest-physical hook pattern of `sonic.h`/`psc.h`).  The core never touches `g_active_*`, `g_page_table`, the MMU, or any sprint-timing global.  On-chip resources (internal RAM, MMIO) decode *inside* the core before the hooks are consulted |
| State | one POD struct, pointers last; checkpoint boundary before the first pointer; hook pointers re-planted on restore |
| **Disassembler** | mandatory, dependency-free, raw words + pc in / text out — linkable standalone (`tools/disasm --arch <name>`) |
| Object class | `machine.<name>` node with register attrs (hex), `instr_count`, `state` (`reset`/`running`/`idle`/`crashed`), methods `step(n)` and `disasm(addr, count)`.  **No `$` aliases** — those stay reserved for the main CPU |
| Logging | own `LOG_USE_CATEGORY_NAME("<arch>")` (in the glue; the core itself stays I/O-free) |
| Tests | unit suite under `tests/unit/suites/<arch>/` against a mock bus |

Why injected hooks and not the global fast path: aux cores are physical
bus masters (the main CPU's translated, mode-switched view would be
wrong under an MMU), the inline accessors charge I/O penalties against
the in-flight main sprint, private address spaces come free, and a core
then links against nothing — unit tests hand it a 16-line mock bus.

The injected-hook rule is an **auxiliary-core** rule.  A core serving as
the machine's *main* CPU (the 68K today, PowerPC per
proposal-powerpc-601-pdm.md) is what the global fast path exists for: it
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
