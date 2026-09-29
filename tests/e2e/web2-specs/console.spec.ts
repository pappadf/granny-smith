// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Terminal's console against the real core.  A value the REPL
// prints becomes one structured entry (a list expands), a failing statement
// becomes an error entry (its stderr claimed by the job's error annotation),
// and "Copy as commands" of a three-command transcript, pasted back and run,
// is one command whose output entries equal the original three's.

import { test, expect, type Page } from '@playwright/test';
import { gotoWeb2 } from '../helpers/web2-fs';
import { CONSOLE_INPUT, consoleLine, terminalRun } from '../helpers/terminal';

test.use({ permissions: ['clipboard-read', 'clipboard-write'] });

const output = (page: Page) => page.locator('.console-output');

async function idle(page: Page): Promise<void> {
  await expect.poll(() => consoleLine(page), { timeout: 15_000 }).toMatch(/^gs>$/);
}

async function menu(page: Page, target: ReturnType<Page['locator']>, label: string) {
  await target.click({ button: 'right' });
  await page.getByRole('menuitem', { name: label, exact: true }).click();
}

test('the console renders values and errors, and Copy as commands round-trips', async ({
  page,
}) => {
  test.setTimeout(120_000);
  await gotoWeb2(page);
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await idle(page);

  // --- A list value: one entry, expandable.
  await terminalRun(page, 'let xs = [10, 20, 30]');
  await idle(page);
  await terminalRun(page, '$xs');
  const value = output(page).locator('.entry.value').last();
  await expect(value).toContainText('10', { timeout: 10_000 });
  await value.locator('summary').click();
  await expect(value.locator('.kv-row')).toHaveCount(3);

  // --- A failing statement: one error entry, not loose stderr as well.  The
  // worker's printErr line can land after the job's end; it must not show
  // a second time once it does (well within the 2 s settle time).
  await idle(page);
  const errorsBefore = await output(page).locator('.entry.error').count();
  await terminalRun(page, 'nosuch.member.anywhere');
  await expect(output(page).locator('.entry.error')).toHaveCount(errorsBefore + 1, {
    timeout: 10_000,
  });
  await expect(output(page).locator('.entry.error').last()).toContainText('nosuch');
  await page.waitForTimeout(2_500);
  await expect(output(page).locator('.entry.stderr')).toHaveCount(0);
  await expect(output(page).locator('.entry.error')).toHaveCount(errorsBefore + 1);

  // --- Copy as commands, over a clean three-command transcript.
  await idle(page);
  await menu(page, output(page), 'Clear');
  await expect(output(page).locator('.entry')).toHaveCount(0);
  for (const w of ['one', 'two', 'three']) {
    await terminalRun(page, `echo "copy-${w}"`);
    await idle(page);
  }
  await expect(output(page).locator('.entry.text')).toHaveText([
    'copy-one',
    'copy-two',
    'copy-three',
  ]);
  const original = await output(page).locator('.entry.text').allInnerTexts();

  await menu(page, output(page).locator('.entry').first(), 'Select all');
  await menu(page, output(page).locator('.entry').first(), 'Copy as commands');
  const copied = await page.evaluate(() => navigator.clipboard.readText());
  expect(copied).toBe('echo "copy-one"\necho "copy-two"\necho "copy-three"');

  // Paste it back and run it: one command entry, the same output entries.
  await menu(page, output(page), 'Clear');
  const input = page.locator(CONSOLE_INPUT);
  await input.focus();
  await input.evaluate((el, t) => {
    const dt = new DataTransfer();
    dt.setData('text/plain', t);
    el.dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt, bubbles: true }));
  }, copied);
  await page.keyboard.press('Enter');
  await idle(page);
  await expect(output(page).locator('.entry.command')).toHaveCount(1);
  await expect(output(page).locator('.entry.command')).toHaveText(copied);
  await expect(output(page).locator('.entry.text')).toHaveText(original);
});
