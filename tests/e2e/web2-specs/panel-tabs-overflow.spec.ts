// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: a panel too narrow for its tabs.  The tabs that do not fit move
// into a "»" menu at the end of the strip (never scrolled out of sight), the
// selected tab always stays in the strip, and a view's header actions fold
// into one "⋯" menu when they would leave no room for it.  Run in every
// skin, since each draws the tabs at its own size.

import { test, expect, type Page } from '@playwright/test';
import { MANIFESTS } from '../../../app/web2/src/skins/manifests';

const TABS = ['terminal', 'machine', 'filesystem', 'images', 'checkpoints', 'debug', 'logs'];

// Load the app with the panel docked right in a narrow window.
async function open(page: Page, skin: string, scheme: string): Promise<void> {
  await page.setViewportSize({ width: 900, height: 700 });
  await page.addInitScript(
    ([k, s]) => {
      localStorage.setItem('gs-skin', k);
      localStorage.setItem('gs-theme', s);
      localStorage.setItem('gs-preview-notice-dismissed-v1', '1');
      localStorage.setItem('gs-panel-pos', 'right');
    },
    [skin, scheme],
  );
  await page.goto('/index.html');
  await page.waitForFunction(
    () => (window as { __gsReady?: boolean }).__gsReady === true,
    undefined,
    {
      timeout: 60_000,
    },
  );
  await page.waitForSelector('html[data-skin-ready]', { state: 'attached' });
  await page.evaluate(() => document.fonts.ready);
}

const strip = (page: Page) => page.locator('.gs-panel-header [role="tablist"]');
const more = (page: Page) => strip(page).locator('.gs-tabs__more');

// The tab keys in the strip, and those behind "»".
async function split(page: Page): Promise<{ shown: string[]; hidden: string[] }> {
  const shown = await strip(page)
    .locator('button.ptab')
    .evaluateAll((els) => els.map((e) => e.getAttribute('data-tab') ?? ''));
  return { shown, hidden: TABS.filter((t) => !shown.includes(t)) };
}

for (const m of MANIFESTS) {
  const scheme = m.schemes[0];
  test(`panel tabs overflow into a menu (${m.id})`, async ({ page }) => {
    await open(page, m.id, scheme);
    const { shown, hidden } = await split(page);
    expect(hidden.length).toBeGreaterThan(0);
    expect(shown.length + hidden.length).toBe(TABS.length);
    // Nothing in the strip is cut off: every shown tab lies within it.
    const box = (await strip(page).boundingBox())!;
    for (const key of shown) {
      const b = (await strip(page).locator(`button.ptab[data-tab="${key}"]`).boundingBox())!;
      expect(b.x).toBeGreaterThanOrEqual(box.x - 1);
      expect(b.x + b.width).toBeLessThanOrEqual(box.x + box.width + 1);
    }
    // A hidden tab chosen from "»" is selected and shown in the strip.
    const target = hidden[hidden.length - 1];
    await more(page).click();
    const items = page.locator('.gs-menu .gs-menu__item');
    await expect(items).toHaveCount(hidden.length);
    await items.last().click();
    const tab = strip(page).locator(`button.ptab[data-tab="${target}"]`);
    await expect(tab).toBeVisible();
    await expect(tab).toHaveAttribute('aria-selected', 'true');
  });

  test(`header actions fold into a menu when narrow (${m.id})`, async ({ page }) => {
    await open(page, m.id, scheme);
    if (!(await strip(page).locator('button.ptab[data-tab="logs"]').isVisible())) {
      await more(page).click();
      await page.locator('.gs-menu .gs-menu__item', { hasText: 'Logs' }).click();
    }
    await expect(strip(page).locator('button.ptab[data-tab="logs"]')).toHaveAttribute(
      'aria-selected',
      'true',
    );
    // The selected tab is never squeezed: it shows whole, whether the
    // actions folded or a skin gave them a row of their own.
    const tab = (await strip(page).locator('button.ptab[data-tab="logs"]').boundingBox())!;
    const box = (await strip(page).boundingBox())!;
    expect(tab.x + tab.width).toBeLessThanOrEqual(box.x + box.width + 1);
    const folded = page.locator('.gs-panel-header .actions-menu');
    if (await folded.isVisible()) {
      await folded.click();
      await expect(page.locator('.gs-menu .gs-menu__item', { hasText: 'Clear' })).toBeVisible();
    } else {
      await expect(
        page.locator('.gs-panel-header .panel-actions button', { hasText: 'Clear' }),
      ).toBeVisible();
    }
  });
}
