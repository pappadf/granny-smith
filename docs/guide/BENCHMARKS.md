# CPU performance benchmarks

How CI measures the emulator's speed, and how to read and run it. The
integration suite checks that the emulator *behaves* the same; this checks
that it is not getting *slower*.

## The problem

A change of 1–3 % in the interpreter's cost per guest instruction is routine
and matters, and wall-clock timing on shared cloud machines cannot see it: the
same binary on two GitHub-hosted runners differs by 15 % or more, and on one
runner by ±15 % run to run. So the benchmarks do not time the PR against a
stored number. They measure three things, from strongest to weakest:

| | Measures | Noise | Use |
|---|---|---|---|
| **A** host instructions per guest instruction (callgrind) | the work the emulator does per guest instruction | none: identical across runs and runners | **gate** every PR at +1.5 % |
| **B** paired wall-clock ratio, PR vs merge base on one runner | real speed, caches and branch prediction included | the runner cancels out; a few % residual | **warn** on PRs |
| **C** MIPS per calibration score | absolute speed over time | per-CPU-model trend lines | **trend**, nightly on main |

## The rows

Each row is a CPU-bound guest state, reached by a deterministic boot, with a
guard that it is still the workload it claims to be. They live in
`tests/bench/` (`<row>.script`, configured in `rows.json`):

| Row | CPU | State |
|---|---|---|
| `plus-boot` | 68000 | Plus, System 6.0.8 floppy boot, from 60 M instructions |
| `se30-finder-idle` | 68030, PMMU idle | SE/30 at the System 7.5.3 Finder desktop (bare volume) |
| `iicx-marathon` | 68030 + 8•24 GC | Marathon at its main menu, into the self-running demo |
| `q840av-finder` | 68040 | Quadra 840AV at the System 7.1 Finder |
| `pm6100-68kemu` | 601 | 6100 ROM boot, the 68K emulator running the Start Manager |
| `pm7500-of` | 604 | 7500 ROM boot past Open Firmware and POST, into the 68K emulator |
| `iici-aux` | 68030, PMMU busy | IIci, A/UX 3.0.1 kernel start-up with translation on |

Every row runs once per executor the build has: the switch cores, and the
predecoded executors (`predecode.enabled`) where the build carries them.

A row script defines `bench_boot()` (boot to the state, assert it was reached)
and `bench_guard()` (assert, after a measured window, that it still holds:
the Finder is the application, the MMU is on, the PowerPC is in the 68K
emulator). The harness also checks every window for:

- **CPU-bound:** memory slow-path accesses per guest instruction stay under the
  row's `slowpath_max` (5 % by default; `iicx-marathon` draws to a NuBus
  framebuffer and is allowed 8 %);
- **not stuck:** the window retired at least 90 % of the instructions it was
  given (a STOP-idle guest retires far fewer).

A window that fails either is reported as **invalid**, never as a regression.
Why the guards matter: the media-free SE/30 looks like an idle machine, but
it is the ROM's SCSI scan busy-waiting on the 5380, every second instruction a
device register read, and it measured the device path rather than the CPU. A
row that drifts into such a state must say so.

Checkpoints refuse a file written by another build, so the harness boots every
row once per binary and saves the checkpoint in its work directory; the
measurements load it. The head and base checkpoints are compared for guest
state (`scripts/bench/ckptdiff.py`, line numbers and host pointers masked): a
difference means the PR changed guest behaviour or timing, which the report
says, while A stays valid per guest instruction.

## Metric A: host instructions per guest instruction

```
A = (Ir(N2) − Ir(N1)) / (G(N2) − G(N1))
```

`Ir(N)` is callgrind's instruction count collected inside the CPU's run
function (`--toggle-collect=cpu_run_68000|cpu_run_68030|cpu_run_68040|ppc_run`,
the row's `run_fn`) over a run of N scheduler instructions from the
checkpoint, `G(N)` the guest instructions retired. N1 = 5 M and N2 = 30 M;
subtracting cancels the warm-up. Everything under the run function counts
(memory slow paths, devices, the scheduler's I/O penalties); the event
dispatch between sprints does not.

The counts are a property of the binary and the input: repeat runs of one
binary are bit-identical. Two builds whose CPU code is the same agree to a
few parts per million, not always to the last digit: the first measurements
found one assert-only lookup (`scheduler_new_cpu_event_ex` searching the
registered event types) whose cost differed by 5 % between two such builds,
5 ppm of the total. The gate compares
head with its merge base in the same job, never with a stored number, so
nothing needs regenerating when main gets faster:

- **fail** when A rises by more than **1.5 %** on any valid row and executor;
- **report** improvements, so a PR can say what it bought.

A deliberate trade-off (correctness first) carries the **`perf-accepted`**
label: the job passes and the table shows the accepted cost. A dev-container
image bump changes the compiler and with it every A value; that PR carries the
label too.

**A2** (nightly, report only) runs the same windows with callgrind's cache and
branch simulation: L1 and last-level misses and mispredicts per guest
instruction. Still deterministic; the model is not the runner's real cache,
but it reacts to a table that outgrows L1 or a dispatch that stops being
predictable — what pure instruction counts miss.

What A cannot see: a change that keeps the instruction count but adds cache
misses, a dependent load chain or mispredicts. A2 narrows that gap; B closes
it for PRs.

## Metric B: paired wall-clock ratio

For each row and executor, head and base each load their checkpoint, run a
warm-up and time a window with `scheduler.host_user_ns` (process CPU time),
five rounds. By default the two run **at the same time**, pinned to cores 0
and 1 (`taskset`), which core each gets swapping from row to row: both see the
same instant of the machine, its frequency and its neighbours, so the ratio
`median(MIPS_head) / median(MIPS_base)` divides the runner out. The report
gives a 95 % bootstrap interval over the rounds and the Hodges–Lehmann
estimate (both robust to the occasional wild round).

- **warn** when the interval's upper bound is below 0.97 (head confidently
  more than 3 % slower);
- **never fail** on B alone. A warning on B with A clean says: look at A2, or
  profile.

`--pairing=alternate` runs them one after the other on one core instead
(H B H B …). On the validation runs it was the noisier mode by half (mean
round-to-round spread 1.7 % against 0.8 %) and warned six times on a change
that left the CPU code alone, where concurrent pairing never came within
2 % of the line. Two emulators at once share the runner (on a 4-vCPU EPYC
7763 runner each ran at half its alone speed, cores 0 and 1 being SMT
siblings), which is fair to both sides of the ratio but makes the MIPS of a
concurrent run useless as absolute numbers: C only uses head-only runs.

B also sees what A cannot, including effects nobody wrote: two builds whose
CPU code is identical differed by up to 4 % on single rows (the PowerPC rows
2 % slower on every runner; `iici-aux` 4–7 % on the EPYC 7763 alone), which
is code placement, not code. A B warning is a prompt to look, never a verdict.

## Metric C: normalized MIPS

C answers "is main faster than it was in June?", which B cannot: neither the
June base nor the June runner exists today. It divides head's MIPS by a
calibration score of the runner, from `gs-headless --calibrate`
(`src/platform/headless/calibrate.c`). A bogomips loop would measure clock
frequency and little else, while an interpreter is dominated by indirect-branch
prediction and cache latency, so the calibration is three kernels shaped like
the emulator:

| Kernel | Models |
|---|---|
| `alu` | issue width, frequency: 8 independent add/xor/shift chains |
| `chase` | cache and memory latency: a dependent pointer chase through a 1 MB random cycle |
| `interp` | indirect-branch prediction: a 64-opcode switch interpreter over a fixed 64 KB program, registers in memory |

`score = geomean(ref/alu, ref/chase, ref/interp)`. The kernels are fixed: a
change bumps `CALIBRATE_VERSION`, which keys the trend. Calibration runs
before and after the measurements; a spread above 5 % marks the run noisy (its
C points are stored but kept out of trend lines). Points are keyed by the
runner's CPU model (`/proc/cpuinfo`) and the calibration version: one trend
line per model. Within a model the score absorbs instance and neighbour
differences; across models it is a best effort.

## Validation

The thresholds (`tests/bench/rows.json`, `"thresholds"`) and the pairing mode
come from eleven runs of the bench on GitHub-hosted runners on a change that
added the calibration code and left the CPU cores alone, five per pairing
mode, plus one run with a deliberate regression (an extra per-instruction
test in the 68030 loop). The runners came as four CPU models: AMD EPYC 7763
(five runs), 9V45 (three), 9V74 and Intel Xeon Platinum 8370C (one each).

| Check | Result |
|---|---|
| A on the no-op change | head = base to the last printed digit on all seven rows, in all ten runs, on all four CPU models, and equal to the values measured in a development container |
| A on the deliberate regression | +2.6 to +2.8 % on the three 68030 rows (FAIL), 0.00 % on the other four; the same to the last digit in a development container and on a fifth CPU model (Xeon Platinum 8573C) |
| B on the deliberate regression (alternate pairing) | did not see it (68030 rows 0.968–0.999: one more well-predicted branch per instruction costs that CPU next to nothing) and warned on the untouched 68040 row at 0.913 |
| B, concurrent pairing | ratios 0.960–1.013, no warning; lowest interval upper bound 0.981 |
| B, alternate pairing | ratios 0.930–1.052, six warnings |
| Calibration spread before/after | 0.03–4.3 %, under the 5 % noise line |
| Runtime | the PR job (A + B, seven rows) took 11 minutes end to end, 8 of them measuring |

The calibration could not show what it was meant to show within a CPU
model, because there was nothing to remove: on the EPYC 7763 the alternate
runs' MIPS agreed to 0.4 % run to run, and so did the score. Across models
the levels disagree widely (the Zen 5 9V45 runs the `interp` kernel three
times faster than the 9V74 and the emulator no faster), so the trend stays
one line per CPU model, as the C section says.

## Running it locally

```bash
make -f Makefile.headless -j$(nproc)                              # head
scripts/bench/build-base.sh "$(git merge-base origin/main HEAD)" tmp/bench-base/gs-headless
python3 scripts/bench/run.py --head build/headless/gs-headless \
    --base tmp/bench-base/gs-headless --rows pr --metrics A,B 2>&1 | tee tmp/bench.log
python3 scripts/bench/compare.py tmp/bench/raw.jsonl --json tmp/bench/bench.json
```

- `--rows` takes `pr` (the rows gated on PRs), `all`, or a comma list;
  `--metrics` any of `A,A2,B,C`; omit `--base` for head-only numbers.
- Every run keeps its driver script and `output.log` under
  `tmp/bench/<head|base>/<row>/<run>/`; callgrind's output is beside it
  (`callgrind_annotate tmp/bench/head/<row>/A-switch-30000000/callgrind.out`
  shows where the instructions went).
- Valgrind and the test data are needed; `taskset` is used when present.
- All emulator runs use `--speed=turbo --jobs=inline --io=sync`: under Valgrind
  a job thread polling the emulator thread costs ~15× the run itself, and
  natively it removes a source of jitter.

## In CI (`.github/workflows/bench.yml`)

| Trigger | Runs |
|---|---|
| PR touching `src/core`, `src/machines`, the headless platform, the Makefiles, the bench rows or harness | head vs merge base: A (gate) and B (warn) on the `pr` rows; the label `perf-accepted` re-runs it |
| nightly 02:40 UTC on main | every row, A + A2 + B + C on main alone; the `history` job appends the numbers to the `bench-data` branch (`history.jsonl`, one line per row, metric and night) |
| `workflow_dispatch` | either mode, with rows, base, pairing, rounds and metrics as inputs |

The job runs beside `CI`, sharing only the dev image, so it adds no wall time
to a PR. The step summary carries the tables; `bench` (30 days, the repository maximum) carries
`raw.jsonl` and `bench.json`. The base binary is cached by commit and image.
Without the test data (forks) the job is skipped, as the integration tiers
are. Only the `history` job, which runs nothing from a PR, has
`contents: write`.

### Reading the table

- **A, Verdict** — `ok`, `improved`, `FAIL` (over 1.5 %), `accepted`
  (over, with the label), `invalid` (a guard tripped: the row no longer
  measures what it claims; fix the row, not the code), `new` (an executor the
  base does not have).
- **B, Ratio and CI** — below 1 is slower. `WARN` only when the whole interval
  is below 0.97.
- **Guest state at the checkpoints** — "differs" is expected when a PR changes
  guest timing; it is otherwise worth a look.

## The browser variant

`tests/e2e/web2-specs/perf-headbase.spec.ts` runs two WASM builds — the
checkout's and a base build's `dist/` (`PERF_BASE_DIST`, served on a second
port) — in two pages of one browser session, and measures their turbo MIPS
alternately on the SE/30 Finder and Marathon rows, plus a JS port of the
calibration kernels. Report only (`PERFBENCH` lines); skipped without
`PERF_BASE_DIST`. Expect its ratios to wander by ±5 %: an A/A run (a build
against a copy of itself, three pairs of 4 s windows) read 1.039.

```bash
scripts/bench/build-base.sh --wasm "$(git merge-base origin/main HEAD)" tmp/bench-base-dist
cd tests/e2e && PERF_BASE_DIST=$PWD/../../tmp/bench-base-dist \
  npx playwright test --config=playwright.web2.config.ts --project=serial perf-headbase.spec.ts
```

## The WebAssembly engine without a browser

`make -f Makefile.headless TARGET=node` builds the same headless program
with emcc into `build/headless[-fast]-node/`: a `gs-headless.js` run by
Node with the host filesystem, and a `gs-headless` launcher beside it, so
`run.py` takes it as `--head` or `--base` unchanged.  It is
single-threaded (the harness's `--jobs=inline --io=sync`), and it is the
browser's CPU cores on the browser's engine: its turbo MIPS on the
PowerPC rows match the browser's.  Paired against the native build,
metric B's ratio is the cost of WebAssembly:

```bash
make -f Makefile.headless MODE=fast -j$(nproc)
make -f Makefile.headless MODE=fast TARGET=node -j$(nproc)
python3 scripts/bench/run.py --head build/headless-fast-node/gs-headless \
    --base build/headless-fast/gs-headless --rows pm8500-finder,g3dt-boot \
    --metrics B --pairing alternate
```

## Page heat

A build with `EXTRA_CFLAGS=-DPD_PROFILE` gains `predecode.profile_reset()`
and `predecode.profile(path)`: per code page, the instructions retired on
the predecoded tier, the visits (runs of instructions inside one page,
entry to exit or sprint end), the stores into it and the stores that reset
decoded entries; with histograms of visit and sprint lengths and the
slow-path accesses made from a predecoded page.  `page_heat.py` boots
PowerPC rows on such a build and prints how few pages carry the window,
how often they are written, and how many translations a page-granular
translation tier would make at a given heat threshold.  The profiler costs
a call per page transition, so the build is for these counts only, never
for timing.

```bash
make -f Makefile.headless MODE=fast BUILD_DIR=build/headless-prof \
    EXTRA_CFLAGS=-DPD_PROFILE -j$(nproc)
python3 scripts/bench/page_heat.py --bin build/headless-prof/gs-headless
```

## Adding a row

1. Write `tests/bench/<row>.script` with `bench_boot()` and `bench_guard()`;
   pin the RTC after `machine.boot`, use only scripted input.
2. Add it to `tests/bench/rows.json`: the start-up ROM, model and RAM, the
   CPU's `run_fn`, and window sizes if the defaults leave the state.
3. Probe it: the slow-path fraction in its windows, that the guard holds, and
   that two A runs agree to the last digit:
   `python3 scripts/bench/run.py --head build/headless/gs-headless --rows <row> --metrics A`
   twice.
