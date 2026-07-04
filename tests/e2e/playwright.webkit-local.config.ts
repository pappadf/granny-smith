// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Reproduces the Safari/WebKit upload failure against the LOCAL build. The
// local server (test_server.py) sends real COOP/COEP headers, so WebKit is
// cross-origin isolated and the WASM module boots without needing the
// coi-serviceworker — which lets the test reach the upload path and exercise
// Safari's OPFS behavior directly.

import { defineConfig } from '@playwright/test';

const PORT = 18092;

export default defineConfig({
  testDir: './webkit-local',
  testMatch: '*.spec.ts',
  timeout: 120_000,
  expect: { timeout: 15_000 },
  fullyParallel: false,
  workers: 1,
  retries: 0,
  reporter: process.env.CI ? 'github' : 'list',
  webServer: {
    command: `bash -c 'cd "$(git rev-parse --show-toplevel)" && make ui2 && exec python3 tests/e2e/test_server.py --root app/web2/dist --port ${PORT}'`,
    url: `http://localhost:${PORT}/index.html`,
    reuseExistingServer: !process.env.CI,
    timeout: 240_000,
  },
  use: {
    baseURL: `http://localhost:${PORT}`,
    headless: true,
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
  },
  projects: [
    // Chromium regression guard: before the fix the worker-side storage.cp
    // couldn't see the main-thread staging write and the upload stranded in
    // /opfs/upload (3/3 fail); after the /tmp-staging fix it PASSES (3/3).
    {
      name: 'chromium',
      use: {
        browserName: 'chromium',
        launchOptions: {
          args: ['--use-gl=angle', '--use-angle=swiftshader-webgl', '--ignore-gpu-blocklist'],
        },
      },
    },
    // WebKit is the Safari engine. Its OPFS does not reliably support main-thread
    // FileSystemFileHandle.createWritable() (throws UnknownError) — the exact call
    // bus/opfs.ts::writeToOPFS uses to stage uploads → FAILS on the bug. NOTE:
    // Playwright's Linux WebKitGTK has NO OPFS at all (navigator.storage.getDirectory
    // is undefined), so the test SKIPS there; run on macOS (real Safari / Playwright's
    // macOS WebKit) to see it go red.
    { name: 'webkit', use: { browserName: 'webkit' } },
  ],
});
