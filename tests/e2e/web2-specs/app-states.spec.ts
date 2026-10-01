// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the real workbench in fixed states, screenshotted in both colour
// schemes.  The UI gallery (tests/e2e/gallery) covers every component against
// fixture state; this spec covers what only a live core shows — the machine
// configuration form, the SYSTEM tree and command browser, the Debug view of a
// paused machine — so a styling change that claims to change nothing can be
// checked against the whole app.  Text that moves on its own (the emulated
// screen, the console's greeting, the disassembly of a machine paused at an
// arbitrary instruction) is masked.
//
// Baselines are platform-sensitive and are recorded only in the CI image
// (tests/e2e/README.md, "UI screenshots").

import { test, expect, type Page, type Locator } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { gsEvalInPage } from '../helpers/web2-eval';
import { stageOpfsFile } from '../helpers/web2-fs';

const PLUS_ROM = path.resolve(__dirname, '../../data/roms/plus-v3-4d1f8172.rom');
const SCHEMES = ['dark', 'light'] as const;
const TABS = ['terminal', 'machine', 'filesystem', 'images', 'checkpoints', 'debug', 'logs'];

test.use({ viewport: { width: 1280, height: 800 } });

// The scheme comes from the persisted preference; the preview notice is
// already dismissed, so no first-visit modal covers the page.
async function prepare(page: Page, scheme: string): Promise<void> {
  await page.addInitScript((s) => {
    localStorage.setItem('gs-theme', s);
    localStorage.setItem('gs-preview-notice-dismissed-v1', '1');
    localStorage.setItem('gs-panel-pos', 'bottom');
  }, scheme);
}

// Load the app and wait for the core.
async function open(page: Page, url = '/index.html'): Promise<void> {
  await page.goto(url);
  await page.waitForFunction(() => (window as { __gsReady?: boolean }).__gsReady === true, undefined, {
    timeout: 60_000,
  });
}

// What changes between runs on its own: the console's greeting names the
// build, and a checkpoint is named after the time it was made.
function masks(page: Page): Locator[] {
  return [page.locator('.console-output'), page.locator('.checkpoints-view .tbody .td')];
}

// Screenshot the page once it has settled.
async function shot(page: Page, name: string, extra: Locator[] = []): Promise<void> {
  await page.mouse.move(1279, 799);
  await page.waitForTimeout(400);
  await expect(page).toHaveScreenshot(name, {
    maxDiffPixels: 0,
    animations: 'disabled',
    caret: 'hide',
    mask: [...masks(page), ...extra],
    // The emulated screen is out of scope (and its picture varies): hide it
    // instead of masking it, since a mask covers whatever lies on top.
    style: '#screen, #screen3d { visibility: hidden !important; }',
  });
}

for (const scheme of SCHEMES) {
  test.describe(`app states (${scheme})`, () => {
    test(`welcome and panel tabs, no machine (${scheme})`, async ({ page }) => {
      test.setTimeout(180_000);
      await prepare(page, scheme);
      await open(page);
      // The storage the Filesystem and Images tabs list.
      await stageOpfsFile(page, '/opfs/images/rom/plus-v3-4d1f8172.rom', PLUS_ROM);
      await shot(page, `welcome-${scheme}.png`);
      for (const tab of TABS) {
        await page.locator(`button.ptab[data-tab="${tab}"]`).click();
        await page.waitForTimeout(300);
        await shot(page, `tab-${tab}-${scheme}.png`);
      }
    });

    test(`machine configuration (${scheme})`, async ({ page }) => {
      test.setTimeout(120_000);
      await prepare(page, scheme);
      await open(page);
      await stageOpfsFile(page, '/opfs/images/rom/plus-v3-4d1f8172.rom', PLUS_ROM);
      await page.reload();
      await open(page);
      await page.locator('.card-row', { hasText: 'New Machine' }).click();
      await expect(page.locator('.config-form select').first()).toBeVisible({ timeout: 30_000 });
      await shot(page, `config-${scheme}.png`);
    });

    test(`URL boot downloading (${scheme})`, async ({ page }) => {
      test.setTimeout(120_000);
      await prepare(page, scheme);
      // The ROM never arrives: the page stays on its download progress.
      await page.route('**/never.rom', () => undefined);
      await open(page, '/index.html?rom=never.rom&model=plus');
      await expect(page.locator('.url-boot-layer')).toBeVisible();
      await shot(page, `url-boot-${scheme}.png`);
    });

    test(`debug view of a paused machine (${scheme})`, async ({ page }) => {
      test.setTimeout(180_000);
      await prepare(page, scheme);
      const body = fs.readFileSync(PLUS_ROM);
      await page.route('**/url-machine.rom', (route) =>
        route.fulfill({ status: 200, contentType: 'application/octet-stream', body }),
      );
      await open(page, '/index.html?rom=url-machine.rom&model=plus');
      await expect(page.locator('.gs-statusbar .sb-state .label')).toHaveText('Running', {
        timeout: 60_000,
      });
      await gsEvalInPage(page, 'scheduler.stop');
      await expect(page.locator('.gs-statusbar .sb-state .label')).toHaveText('Paused', {
        timeout: 15_000,
      });
      await page.locator('button.ptab[data-tab="debug"]').click();
      for (const title of ['Registers', 'Breakpoints']) {
        const toggle = page.locator('header.header', { hasText: title }).locator('button.toggle');
        if ((await toggle.getAttribute('aria-expanded')) !== 'true') await toggle.click();
      }
      await expect(page.locator('.disasm-pane .row').first()).toBeVisible({ timeout: 15_000 });
      // Where the machine stopped varies: mask the values, keep the chrome.
      await shot(page, `debug-paused-${scheme}.png`, [
        page.locator('.disasm-pane .banner, .disasm-pane .addr, .disasm-pane .mnem, .disasm-pane .ops'),
        page.locator('input.reg-value'),
        page.locator('.sb-drive'),
      ]);
    });
  });
}
