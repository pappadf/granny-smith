// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the shell prompt is short and state-aware (shell.c
// shell_build_prompt): "gs> " with no machine, "gs <model>> " while the
// machine free-runs (a sampled PC would be stale), and
// "gs <model> @<pc>> " when the scheduler is stopped. The prompt travels
// the real path — `shell.prompt` seeds the terminal on mount and every
// `shell.run` returns the next prompt, which xterm renders — so the
// assertions read the rendered terminal, not the attribute.
//
// The machine is an SE/30 with no media, free-running at the ROM's
// insert-disk prompt (same fixture as scheduler-accelerated.spec.ts).

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const SE30_ROM = path.join(DATA, 'roms', 'iix-iicx-se30-97221136.rom');

// Last non-empty rendered terminal line — the input line, i.e. the
// current prompt (xterm innerText drops trailing blanks/spaces).
async function lastTermLine(page: Page): Promise<string> {
  const text = await page.locator('.xterm-rows').innerText();
  const lines = text
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l.length > 0);
  return lines.length ? lines[lines.length - 1] : '';
}

// Type one shell line into the Terminal panel's xterm. A trailing settle
// lets the async worker round-trip land before the next interaction.
async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(250);
}

test('terminal Tab completion replaces the right span', async ({ page }) => {
  test.setTimeout(3 * 60 * 1000);
  await gotoWeb2(page);

  // Terminal is live pre-machine (object-model shell needs no emulated
  // machine), which keeps this test cheap.
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });
  await expect
    .poll(() => lastTermLine(page), { timeout: 15_000 })
    .toMatch(/^gs>$/);
  const term = page.locator('.xterm');
  await term.click();

  // Object completion: a lone object candidate completes to "shell."
  // with NO trailing space (bash's directory idiom), so Tab twice
  // drills in instead of dead-ending on "shell ".
  await page.keyboard.type('she');
  await page.keyboard.press('Tab');
  await expect
    .poll(() => lastTermLine(page), { timeout: 10_000 })
    .toMatch(/^gs> shell\.$/);

  // Second Tab proposes the object's members.
  await page.keyboard.press('Tab');
  await expect
    .poll(() => page.locator('.xterm-rows').innerText(), { timeout: 10_000 })
    .toContain('shell.complete');

  // A leaf attribute completes with the trailing space.
  await page.keyboard.type('pro');
  await page.keyboard.press('Tab');
  await expect
    .poll(() => lastTermLine(page), { timeout: 10_000 })
    .toMatch(/^gs> shell\.prompt$/);
  await page.keyboard.press('Enter'); // run it; harmless readback
  await expect
    .poll(() => lastTermLine(page), { timeout: 10_000 })
    .toMatch(/^gs>$/);

  // Filesystem-path completion in a method arg: candidates are bare
  // entry names (span narrows to the basename after the last '/'), and
  // a directory completes to "name/" — the browser VFS root always
  // contains /opfs.
  await page.keyboard.type('vfs.ls /op');
  await page.keyboard.press('Tab');
  await expect
    .poll(() => lastTermLine(page), { timeout: 10_000 })
    .toMatch(/^gs> vfs\.ls \/opfs\/$/);
});

test('shell prompt reflects machine and run state', async ({ page }) => {
  test.setTimeout(5 * 60 * 1000);
  await gotoWeb2(page);

  // --- No machine: bare "gs> " ------------------------------------------
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });
  await expect
    .poll(() => lastTermLine(page), { timeout: 15_000 })
    .toMatch(/^gs>$/);

  // --- Boot an SE/30 (ROM upload + New Machine, no media) ---------------
  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await romChooser.setFiles(SE30_ROM);

  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator('option[value="se30"]')).toHaveCount(1, {
    timeout: 30_000,
  });
  await model.selectOption('se30');
  await page.locator('#cfg-ram').selectOption('8 MB');
  await page.getByRole('button', { name: 'Start Machine' }).click();
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'Machine started' }),
  ).toBeVisible({ timeout: 60_000 });

  // --- Running: "gs se30> ", no PC --------------------------------------
  // No command is typed first: TerminalPane reseeds the rendered prompt on
  // the machine.status edge, so the idle input line must update by itself.
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect
    .poll(() => lastTermLine(page), { timeout: 15_000 })
    .toMatch(/^gs se30>$/);

  // --- Toolbar pause/resume: idle prompt repaints by itself -------------
  // No terminal input at all — the run-state edge alone must flip the
  // rendered prompt to the halted-PC form and back.
  await page.getByRole('button', { name: 'Pause', exact: true }).click();
  await expect
    .poll(() => lastTermLine(page), { timeout: 15_000 })
    .toMatch(/^gs se30 @[0-9A-F]{8}>$/);
  await page.getByRole('button', { name: 'Run', exact: true }).click();
  await expect
    .poll(() => lastTermLine(page), { timeout: 15_000 })
    .toMatch(/^gs se30>$/);

  // --- Stopped: "gs se30 @<pc>> " ---------------------------------------
  // shell.run computes the returned prompt after the command executes, so
  // the stop's own return already carries the halted-PC form.
  await terminalRun(page, 'scheduler.stop');
  await expect
    .poll(() => lastTermLine(page), { timeout: 15_000 })
    .toMatch(/^gs se30 @[0-9A-F]{8}>$/);

  // --- Resumed: back to "gs se30> " -------------------------------------
  await terminalRun(page, 'scheduler.run');
  await expect
    .poll(() => lastTermLine(page), { timeout: 15_000 })
    .toMatch(/^gs se30>$/);
});
