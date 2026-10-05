// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the shell prompt is short and state-aware (shell.c
// shell_build_prompt): "gs> " with no machine, "gs <model>> " while the
// machine free-runs (a sampled PC would be stale), and
// "gs <model> @<pc>> " when the scheduler is stopped. The prompt travels
// the real path — `shell.prompt` seeds the console on mount and every
// `shell.run` returns the next prompt, which the console shows beside its
// input — so the assertions read the rendered console, not the attribute.
//
// The machine is an SE/30 with no media, free-running at the ROM's
// insert-disk prompt (same fixture as scheduler-accelerated.spec.ts).

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';
import { terminalRun as typeLine, consoleLine, CONSOLE_INPUT } from '../helpers/terminal';

// Output is read right after each line: type, submit, then settle.
const terminalRun = (page: Page, line: string) =>
  typeLine(page, line, { settleMs: 250 });

const DATA = path.resolve(__dirname, '../../data');
const SE30_ROM = path.join(DATA, 'roms', 'iix-iicx-se30-97221136.rom');

const lastTermLine = consoleLine;

test('terminal Tab completion replaces the right span', async ({ page }) => {
  test.setTimeout(3 * 60 * 1000);
  await gotoWeb2(page);

  // Terminal is live pre-machine (object-model shell needs no emulated
  // machine), which keeps this test cheap.
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.console')).toBeVisible({ timeout: 15_000 });
  await expect
    .poll(() => lastTermLine(page), { timeout: 15_000 })
    .toMatch(/^gs>$/);
  const term = page.locator('.console');
  await term.click();

  // Object completion: a lone object candidate completes to "shell."
  // with NO trailing space (bash's directory idiom), so Tab twice
  // drills in instead of dead-ending on "shell ".
  await page.keyboard.type('she');
  await page.keyboard.press('Tab');
  await expect
    .poll(() => lastTermLine(page), { timeout: 10_000 })
    .toMatch(/^gs> shell\.$/);

  // Second Tab proposes the object's members in a popup, each with its
  // doc.
  await page.keyboard.press('Tab');
  const popup = page.locator('.cm-tooltip-autocomplete');
  await expect(popup).toContainText('complete', { timeout: 10_000 });
  await expect(popup.locator('.cm-completionDetail').first()).not.toBeEmpty();

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
  await page.keyboard.type('files.ls /op');
  await page.keyboard.press('Tab');
  await expect
    .poll(() => lastTermLine(page), { timeout: 10_000 })
    .toMatch(/^gs> files\.ls \/opfs\/$/);
});

test('shell history persists across reloads', async ({ page }) => {
  test.setTimeout(3 * 60 * 1000);
  await gotoWeb2(page);
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.console')).toBeVisible({ timeout: 15_000 });
  await page.locator('.console').click();
  await page.keyboard.type('echo history-survives-reload');
  await page.keyboard.press('Enter');
  await expect.poll(() => lastTermLine(page), { timeout: 10_000 }).toMatch(/^gs>$/);

  // Fresh page load, same origin storage: ArrowUp must recall the line.
  await gotoWeb2(page);
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.console')).toBeVisible({ timeout: 15_000 });
  await page.locator('.console').click();
  await page.keyboard.press('ArrowUp');
  await expect
    .poll(() => lastTermLine(page), { timeout: 10_000 })
    .toMatch(/^gs> echo history-survives-reload$/);
});

test('shell prompt reflects machine and run state', async ({ page }) => {
  test.setTimeout(5 * 60 * 1000);
  await gotoWeb2(page);

  // --- No machine: bare "gs> " ------------------------------------------
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.console')).toBeVisible({ timeout: 15_000 });
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
  await page.locator('#cfg-opt-memory').selectOption('8 MB');
  await page.getByRole('button', { name: 'Start', exact: true }).click();
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'Machine started' }),
  ).toBeVisible({ timeout: 60_000 });

  // --- Running: "gs se30> ", no PC --------------------------------------
  // No command is typed first: the console reseeds the rendered prompt on
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

  // --- The core's own events -------------------------------------------
  // Each stop above ended a mode, reported on the event ring with its
  // reason; the last run opened one that is still running.  A bounded
  // step ends by budget, before its own result lands.
  type Ev = { kind: string; event: string; data: Record<string, unknown> };
  const events = () =>
    page.evaluate(
      () => (window as unknown as { __gsCoreEvents?: Ev[] }).__gsCoreEvents ?? [],
    );
  // Only the mode events: perf samples and speed changes interleave.
  const modes = (evs: Ev[]) => evs.filter((e) => e.event.startsWith('mode_'));
  const before = modes(await events());
  const stops = before.filter((e) => e.event === 'mode_ended');
  expect(stops.length).toBeGreaterThanOrEqual(2);
  expect(stops.every((e) => e.data.reason === 'stop_request')).toBe(true);
  expect(before[before.length - 1]?.event).toBe('mode_started');
  await terminalRun(page, 'scheduler.stop');
  await terminalRun(page, 'debug.step 100');
  await expect
    .poll(
      async () => (await events()).filter((e) => e.event === 'mode_ended').length,
      { timeout: 15_000 },
    )
    .toBeGreaterThanOrEqual(stops.length + 2);
  const after = modes(await events());
  const last = after[after.length - 1];
  expect(last.event).toBe('mode_ended');
  expect(last.data.reason).toBe('budget');
  expect(typeof last.data.pc).toBe('number');
});

// The console stays mounted while another tab shows: its scrollback
// survives a tab switch, and output printed meanwhile is there on return.
// A pasted line is inserted for review and runs on Enter; a pasted block
// runs as one job.
test('terminal keeps its scrollback across tab switches, and paste runs', async ({ page }) => {
  test.setTimeout(3 * 60 * 1000);
  await gotoWeb2(page);
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect
    .poll(() => lastTermLine(page), { timeout: 15_000 })
    .toMatch(/^gs>$/);

  await terminalRun(page, 'echo "before-switch-marker"');
  await expect(page.locator('.console-output')).toContainText('before-switch-marker', {
    timeout: 10_000,
  });

  await page.locator('button.ptab[data-tab="machine"]').click();
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.console-output')).toContainText('before-switch-marker', {
    timeout: 10_000,
  });

  const paste = async (text: string) => {
    const input = page.locator(CONSOLE_INPUT);
    await input.focus();
    await input.evaluate((el, t) => {
      const dt = new DataTransfer();
      dt.setData('text/plain', t);
      el.dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt, bubbles: true }));
    }, text);
  };

  // One line, pasted with its newline and a copied prompt glyph: inserted,
  // not run, until Enter.
  await paste('› echo "pasted-marker"\r\n');
  await expect.poll(() => lastTermLine(page)).toBe('gs> echo "pasted-marker"');
  await page.keyboard.press('Enter');
  await expect
    .poll(() => page.locator('.console-output').innerText(), { timeout: 10_000 })
    .toMatch(/^\s*pasted-marker\s*$/m);

  // A block: shown whole, one Enter runs it as one command.
  await paste('echo "block-one"\necho "block-two"');
  await page.keyboard.press('Enter');
  await expect(page.locator('.console-output')).toContainText('block-two', { timeout: 10_000 });
  const commands = page.locator('.console-output .entry.command');
  await expect(commands.last()).toHaveText('echo "block-one"\necho "block-two"');
});
