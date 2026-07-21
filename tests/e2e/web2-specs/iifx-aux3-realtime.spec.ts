// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: IIfx A/UX 3.0.1 boot to the graphical login — FAITHFUL real-time
// run. Port of the legacy iifx-aux3-login-realtime.spec.ts (retired with the
// legacy UI); the deterministic fixed-budget sibling lives headless as
// tests/integration/iifx-aux3-boot-8bpp.
//
// This spec is the load-bearing guard against host-pacing-only regressions:
// the machine free-runs under the real RAF loop (scheduler_main_loop) in the
// default `live` mode — host-paced VBLs, one frame-unit per vsync. NO
// scheduler stop, NO fixed budget, NO rtc pin, NO speed override. Nothing
// that could paper over a real-time-only failure. 8bpp DID hang here
// historically while 1bpp didn't (the f_trap demand-page retry mis-detected
// as a double bus fault under RAF cadence — memory
// project_iifx_aux_8bpp_scc_iop_hang); the fixed-budget tests masked it.
//
// The only "assertion" is: the machine must reach the A/UX login. We detect
// that with the core's own bitwise framebuffer compare (machine.screen.match)
// against the checked-in 8bpp login framebuffer, polled through the shipped
// Terminal panel — web2 has no window.gsEval, and the terminal IS the typed
// UI path to the object model. The reference is staged into OPFS (the wasm
// module mounts it at /opfs) so the worker-side match can read it. The login
// window has a blinking text cursor, so a poll only matches when it lands on
// the reference's blink phase — polling every few seconds converges, exactly
// as the legacy spec did. If the boot hangs or resets, no poll ever matches
// and the test times out with the stuck frame in the failure screenshot.

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile, stageOpfsFileStreaming } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const IIFX_ROM = path.join(DATA, 'roms', 'iifx-4147dd77.rom');
const JMFB_VROM = path.join(DATA, 'roms', 'mdc-8-24-revb-d1629664.vrom');
const AUX_HD = path.join(DATA, 'aux', 'aux_3.0.1', 'hd160-with-aux-301.img');
// The 8bpp A/UX login framebuffer — same bytes as the integration
// reference (tests/integration/iifx-aux3-boot-8bpp/aux-login-8bpp.png).
const LOGIN_REF = path.join(
  __dirname,
  '..',
  '..',
  'integration',
  'iifx-aux3-boot-8bpp',
  'aux-login-8bpp.png',
);

// Type one shell line into the Terminal panel's xterm.
async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
}

test('IIfx A/UX 3.0.1 free-runs under the real RAF scheduler to the login', async ({ page }) => {
  test.setTimeout(12 * 60 * 1000);
  await gotoWeb2(page);

  // Stage the JMFB vROM (identified by content; the card factory finds it on
  // disk), the login reference the worker-side screen.match will read, and
  // the 169 MB A/UX HD. The HD is a fixture precondition here — persisting a
  // file this size through the dialog's upload path takes several minutes of
  // worker-side copying, and the streaming upload path has its own e2e
  // (webkit-local/upload.spec.ts). Staged before the ROM upload so the config
  // slide's re-scan lists everything in one pass.
  await stageOpfsFile(page, '/opfs/images/vrom/mdc-8-24-revb-d1629664.vrom', JMFB_VROM);
  await stageOpfsFile(page, '/opfs/upload/login-ref.png', LOGIN_REF);
  await stageOpfsFileStreaming(page, '/opfs/images/hd/hd160-with-aux-301.img', AUX_HD);

  // IIfx ROM via the Welcome "Upload ROM..." button.
  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await romChooser.setFiles(IIFX_ROM);

  // New Machine: IIfx, 16 MB, 13" RGB at 8 bpp, the A/UX HD.
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator('option[value="iifx"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption('iifx');

  await page.locator('#cfg-ram').selectOption('16 MB');

  const videoMode = page.locator('#cfg-video-mode');
  await expect(videoMode).toBeVisible({ timeout: 30_000 });
  await videoMode.selectOption('13in_rgb_8bpp');

  // The pre-staged A/UX HD shows up in the dialog's OPFS scan.
  const hd = page.locator('#cfg-hd');
  await expect(hd.locator('option', { hasText: 'hd160-with-aux-301.img' })).toHaveCount(1, {
    timeout: 30_000,
  });
  await hd.selectOption('hd160-with-aux-301.img');

  // Start. From here the machine free-runs under the real RAF loop — exactly
  // the path a user's browser takes. We touch nothing about scheduling.
  await page.getByRole('button', { name: 'Start Machine' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 60_000,
  });

  // The JMFB must have come up at 640x480 (the worker resizes the canvas).
  await expect
    .poll(() => page.locator('#screen').evaluate((el) => (el as HTMLCanvasElement).width), {
      timeout: 30_000,
    })
    .toBe(640);

  // Open the Terminal panel once; then poll the framebuffer compare until the
  // login window is on screen. `machine.screen.match` prints its boolean on
  // its own line, so a row that is exactly "true" is the success signal
  // (false polls print "false"; nothing else in this session prints a bare
  // "true" line).
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });

  await expect
    .poll(
      async () => {
        await terminalRun(page, 'machine.screen.match "/opfs/upload/login-ref.png"');
        // Give the worker round-trip + echo a moment to land in the buffer.
        await page.waitForTimeout(1_000);
        const text = await page.locator('.xterm-rows').innerText();
        return text
          .split('\n')
          .map((l) => l.trim())
          .some((l) => l === 'true');
      },
      { timeout: 8 * 60 * 1000, intervals: [4_000] },
    )
    .toBe(true);
});
