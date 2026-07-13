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
├── playwright.webkit-local.config.ts# Local WebKit upload/OPFS checks (webkit-local/)
├── package.json                     # Node dependencies (Playwright)
├── tsconfig.json
├── test_server.py                   # COOP/COEP-enabled static server (web2 + webkit configs)
├── scripts/prod-smoke-server.mjs    # Subpath server without COI headers (prod-smoke)
│
├── web2-specs/                      # Main functional suite (playwright.web2.config.ts)
│   ├── checkpoint-resume.spec.ts    # Checkpoint save → reload → resume (+ SE/30 profile restore)
│   ├── display-card-config.spec.ts  # New Machine dialog: card-by-name video config
│   ├── display-drop.spec.ts         # Drag-and-drop onto the Display (ROM/floppy/checkpoint)
│   ├── filesystem-tab.spec.ts       # Filesystem tab: descend image, copy/move/rename/unpack
│   ├── iicx-video-modes.spec.ts     # Post-shader WebGL canvas baselines (per monitor × depth)
│   ├── iifx-aux3-realtime.spec.ts   # A/UX 3.0.1 boot to login under the real RAF scheduler
│   ├── lisa-xenix-profile.spec.ts   # Lisa/XL ProFile-vs-SCSI config + boot
│   └── url-boot.spec.ts             # ?rom=… URL-parameter boot
│
├── ui-prod-smoke/                   # Production-bundle smoke (playwright.prod-smoke.config.ts)
│   └── prod-smoke.spec.ts           # dist/ on a subpath w/o COI headers reaches __gsReady
│
├── webkit-local/                    # Local WebKit only (playwright.webkit-local.config.ts)
│   └── upload.spec.ts               # OPFS streaming upload (Safari createWritable regression)
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
