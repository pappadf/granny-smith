# MMU inspection: translate, walk, map, descriptor

The inspection methods every MMU kind's `machine.cpu.mmu` node answers the
same way. The shared half — result shapes, the `map` sweep, the argument
tables — is
[src/core/debug/debug_mmu.c](../../../../src/core/debug/debug_mmu.c) /
[debug_mmu.h](../../../../src/core/debug/debug_mmu.h). The trace types are in
[src/core/memory/mmu_trace.h](../../../../src/core/memory/mmu_trace.h). The
per-MMU halves live with each MMU:
[mmu.c](../../../../src/core/memory/mmu.c) (68030),
[mmu040.c](../../../../src/core/memory/mmu040.c) (68040),
[ppc_mmu.c](../../../../src/core/cpu/ppc/ppc_mmu.c) (PowerPC 601/604) and
[lisa_mmu.c](../../../../src/core/memory/lisa_mmu.c) (the Lisa's segment
MMU). Their member tables are in
[cpu.c](../../../../src/core/cpu/cpu.c),
[ppc.c](../../../../src/core/cpu/ppc/ppc.c) and `lisa_mmu.c`.

## 1. Responsibilities & design

A debugger asks the same four questions of any MMU:

- **translate**: where does this logical address go?
- **walk**: how did the MMU decide that?
- **map**: what is mapped at all?
- **descriptor**: what does this raw table entry say?

Each answer has one shape on all five MMU models. A script or the web Debug
view reads them with no per-architecture code; only the step names and the
descriptor fields differ, because the hardware does.

Two rules keep the answers trustworthy:

- **One translation per MMU.** Each MMU exposes a single side-effect-free
  translation function that fills an `mmu_xlate_t` and, when given one, an
  `mmu_trace_t`. `translate`, `walk` and `map` all call it; the walk is the
  same call with a trace attached.
- **The walker records its own trace.** The trace is written from inside
  the real table walker (`mmu_table_walk`, `m040_walk`, `xlate` /
  `bat_xlate` / `htab_search`, `lisa_resolve_traced`), at the point each
  decision is taken. It is not a second walker reconstructing what the
  first one did, so it cannot drift from the translation it explains. The
  hot paths pass `NULL` and pay one predictable branch per level.

Nothing here changes machine state. The walks run with the U/M (68K) and
R/C (PowerPC) history-bit updates off and fill no SoA entries. A PowerPC
protection failure still resolves, retried with the supervisor key, as
debug reads always have.

## 2. Key types & files

| Type / function | File | Role |
|---|---|---|
| `mmu_xlate_t` | `mmu_trace.h` | one answer: `phys`, `valid`, `via`, `access`, `space`, `span_bits` |
| `mmu_trace_t`, `mmu_trace_step_t` | `mmu_trace.h` | up to 12 steps of up to 16 named fields; header-only recording helpers |
| `mmu_debug_translate` | `mmu.c` | 68030 (and dispatch to the 68040) |
| `mmu040_debug_translate` | `mmu040.c` | 68040: the TT registers (the access kind's pair first), then the walk |
| `ppc_mmu_debug_translate` | `ppc_mmu.c` | 601/604 (`ppc_mmu_translate_debug_ex` is now a wrapper) |
| `lisa_mmu_debug_translate` | `lisa_mmu.c` | the segment MMU |
| `debug_mmu_translate` / `_walk` / `_map` | `debug_mmu.c` | the method bodies over a `debug_mmu_xlate_fn` |

`span_bits` is the log2 size of the aligned region around the address that
translates the same way: linear `phys` with the same `via`/`access`, or
equally invalid. It is what lets `map` skip a 68030 early-termination
block, an invalid table branch or a T=1 segment in one step.

## 3. Behaviour

### 3.1 Result shapes

| Method | Arguments | Result |
|---|---|---|
| `translate` | `addr, [supervisor], [fetch]` | `{phys?, valid, via, access?, space?}` |
| `walk` | `addr, [supervisor], [fetch]` | translate's map plus `steps: [{step, outcome, …}]` |
| `map` | `[start], [end], [supervisor], [fetch], [limit]` | `[{start, size, phys, via, access, space?}]` |
| `descriptor` | `addr, [count], [format]` (the Lisa: `segment, [count], [context]`) | `[{addr, desc, type, …}]` |

- `phys` and `access` appear only for a valid translation.
- `via` is `identity` (translation off), `tt` (68K transparent
  translation), `bat`, `segment` (a PowerPC T=1 segment, or the Lisa), or
  `page` (a table walk).
- `access` is what the queried privilege may do: `rw`, `ro` or `none`. A
  68K supervisor-only page reads as `none` for the user. A PowerPC key/PP
  pair that denies the access still translates, and reports `none`.
- `space` is the Lisa's physical space: `ram`, `io`, `rom` or `mmureg`.
- An omitted `supervisor` means the CPU's current state (68K SR[S],
  PowerPC MSR[PR]). `fetch` selects the instruction side: the 68040's ITT
  registers (consulted before the DTTs), the PowerPC's IBATs and MSR[IT].
  The 68030 and the Lisa accept it and do not distinguish.

### 3.2 Walk steps

Every step has `step` (what was consulted) and `outcome`:

- `miss`: this did not apply, and the search went on.
- `next`: a table descriptor, and the search followed it.
- `hit`: the translation was resolved here.
- `fault`: the search stopped here without one; `reason` says why.

The common fields are `name`, `index`, `addr` (the physical address
read), `desc` / `desc_lo` (the word or words read), `type`, `next` (the
next table) and `phys`. The steps follow the hardware's own order:

| MMU | Steps |
|---|---|
| 68030 | `tt` (`name` tt0/tt1 on a hit) · `root` (crp/srp: both words, limit) · `level` A–D (index, `addr`, descriptor, `dt`, `wp`/`u`/`s`, then `next`, or `m`/`ci`/`phys`/`size` on a page). `reason`: `invalid`, `limit`, `no page descriptor` |
| 68040 | `tt` (the access kind's pair first, then the other: the software TLB serves both streams, so any matching TTR translates) · `root` (urp/srp) · `level` root, pointer, page, and `indirect` when the page descriptor is indirect (`g`, `cm`, `s` on the page) |
| PowerPC 601 | `segment` (`sr`, `index`, `t`, `ks`, `kp`, `key`, `vsid` or `buid`) · `bat` · `pteg` primary · `pteg` secondary. A T=1 segment prevails over the BATs and ends the walk. |
| PowerPC 604 | `bat` (`dbat`/`ibat`) · `segment` · `pteg` primary · `pteg` secondary |
| Lisa | one `segment` step: `index`, `context`, `sor`, `slr`, `type`, `page`, `limit`. `reason`: `invalid`, `limit`, `empty slot` |

A BAT hit carries `index`, both BAT words, `key`, `pp`, `size` and `phys`.
A PTE group carries its `addr`, `hash` and `api`; on a hit it adds the
`slot`, the two PTE words, `pp`, `wimg`, `r`, `c` and `phys`.

Translation off, the Lisa's START-mode bypass, and a PowerPC fetch with
MSR[IT]=0 resolve as `identity` with no steps. The 601 with MSR[DT]=0
still consults the segment register, because a T=1 segment translates
regardless, so its walk shows that one step.

### 3.3 Map

`map` sweeps `[start, end)` (`end` defaults to the end of the space: 4 GB,
or 16 MB on the Lisa) one region at a time. Each step covers the
answer's `span_bits` region, or one page (4 KB; 512 bytes on the Lisa)
when the answer gives none. The sweep merges adjacent valid regions into
a run while `phys` stays linear and `via`/`access`/`space` stay equal,
and lists at most `limit` runs (default 512). To continue, call `map`
again from the last run's end.

The spans are chosen so that a skipped region is truly uniform:

- **68030:** the level at which the walk ended.
- **68040:** 32 MB / 256 KB for an invalid root / pointer entry, else one
  page.
- **TT:** 16 MB, the A31–A24 match granule.
- **PowerPC BAT:** 128 KB. Every BAT block boundary lies on a 128 KB
  boundary, so one granule sees one winning BAT.
- **601 T=1 segment:** the whole 256 MB segment, since T=1 prevails.
- **604 T=1 segment:** only 128 KB, because a BAT elsewhere in the segment
  would win there.

A full 4 GB sweep of a PowerPC page table runs in tens of milliseconds
natively.

The PowerPC map covers the current context, meaning the VSIDs the segment
registers hold. Page-table entries of other address spaces are visible
through `descriptor`, not `map`.

### 3.4 Descriptor formats

| MMU | `format` | Decoded |
|---|---|---|
| 68030 | `short` (default), `long` | `dt`, `type` (`page` / `table`; at the last level a table type means indirect), `wp`, `u`, `s` (long), `m`, `ci`, `phys` or `next` + `next_format`, `limit` (long). The page-address mask follows TC.PS. |
| 68040 | `page` (default), `pointer`, `root` | the level decides how a 4-byte word reads: `table` + `next`, or `page` (`wp`, `u`, `m`, `s`, `g`, `cm`, `phys`), or `indirect` + `next` |
| PowerPC | `pte` | `vsid`, `h`, `api`, `phys`, `r`, `c`, `wimg`, `pp`, and `ea` (see below) |
| Lisa | — (third argument is the `context`) | `segment`, `context`, `base`, `sor`, `slr`, `type`, `limit`, `stack`/`ro` (memory segments), `phys` (the segment's first page) |

The PowerPC decoder also reverses the hash. A PTE group's address holds
the low ten bits of the hash, complemented for a secondary entry, and
hash = VSID ⊕ page index. So the page index is those bits ⊕ the VSID's
low ten bits, under the API's top six. When a segment register currently
holds the entry's VSID, the entry gets `ea`, the effective page address
it maps.

## 4. Object-model / shell surface

```
machine.cpu.mmu.walk 0x40800000                 # the CPU's current privilege
machine.cpu.mmu.walk(0x2000, false).steps       # user root
machine.cpu.mmu.map supervisor=false limit=32
machine.cpu.mmu.descriptor 0x40800050 4         # 68030 short descriptors
machine.cpu.mmu.descriptor 0x004e0000 8         # one PowerPC PTE group

# Which level stopped the walk?
let w = machine.cpu.mmu.walk(0x11004000)
let s = $w.steps[len($w.steps) - 1]
echo "${$s.step} ${$s.outcome} ${$s.reason}"
```

The web Debug view's MMU section has four tabs, each reading these
methods through `app/web2/src/bus/mmu.ts`:

- **State:** the MMU's registers.
- **Translate:** `walk`, one line per step.
- **Map:** `map`; clicking a run walks its start.
- **Descriptors:** `descriptor`, seeded from the PC's walk.

## 5. Checkpointing

Nothing to save: the methods read live state and keep none.

## 6. Testing

- Unit suites:
  - `tests/unit/suites/mmu/`: the 68030 trace, side-effect-freedom,
    `access`, TT, and the `map` sweep over a stand-in MMU.
  - `tests/unit/suites/mmu040/`: every 040 level including the indirect
    hop, and the ITT/DTT split.
  - `tests/unit/suites/ppc_mmu/`: the 601 and 604 step orders, a secondary
    PTE group hit, BAT and T=1 traces, and R/C left untouched.
- Integration row `mmu-inspect` boots a 68030, 68040, 601, 604 and Lisa
  machine. On each it checks against live guest tables that `walk`
  answers what `translate` answers, that every `map` run translates
  linearly to its last byte, and that `descriptor` reads the word the walk
  read. On the 604 it also checks that a PTE's reversed `ea` translates
  back to its page.

## 7. Known debts

- The PowerPC `map` lists only the current context. A scan of the whole
  page table, grouping entries by VSID, would map every address space.
- The 68030 decoder cannot tell a table descriptor from an indirect one
  without knowing the level; `walk` can, since it knows the level.

## 8. See also

- [memory.md](../memory/memory.md): the 68030 PMMU and the SoA fast path.
- [mmu040.md](../memory/mmu040.md): the 68040 walker.
- [ppc.md](../cpu/ppc.md): the PowerPC MMU front end.
