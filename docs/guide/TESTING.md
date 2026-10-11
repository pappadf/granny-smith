# Testing

This document describes the test architecture for Granny Smith. The project uses
three test tiers: native C unit tests, native C integration tests (headless
emulator), and browser-based end-to-end tests (Playwright).

## Quick Reference

| Tier | Command | In CI | Test data |
|------|---------|-------|-----------|
| Unit | `make -j$(nproc) -C tests/unit run` | 2½ min native, then 40 s for the wasm32 rerun | No, but the `third-party/single-step-tests` and `third-party/powerpc-test` submodules must be initialised |
| Integration, unit tier | `make integration-test TIER=unit -j$(nproc)` | 1 min | Yes |
| Integration, matrix tier | `make integration-test TIER=matrix -j$(nproc)` | 17 min | Yes |
| Integration, extended tier | `make integration-test TIER=extended -j$(nproc)` | 31 min of CPU (8 min at -j4) | Yes |
| Integration, all tiers as CI runs them | `make integration-test TIER="unit matrix extended" SHARD=1/3 -j$(nproc)` | one third of the work per shard | Yes |
| E2E | `make e2e-test` | 21 min, one worker (CI splits it over three runners) | Yes |
| Unit + every integration tier | `make test` | the sum of the rows above | Yes, for the integration part |

The times are CI step times on GitHub's 4-core `ubuntu-24.04` runner
(September 2026); `-j` is what CI passes, and a row without it runs serially
there. Locally, `-j$(nproc)` works for every tier.

Test data is fetched via `scripts/fetch-test-data.sh` (requires
`GS_TEST_DATA_TOKEN`). See [TEST_DATA.md](TEST_DATA.md).

## Directory Layout

```
tests/
├── data/                           # Proprietary test assets (.gitignored)
├── unit/                           # Native C unit tests
│   ├── Makefile                   #   Orchestrator (discovers suites/*/Makefile)
│   ├── common.mk                  #   The build recipe every suite includes
│   ├── suites/<name>/             #   One directory per suite
│   └── support/                   #   Harnesses, stubs, header shims
├── integration/                    # Headless emulator integration tests
│   ├── lib/                       #   Shared row/wait/golden library (include'd)
│   ├── suite-<family>/            #   Per-machine suites (rows, goldens/)
│   └── <name>/                    #   One directory per test (config.mk, test.script)
└── e2e/                            # Browser Playwright E2E tests (web2 UI)
    ├── web2-specs/                #   Functional suite (playwright.web2.config.ts)
    ├── ui-prod-smoke/             #   Production-bundle boot smoke
    └── helpers/web2-fs.ts         #   OPFS staging + drag helpers
```

The inventories are the tools, not this page:

```bash
make -C tests/unit list           # every unit suite
make -C tests/integration list    # every integration test, with its tier and description
```

and [tests/e2e/README.md](../../tests/e2e/README.md) annotates every e2e spec.

---

## Unit Tests

Each suite under `tests/unit/suites/` is one native binary that compiles only
the emulator sources it tests and links stubs for everything else. A suite
declares a **harness mode** in its Makefile:

| Mode | What the suite gets |
|------|---------------------|
| `isolated` (default) | A `test_context_t` harness and stubs for every subsystem |
| `cpu` | The same harness with the real 68k CPU, FPU, memory and MMU |
| `none` | No harness and no default stubs: its own `main()` and mocks, plus any stubs it names |

```bash
make -j$(nproc) -C tests/unit run   # build every suite in parallel, then run them all
make -C tests/unit test-disasm      # build and run one suite
make -C tests/unit list             # list the suites
```

`run` builds with `-k` and then runs every suite, so one failure, in the
build or in a test, does not hide the rest; it exits non-zero if any suite
failed. Within a suite the first failed `ASSERT_*` ends that binary.

[tests/unit/README.md](../../tests/unit/README.md) is the reference for
writing a suite: the Makefile variables, the three harness modes, every stub
and what it provides, the assertion macros, and the disassembler corpus
switch.

---

## Integration Tests

Integration tests exercise the full emulator in headless mode (native binary,
no browser). Each test has a `config.mk` (ROM/disk paths, arguments) and a
`test.script` (shell commands + expected-output checks).

### Running Integration Tests

```bash
make integration-test               # Build headless + run all
make integration-test TIER=matrix   # One tier: unit | matrix | extended
make integration-test -j$(nproc)    # Parallel (safe: per-test storage cache)
make integration-test-valgrind      # Under Valgrind (scope to TIER=unit)
make -C tests/integration test-suite-plus    # Run a single test/suite
make -C tests/integration list      # List available tests (with tiers)
make integration-test TIER="matrix extended" -j$(nproc)   # several tiers, one pool
make integration-test SHARD=2/3 -j$(nproc)                # one of CI's shards
```

**Order and shards.** Tests start longest first: `scripts/order-tests.py`
sorts them by the wall seconds recorded in
`tests/integration/test-weights.json`, so the long suites start at once
instead of whenever the alphabet reaches them, and a test with no recorded
weight starts first of all.  `SHARD=K/N` keeps one of N shards, bin-packed
from the same weights, which is how CI splits the run across runners.  The
runner prints each test's seconds on its result line and appends a record
to `test-results/durations.jsonl`; `scripts/gen-test-weights.py` turns
those (CI uploads them per shard) into a new `test-weights.json`.  Only the
order depends on the weights, never a result, so they are refreshed when
they drift far enough to unbalance the shards, not on every change.

Every test runs with a private `GS_STORAGE_CACHE` under its work
directory: the emulator routes all delta/journal sidecars and scratch
files there, so nothing ever writes into `tests/data` and independent
tests can run concurrently. The per-test runner logic lives in
`scripts/run-integration-test.sh`.

### Tiers

Each `config.mk` declares a `TEST_TIER`: `unit` (zero/near-zero guest
cycles, seconds for the whole tier), `matrix` (per-machine boot suites —
the PR gate), and `extended` (long diagnostics, installers, app
choreography — nightly). CI runs by tier, so a test with no recognised
tier runs nowhere.

The unit tier's ceiling is about a second of host CPU per test and under
15 s for the whole tier (about 10 s today). Measure a candidate, don't
count its instructions: a 68k machine runs 60 M instructions in about a
second, while a PowerPC ROM ladder spends several seconds before Open
Firmware assigns a BAR. A test that needs more belongs in `matrix`. That
is why the PPC ROM ladders, the TNT PCI tests and `machine-restart` are
matrix tests. The nightly Valgrind run still reaches every PowerPC family:
the ANS through the unit tier's `ans-pci-slots` and
`ans-machine-restart`, and PDM and TNT through their own short runs
(`nightly.yml`).

### Suites and the shared script library

Machine families are covered by *suite* directories (`suite-plus`,
`suite-se30`, `suite-iix`, `suite-iicx`, `suite-iici`, `suite-iisi`,
`suite-iifx`, `suite-quadra`, `suite-av`, `suite-lisa`, `suite-pdm`,
`suite-tnt`, `suite-ans`, `suite-gossamer`): one daemon run, one row per (system, media, RAM,
video) cell, re-instantiating via `machine.boot` between rows. A boot
assertion belongs as a row in its machine's suite.

Two rules suite rows must follow, both learned from real failures:

- **Name every staging argument the row depends on.** `machine.boot`
  takes a complete document: `model` and `rom` are required (the harness
  binds `--var ROM` to the test's configured ROM, so `rom="${$ROM}"` is
  the idiom), and every omitted field resolves to the model's own
  default — never to what a previous row booted. A row that cares about
  the video card, vROM, or monitor sense still passes it explicitly so
  the row reads as its own specification. `machine.restart` power-cycles
  the current machine (nothing is torn down, so its PRAM/NVRAM and media
  are simply still there); a different configuration is a new
  `machine.boot`, which inherits nothing. Device state a row pins --
  the clock, a PRAM byte, an NVRAM setting -- is written on the line
  after the boot, never before it.
- **Interacting rows use `wait_stable` + `check`, not `wait_match`.**
  `wait_match` stops at the first quantum whose frame equals the golden,
  which can precede quiescence; `wait_stable` behaves identically when
  capturing and when verifying, so menu/click choreography lands the
  same way in both. Screens that animate (a blinking "?" icon, a text
  caret) never settle at all — those rows use a fixed `run_ticks`
  window and their goldens are animation-phase-sensitive by design. Shared
harness functions live in `tests/integration/lib/mac.script`, pulled
in with the shell's `include` statement. The library provides
condition-based waits (`wait_match`, `wait_stable`, `wait_change`,
`wait_global`, `wait_desktop` — a wait states its condition; ceilings are
hang detectors), budgets that end early on their golden
(`run_until_match`, `run_until_match_ex`: for the screens a stability
wait cannot find, they poll the golden; under `REGEN=1` they poll an
existing golden the same way, so the goldens after it are captured on the
timeline verify mode runs, and run the whole budget only for a golden not
captured yet), input waits that run until the guest has taken the input or
answered it (`serial_type`: a line typed on a serial port, which the
emulated line delivers one character time apart, run until the guest has
read it; `run_until_sent`: run until a serial port has transmitted a
marker such as a prompt, in slices that start small and double;
`wait_mouse_taken`: run until `machine.adb.mouse.pending` clears, which
on ADB includes a press and release queued behind each other;
`wait_keys_taken`: the same for `machine.adb.keyboard.pending`;
`wait_cursor`: run until the guest's cursor task has taken a `"global"`
move), budgets that end once the guest has reacted (`settle`: a fixed
budget that ends early once the screen has held still for a third of a
second, sampled in guest ticks, never failing at its ceiling;
`settle_steps` and `type_settle`, the same sampled in instructions for a
guest without Mac OS `Ticks`, such as an Open Firmware or Linux console;
`reach`: a head start and a stability wait that end on a golden in
verify mode, while REGEN still runs the head start and settles as the golden
was captured), `skip_test` for a test whose media is absent (the runner then prints
`=== SKIP: <test> (<why>) ===` instead of PASS and records `SKIP` in
`durations.jsonl`), guest-tick choreography (`run_ticks`, `click` and `double_click`, which
press and release back to back and run until the guest has taken the click,
`about_box`), the row harness (`row_on`/`row_end`/`suite_done`,
milestone rows), addressing-mode asserts (`assert_addr`), and
machine-read coverage records (`@@COV` lines).

A second, machine-specific library sits beside it:
`tests/integration/lib/ans.script`, for the Apple Network Servers. Those
rows drive Open Firmware over the guest serial console rather than reading a
screen, and two things about that machine are awkward exactly once and
identical in every row — its console is the *monitor* until the firmware is
told otherwise (`ans_boot_serial`, which types Apple's documented `setenv`
pair on the machine's own ADB keyboard and cold-boots), and its console
drops characters from an input burst, which the paced serial line avoids
(`ans_send` types a whole command and runs until the firmware has read it).
See the library's header; the reasoning is worth reading before writing a
row against a machine that narrates instead of drawing.

A third, `tests/integration/lib/gossamer.script`, holds the beige G3's
Open Firmware entry (Command-Option-O-F on the keyboard, then `ttya io`)
and the Mac OS 9.2.1 installer choreography the install rows share.

Suite variables are passed via `TEST_VARS`:

```bash
make -C tests/integration test-suite-quadra TEST_VARS="ROW=q700-chime"   # one row
make -C tests/integration test-suite-av TEST_VARS="ROW_SET=*"           # every row of a split suite
make -C tests/integration test-suite-quadra TEST_VARS="KEEP_GOING=1"     # nightly: run past red rows
make -C tests/integration test-suite-quadra TEST_VARS="REGEN=1"          # recapture goldens (review the diff!)
```

Suite goldens live in `<suite>/goldens/` named
`<model>-<system>-<WxHxD>[-<state>].png`.

**Review the REGEN diff, and never skim it.** Recapturing turns *whatever is
on screen* into the expected result, so a row whose choreography stopped
advancing recaptures into a green test that asserts a stuck frame. That is not
theoretical: an attempt to re-host `iicx-mactest` to the IIci recaptured all
seven of its checkpoints to one frame — MacTest's "SUSPECTED PROBLEM: Logic
board" dialog — and CI passed, with a golden named `floppy-test-success.png`
holding a picture of a hardware failure.

`scripts/check-goldens.py` gates the cheapest signal for that failure: within
one script, two *different* goldens must not hold identical bytes. It runs in
CI before the test tiers (it needs neither a build nor test data) and is worth
running by hand after any recapture:

```bash
python3 scripts/check-goldens.py
```

Some collisions are legitimate — the Welcome splash is pure black-and-white, so
it scans out identically at 1 bpp and 2 bpp, and identically from two cards at
the same geometry. Those rows prove the state changed with asserts on the
hardware (savedMode, RowWords, CLUT PBCR, the card's sister byte) and use the
golden only to pin that the raster still scans out. Waive them explicitly, in
the script, naming both files and the proof:

```
# golden-collision-ok: welcome-13in_rgb-640x480-1bpp.png welcome-13in_rgb-640x480-2bpp.png
# - black-and-white splash, identical at both depths; the depth change is proven
# by the savedMode/RowWords asserts above, not by the frame.
```

A waiver must name every file in the collision, so it cannot silently grow to
cover a third golden that collides later. A waiver with no separate proof behind
it is the same failure wearing a comment.

`check-goldens.py` only catches goldens identical to *each other*. A lone bad
one — a single row whose reference is a blank screen — has nothing to collide
with. For that, `scripts/golden-triage.py` ranks every reference image by how
much is actually on it (distinct 8x8 tiles: a dither pattern is a handful, a
Welcome splash ~64, a Finder desktop 100+):

```bash
python3 scripts/golden-triage.py
```

It has no pass/fail and is not run by CI — "how sparse may a legitimate screen
be" is a judgement. Read it against a golden's **siblings**, not against an
absolute number: within one depth sweep every cell lands on the same splash, so
one cell reading 8 where its three siblings read 64 is wrong whatever the
threshold. That comparison found four goldens that were pictures of an empty
screen; the absolute value alone would have been merely suggestive.

Neither script changes how goldens are compared. Matching is byte-exact via
`machine.screen.match`, with no tolerance and no fuzzy comparison anywhere.

### Checkpoint fixtures

Some tests start where another test has already been: `gossamer-checkpoint`
restores the 6 G mid-boot point `suite-gossamer`'s g3dt row passes through.
Instead of booting there again, the consumer restores the producer's
checkpoint:

- The producer's `config.mk` says `TEST_PROVIDES := <name>` and its script
  calls `fixture_save("<name>")` at that point; the consumer says
  `TEST_NEEDS := <name>` and calls `fixture_load("<name>")`, which returns
  false when there is no fixture to load.
- Only the aggregate `make test` provides fixtures.  It clears
  `build/integration/fixtures/`, makes each consumer wait for its producer
  (an order-only prerequisite), and passes the directory to every test as
  `$FIXTURES`.  `scripts/order-tests.py` bin-packs a producer and its
  consumers as one item, so they always share a CI shard.
  `make test TESTS="a b"` runs a chosen set the same way.
- A test run on its own (`make test-<name>`) gets no fixtures, so a consumer
  always has its own way to the same state (`if !fixture_load(...) { ... }`).
  That way must reach the fixture's state byte for byte, or the consumer's
  goldens would depend on how it ran.  A restored run is deterministic but
  is not always the straight run's timeline (`tnt-voodoo2-glide-sw`'s Quake
  demo is a few frames elsewhere after a restore), so a consumer that plays
  on from a restore does a save and a load itself when it boots inline.
- A producer that fails leaves no fixture; its consumers then boot inline
  rather than fail with it.  Fixtures never cross runs or builds.
- A consumer starts only when its producer has **finished**, not when it
  saved.  Use a fixture when the producer is short after the save, or the
  consumer is: two long tests chained this way run back to back where they
  used to run side by side.  (`tnt-voodoo2-glide-sw` was a consumer of its
  sibling's Quake launch until CI showed exactly that: ~460 + ~260 s on one
  shard set the run's floor.  It now saves and restores the launch itself.)

### What CI runs

| Trigger | Runs |
|---|---|
| PR / push (`tests.yml`) | `static` (headless build, core layering, tier check, golden distinctness), `unit` (native and wasm32 unit suites) and four `integration` shards run in parallel; each shard runs its quarter of **all three tiers** as one longest-first `-j` pool (a fixture producer and its consumers always share a shard; the extended tier is on the PR gate while the integration-test rework settles; in the pool it costs CPU on whichever shard it lands rather than a serial half hour). `contracts` then checks coverage (both tiers) and the perf baselines over the union of the shard logs, and puts coverage, milestone rows, skipped tests, per-row spends and the slowest tests into the step summary. The web2 frontend runs beside them as `ui` (svelte-check, lint, Vite and WASM builds, Vitest, prod-smoke), `ui-gallery` (component screenshots) and three `ui-e2e` jobs, each running the spec files `scripts/e2e-shard.py` assigns it (a duration-weighted split from `tests/e2e/e2e-weights.json`, regenerated with `scripts/gen-e2e-weights.py` from the JSON reports CI uploads) in two passes: the `parallel` project with three workers, then the `serial` project (pacing, jitter and real-time media specs) alone. Integration shards print each test's output as one block (`--output-sync=target`), and each e2e shard uploads a JSON report with per-test durations. |
| Nightly 03:20 UTC (`nightly.yml`) | the extended tier in `KEEP_GOING=1` mode (so one red row does not truncate the report), plus Valgrind rescoped to the unit tier, one short run per PowerPC family the unit tier does not boot (`pdm-rom-ladder`, `tnt-pci-slots`) and one 68k boot, with `PERF_FLOORS=off`. Failure uploads `tests/integration/test-results/**`. |

Valgrind is deliberately *not* a full sweep: at its 20–50× slowdown over
billions of guest cycles, `test-valgrind` across every test cannot run to
completion, and the throughput floors would fail by construction — hence
`PERF_FLOORS=off` for those runs.

### Coverage and performance contracts

Two committed files gate the suite as a whole, and they have deliberately
different semantics:

| File | Represents | Regenerated? |
|---|---|---|
| `tests/integration/matrix-targets.json` | the **declared** cells the suite must cover, and which directory owes each | **No — hand-authored.** Generating it from a run would make it agree with whatever the run covered |
| `tests/integration/perf-baselines.json` | the observed per-row instruction spend, gated on drift | Yes — `scripts/gen-baselines.py <logs>`, reviewed as a diff in the PR that changed timing |

Rows emit `@@COV` records (read from the live machine) and `@@PERF`
records. Diff achieved against declared:

```bash
make -C tests/integration test TIER=matrix 2>&1 | tee run.log
python3 scripts/test-matrix.py --check --tier=matrix run.log  # 1 on a coverage regression
python3 scripts/test-matrix.py --perf run.log                # 1 if a row drifted out of band
python3 scripts/test-matrix.py --from-results run.log        # render what a run covered
```

A declared cell that was not covered fails; a covered cell nobody
declared is a warning telling you to claim it. Four cases warn instead of
failing, because none of them means coverage regressed: the owing suite
directory does not exist yet (derived from the filesystem, so it cannot
be faked), the cell is owed by a test in a **different tier** than the
one being checked (`--tier=` reads each owner's `TEST_TIER` from its own
config.mk), the cell is `blocked` by a known emulator defect (the
cell-level twin of a milestone row), or it is `media_gated` and its row
skipped because private test data is absent.

`--perf` gates each row's guest instruction spend against
`perf-baselines.json` within its tolerance band (±20% by default). The
spend is deterministic per build, so a band violation is a real change,
not flake — absorb legitimate ones with a reviewed
`scripts/gen-baselines.py` diff in the PR that caused them.

### Writing a New Integration Test

1. Create `tests/integration/foo/` with `config.mk` and `test.script`.
2. `config.mk` sets `TEST_ROM`, `TEST_ARGS`, `TEST_TIER`, and optionally
   `TEST_SETUP`/`TEST_RUNNER`.
3. `test.script` contains shell commands sent to the headless emulator.
   A boot assertion belongs as a row in its machine's suite, not a new
   directory — new directories are for genuinely new mechanisms.
4. Run: `make -C tests/integration test-foo`

---

## End-to-End Tests (Playwright)

Browser-based tests that exercise the full WASM emulator through the **web2**
UI (`app/web2/`) in Chromium. Specs live under `tests/e2e/web2-specs/`; the
one shared helper is `tests/e2e/helpers/web2-fs.ts`. See
[tests/e2e/README.md](../../tests/e2e/README.md) for the full layout.

> The legacy web UI and its `tests/e2e/specs/**` suite were retired; unique
> coverage moved here or into the headless integration tests above.

### Specs

[tests/e2e/README.md](../../tests/e2e/README.md) lists every spec under
`tests/e2e/web2-specs/` with what it tests; the Hygiene workflow
(`scripts/check-doc-inventories.py`) fails if a spec is missing from it.

Two more configs run separately: `ui-prod-smoke/` (production-bundle boot
smoke, no data) and `playwright.webkit-local.config.ts`, which runs
`web2-specs/upload.spec.ts` on WebKit (macOS only: Linux WebKitGTK has no
OPFS).

### Running E2E Tests

```bash
make ui2-e2e                        # Build + run the functional web2 suite (alias: make e2e-test)
make ui2-prod-smoke                 # Production-bundle smoke test

# A specific spec
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts url-boot

# Headed mode
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts --headed
```

The config's `webServer` block builds + serves `app/web2/dist` automatically.

### Prerequisites

```bash
cd tests/e2e && npm ci
npx playwright install --with-deps chromium
./scripts/fetch-test-data.sh        # functional specs boot real machines
```

### Baselines

Playwright `toMatchSnapshot` baselines live in `<spec>.ts-snapshots/` beside
the spec. Regenerate with `--update-snapshots`. Raw-framebuffer pixel oracles
live in the headless integration tests, not here.

### Writing a New E2E Spec

1. Add `tests/e2e/web2-specs/foo.spec.ts`.
2. Import `test` from `helpers/test.ts` (Playwright's, with every
   `page.goto()` at max speed: it appends `speed=turbo` unless the URL
   names a speed) and the web2 helpers:
   ```typescript
   import { test, expect } from '../helpers/test';
   import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';
   ```
   A spec whose point is pacing, or that feeds real-time media into the
   guest, keeps web2's default with `test.use({ gsSpeed: null })`, and goes
   in the config's `SERIAL` list if it must not share the machine with
   another running emulator (the `parallel` project runs
   `GS_E2E_WORKERS` specs at once; the default is 1).
3. Drive through the shipped UI (dialog, drag-and-drop); read or call the
   object model with `gsEvalInPage` / `gsCallInPage` (`helpers/web2-eval.ts`,
   see `tests/e2e/README.md`), not by typing into the Terminal.
4. Run: `npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts foo`

### Debugging E2E Failures

- Traces: `npx playwright show-trace tests/e2e/test-results/<test>/trace.zip`
- Screenshots/artifacts land under `tests/e2e/test-results/<test>-<project>/`
- Drive the emulator's shell from a spec via the Terminal console (`tests/e2e/helpers/terminal.ts`: its input is `.console .cm-content`, its output `.console-output`)
