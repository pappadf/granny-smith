// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Playwright config for the web2 (Svelte) Filesystem-tab e2e.
//
// Unlike the prod-smoke config (which only checks the production bundle
// boots), this exercises real OPFS filesystem operations end-to-end, so it
// needs the worker / WasmFS bridge fully live. It builds the web2 dist and
// serves it on localhost with the COOP/COEP headers SharedArrayBuffer needs.
// No ROM / disk-image globalSetup is required: the Filesystem tab, the
// object-model shell and OPFS are all live without an emulated machine.

import { defineConfig } from '@playwright/test';

const PORT = 18090;

// Specs that must not share the machine with another running emulator: they
// measure pacing, latency or wall-clock jitter, or feed real-time media into
// the guest.  They run in the `serial` project, one at a time; everything else
// runs in `parallel`, GS_E2E_WORKERS at once (each test has its own browser
// context, so its own OPFS).  CI runs the two projects one after the other
// (`--project=parallel`, then `--project=serial`), so the serial specs
// never overlap anything.
const SERIAL = [
  'iifx-aux3-realtime.spec.ts',
  'scheduler-accelerated.spec.ts',
  'perf-bench.spec.ts',
  'av-camera.spec.ts',
  'av-microphone.spec.ts',
  'av-sound-record.spec.ts',
  'av-speech-recognition.spec.ts',
  'checkpoint-stall.spec.ts',
  'copy-jitter.spec.ts',
  'highlight.spec.ts',
];
const WORKERS = Number(process.env.GS_E2E_WORKERS ?? 1);

export default defineConfig({
  testDir: './web2-specs',
  // The baselines are named for the single `chromium` project this config
  // had before it split into `parallel` and `serial`; both keep that name.
  snapshotPathTemplate: '{snapshotDir}/{testFileDir}/{testFileName}-snapshots/{arg}-chromium{-snapshotSuffix}{ext}',
  timeout: 120_000,
  expect: { timeout: 10_000 },
  fullyParallel: false,
  workers: WORKERS,
  forbidOnly: !!process.env.CI,
  retries: 0,
  // The JSON report keeps per-test durations (the github reporter prints
  // none); CI uploads it.
  reporter: process.env.CI
    ? [['github'], ['json', { outputFile: process.env.GS_E2E_JSON ?? 'test-results/report.json' }]]
    : 'list',
  webServer: {
    // Build the web2 bundle (`make ui2` brings the wasm core up to date
    // first — a no-op that keeps the build ID when it is fresh — and copies
    // it into the dist), then serve it with the cross-origin-isolation
    // headers the worker needs.
    // `cwd` below starts this in tests/e2e, so `cd ../..` reaches the repo
    // root. Do NOT use `git rev-parse --show-toplevel` — CI containers trip
    // git's "dubious ownership" guard, emptying the $(...) so `make ui2` runs
    // in the wrong directory ("No rule to make target 'ui2'").
    command: `bash -c 'cd ../.. && make ui2 && exec python3 tests/e2e/test_server.py --root app/web2/dist --port ${PORT}'`,
    cwd: __dirname,
    url: `http://localhost:${PORT}/index.html`,
    reuseExistingServer: !process.env.CI,
    timeout: 240_000,
  },
  use: {
    baseURL: `http://localhost:${PORT}`,
    headless: true,
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
    launchOptions: {
      // Headless Chromium has no GPU; web2's WebGL2 probe must pass for the
      // app to mount, so force software WebGL via swiftshader (mirrors the
      // prod-smoke config and scripts/ui2-diag.mjs).
      args: [
        '--use-gl=angle',
        '--use-angle=swiftshader-webgl',
        '--ignore-gpu-blocklist',
        // Containers (Codespaces/devcontainers) mount a 64 MB /dev/shm;
        // Chromium's renderer shared memory must fall back to /tmp or the
        // tab crashes under memory-heavy specs (large OPFS uploads, wasm).
        '--disable-dev-shm-usage',
      ],
    },
  },
  projects: [
    { name: 'parallel', use: { browserName: 'chromium' }, testIgnore: SERIAL },
    { name: 'serial', use: { browserName: 'chromium' }, testMatch: SERIAL, workers: 1 },
  ],
});
