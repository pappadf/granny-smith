// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Terminal's command browser is a structural view of the live
// object model.  With no machine booted, the root shows the emulator's nodes
// under their domain dividers; expanding `files` lists its members, and
// selecting a method both writes its path into the prompt and shows the
// usage text the core renders (shell.usage).

import { test, expect } from '@playwright/test';
import { gotoWeb2 } from '../helpers/web2-fs';

test('the command browser walks the model and shows usage', async ({ page }) => {
  test.setTimeout(120_000);
  await gotoWeb2(page);
  await page.locator('button.ptab[data-tab="terminal"]').click();
  const browser = page.locator('.cmd-browser');
  await expect(browser).toBeVisible({ timeout: 15_000 });

  // Domain dividers, from the model's root domains (Network appears once a
  // machine brings AppleTalk up).
  await expect(browser.locator('.divider')).toHaveText(['Machine', 'Emulator'], { timeout: 15_000 });
  // Task chips come from shell.tasks.
  await expect(browser.locator('.chip').first()).toHaveText('Run');

  const rowNamed = (name: string) =>
    browser.locator('.cmd-row').filter({ has: page.locator('.name', { hasText: new RegExp(`^${name}$`) }) });

  await rowNamed('files').locator('.twistie').click();
  const ls = rowNamed('ls');
  await expect(ls).toBeVisible({ timeout: 10_000 });
  await ls.locator('.cmd-line').click();
  await expect(ls.locator('.usage')).toContainText('files.ls [path]', { timeout: 10_000 });
  await expect(page.locator('.console .cm-content')).toContainText('files.ls', { timeout: 10_000 });
});
