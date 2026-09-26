// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// THROWAWAY SPIKE spec — drives src/platform/wasm/em_spike_baton.c.
// Boots an SE/30 (so the guest-state leaves have a machine to read), then
// collects the "SPIKE ..." console lines the worker emits while the baton
// rounds run, and keeps the ordinary bridge busy in parallel to show that
// page requests are still served between rounds.

import { test, expect } from '@playwright/test';
import * as path from 'node:path';
import * as fs from 'node:fs';
import { gotoWeb2 } from '../helpers/web2-fs';
import { gsEvalInPage } from '../helpers/web2-eval';

const DATA = path.resolve(__dirname, '../../data');
const SE30_ROM = path.join(DATA, 'roms', 'iix-iicx-se30-97221136.rom');
const OUT = process.env.SPIKE_OUT ?? '/tmp/spike-baton.log';

test('baton hand-off to a job pthread', async ({ page }) => {
  test.setTimeout(5 * 60 * 1000);
  const lines: string[] = [];
  page.on('console', (msg) => {
    const t = msg.text();
    if (t.includes('SPIKE')) lines.push(t);
  });
  page.on('worker', (w) => {
    w.on('console', (msg) => {
      const t = msg.text();
      if (t.includes('SPIKE')) lines.push(t);
    });
  });

  await gotoWeb2(page);

  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await romChooser.setFiles(SE30_ROM);
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator('option[value="se30"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption('se30');
  await page.locator('#cfg-ram').selectOption('8 MB');
  await page.getByRole('button', { name: 'Start Machine' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 60_000,
  });

  // Keep the bridge busy from the page while the rounds run.
  let bridgeCalls = 0;
  let bridgeErrors = 0;
  const t0 = Date.now();
  while (Date.now() - t0 < 25_000 && !lines.some((l) => l.includes('SPIKE done'))) {
    const r = (await gsEvalInPage(page, 'storage.list_dir', ['/opfs'])) as unknown;
    bridgeCalls++;
    if (!Array.isArray(r)) bridgeErrors++;
    await page.waitForTimeout(50);
  }
  lines.push(`SPEC bridge calls during rounds: ${bridgeCalls}, non-list results: ${bridgeErrors}`);

  fs.writeFileSync(OUT, lines.join('\n') + '\n');
  for (const l of lines) console.log(l);

  expect(lines.some((l) => l.includes('SPIKE done'))).toBe(true);
  expect(lines.filter((l) => l.includes('SPIKE FAIL'))).toEqual([]);
  expect(bridgeErrors).toBe(0);
});
