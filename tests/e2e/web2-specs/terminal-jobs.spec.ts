// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// A terminal line is a job on the job thread, not a call on the emulator
// thread: `scheduler.run N` inside it waits for N, a script that computes
// forever costs the machine nothing and Ctrl-C cancels it, and Ctrl-C with
// nothing to cancel stops only a run the terminal itself started.

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';
import { focusTerminal, consoleLine, terminalRun as typeLine } from '../helpers/terminal';

const terminalRun = (page: Page, line: string) => typeLine(page, line, { settleMs: 250 });

const DATA = path.resolve(__dirname, '../../data');
const SE30_ROM = path.join(DATA, 'roms', 'iix-iicx-se30-97221136.rom');

type Ev = { kind: string; event: string; data: Record<string, unknown> };
const coreEvents = (page: Page) =>
  page.evaluate(() => (window as unknown as { __gsCoreEvents?: Ev[] }).__gsCoreEvents ?? []);

const lastTermLine = consoleLine;

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
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect.poll(() => lastTermLine(page), { timeout: 15_000 }).toMatch(/^gs se30>$/);
}

test('a terminal line waits for the run it starts, and Ctrl-C cancels a runaway script', async ({
  page,
}) => {
  test.setTimeout(5 * 60 * 1000);
  await gotoWeb2(page);
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.console')).toBeVisible({ timeout: 15_000 });
  await bootSE30(page);

  // --- scheduler.run N as a line means "run N": the line's own result
  // (the prompt) arrives after the run stopped -- 20 M instructions is a
  // couple of seconds at 1x -- and the machine is then stopped.
  await terminalRun(page, 'scheduler.stop');
  await expect.poll(() => lastTermLine(page), { timeout: 15_000 }).toMatch(/^gs se30 @[0-9A-F]{8}>$/);
  const before = (await coreEvents(page)).filter((e) => e.event === 'mode_ended').length;
  const startedBefore = (await coreEvents(page)).filter((e) => e.event === 'mode_started').length;
  await typeLine(page, 'scheduler.run 20000000');
  // The mode opens; while it runs the line has not returned (no prompt).
  await expect
    .poll(async () => (await coreEvents(page)).filter((e) => e.event === 'mode_started').length, {
      timeout: 15_000,
    })
    .toBe(startedBefore + 1);
  const t0 = Date.now();
  await page.waitForTimeout(500);
  expect(await lastTermLine(page)).not.toMatch(/^gs se30( @[0-9A-F]{8})?>$/);
  await expect
    .poll(() => lastTermLine(page), { timeout: 60_000 })
    .toMatch(/^gs se30 @[0-9A-F]{8}>$/);
  expect(Date.now() - t0).toBeGreaterThan(1000);
  await terminalRun(page, 'let r = scheduler.running');
  await terminalRun(page, 'echo "after-run $r"');
  await expect(page.locator('.console-output')).toContainText('after-run false', { timeout: 15_000 });
  const ended = (await coreEvents(page)).filter((e) => e.event === 'mode_ended');
  expect(ended.length).toBe(before + 1);
  expect(ended[ended.length - 1].data.reason).toBe('budget');
  expect(ended[ended.length - 1].data.owner).toBe(2);

  // --- A runaway script on a running machine: the machine keeps running
  // (the job thread computes, the emulator thread ticks), Ctrl-C cancels
  // the script at its next statement, and the prompt comes back.
  await terminalRun(page, 'scheduler.run');
  await expect.poll(() => lastTermLine(page), { timeout: 15_000 }).toMatch(/^gs se30>$/);
  await terminalRun(page, 'let n = 0');
  await typeLine(page, 'while true { $n = $n + 1 }');
  await page.waitForTimeout(1500);
  await page.keyboard.press('Control+C');
  await expect.poll(() => lastTermLine(page), { timeout: 15_000 }).toMatch(/^gs se30>$/);
  // The job was the foreground and took the Ctrl-C; the run it did not
  // start stays.
  await terminalRun(page, 'let r = scheduler.running');
  await terminalRun(page, 'echo "running $r"');
  await expect(page.locator('.console-output')).toContainText('running true', { timeout: 15_000 });

  // --- Ctrl-C with nothing to cancel stops the run the terminal started.
  await page.keyboard.press('Control+C');
  await expect.poll(() => lastTermLine(page), { timeout: 15_000 }).toMatch(/^gs se30 @[0-9A-F]{8}>$/);
  const stopped = (await coreEvents(page)).filter((e) => e.event === 'mode_ended');
  expect(stopped[stopped.length - 1].data.reason).toBe('stop_request');
  expect(stopped[stopped.length - 1].data.owner).toBe(2);

  // --- A run started elsewhere (the toolbar) is not the terminal's to stop.
  await page.getByRole('button', { name: 'Run', exact: true }).click();
  await expect.poll(() => lastTermLine(page), { timeout: 15_000 }).toMatch(/^gs se30>$/);
  await focusTerminal(page);
  await page.keyboard.press('Control+C');
  await expect(page.locator('.console-output')).toContainText('nothing to interrupt', { timeout: 15_000 });
  await terminalRun(page, 'let r = scheduler.running');
  await terminalRun(page, 'echo "still $r"');
  await expect(page.locator('.console-output')).toContainText('still true', { timeout: 15_000 });
});
