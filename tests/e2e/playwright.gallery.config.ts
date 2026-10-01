// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Playwright config for the UI gallery screenshots (gallery/ui-gallery.spec.ts).
//
// The gallery is a development-only page (`?gallery`), so this serves the
// Vite dev server instead of the production dist.  It needs neither the WASM
// build nor any test data: every story renders components against fixture
// state.  Baselines are platform-sensitive (fonts, antialiasing) and are
// recorded only in the CI image (ghcr.io/pappadf/granny-smith-dev); see
// tests/e2e/README.md, "UI gallery screenshots".

import { defineConfig } from '@playwright/test';

const PORT = 18192;

export default defineConfig({
  testDir: './gallery',
  // GS_GALLERY_SNAPSHOTS points the baselines elsewhere (a scratch set for a
  // before/after comparison outside the CI image).
  ...(process.env.GS_GALLERY_SNAPSHOTS
    ? { snapshotPathTemplate: `${process.env.GS_GALLERY_SNAPSHOTS}/{arg}{ext}` }
    : {}),
  timeout: 60_000,
  expect: {
    timeout: 10_000,
    toHaveScreenshot: { maxDiffPixels: 0, animations: 'disabled', caret: 'hide', scale: 'css' },
  },
  fullyParallel: true,
  workers: process.env.CI ? 2 : 4,
  forbidOnly: !!process.env.CI,
  retries: 0,
  reporter: process.env.CI ? 'github' : 'list',
  webServer: {
    command: `npx vite --port ${PORT} --strictPort`,
    cwd: '../../app/web2',
    url: `http://localhost:${PORT}/?gallery`,
    reuseExistingServer: !process.env.CI,
    timeout: 120_000,
  },
  use: {
    baseURL: `http://localhost:${PORT}`,
    headless: true,
    trace: 'retain-on-failure',
    // A fixed device scale and no smooth scrolling keep pixels stable.
    deviceScaleFactor: 1,
    launchOptions: { args: ['--disable-dev-shm-usage', '--font-render-hinting=none'] },
  },
  projects: [{ name: 'chromium', use: { browserName: 'chromium' } }],
});
