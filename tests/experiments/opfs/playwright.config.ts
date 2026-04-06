import { defineConfig, devices } from '@playwright/test';
import path from 'path';

const expDir = path.resolve(__dirname);

export default defineConfig({
  testDir: expDir,
  testMatch: '*.spec.ts',
  timeout: 60_000,
  expect: { timeout: 10_000 },
  fullyParallel: false,  // Run sequentially — experiments share OPFS state
  retries: 0,
  workers: 1,
  reporter: 'list',
  use: {
    baseURL: 'http://localhost:18090',
    headless: true,
  },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
  ],
  webServer: {
    command: `python3 ${expDir}/serve.py 18090`,
    port: 18090,
    reuseExistingServer: true,
    timeout: 30_000,
  },
});
