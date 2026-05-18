// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Standalone Playwright config for the prod-smoke test under
// tests/e2e/ui-prod-smoke/. Deliberately does NOT use the main e2e
// config's globalSetup — the prod-smoke test doesn't need ROM /
// disk-image fixtures and we want it to be runnable from a clean
// checkout (CI included).

import { defineConfig } from '@playwright/test';

const PORT = 18181;
const SUBPATH = '/sub/path/';
// Use a loopback IP that's NOT in the bundle's isLocalhost allowlist
// (localhost / 127.0.0.1 / empty), so the COI service worker actually
// registers — exactly what happens on a real deploy. Without this,
// the bundle skips SW registration, crossOriginIsolated stays false,
// and SharedArrayBuffer is unavailable.
const HOST = '127.0.0.2';

export default defineConfig({
  testDir: './ui-prod-smoke',
  testMatch: 'prod-smoke.spec.ts',
  timeout: 60_000,
  expect: { timeout: 5_000 },
  fullyParallel: false,
  workers: 1,
  forbidOnly: !!process.env.CI,
  retries: 0,
  reporter: process.env.CI ? 'github' : 'list',
  webServer: {
    command: `node scripts/prod-smoke-server.mjs --port=${PORT} --subpath=${SUBPATH}`,
    cwd: __dirname,
    url: `http://${HOST}:${PORT}${SUBPATH}`,
    reuseExistingServer: !process.env.CI,
    timeout: 20_000,
  },
  use: {
    baseURL: `http://${HOST}:${PORT}`,
    headless: true,
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
  },
  projects: [
    {
      name: 'chromium',
      use: {
        browserName: 'chromium',
        // Headless Chromium has no GPU by default — force software
        // WebGL via swiftshader so the new UI's `checkWebGL2Available`
        // probe succeeds. Mirrors the args scripts/ui2-diag.mjs uses.
        launchOptions: {
          args: ['--use-gl=angle', '--use-angle=swiftshader-webgl', '--ignore-gpu-blocklist'],
        },
      },
    },
  ],
});
