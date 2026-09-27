// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Measurement, not a gate: what a large host copy does to the emulator
// thread while the machine runs.  The copy (`storage.cp` of a 192 MB blank
// image) is an I/O job on the worker in the "after" build and runs inside
// the leaf on the emulator thread in the "before" build; the probe is the
// page's request round trip, as in checkpoint-stall.spec.ts.  Numbers land
// in $GS_MEASURE_OUT (JSON lines).  Needs a VITE_GS_MEASURE=1 build.

import { test, expect } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const ROM = path.join(DATA, 'roms', 'iix-iicx-se30-97221136.rom');

async function typeLine(page: import('@playwright/test').Page, line: string): Promise<void> {
  await page.locator('.xterm').click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
}

test('request round trips while a 192 MB copy runs', async ({ page }) => {
  test.setTimeout(6 * 60 * 1000);
  await gotoWeb2(page);
  await stageOpfsFile(page, '/opfs/images/rom/97221136', ROM);
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });
  await typeLine(page, 'machine.boot model="iicx" ram=8192 rom="/opfs/images/rom/97221136"');
  await page.waitForTimeout(3_000);
  // The source: a sparse blank image is fast to make; the copy reads and
  // writes every byte.
  await typeLine(page, 'storage.hd_create("/opfs/images/hd/src.img", "192mb")');
  await expect(page.locator('.xterm-rows')).toContainText('hd create: created', {
    timeout: 60_000,
  });
  await page.waitForTimeout(500);

  const probe = async (ms: number) =>
    page.evaluate(async (windowMs) => {
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
      return { samples: lat.length, p50: q(0.5), p99: q(0.99), max: lat[lat.length - 1] };
    }, ms);

  const baseline = await probe(8_000);
  const t0 = Date.now();
  await typeLine(page, 'storage.cp("/opfs/images/hd/src.img", "/opfs/images/hd/dst.img")');
  const during = await probe(20_000);
  await expect(page.locator('.xterm-rows')).toContainText('copied 1 file(s)', { timeout: 240_000 });
  const copyMs = Date.now() - t0;
  const line = JSON.stringify({ copyBytes: 192 * 1024 * 1024, copyMs, baseline, during });
  console.log(`copy-jitter ${line}`);
  if (process.env.GS_MEASURE_OUT) fs.appendFileSync(process.env.GS_MEASURE_OUT, line + '\n');
  expect(during.samples).toBeGreaterThan(10);
});
