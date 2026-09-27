// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Measurement, not a gate: how long the emulator thread stalls when the
// background checkpoint fires (every ~15 s of ticks while `checkpoint.auto`
// is on), on a 32 MB and a 128 MB IIcx.  The probe is the page's own
// request round trip: a trivial leaf served at every frame boundary
// answers in a frame or so; a stall of the emulator thread -- a checkpoint
// serialised and written inside the tick -- shows as one long round trip.
// Run against two builds (before: the write inside the tick; after: the
// write on the I/O worker) and compare the worst round trip.  The numbers
// land in $GS_MEASURE_OUT (JSON lines) and the test log.
//
// Needs `window.__gsEval` (bus/emulator.ts exposes it under
// import.meta.env.DEV or when GS_MEASURE is set); it is not a shipped
// surface.

import { test, expect } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const ROM = path.join(DATA, 'roms', 'iix-iicx-se30-97221136.rom');
const WINDOW_MS = 50_000; // covers three auto checkpoints

async function typeLine(page: import('@playwright/test').Page, line: string): Promise<void> {
  await page.locator('.xterm').click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
}

for (const ramKb of [32768, 131072]) {
  test(`checkpoint stall on a ${ramKb / 1024} MB IIcx`, async ({ page }) => {
    test.setTimeout(4 * 60 * 1000);
    await gotoWeb2(page);
    await stageOpfsFile(page, '/opfs/images/rom/97221136', ROM);
    await page.locator('button.ptab[data-tab="terminal"]').click();
    await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });
    await typeLine(page, `machine.boot model="iicx" ram=${ramKb} rom="/opfs/images/rom/97221136"`);
    await page.waitForTimeout(3_000);
    await typeLine(page, 'checkpoint.auto = true');
    await page.waitForTimeout(1_000);

    const result = await page.evaluate(async (windowMs) => {
      const w = window as unknown as { __gsEval?: (p: string) => Promise<unknown> };
      if (!w.__gsEval) throw new Error('window.__gsEval is not exposed in this build');
      const lat: number[] = [];
      const t_end = performance.now() + windowMs;
      while (performance.now() < t_end) {
        const t0 = performance.now();
        await w.__gsEval('machine.cpu.pc');
        lat.push(performance.now() - t0);
        await new Promise((r) => setTimeout(r, 10));
      }
      lat.sort((a, b) => a - b);
      const q = (p: number) => lat[Math.min(lat.length - 1, Math.floor(p * lat.length))];
      return {
        samples: lat.length,
        p50: q(0.5),
        p99: q(0.99),
        max: lat[lat.length - 1],
        over50ms: lat.filter((x) => x > 50).length,
        over200ms: lat.filter((x) => x > 200).length,
      };
    }, WINDOW_MS);
    const line = JSON.stringify({ ramKb, ...result });
    console.log(`checkpoint-stall ${line}`);
    if (process.env.GS_MEASURE_OUT) fs.appendFileSync(process.env.GS_MEASURE_OUT, line + '\n');
    expect(result.samples).toBeGreaterThan(100);
  });
}
