// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { defineConfig, devices } from '@playwright/test';

export default defineConfig({
  testDir: './specs',
  // Explicitly set output directory for all Playwright artifacts so VS Code extension can surface them
  outputDir: 'test-results',
  timeout: 60_000,
  expect: { timeout: 5_000 },
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 0,
  workers: (() => {
    const env = process.env.PWTEST_WORKERS ? parseInt(process.env.PWTEST_WORKERS, 10) : undefined;
    if (env && !Number.isNaN(env)) return Math.min(2, Math.max(1, env));
    return 2;
  })(),
  reporter: [
    [process.env.CI ? 'github' : 'list'],
    ['json', { outputFile: 'test-results/report.json' }],
    ['junit', { outputFile: 'test-results/junit.xml' }]
  ],
  globalSetup: require.resolve('./global-setup'),
  globalTeardown: require.resolve('./global-teardown'),
  use: {
    baseURL: 'http://localhost:18080',
    viewport: { width: 1100, height: 1800 },
    deviceScaleFactor: 1,
    trace: 'retain-on-failure',
    // Video recording is off by default to save CPU (ffmpeg). Enable with PWTEST_VIDEO=1
    video: process.env.PWTEST_VIDEO ? 'retain-on-failure' : 'off',
    screenshot: 'only-on-failure',
    headless: true
  },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } }
  ]
});
