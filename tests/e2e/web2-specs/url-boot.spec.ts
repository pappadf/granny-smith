// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: URL-parameter boot. Replaces the browser-level coverage of the
// legacy configuration.spec.ts URL-param boot path (retired with the legacy
// UI); app/web2 has only a urlMedia *unit* test (urlMedia.parse), nothing
// that drives the real ?rom=… fetch → stage → boot pipeline end-to-end.
//
// urlMedia.ts fetches each media URL over HTTP from the page origin, stages
// it in the OPFS scratch area, identifies the ROM and boots the first
// compatible model (honouring ?model= when compatible). The Playwright test server serves
// app/web2/dist, so instead of planting fixtures there we intercept the
// media fetch with page.route() and fulfil it from the real ROM on disk —
// only the transport is stubbed; the identify/boot pipeline runs for real.

import { test, expect, type Page } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { gsEvalInPage } from '../helpers/web2-eval';

const PLUS_ROM = path.resolve(__dirname, '../../data/roms/plus-v3-4d1f8172.rom');
const SYSTEM_FD = path.resolve(__dirname, '../../data/systems/System_6_0_8.dsk');
const IICX_ROM = path.resolve(__dirname, '../../data/roms/iix-iicx-se30-97221136.rom');
// The declaration ROM of the IIcx's slot-$9 default card (mdc_8_24).
const JMFB_VROM = path.resolve(__dirname, '../../data/roms/mdc-8-24-revb-d1629664.vrom');

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

// A page opened to boot from its URL asks nothing and shows the download in
// Welcome's place: the headline, one progress bar per file, and no "preview
// build" notice (a fresh browser context would otherwise get it).  The ROM's
// response is held until the view has been seen, so this does not race the
// download.
test('a URL boot shows download progress and no start-up dialogs', async ({ page }) => {
  test.setTimeout(90_000);
  const body = fs.readFileSync(PLUS_ROM);
  let release!: () => void;
  const held = new Promise<void>((r) => (release = r));
  await page.route('**/url-held-plus.rom', async (route) => {
    await held;
    await route.fulfill({ status: 200, contentType: 'application/octet-stream', body });
  });

  await page.goto('/index.html?rom=url-held-plus.rom&model=plus');
  const view = page.getByTestId('url-boot-view');
  await expect(view).toBeVisible({ timeout: 60_000 });
  await expect(view.locator('.title')).toHaveText('Granny Smith');
  await expect(view.locator('.headline')).toContainText('Downloading');
  // Listed under its slot and the time, not a name from the URL.
  await expect(view.locator('[data-slot="rom"]')).toContainText(/rom_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}/);
  await expect(view.getByRole('progressbar')).toHaveCount(1);
  await expect(page.locator('.welcome-view')).toHaveCount(0);
  await expect(page.getByText('Granny Smith — preview build')).toHaveCount(0);

  release();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Booted plus from URL parameters' }))
    .toBeVisible({ timeout: 60_000 });
  await expect(page.locator('.gs-statusbar .sb-state .label')).toHaveText('Running', {
    timeout: 15_000,
  });
  await expect(view).toHaveCount(0);
  await expect(page.getByText('Granny Smith — preview build')).toHaveCount(0);
});

// Type one line into the Terminal panel (web2 has no window.gsEval) and wait
// for its output to match.
async function terminalExpect(page: Page, line: string, pattern: RegExp): Promise<void> {
  await page.locator('button.ptab[data-tab="terminal"]').click();
  const term = page.locator('.console');
  await expect(term).toBeVisible({ timeout: 15_000 });
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
  await expect(page.locator('.console-output')).toContainText(pattern, { timeout: 15_000 });
}

// URL media is kept the way an upload is: the fetched floppy is stored in
// /opfs/images/fd/ under its slot and the time it was fetched
// (fd0_2026-10-04_17-42-05) and inserted from there, not from
// volatile /tmp.  It then survives a reload and shows in
// the Images tab like any other floppy.
test('?fd0= media is persisted to /opfs/images/fd and inserted from there', async ({ page }) => {
  test.setTimeout(120_000);
  await routeRom(page, 'url-plus.rom', PLUS_ROM);
  await routeRom(page, 'url-system.dsk', SYSTEM_FD);

  await page.goto('/index.html?rom=url-plus.rom&model=plus&fd0=url-system.dsk');
  await page.waitForFunction(() => (window as { __gsReady?: boolean }).__gsReady === true, undefined, {
    timeout: 60_000,
  });
  await expect(page.locator('.toast .msg').filter({ hasText: 'Booted plus from URL parameters' }))
    .toBeVisible({ timeout: 60_000 });
  // First visit shows the "preview build" notice, whose backdrop takes
  // every click until it is dismissed (see helpers/web2-fs.ts gotoWeb2).
  const cont = page.getByRole('button', { name: 'Continue' });
  if (await cont.isVisible().catch(() => false)) await cont.click();

  const STORED = /\/opfs\/images\/fd\/fd0_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}/;
  await terminalExpect(page, 'machine.floppy.drive[0].disk.filename', STORED);
  const stored = (await opfsFiles(page)).find((p) => STORED.test(p));
  expect(stored).toBeDefined();
  await terminalExpect(page, `files.path_exists "${stored}"`, /true/);
});

// Every file under /opfs, from the browser's own view of OPFS.
async function opfsFiles(page: Page): Promise<string[]> {
  return page.evaluate(async () => {
    const out: string[] = [];
    const walk = async (dir: unknown, prefix: string): Promise<void> => {
      for await (const [n, h] of (dir as { entries(): AsyncIterable<[string, { kind: string }]> }).entries()) {
        const p = `${prefix}/${n}`;
        if (h.kind === 'directory') await walk(h, p);
        else out.push(p);
      }
    };
    await walk(await navigator.storage.getDirectory(), '/opfs');
    return out.sort();
  });
}

// A download that does not validate as its slot's kind is rejected exactly as
// the same file dropped on that category is: an 800 KB floppy image is not a
// hard disk.  The machine boots without it, the page says why, nothing of it
// is stored, and nothing is left in the scratch area -- it used to be
// attached from its scratch copy, /opfs/upload/url_hd0.
test('?hd0= that is not a hard disk is rejected, and the machine boots without it', async ({ page }) => {
  test.setTimeout(120_000);
  await routeRom(page, 'url-plus.rom', PLUS_ROM);
  await routeRom(page, 'url-floppy-as-hd.img', SYSTEM_FD);

  await page.goto('/index.html?rom=url-plus.rom&model=plus&hd0=url-floppy-as-hd.img');
  await expect(
    page.locator('.toast .msg').filter({ hasText: /HD0: 'hd0_[\d_-]+' is not a valid Hard Disk image/ }),
  ).toBeVisible({ timeout: 60_000 });
  await expect(page.locator('.toast .msg').filter({ hasText: 'Booted plus from URL parameters' }))
    .toBeVisible({ timeout: 60_000 });
  await expect(page.locator('.gs-statusbar .sb-state .label')).toHaveText('Running', {
    timeout: 15_000,
  });

  const files = await opfsFiles(page);
  expect(files.filter((p) => p.startsWith('/opfs/images/hd/'))).toEqual([]);
  expect(files.filter((p) => p.startsWith('/opfs/upload/'))).toEqual([]);
});

// The URL's vROM is part of the boot document, so it is the ROM of the slot
// whose card it provides -- not a file left in the ROM catalog for the card
// to find if it happens to win the catalog's pick order.
test('?vrom= goes in the boot document: the card runs the URL’s declaration ROM', async ({ page }) => {
  test.setTimeout(120_000);
  await routeRom(page, 'url-iicx.rom', IICX_ROM);
  await routeRom(page, 'url-card.vrom', JMFB_VROM);

  await page.goto('/index.html?rom=url-iicx.rom&model=iicx&vrom=url-card.vrom');
  await expect(page.locator('.toast .msg').filter({ hasText: 'Booted iicx from URL parameters' }))
    .toBeVisible({ timeout: 60_000 });
  expect(await gsEvalInPage(page, 'machine.nubus.slot[9].card.id')).toBe('mdc_8_24');
  expect(await gsEvalInPage(page, 'machine.nubus.slot[9].card.declrom.path')).toBe(
    '/opfs/images/vrom/d1629664',
  );
  expect(await gsEvalInPage(page, 'machine.nubus.slot[9].card.declrom.present')).toBe(true);
});

// A vROM that is not one is rejected like any other URL medium; the boot goes
// ahead without it and says so.
test('?vrom= that is not a declaration ROM is left out of the boot, which says so', async ({
  page,
}) => {
  test.setTimeout(120_000);
  await routeRom(page, 'url-plus.rom', PLUS_ROM);
  await routeRom(page, 'url-not-a.vrom', SYSTEM_FD);

  await page.goto('/index.html?rom=url-plus.rom&model=plus&vrom=url-not-a.vrom');
  await expect(
    page.locator('.toast .msg').filter({ hasText: /VROM: 'vrom_[\d_-]+' is not a valid Video ROM image/ }),
  ).toBeVisible({ timeout: 60_000 });
  await expect(
    page.locator('.toast .msg').filter({ hasText: "Booting plus without the URL's video ROM" }),
  ).toBeVisible({ timeout: 60_000 });
  await expect(page.locator('.toast .msg').filter({ hasText: 'Booted plus from URL parameters' }))
    .toBeVisible({ timeout: 60_000 });
  expect((await opfsFiles(page)).filter((p) => p.startsWith('/opfs/images/vrom/'))).toEqual([]);
});
