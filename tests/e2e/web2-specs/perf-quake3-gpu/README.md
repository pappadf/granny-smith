# perf-quake3-gpu — Quake III's demo on a real GPU, timed

Runbook for the agent (or person) running this measurement on an **Apple
silicon Mac**.  The spec, `perf-quake3-gpu.spec.ts`, plays Quake III
Arena's built-in demo `four` on the emulated beige G3 with an ATI Rage 128
GL, in the web app, with the Rage 128's 3D drawn by the host GPU through
WebGPU and the PowerPC JIT on — what a user sees in the browser — and
prints how long it took in host and in guest time, with the frame rate.

It is a measurement, not a gate: it is opt-in (`GS_Q3_GPU=1`) and skips on
a software WebGPU adapter (SwiftShader), where it would time SwiftShader.

## Why here, and why not in a container

The headless emulator (the integration rows `g3-rage128-quake3-demo` and
`-jit`) has no WebGPU: it draws the 3D with its software rasteriser, about
70 % of its host time.  Only the browser has the GPU path, so this is an
e2e spec.  Run it **natively on macOS**: Docker on a Mac runs Linux in a VM
without the Apple GPU, so Chromium there falls back to SwiftShader.

## Prerequisites

- macOS on Apple silicon, Node 22, Python 3, git.
- emsdk 6.0.7 (`emcc` on `PATH`): the spec's web server runs `make ui2`,
  which builds the wasm core.  `source /path/to/emsdk/emsdk_env.sh`.
- Access to the private `pappadf/gs-test-data` repository, for the test
  data (below).
- About 1 GB free disk; the browser stages 78 MB of disk images into its
  storage (OPFS).

## Setup

```sh
git clone https://github.com/pappadf/granny-smith && cd granny-smith
git checkout <the branch with this spec>
scripts/fetch-test-data.sh          # tests/data at tests/data-version; extracts the .7z files
make ui2                            # the wasm core and the web bundle: several minutes the first time
cd tests/e2e && npm ci && npx playwright install chromium
```

Build `make ui2` first: the spec's web server runs it too, but gives up
after 240 s, which a first build of the wasm core can exceed.

Check the four inputs exist (the spec skips, naming the missing one,
otherwise):

```
tests/data/roms/pmg3dt-pmg3mt-78f57389.rom                     the G3's ROM (Rev C)
tests/data/roms/rage128-nexus128-103-35b433aa.prom             the Rage 128's expansion ROM
tests/data/systems/mac_os_9_2_1_516mb_g3_rage128_demos.dmg     Mac OS 9.2.1, stripped (UDIF)
tests/data/apps/quake3_1_30_demo_four_cd.iso                   the Quake III 1.30 CD cut to its demo (from .7z)
```

## Run

From `tests/e2e`:

```sh
GS_Q3_GPU=1 npx playwright test -c playwright.web2.config.ts --project=serial perf-quake3-gpu
```

The web server re-runs `make ui2` (a no-op when it is current) and serves
`app/web2/dist`.  Close other heavy applications: this times the machine.

| Variable | Default | Meaning |
|---|---|---|
| `GS_Q3_GPU` | — | `1` to run at all |
| `GS_Q3_MODES` | `paced,turbo` | One demo run per entry, in order: `paced` (real time; the guest's clock follows the wall clock), `fast` / `accelerated` (real-time clock, faster CPU), `turbo` (as fast as possible) |
| `GS_Q3_ENGINE` | `jit` | `jit` (the browser's "JIT + kernel"), `predecoded`, `interpreter` |
| `GS_Q3_RASTER` | `webgpu` | The Rage 128's 3D: `webgpu` (the host GPU), or `sw` (its software rasteriser in the browser: the comparison row) |
| `GS_Q3_TIMEDEMO` | — | `1`: also `timedemo 1` before each run; Quake then draws every frame as fast as it can and prints its own `frames, seconds, fps` (reported as `quake_timedemo`) |
| `GS_Q3_POLL_MS` | `100` | How often the end is polled (the end's resolution) |
| `GS_Q3_HEADED` | — | `1`: a visible browser window (if headless gets no GPU) |
| `GS_Q3_CHANNEL` | — | e.g. `chrome`: the installed Google Chrome instead of Playwright's Chromium |
| `GS_Q3_LABEL` | `unlabeled` | A label carried into the results (machine, commit, setting) |
| `GS_Q3_TIMEOUT_MIN` | `90` | The test's time limit |
| `GS_Q3_ALLOW_SOFTWARE_GPU` | — | `1`: run on SwiftShader anyway (checks the flow, not the GPU) |
| `GS_Q3_KEEP_WATCHDOG` | — | `1`: keep the page's 3 s heartbeat watch (by default the spec lifts it to 600 s, so a stall is ridden out, not fatal) |

**First, confirm the GPU.**  The first `Q3GPU` line names the adapter:

```
Q3GPU adapter {"vendor":"apple","architecture":"metal-3",...}
```

`apple` is the real GPU.  If the test skips with "no hardware WebGPU
adapter", run again with `GS_Q3_HEADED=1`, then with
`GS_Q3_CHANNEL=chrome`.

## How it works

1. Opens the web app, uploads the ROM and the PROM through *Load ROM...*,
   and stages the disk and the CD into OPFS.
2. Boots the G3 (256 MB, the Rage 128 with `raster=webgpu`, 13" monitor on
   the card), turns on the chosen CPU engine, attaches the disk (SCSI 0)
   and the CD (SCSI 3), and runs in turbo.  Waits are counted in guest
   instructions, not seconds.
3. In the Finder, by typing names and Command-O: the CD `Q3A`, its folder
   `Quake3`, the application `Quake3`.  The CD key screen needs no key:
   Tab to ACCEPT, hold Return (Quake reads the mouse as deltas, so clicks
   cannot be aimed); the held Return carries on into Single Player, so
   Escape back to the main menu.  Before each keyboard step a Shift wakes
   the display: in turbo the guest's clock runs ahead of its instructions
   (Ticks advance with host time times the speed), so Energy Saver can
   blank the display between steps, and the key that wakes it is lost.
   The spec checks that Quake3 is the front application (`CurApName`),
   opening it once more if not, before it waits for the CD key screen.
4. Per run: the pacing mode; Quake's console (`~`, ADB key `0x32`);
   `set nextdemo "echo gsdemoend<n>"`; `demo four` typed.
5. **Start:** the scheduler is paused, every counter read, Return queued,
   the scheduler resumed.  The start is exact to the injected key.
6. **End:** when a demo finishes, Quake disconnects and runs its `nextdemo`
   cvar (`CL_DemoCompleted` → `CL_NextDemo`), so `gsdemoend<n>` is printed
   on its console at that moment.  The spec reads Quake's console from
   guest RAM: a ring of 16-bit cells (colour, character), found by the new
   echo of `demo four`.  It polls (every `GS_Q3_POLL_MS`) for the demo's
   last event, " hit the fraglimit", then for the `gsdemoend<n>` line that
   is not the echo of the typed `set` (that one is followed by a quote).
   There it pauses the scheduler and reads the counters again.  The end is
   late by at most one poll plus the pause's round trip.
7. Writes the console text and a screenshot, asserts no file was missing
   (no "could not find" on Quake's console) and that frames were drawn,
   resumes, and does the next run from the main menu.

The demo itself: `demos/four.dm_66`, an eight-player FFA on q3dm6 ("The
Campgrounds"), 63 s, ending when THEINDIGO reaches the fraglimit.  The
timed interval also holds the level load (about 13 s of guest time).

## What to time, and what the numbers mean

Each run prints one `Q3GPU run=<n> mode=... key=value ...` line and adds an
entry to `test-results/perf-quake3-gpu/perf-quake3-gpu.json`:

| Key | Meaning |
|---|---|
| `host_wall_s` | Host wall-clock time from Return to the end (the emulator's monotonic clock) |
| `node_wall_s` | The same, by the test's own clock (a cross-check) |
| `guest_s` | Emulated time: guest CPU cycles over the clock (267.28 MHz) |
| `guest_ticks_s` | Emulated time by Mac OS's 60 Hz Ticks (a cross-check) |
| `speed_x` | `guest_s / host_wall_s`: 1.0 = real time |
| `instructions`, `mips_guest`, `mips_host` | PowerPC instructions retired; per guest second (≈ 267 for this G3) and per host second |
| `jit_pct` | Share of instructions that ran translated |
| `frames`, `fps_guest`, `fps_host` | Frames Quake drew (the card's page flips) per guest second (what the game "sees") and per host second (what you see) |
| `prims3d` | 3D primitives the card was given |
| `gpu_presents`, `gpu_presents_per_s` | Frames the GPU worker presented; per host second (bounded by the display's refresh) |
| `gpu_batches`, `gpu_draws`, `gpu_readback_bands`, `gpu_upload_rows`, `gpu_tex_upload_mb`, `gpu_fallbacks` | The WebGPU takeover's work: readbacks are stalls, fallbacks are batches drawn by the software rasteriser instead |
| `fraglimit_to_end_s` | From the demo's last event to its end, host time |
| `quake_timedemo` | With `GS_Q3_TIMEDEMO=1`: Quake's own `frames, seconds, fps` |
| `missing_files` | Must be 0 |

Suggested set, three times each, reporting the median:

1. `GS_Q3_MODES=paced` — does it hold real time?  `speed_x` ≈ 1.0 and
   `fps_host` ≈ `fps_guest` mean yes; below 1.0, by how much.
2. `GS_Q3_MODES=turbo` — throughput: `speed_x`, `mips_host`, `fps_host`.
3. `GS_Q3_MODES=turbo GS_Q3_ENGINE=predecoded` — the JIT's share of it.
4. `GS_Q3_MODES=turbo GS_Q3_TIMEDEMO=1` — Quake's own benchmark number.
5. `GS_Q3_MODES=turbo GS_Q3_RASTER=sw` — the same in the browser without
   the GPU: what the WebGPU takeover buys.

Compare with the headless rows on the same Mac
(`make integration-test-g3-rage128-quake3-demo` and `-jit` from the repo
root): same demo, same counters, software 3D.  The guest-side numbers
(`instructions`, `frames`, `guest_s`) differ between engines and modes only
through timing; the host-side ones are the measurement.

## Results

`tests/e2e/test-results/perf-quake3-gpu/`:

- `perf-quake3-gpu.json` — every run's numbers;
- `console-run<n>-<mode>.txt` — Quake's console text around the run;
- `1-desktop.png`, `2-cdkey.png` (and `2-finder-retry.png` if the first open missed), `3-menu.png`, `4-end-run<n>-<mode>.png`;
- `browser-console.log` — the page's console (the GPU worker's messages),
  written as it comes;
- `terminal-alert<n>.txt` — the web app's terminal (the emulator's output)
  each time its alert count rose; `terminal-*.txt` when Quake3 would not
  open.

Report the `Q3GPU` lines, the JSON, the adapter line, the Mac model and
macOS version, and the branch and commit.

## Troubleshooting

- **Skipped, "no hardware WebGPU adapter"**: see *First, confirm the GPU*.
- **Stuck before Quake** (`2-cdkey.png` shows the Finder): the Finder
  steps type names; look at `1-desktop.png` for the `Q3A` CD icon.  A
  slow host may need longer waits (the `open(...)` calls).
- **"Quake3 is the front application" fails**: `2-finder-retry.png` shows
  the Finder; a blank screen there is display sleep (see *How it works*,
  step 3).
- **"the console echoes `demo four`" fails**: the console key or typing
  did not reach Quake; look at the end screenshot.
- **The demo never reaches its end** (time limit): `console-run<n>-*.txt`
  says how far it got; `browser-console.log` names WebGPU errors (a lost
  device makes the card fall back to its software rasteriser).
- **Stalls**: the page's heartbeat watch declares the emulator dead after
  3 s without an answer ("The emulator stopped"), which ends a run.  The
  spec lifts it to 600 s in the served bundle (its log says
  `heartbeat watch lifted to 600 s: true`; `false` means the bundle no
  longer matches the pattern, and a stall will end the run).  A stall
  still costs host time: if a run's `host_wall_s` is far off the others,
  that is a finding -- re-run with `GS_Q3_KEEP_WATCHDOG=1` to see it
  happen.  The branch `claude/blissful-albattani-supjyo-q3diag` makes
  stalls non-fatal in the app and adds per-second `perf:` lines
  (`log.perf_every`); merging it in gives far more to look at.
- **"could not be opened, because a disk error occurred", or a Mac OS
  system error after Quake starts**: look at `terminal-alert<n>.txt` (the
  spec saves the terminal at every new alert).  Seen in a Linux container
  (SwiftShader, a slow host): `host file read failed:
  /opfs/upload/q3/quake3_1_30_demo_four_cd.iso ... I/O error (errno 29)` --
  `pread()` on the browser build's OPFS file failing, intermittently (one
  run in three got through), which the card turns into a SCSI MEDIUM ERROR,
  and once a hard-disk write the storage took only part of (`SCSI WRITE:
  storage took 512 of 1536 bytes`; the same write succeeds natively).  It
  is a bug in the browser build's OPFS storage, not in this spec: report
  the terminal files.  A first-try failure to open Quake3 is retried once.
- **The page shows "The browser took the emulator's graphics away"**: the
  browser's GPU process died (seen with SwiftShader in a container:
  `device lost: ... A valid external Instance reference no longer
  exists`).  On the Mac, report it with `browser-console.log`.
- **`missing_files` > 0**: the CD is not the one at `tests/data-version`;
  re-run `scripts/fetch-test-data.sh`.
