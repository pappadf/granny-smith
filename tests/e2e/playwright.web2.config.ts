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

export default defineConfig({
  testDir: './web2-specs',
  timeout: 120_000,
  expect: { timeout: 10_000 },
  fullyParallel: false,
  workers: 1,
  forbidOnly: !!process.env.CI,
  retries: 0,
  reporter: process.env.CI ? 'github' : 'list',
  webServer: {
    // Build the web2 bundle (copies the freshly-built wasm into the dist),
    // then serve it with the cross-origin-isolation headers the worker needs.
    // `make ui2` does not rebuild the wasm — it expects `make` to have
    // produced build/main.{mjs,wasm} already.
    command: `bash -c 'cd "$(git rev-parse --show-toplevel)" && make ui2 && exec python3 tests/e2e/test_server.py --root app/web2/dist --port ${PORT}'`,
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
      args: ['--use-gl=angle', '--use-angle=swiftshader-webgl', '--ignore-gpu-blocklist'],
    },
  },
  projects: [{ name: 'chromium', use: { browserName: 'chromium' } }],
});
