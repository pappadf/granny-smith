// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the screen recorder from the display toolbar.
//
// A Plus boots, the record button's menu starts a recording, the status bar
// shows it, the stop button ends it, and the finished file is offered: it
// is in OPFS, starts with the EBML magic, and the core's own reader
// (recorder.inspect) decodes every frame of it.  Hiding the page finalises
// the file being written (a complete, seekable file if the tab is killed);
// showing it again continues the recording in the next file, name-2.mkv.
// See docs/internals/core/record/record.md.

import { test, expect, type Page } from '../helpers/test';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';
import { gsCallInPage, gsEvalInPage } from '../helpers/web2-eval';
import { ROMS } from '../helpers/paths';

const PLUS_ROM = path.join(ROMS, 'plus-v3-4d1f8172.rom');
const ROM_PATH = '/opfs/images/rom/plus-v3-4d1f8172.rom';

// What recorder.inspect reports (the fields these tests read).
interface Inspect {
  frames: number;
  keyframes: number;
  decode_errors: number;
  truncated: boolean;
  duration_ms: number;
  audio_frames: number;
  size: number;
}

// Boot a Plus and run it, so the page shows a running machine.
async function bootPlus(page: Page): Promise<void> {
  await gotoWeb2(page);
  await stageOpfsFile(page, ROM_PATH, PLUS_ROM);
  await gsCallInPage(page, 'machine.boot', { model: 'plus', ram: 4096, rom: ROM_PATH });
  // A hidden page writes a background checkpoint too; not this test's subject
  await gsCallInPage(page, 'checkpoint.auto', [false]);
  await gsCallInPage(page, 'scheduler.run');
  await expect(page.locator('.gs-statusbar')).toHaveAttribute('data-state', 'running', {
    timeout: 30_000,
  });
}

// Start a recording from the record button's menu; the path being written.
async function startFromToolbar(page: Page): Promise<string> {
  const btn = page.locator('.record-btn');
  await expect(btn).toBeEnabled();
  await btn.click();
  await page.locator('.context-menu .item', { hasText: 'Start Recording' }).click();
  await expect(page.getByTestId('recording')).toBeVisible({ timeout: 15_000 });
  await expect(btn).toHaveAttribute('data-recording', '');
  return (await gsEvalInPage(page, 'recorder.path')) as string;
}

// Wait until the recording has `n` more frames than now.
async function recordFrames(page: Page, n: number): Promise<number> {
  const from = (await gsEvalInPage(page, 'recorder.frames')) as number;
  await expect
    .poll(async () => (await gsEvalInPage(page, 'recorder.frames')) as number, { timeout: 60_000 })
    .toBeGreaterThan(from + n);
  return (await gsEvalInPage(page, 'recorder.frames')) as number;
}

// The first bytes of an OPFS file, read by the page itself.
async function opfsHead(page: Page, file: string, n = 4): Promise<number[]> {
  return page.evaluate(
    async ({ file, n }) => {
      const rel = file.replace(/^\/opfs\/?/, '').split('/');
      const name = rel.pop() as string;
      let dir = await navigator.storage.getDirectory();
      for (const part of rel) dir = await dir.getDirectoryHandle(part);
      const f = await (await dir.getFileHandle(name)).getFile();
      return Array.from(new Uint8Array(await f.slice(0, n).arrayBuffer()));
    },
    { file, n },
  );
}

// The page as the browser shows it when the tab is hidden or shown: the
// state, then the event (the core listens for it, em_main.c).
async function setPageHidden(page: Page, hidden: boolean): Promise<void> {
  await page.evaluate((h) => {
    Object.defineProperty(document, 'hidden', { configurable: true, get: () => h });
    Object.defineProperty(document, 'visibilityState', {
      configurable: true,
      get: () => (h ? 'hidden' : 'visible'),
    });
    document.dispatchEvent(new Event('visibilitychange'));
  }, hidden);
}

const EBML_MAGIC = [0x1a, 0x45, 0xdf, 0xa3];

test('record from the toolbar: status bar, stop, the file in OPFS, decoded', async ({ page }) => {
  test.setTimeout(180_000);
  await bootPlus(page);
  const file = await startFromToolbar(page);
  expect(file).toMatch(/^\/opfs\/recordings\/plus-\d{8}-\d{6}\.mkv$/);

  // The indicator counts the recorded (emulated) time and the file's size
  await recordFrames(page, 150);
  const indicator = page.getByTestId('recording');
  await expect(indicator.locator('.rec-time')).not.toHaveText('0:00', { timeout: 15_000 });
  await expect(indicator.locator('.rec-size')).not.toHaveText('0 B');

  // Stop: the red stop button, then the finished file is offered
  await page.locator('.record-btn').click();
  const toast = page.locator('.toast', { hasText: 'Recording saved' });
  await expect(toast).toBeVisible({ timeout: 30_000 });
  await expect(indicator).toHaveCount(0);
  expect(await gsEvalInPage(page, 'recorder.active')).toBe(false);
  const frames = (await gsEvalInPage(page, 'recorder.frames')) as number;

  expect(await opfsHead(page, file)).toEqual(EBML_MAGIC);
  const info = (await gsCallInPage(page, 'recorder.inspect', [file])) as Inspect;
  expect(info.frames).toBe(frames);
  expect(info.frames).toBeGreaterThan(150);
  expect(info.decode_errors).toBe(0);
  expect(info.truncated).toBe(false);
  expect(info.duration_ms).toBeGreaterThan(0);

  // Download goes through the Filesystem panel's download path
  const download = page.waitForEvent('download');
  await toast.getByRole('button', { name: 'Download' }).click();
  const d = await download;
  expect(d.suggestedFilename()).toBe(path.basename(file));
});

test('a hidden page finalises the file; shown again, the next segment', async ({ page }) => {
  test.setTimeout(180_000);
  await bootPlus(page);
  const file = await startFromToolbar(page);
  await recordFrames(page, 60);

  // Hidden: the file is closed and complete before anything else happens
  await setPageHidden(page, true);
  await expect
    .poll(() => gsEvalInPage(page, 'recorder.suspended'), { timeout: 15_000 })
    .toBe(true);
  expect(await gsEvalInPage(page, 'recorder.active')).toBe(true);
  const frames1 = (await gsEvalInPage(page, 'recorder.frames')) as number;
  expect(await opfsHead(page, file)).toEqual(EBML_MAGIC);
  const a = (await gsCallInPage(page, 'recorder.inspect', [file])) as Inspect;
  expect(a.frames).toBe(frames1);
  expect(a.truncated).toBe(false);
  expect(a.duration_ms).toBeGreaterThan(0);
  expect(a.decode_errors).toBe(0);
  // The machine runs on; nothing is recorded while hidden
  await page.waitForTimeout(1000);
  expect(await gsEvalInPage(page, 'recorder.frames')).toBe(frames1);
  expect(await gsEvalInPage(page, 'recorder.segments')).toBe(1);

  // Shown: the recording continues in name-2.mkv
  await setPageHidden(page, false);
  const second = file.replace(/\.mkv$/, '-2.mkv');
  await expect
    .poll(() => gsEvalInPage(page, 'recorder.path'), { timeout: 15_000 })
    .toBe(second);
  expect(await gsEvalInPage(page, 'recorder.suspended')).toBe(false);
  await recordFrames(page, 60);

  await page.locator('.record-btn').click();
  const toast = page.locator('.toast', { hasText: 'Recording saved' });
  await expect(toast).toBeVisible({ timeout: 30_000 });
  await expect(toast).toContainText('and 1 more');
  const frames = (await gsEvalInPage(page, 'recorder.frames')) as number;
  const b = (await gsCallInPage(page, 'recorder.inspect', [second])) as Inspect;
  expect(await opfsHead(page, second)).toEqual(EBML_MAGIC);
  expect(b.decode_errors).toBe(0);
  expect(b.keyframes).toBeGreaterThanOrEqual(1);
  expect(a.frames + b.frames).toBe(frames);
});
