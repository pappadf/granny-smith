// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: URL-parameter boot. Replaces the browser-level coverage of the
// legacy configuration.spec.ts URL-param boot path (retired with the legacy
// UI); app/web2 has only a urlMedia *unit* test (urlMedia.parse), nothing
// that drives the real ?rom=… fetch → stage → boot pipeline end-to-end.
//
// urlMedia.ts fetches each media URL over HTTP from the page origin, stages
// it into /tmp, identifies the ROM and boots the first compatible model
// (honouring ?model= when compatible). The Playwright test server serves
// app/web2/dist, so instead of planting fixtures there we intercept the
// media fetch with page.route() and fulfil it from the real ROM on disk —
// only the transport is stubbed; the identify/boot pipeline runs for real.

import { test, expect, type Page } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';

const PLUS_ROM = path.resolve(__dirname, '../../data/roms/plus-v3-4d1f8172.rom');

// Serve the ROM bytes from disk for any request to the sentinel path the
// URL params point at. Must be registered before goto so the in-page
// fetch() is intercepted.
async function routeRom(page: Page, urlSuffix: string, file: string): Promise<void> {
  const body = fs.readFileSync(file);
  await page.route(`**/${urlSuffix}`, (route) =>
    route.fulfill({
      status: 200,
      contentType: 'application/octet-stream',
      body,
    }),
  );
}

test('?rom= boots the identified machine without going through Welcome', async ({ page }) => {
  test.setTimeout(90_000);
  await routeRom(page, 'url-plus.rom', PLUS_ROM);

  // Navigate straight into a URL-param boot. No Welcome interaction.
  await page.goto('/index.html?rom=url-plus.rom&model=plus');
  await page.waitForFunction(() => (window as { __gsReady?: boolean }).__gsReady === true, undefined, {
    timeout: 60_000,
  });

  // The ROM is fetched, identified, and the Plus boots straight to a running
  // machine — the Welcome layer never blocks.
  await expect(page.locator('.toast .msg').filter({ hasText: 'Booted plus from URL parameters' }))
    .toBeVisible({ timeout: 60_000 });
  await expect(page.locator('.welcome-layer')).toHaveCount(0);
  await expect(page.locator('.gs-statusbar .sb-state .label')).toHaveText('Running', {
    timeout: 15_000,
  });

  // The Plus framebuffer is 512x342 — confirms the machine really came up on
  // the model the URL selected (not a default/stub).
  await expect
    .poll(() => page.locator('#screen').evaluate((el) => (el as HTMLCanvasElement).width), {
      timeout: 15_000,
    })
    .toBe(512);
});
