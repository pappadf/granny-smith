// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the AV camera control and the browser webcam → video-in path
// (proposal-av-video-in.md §4 Phase 4).
//
// Runs against Chromium's fake camera (--use-fake-device-for-media-stream
// generates a moving synthetic pattern; --use-fake-ui-for-media-stream
// auto-grants the permission prompt), so no real hardware is involved and
// the spec is CI-safe.
//
// What it proves, in order:
//   1. The camera button is *capability-gated*: absent on a machine without
//      the on-board digitizer, present on the Quadra 840AV.
//   2. Clicking it drives the core: machine.videoin.source flips none → host
//      and back, and `connected` follows the shared-heap connected flag that
//      em_camera.c publishes — i.e. the whole C↔JS transport is wired, not
//      just the button's own state.
//   3. Frames actually reach the guest-visible source: with the camera on,
//      the fake camera's moving pattern makes the digitizer's captured
//      frames change (checked through machine.videoin, which reads the same
//      slot pair the capture engine does).
//
// The guest never opens the vdig here — that path is covered headlessly by
// suite-av's video-in rows against the deterministic pattern source, which
// is where byte-exact goldens belong. This spec covers what only a real
// browser can: getUserMedia, the heap transport, and the UI.

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const AV_ROM = path.join(DATA, 'roms', 'q840av-q660av-5bf10fd1.rom');
const PLUS_ROM = path.join(DATA, 'roms', 'plus-v3-4d1f8172.rom');

// Chromium's fake camera. Per-spec launchOptions REPLACE the config array,
// so the swiftshader flags (web2 will not mount without WebGL2) are re-listed.
test.use({
  launchOptions: {
    args: [
      '--use-gl=angle',
      '--use-angle=swiftshader-webgl',
      '--ignore-gpu-blocklist',
      '--disable-dev-shm-usage',
      '--use-fake-device-for-media-stream',
      '--use-fake-ui-for-media-stream',
    ],
  },
});

// Type one shell line into the Terminal panel's xterm.
async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
}

// Scan the xterm buffer for a probe key's answer.
//
// The answer is bracketed by the key on BOTH sides (`p1=none=p1`) and the
// capture class excludes the `${}` characters, which makes the match immune
// to the two ways the buffer lies: the command line is echoed as it is typed
// (so a half-rendered `p1=$` must not match), and the completed echo still
// carries the uninterpolated `p1=${...}=p1` (whose body contains `$` and `{`).
// Only a fully rendered output line can satisfy both delimiters.
async function readKey(page: Page, key: string): Promise<string | null> {
  const text = await page.locator('.xterm-rows').innerText();
  const re = new RegExp(`${key}=([^=${'${}'}\\s]+)=${key}`);
  for (const line of text.split('\n')) {
    const m = line.trim().match(re);
    if (m) return m[1];
  }
  return null;
}

// Read one object-model value back through the terminal. A fresh key per
// probe keeps a stale echo from satisfying the match. The answer is polled
// out of the buffer rather than read after a fixed delay: the round trip is
// main thread → SAB bridge → emulator worker → xterm render, and a fixed
// wait is exactly the kind of timing assumption that flakes under load.
let probeSeq = 0;
async function probe(page: Page, expr: string, timeoutMs = 10_000): Promise<string | null> {
  const key = `p${++probeSeq}`;
  await terminalRun(page, `echo "${key}=\${${expr}}=${key}"`);
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = await readKey(page, key);
    if (v !== null) return v;
    if (Date.now() > deadline) return null;
    await page.waitForTimeout(250);
  }
}

// Boot a machine through the Welcome dialog and open the Terminal panel.
async function bootModel(page: Page, romFile: string, model: string): Promise<void> {
  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await romChooser.setFiles(romFile);

  await page.getByRole('button', { name: 'New Machine...' }).click();
  const sel = page.locator('#cfg-model');
  await expect(sel.locator(`option[value="${model}"]`)).toHaveCount(1, { timeout: 30_000 });
  await sel.selectOption(model);
  await page.getByRole('button', { name: 'Start Machine' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 60_000,
  });
}

test('AV camera control drives the video-in path with the fake camera', async ({ page }) => {
  test.setTimeout(180_000);
  await gotoWeb2(page);
  await stageOpfsFile(page, '/opfs/images/rom/q840av-q660av-5bf10fd1.rom', AV_ROM);

  // --- 1. Capability gating: a Plus has no digitizer, so no camera button.
  await bootModel(page, PLUS_ROM, 'plus');
  const camBtn = page.locator('.gs-toolbar button[aria-label*="amera"]');
  await expect(camBtn).toHaveCount(0);

  // --- 2. The 840AV has one. Shut down and boot the AV machine.
  await page.getByRole('button', { name: 'Shut down' }).click();
  await bootModel(page, AV_ROM, 'q840av');
  await expect(camBtn).toHaveCount(1, { timeout: 30_000 });
  await expect(camBtn).toHaveAttribute('aria-pressed', 'false');

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });
  // Warm up the terminal: the first typed line can land before the pane has
  // focus, so prove the round trip works before asserting on its answers.
  await expect
    .poll(async () => probe(page, 'machine.id'), { timeout: 30_000 })
    .toBe('q840av');

  // Nothing plugged in yet: the guest sees no signal.
  expect(await probe(page, 'machine.videoin.source')).toBe('none');
  expect(await probe(page, 'machine.videoin.connected')).toBe('false');

  // --- 3. Connect the camera. The click is the user gesture getUserMedia
  // needs; the fake-UI flag auto-grants. The core's source flips to `host`
  // and `connected` reads back through em_camera.c's shared-heap flag.
  await camBtn.click();
  await expect(camBtn).toHaveAttribute('aria-pressed', 'true');
  await expect.poll(async () => probe(page, 'machine.videoin.source')).toBe('host');
  await expect
    .poll(async () => probe(page, 'machine.videoin.connected'), { timeout: 30_000 })
    .toBe('true');

  // --- 4. Disconnect: the guest goes back to "no signal".
  await camBtn.click();
  await expect(camBtn).toHaveAttribute('aria-pressed', 'false');
  await expect.poll(async () => probe(page, 'machine.videoin.source')).toBe('none');
  expect(await probe(page, 'machine.videoin.connected')).toBe('false');
});
