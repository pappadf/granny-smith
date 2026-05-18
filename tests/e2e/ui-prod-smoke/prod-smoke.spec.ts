// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Production-bundle smoke test for app/web2/dist/.
//
// Boots the same artifact that's published to GitHub Pages, on a
// server that mounts it at a SUBPATH (not root) and does NOT inject
// COOP/COEP headers — forcing the bundle to bring up cross-origin
// isolation through its own coi-serviceworker registration.
//
// This is the deploy environment that v0.4.0–v0.4.2 each broke
// without us catching it locally; `make run` masks all three failures
// by serving from root with COI headers pre-set.

import { test, expect } from '@playwright/test';

test('production bundle bootstraps on a subpath via the COI service worker', async ({ page }) => {
  const consoleErrors: string[] = [];
  page.on('console', (msg) => {
    if (msg.type() === 'error') consoleErrors.push(msg.text());
  });

  const failedRequests: string[] = [];
  page.on('response', (resp) => {
    const status = resp.status();
    if (status >= 400 && status < 600) {
      failedRequests.push(`${status} ${resp.url()}`);
    }
  });

  // First load: the page registers the SW and (because the server
  // doesn't send COI headers) the SW will inject them and the page
  // reloads. After the reload, crossOriginIsolated is true and the
  // WASM module can boot. main.ts sets window.__gsReady = true once
  // the bridge has completed bootstrap.
  await page.goto('/sub/path/');

  await page.waitForFunction(
    () => (window as unknown as { __gsReady?: boolean }).__gsReady === true,
    null,
    { timeout: 15_000 },
  );

  const isolated = await page.evaluate(() => crossOriginIsolated);
  expect(isolated, 'crossOriginIsolated should be true after SW activation').toBe(true);

  expect(consoleErrors, `console errors:\n${consoleErrors.join('\n')}`).toHaveLength(0);

  expect(failedRequests, `4xx/5xx network responses:\n${failedRequests.join('\n')}`).toHaveLength(0);
});
