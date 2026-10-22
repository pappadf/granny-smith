// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: browser shortcuts while the screen has the keyboard (#352).
//
// The core's key callbacks run on the emulator pthread, so their return value
// never reaches preventDefault(); app/web2 lib/screenKeys.ts suppresses the
// defaults on the main thread while #screen holds the pointer lock, and asks
// before a live machine's page unloads.  This boots a Plus from the URL,
// grabs the screen with a click and checks both, in the shipped page.

import { test, expect, type Page } from '../helpers/test';
import * as fs from 'node:fs';
import * as path from 'node:path';

const PLUS_ROM = path.resolve(__dirname, '../../data/roms/plus-v3-4d1f8172.rom');

// Boot a Plus from ?rom=, the ROM served from disk, and wait until it runs.
async function bootPlus(page: Page): Promise<void> {
  const body = fs.readFileSync(PLUS_ROM);
  await page.route('**/keys-plus.rom', (route) =>
    route.fulfill({ status: 200, contentType: 'application/octet-stream', body }),
  );
  await page.goto('/index.html?rom=keys-plus.rom&model=plus');
  await expect(page.locator('.gs-statusbar .sb-state .label')).toHaveText('Running', {
    timeout: 60_000,
  });
}

// Click the screen and wait for it to hold the pointer lock.
async function grabScreen(page: Page): Promise<void> {
  await page.locator('#screen').click();
  await page.waitForFunction(() => document.pointerLockElement?.id === 'screen', undefined, {
    timeout: 10_000,
  });
}

test('keys the grabbed screen gets have their browser default prevented', async ({ page }) => {
  test.setTimeout(90_000);
  await bootPlus(page);
  await grabScreen(page);

  // Record, after every other listener, whether each key's default was
  // prevented; a marker on window shows the page was not reloaded.
  await page.evaluate(() => {
    const w = window as unknown as { __keys: string[]; __marker: boolean };
    w.__keys = [];
    w.__marker = true;
    window.addEventListener('keydown', (e) => w.__keys.push(`${e.code}:${e.defaultPrevented}`));
  });
  await page.keyboard.press('F5');
  await page.keyboard.press('Control+d');
  await page.keyboard.press('a');

  const keys = await page.evaluate(() => (window as unknown as { __keys: string[] }).__keys);
  expect(keys).toEqual(['F5:true', 'ControlLeft:true', 'KeyD:true', 'KeyA:true']);
  expect(await page.evaluate(() => (window as unknown as { __marker?: boolean }).__marker)).toBe(
    true,
  );
});

test('reloading a page with a running machine asks first', async ({ page }) => {
  test.setTimeout(90_000);
  await bootPlus(page);
  // The click is the user activation Chromium needs before it asks at all.
  await grabScreen(page);
  await page.evaluate(() => document.exitPointerLock());

  const dialogs: string[] = [];
  page.on('dialog', (d) => {
    dialogs.push(d.type());
    void d.accept();
  });
  await page.reload();
  expect(dialogs).toEqual(['beforeunload']);
});
