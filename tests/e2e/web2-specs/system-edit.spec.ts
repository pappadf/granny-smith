// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the SYSTEM tab edits the model.  On a paused SE/30, a
// double-click on machine.cpu.d0 opens its editor; a hex literal commits
// through the bridge, the row shows the new value as the REPL prints it, and
// the console echoes the statement.  An expression commits as a console
// statement instead.  Copy path copies the row's path.

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const SE30_ROM = path.join(DATA, 'roms', 'iix-iicx-se30-97221136.rom');

async function bootSE30(page: Page): Promise<void> {
  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Load ROM...' }).click(),
  ]);
  await romChooser.setFiles(SE30_ROM);
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator('option[value="se30"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption('se30');
  await page.locator('#cfg-opt-memory').selectOption('8 MB');
  await page.getByRole('button', { name: 'Start', exact: true }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 60_000,
  });
}

const row = (page: Page, p: string) => page.locator(`.sys-row[data-path='${p}']`);

test('SYSTEM edits machine.cpu.d0 and echoes the statement', async ({ page }) => {
  test.setTimeout(3 * 60 * 1000);
  await gotoWeb2(page);
  await bootSE30(page);
  await page.getByRole('button', { name: 'Pause', exact: true }).click();

  await page.locator('button.ptab[data-tab="machine"]').click();
  await expect(page.locator('.sys-tree .group-divider').first()).toHaveText('Machine', {
    timeout: 15_000,
  });
  await row(page, 'machine').locator('.sys-line').first().click();
  await row(page, 'machine.cpu').locator('.sys-line').first().click();
  const d0 = row(page, 'machine.cpu.d0');
  await expect(d0.locator('.value .text')).toHaveText(/^0x[0-9a-f]+$/, { timeout: 15_000 });

  // A literal: written through the bridge, shown as the REPL prints it.
  await d0.locator('.value').dblclick();
  const input = d0.locator('.value input');
  await expect(input).toBeFocused();
  await input.fill('0x1234');
  await input.press('Enter');
  await expect(d0.locator('.value .text')).toHaveText('0x1234', { timeout: 10_000 });

  // An expression: run as a console statement, then re-read.
  await d0.locator('.value').dblclick();
  await d0.locator('.value input').fill('0x1000 + 0x234');
  await d0.locator('.value input').press('Enter');
  await expect(d0.locator('.value .text')).toHaveText('0x1234', { timeout: 10_000 });
  await d0.locator('.value').dblclick();
  await d0.locator('.value input').fill('0x10 * 2');
  await d0.locator('.value input').press('Enter');
  await expect(d0.locator('.value .text')).toHaveText('0x20', { timeout: 10_000 });

  // The console has the echo, and the statement's own command entry.
  await page.locator('button.ptab[data-tab="terminal"]').click();
  const out = page.locator('.console-output');
  await expect(out.locator('.entry.echo')).toContainText(['machine.cpu.d0 = 0x1234']);
  await expect(out.locator('.entry.command')).toContainText([
    'machine.cpu.d0 = 0x1000 + 0x234',
    'machine.cpu.d0 = 0x10 * 2',
  ]);

  // Copy path from the context menu.
  await page.locator('button.ptab[data-tab="machine"]').click();
  await page.context().grantPermissions(['clipboard-read', 'clipboard-write']);
  await row(page, 'machine.cpu.d0').locator('.sys-line').click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Copy path', exact: true }).click();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toBe('machine.cpu.d0');
});
