// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// A download from the core reaches the browser through staged buffers: the
// I/O worker reads the file a chunk at a time into a buffer the page can
// see, each chunk is announced as a `download_chunk` event, the page
// copies it into a Blob part and acknowledges the buffer, and the last
// chunk saves the Blob (bus/download.ts; docs/guide/web.md "Downloads").
// Neither the emulator thread nor the page waits on the other.
//
// The file is a 9 MB blank image made and downloaded from the terminal:
// three chunks, so the ack-and-refill path runs, and a size the download
// event can check.  The Save State button exercises the same road with a
// checkpoint (checkpoint-resume.spec.ts covers the checkpoint itself).

import { test, expect } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';
import { terminalRun } from '../helpers/terminal';

const DATA = path.resolve(__dirname, '../../data');
const ROM = path.join(DATA, 'roms', 'iix-iicx-se30-97221136.rom');

test('a download arrives in staged chunks and lands as one file', async ({ page }) => {
  test.setTimeout(3 * 60 * 1000);
  await gotoWeb2(page);
  await stageOpfsFile(page, '/opfs/images/rom/97221136', ROM);

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });
  await terminalRun(page, 'machine.boot model="iicx" ram=8192 rom="/opfs/images/rom/97221136"');
  await page.waitForTimeout(2_000);

  // A 9 MB blank image (an I/O job itself), then its download.
  await terminalRun(page, 'files.hd_create("/tmp/dl.img", "9m")');
  await expect(page.locator('.xterm-rows')).toContainText('hd create: created', {
    timeout: 60_000,
  });
  const download = page.waitForEvent('download', { timeout: 60_000 });
  await terminalRun(page, 'files.download "/tmp/dl.img"');
  const dl = await download;
  expect(dl.suggestedFilename()).toBe('dl.img');
  const saved = await dl.path();
  expect(saved).toBeTruthy();
  const fs = await import('node:fs');
  const size = fs.statSync(saved!).size;
  expect(size).toBeGreaterThan(8 * 1024 * 1024);
  // The terminal saw the leaf's own confirmation, via the job's output.
  await expect(page.locator('.xterm-rows')).toContainText("download: requested 'dl.img'", {
    timeout: 30_000,
  });
  // Three chunks announced, the last one flagged, and every one acknowledged
  // (the trace keeps the last 64 events).
  const chunks = await page.evaluate(() => {
    const evs = (window as unknown as { __gsCoreEvents?: Array<{ event: string; data: Record<string, unknown> }> })
      .__gsCoreEvents ?? [];
    return evs.filter((e) => e.event === 'download_chunk').map((e) => e.data);
  });
  expect(chunks.length).toBeGreaterThanOrEqual(3);
  expect(chunks[chunks.length - 1].last).toBe(1);
  expect(new Set(chunks.map((c) => c.handle)).size).toBe(1);
});
