# Playwright End-to-End Tests

Browser-based end-to-end tests for the **web2** Granny Smith UI (Svelte 5 +
Vite, `app/web2/`), driven through the shipped interface — the New Machine
dialog, the Terminal panel, the Filesystem tab, and drag-and-drop — against a
live WASM worker.

All Node dev dependencies (Playwright) live here to keep the repository root
focused on the C/WebAssembly sources.

> The legacy web UI (`app/web-legacy/`) and its Playwright suite were retired;
> the coverage that was unique to it now lives either here or in the headless
> integration tests (`tests/integration/`). See the project history for the
> parity work.

## Directory structure

```
tests/e2e/
├── playwright.web2.config.ts        # Main functional config (testDir → web2-specs/)
├── playwright.prod-smoke.config.ts  # Production-bundle boot smoke (ui-prod-smoke/)
├── playwright.webkit-local.config.ts# web2-specs/upload.spec.ts on WebKit (macOS)
├── playwright.gallery.config.ts     # UI gallery screenshots (gallery/), Vite dev server
├── package.json                     # Node dependencies (Playwright)
├── tsconfig.json
├── test_server.py                   # COOP/COEP-enabled static server (web2 + webkit configs)
├── scripts/prod-smoke-server.mjs    # Subpath server without COI headers (prod-smoke)
│
├── web2-specs/                      # Main functional suite (playwright.web2.config.ts)
│   ├── ans-bitblt-repro.spec.ts         # Reproduction (REPRO_E28=1): NT GUI Setup on the ANS 500 drawn in the browser
│   ├── app-states.spec.ts               # The workbench in fixed states, screenshotted in every skin
│   ├── av-boot-no-slots.spec.ts         # A slotless model (q660av) boots after a carded one in the same session
│   ├── av-camera.spec.ts                # AV video-in against Chromium's fake camera
│   ├── av-microphone.spec.ts            # Browser mic → shared-heap ring → guest RAM (no OS)
│   ├── av-sound-record.spec.ts          # Browser mic → the guest's own Sound cdev, record + play
│   ├── av-speech-recognition.spec.ts    # PlainTalk recognition from the browser mic
│   ├── checkpoint-resume.spec.ts        # Checkpoint save → reload → resume (+ SE/30 profile restore)
│   ├── checkpoint-stall.spec.ts         # Measurement: request round trip across the background checkpoint (VITE_GS_MEASURE=1)
│   ├── command-browser.spec.ts          # Terminal command browser: dividers, usage; follows the console and writes to it
│   ├── console.spec.ts                  # Terminal console: value/error entries, Copy as commands → paste → one job
│   ├── debug-panel.spec.ts              # Debug view on a live machine: register edit, breakpoints, repaint while paused
│   ├── display-card-config.spec.ts      # New Machine dialog: card-by-name video config
│   ├── compact-import.spec.ts           # 2 GiB disk via HD=/zip/local file stored as UDIF; origin usage never grows past it, zip heap bounded, cancel/out-of-quota leave nothing
│   ├── copy-jitter.spec.ts              # Measurement: request round trip while a 192 MB files.cp runs (VITE_GS_MEASURE=1)
│   ├── display-drop.spec.ts             # Drag-and-drop onto the Display (ROM/floppy/checkpoint)
│   ├── fd-duplicate-name.spec.ts        # Duplicate floppy names in the image library
│   ├── filesystem-tab.spec.ts           # Filesystem tab: descend image, copy/move/rename/unpack
│   ├── host-state-across-machines.spec.ts # ?speed= reaches the core before a boot; the screen follows each machine built
│   ├── highlight.spec.ts                # Console highlighting: unknown path segments, enums, entry colours; shell.highlight p95 latency under turbo
│   ├── download-transfer.spec.ts        # A core download reaches the page in chunks through a transfer buffer, acked one by one
│   ├── iicx-video-modes.spec.ts         # Post-shader WebGL canvas baselines (per monitor × depth)
│   ├── iifx-aux3-realtime.spec.ts       # A/UX 3.0.1 boot to login under the real RAF scheduler
│   ├── imagewriter-print.spec.ts        # An ImageWriter job, fed to the printer, opens in the print viewer
│   ├── laserwriter-print.spec.ts        # LaserWriter print from System 6 ends as a PDF download (platen worker)
│   ├── lisa-xenix-profile.spec.ts       # Lisa/XL ProFile-vs-SCSI config + boot
│   ├── machine-restart.spec.ts          # Restart power-cycles the machine; the attached disk survives, same open instance
│   ├── panel-tabs-overflow.spec.ts      # Narrow panel: tabs overflow into a » menu, header actions fold into ⋯ (every skin)
│   ├── pci-prom-ingest.spec.ts          # A 9500 configured on an uploaded PCI display card; the .prom survives a reload
│   ├── pdm-double-boot.spec.ts          # pm6100 + Mac OS 8.1 boots exactly once (PRAM seeding), also on a reused image
│   ├── perf-bench.spec.ts               # Accelerated + turbo throughput (tracked numbers)
│   ├── rom-upload-identity.spec.ts      # A ROM is stored by content id; a damaged dump of it is refused, not stored
│   ├── rom-upload-listing.spec.ts       # A Welcome-page ROM upload shows up in an already-open Filesystem tab
│   ├── scheduler-accelerated.spec.ts    # Accelerated mode: faster CPU, real-time timebase
│   ├── shell-prompt.spec.ts             # Terminal: prompt state, Tab completion, history across reloads, scrollback, paste
│   ├── system-edit.spec.ts              # SYSTEM tab: edit machine.cpu.d0 (literal / expression / error), echo, Copy path
│   ├── terminal-jobs.spec.ts            # Terminal lines as jobs: a run waits, a runaway loop costs nothing, Ctrl-C semantics
│   ├── upload.spec.ts                   # Upload picker: streamed staging through the core (Safari regression)
│   ├── url-archive-boot.spec.ts         # ?ROM=…zip/member, archive.org links fetched as given, bare-volume HD boot
│   ├── url-boot.spec.ts                 # ?rom=… URL-parameter boot
│   ├── voodoo2-thread.spec.ts           # Voodoo2 raster on a second Web Worker; LFB/counter fences
│   ├── voodoo2-webgpu-fallback.spec.ts  # voodoo2_webgpu without WebGPU falls back to the thread backend, and says so
│   ├── voodoo2-webgpu.spec.ts           # Voodoo2 WebGPU takeover: engagement, exact coverage, fallback
│   └── vrom-offer-ingest.spec.ts        # Mid-session vROM upload is offered to "(auto)"
│
├── gallery/                         # UI gallery screenshots (playwright.gallery.config.ts)
│   └── ui-gallery.spec.ts           # Every story × variant × skin, pixel-compared with baselines
│
├── ui-prod-smoke/                   # Production-bundle smoke (playwright.prod-smoke.config.ts)
│   └── prod-smoke.spec.ts           # dist/ on a subpath w/o COI headers reaches __gsReady
│
├── helpers/
│   └── web2-fs.ts                   # gotoWeb2, OPFS staging, tree/file drag helpers
│
└── test-results/                    # Generated: traces, screenshots, reports
```

## Running tests

From the repository root (recommended):

```bash
# The functional web2 suite (Makefile)
make ui2-e2e            # or: make e2e-test  (alias)

# The production-bundle smoke test
make ui2-prod-smoke

# The UI gallery screenshots (no WASM, no test data)
make ui2-gallery

# Via npx directly
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts

# A single spec
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts url-boot

# Headed (watch the browser)
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts --headed
```

Each config's `webServer` block builds `app/web2/dist` (`make ui2`) and serves
it with the COOP/COEP headers `SharedArrayBuffer` needs — no manual server step.
`make ui2` expects the WASM (`make`) to have been built already.

## Prerequisites

Most functional specs boot real machines and need proprietary test data (ROMs,
disk images) fetched via `scripts/fetch-test-data.sh` — see
[docs/guide/TEST_DATA.md](../../docs/guide/TEST_DATA.md). The prod-smoke test
needs no data.

Install Playwright + Chromium:

```bash
cd tests/e2e && npm ci
npx playwright install --with-deps chromium
```

## Baselines & snapshots

Playwright `toMatchSnapshot` baselines live in a `<spec>.ts-snapshots/`
directory beside the spec (e.g. `web2-specs/iicx-video-modes.spec.ts-snapshots/`).
Regenerate with `--update-snapshots`:

```bash
npx --prefix tests/e2e playwright test --config=tests/e2e/playwright.web2.config.ts \
    iicx-video-modes --update-snapshots
```

### UI screenshots

Two sets pin the look of the UI itself:

- `gallery/ui-gallery.spec.ts` opens the development-only UI gallery
  (`?gallery` on the Vite dev server, `app/web2/src/gallery/`) once per story,
  variant and skin, and compares the page with its baseline at
  `maxDiffPixels: 0`. The baselines are `<story>-<variant>-<skin>`. The
  stories render components against fixture state, so
  this needs neither the WASM build nor test data, and runs in every CI run
  (`make ui2-gallery`). Opened without `&story=`, the gallery is an index
  with a skin / reduced-motion toolbar, a token table (`&view=tokens`)
  and a coverage list (`&view=coverage`) of which story shows each UI element.
- `web2-specs/app-states.spec.ts` screenshots the real workbench (welcome,
  every panel tab, the configuration form, toasts of each severity, the
  print dialog, a URL boot, the Debug view of a paused Plus) with the text that changes on its own masked. It runs with
  the functional suite (`make ui2-e2e`, needs test data).

Both allow a per-pixel colour difference of `threshold: 0.05` in every
skin, still with no pixel beyond it: anti-aliased edges (even a button's
rounded corners, or a control's edge over Midnight's translucent glass),
gradients and translucency come out up to about ten levels apart depending
on Chromium's compositing history (the same page, shot twice, can differ),
which an exact comparison turns into flakes. A real colour change, such as
a text colour moving by a step, is well beyond it.

`web2-specs/appearance.spec.ts` checks the skin plumbing without
screenshots: a persisted skin is on the page before any of the app's
JavaScript runs, and a skin switch restyles the console input at runtime.

A styling change that claims to change nothing must pass both unchanged. An
intentional change regenerates the affected baselines and lists them in its PR.

Both sets are font- and antialiasing-sensitive, so their baselines are
recorded **in the CI image only** (`ghcr.io/pappadf/granny-smith-dev`, with
Playwright's Chromium dependencies installed as CI installs them):

```bash
docker run --rm -v "$PWD":"$PWD" -w "$PWD/tests/e2e" <ci-image-with-chromium-deps> \
    npx playwright test --config=playwright.gallery.config.ts --update-snapshots
```

`GS_GALLERY_SNAPSHOTS=<dir>` points the gallery's baselines at a scratch
directory: record a set before a change and compare against it after.

Framebuffer-level pixel oracles (raw `screen.save` PNGs, monitor × depth
matrices, boot baselines) live in the headless integration tests
(`tests/integration/`); the web2 specs pin the *post-shader canvas* and UI
behaviour that only a browser exercises.

## Driving the emulator from a spec

web2 has no `window.gsEval`. The typed object-model path from a test is the
**Terminal panel**: click `.xterm`, type a shell line (`machine.cpu.pc`,
`scheduler.run N`, `debug.breakpoints.add(…)`), and read results back from
`.xterm-rows`. The machine auto-runs after boot, so `scheduler.stop` before any
bounded `scheduler.run`. See `helpers/web2-fs.ts` and the existing specs for the
OPFS-staging and drag-gesture patterns (only the HTML5 drag *gesture* is
synthesised; the handlers, worker, and OPFS run for real).

## Notes

- CI runs the functional suite in the `ui` job (`make ui2-e2e`, gated on test
  data) plus the always-on prod-smoke; see `.github/workflows/tests.yml`.
- Devcontainers mount a small `/dev/shm`; the web2 config passes
  `--disable-dev-shm-usage` so the renderer doesn't crash under memory-heavy
  specs.
