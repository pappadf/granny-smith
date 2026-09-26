// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Debug view against a real machine (11-WORK-ORDER unit 0.4).
//
// Until this spec there was no e2e coverage of the Debug view at all, and its
// component tests mock the bus functions under test — so register editing
// (sent a shell statement as a gsEval path) and the Breakpoints panel (read a
// path that never resolved, and "removed" by adding a second breakpoint) were
// broken on every model with every test green.  Each case here drives the real
// UI and then asks the core, through the automation-only __gsEvalForTests
// hook, whether the action actually happened.

import { test, expect, type Page } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { gsEvalInPage } from '../helpers/web2-eval';

const PLUS_ROM = path.resolve(__dirname, '../../data/roms/plus-v3-4d1f8172.rom');
const PDM_ROM = path.resolve(__dirname, '../../data/roms/pm6100-pm7100-pm8100-9feb69b3.rom');

// Boot a machine from a URL parameter (the ROM fetch is served from disk),
// wait for it to run, then pause it so the Debug view shows state.
async function bootPaused(page: Page, rom: string, model: string): Promise<void> {
  const body = fs.readFileSync(rom);
  await page.route('**/url-machine.rom', (route) =>
    route.fulfill({ status: 200, contentType: 'application/octet-stream', body }),
  );
  await page.goto(`/index.html?rom=url-machine.rom&model=${model}`);
  await page.waitForFunction(() => (window as { __gsReady?: boolean }).__gsReady === true, undefined, {
    timeout: 60_000,
  });
  await expect(page.locator('.gs-statusbar .sb-state .label')).toHaveText('Running', { timeout: 60_000 });
  const cont = page.getByRole('button', { name: 'Continue' });
  if (await cont.isVisible().catch(() => false)) await cont.click();
  await gsEvalInPage(page, 'scheduler.stop');
  await expect(page.locator('.gs-statusbar .sb-state .label')).toHaveText('Paused', { timeout: 15_000 });
  await page.locator('button.ptab[data-tab="debug"]').click();
}

async function bootPlusPaused(page: Page): Promise<void> {
  await bootPaused(page, PLUS_ROM, 'plus');
}

// A register read as a number.  Register attributes carry the hex display
// flag, and the JSON encoding turns a hex-flagged integer into a "0x..."
// string (a bare 0x literal is not JSON), so both shapes are accepted.
async function readReg(page: Page, name: string): Promise<number> {
  const v = await gsEvalInPage(page, `machine.cpu.${name}`);
  if (typeof v === 'number') return v;
  if (typeof v === 'string' && /^0x[0-9a-f]+$/i.test(v)) return parseInt(v, 16);
  throw new Error(`machine.cpu.${name}: not a number: ${JSON.stringify(v)}`);
}

// Open one collapsible Debug section by its title.
async function openSection(page: Page, title: string): Promise<void> {
  const header = page.locator('header.header', { hasText: title });
  if ((await header.getAttribute('aria-expanded')) !== 'true') await header.click();
}

test('a register edit reaches the core', async ({ page }) => {
  test.setTimeout(120_000);
  await bootPlusPaused(page);
  await openSection(page, 'Registers');

  const d0 = page.getByLabel('D0 register value');
  await expect(d0).toBeVisible({ timeout: 15_000 });
  await d0.fill('00001234');
  await d0.press('Enter');

  await expect.poll(() => readReg(page, 'd0'), { timeout: 10_000 }).toBe(0x1234);
  await expect(page.locator('.toast .msg').filter({ hasText: 'Failed to write' })).toHaveCount(0);
});

test('breakpoints are listed, and Remove removes', async ({ page }) => {
  test.setTimeout(120_000);
  await bootPlusPaused(page);
  await openSection(page, 'Breakpoints');

  // Add through the section's own row.
  await page.locator('.add-btn[title="Add breakpoint"]').click();
  const addr = page.getByLabel('Breakpoint address');
  await addr.fill('0x400100');
  await addr.press('Enter');

  const rows = page.locator('.bp-row');
  await expect(rows).toHaveCount(1, { timeout: 10_000 });
  await expect(rows.first()).toContainText('00400100');
  expect(await gsEvalInPage(page, 'debug.breakpoints.count')).toBe(1);

  // Adding the same address again must not stack a second entry.
  await page.locator('.add-btn[title="Add breakpoint"]').click();
  await addr.fill('0x400100');
  await addr.press('Enter');
  await expect
    .poll(() => gsEvalInPage(page, 'debug.breakpoints.count'), {
      timeout: 10_000,
    })
    .toBe(1);
  await expect(rows).toHaveCount(1);

  // Remove through the row's context menu: gone from the list and the core.
  await rows.first().click({ button: 'right' });
  await page.getByRole('menuitem', { name: 'Remove' }).click();
  await expect(rows).toHaveCount(0, { timeout: 10_000 });
  expect(await gsEvalInPage(page, 'debug.breakpoints.count')).toBe(0);
});

// A paused machine repaints after a request that changes the screen (D8).
// Before, video was refreshed only while the scheduler ran, so a poke into the
// framebuffer (or a step) stayed invisible until the next resume.
test('a paused machine repaints after a framebuffer poke', async ({ page }) => {
  test.setTimeout(120_000);
  await bootPlusPaused(page);
  const screen = page.locator('#screen');
  const before = await screen.screenshot();

  // Invert a band of the Plus framebuffer: 40 rows x 64 bytes, as one
  // shell.run so it is one request.  The Plus video scans from the top of RAM
  // minus $5900 whatever the boot has reached (ScrnBase may not be set yet).
  const ramKb = (await gsEvalInPage(page, 'machine.ram')) as number;
  const band = ramKb * 1024 - 0x5900 + 64 * 100;
  const script =
    `for i in 0..640 { machine.memory.poke.l(${band} + $i * 4, ` +
    `machine.memory.peek.l(${band} + $i * 4) ^ 0xFFFFFFFF) }`;
  await gsEvalInPage(page, 'shell.run', [script]);

  await expect
    .poll(async () => Buffer.compare(before, await screen.screenshot()) !== 0, {
      timeout: 10_000,
    })
    .toBe(true);
  // Still paused: the repaint did not come from resuming.
  await expect(page.locator('.gs-statusbar .sb-state .label')).toHaveText('Paused');
});

// The Debug view on a PowerPC machine (D2/D4).  Before, debug.frame failed on
// every PPC model and the Registers pane said "No machine running" while it
// was paused.
test('a PowerPC machine shows its own register file', async ({ page }) => {
  test.setTimeout(150_000);
  await bootPaused(page, PDM_ROM, 'pm7100');
  await openSection(page, 'Registers');
  const r1 = page.getByLabel('R1 register value');
  await expect(r1).toBeVisible({ timeout: 15_000 });
  const core = await readReg(page, 'r1');
  await expect(r1).toHaveValue(core.toString(16).toUpperCase().padStart(8, '0'));
  await expect(page.getByLabel('D0 register value')).toHaveCount(0);
});
