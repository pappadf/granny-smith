// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// IIfx A/UX 3.0.1 boot-to-login — FAITHFUL reproduction of the real web-UI run.
//
// Unlike iifx-aux3-login.spec.ts (which drives a fixed scheduler.run budget in
// max-speed mode so it can match pixel-exact), this spec runs the machine EXACTLY
// as "make run" + the config dialog does:
//   * config dialog → Start, then the machine free-runs under the real RAF loop
//     (scheduler_main_loop) in the default `real_time` mode — host-paced VBLs, one
//     frame-unit per host vsync.
//   * NO scheduler.stop, NO fixed budget, NO rtc.time pin, NO speed override,
//     NO tolerance.  Nothing that could paper over a real-time-only failure.
//
// It exists because the deterministic test can hide bugs that only appear under the
// real host-time pacing.  In fact 8bpp DID hang here while 1bpp didn't: under the
// RAF VBL cadence A/UX's exec of /etc/init demand-paged init's text page and a
// not-yet-resident instruction-fetch retry was mis-detected as a double bus fault →
// HALT → reset (see memory project_iifx_aux_8bpp_scc_iop_hang); at the time the
// headless run used a different (cycle-driven) VBL cadence that happened to have the
// page ready on retry, masking it.  (Headless now runs the same frame-unit model, so
// it no longer masks such bugs — but this real-time run remains the load-bearing
// guard against host-pacing-only regressions.)
//
// Here the only "assertion" is: the machine must reach the A/UX login.  We detect
// that with the boot ROM's own bitwise framebuffer compare (screen.match) against a
// checked-in 8bpp login framebuffer — so it matches the instant the login window is
// on screen (the blinking cursor just means the match lands on the matching blink
// phase), and FAILS hard (timeout) if the boot hangs or resets before getting there.
// Playwright captures the stuck frame on failure.

import { test, expect } from '../../fixtures';
import * as fs from 'node:fs';
import * as path from 'node:path';

const TEST_DATA = path.join(process.cwd(), 'tests', 'data');
const ROM_ABS = path.join(TEST_DATA, 'roms', '4147DD77-IIfx.rom');
const VROM_ABS = path.join(TEST_DATA, 'roms', '341-0868.vrom');
const HD_ABS = path.join(TEST_DATA, 'aux', 'aux_3.0.1', 'hd160-with-aux-301.img');
const VIDEO_MODE = '13in_rgb_8bpp';
// The checked-in 8bpp A/UX login framebuffer (same image the deterministic
// iifx-aux3-login.spec.ts pins as its pixel-exact snapshot).
const LOGIN_REF = path.join(process.cwd(), 'tests', 'e2e', 'specs', 'iifx-aux3-boot', 'aux-login-8bpp.png');

test.describe('IIfx A/UX 3.0.1 boot to login — real-time (faithful UI run)', () => {
  test('free-runs under the real RAF scheduler and reaches the A/UX login', async ({ page, log }) => {
    test.setTimeout(8 * 60 * 1000);

    // The UI shows a "resume from checkpoint?" dialog if OPFS has one; a user
    // clicks "Start fresh" — do the same so we cold-boot.
    await page.addInitScript(() => {
      const observe = () => {
        if (!document.body) { document.addEventListener('DOMContentLoaded', observe); return; }
        new MutationObserver(() => {
          const dlg = document.getElementById('checkpoint-dialog');
          if (dlg && dlg.getAttribute('aria-hidden') === 'false') {
            const btn = dlg.querySelector('[data-checkpoint-fresh]') as HTMLElement | null;
            if (btn) btn.click();
          }
        }).observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ['aria-hidden'] });
      };
      observe();
    });

    await page.goto('/index.html');
    await page.waitForLoadState('domcontentloaded');

    await page.waitForFunction(() => {
      const d = document.getElementById('rom-upload-dialog');
      return d && d.getAttribute('aria-hidden') === 'false';
    }, { timeout: 30_000 });
    await (await page.$('#rom-upload-file-input'))!.setInputFiles(ROM_ABS);

    await page.waitForFunction(() => {
      const c = document.getElementById('config-dialog');
      return c && c.getAttribute('aria-hidden') === 'false';
    }, { timeout: 30_000 });
    await page.locator('#config-model').selectOption('iifx');
    await page.waitForFunction(() => !!document.getElementById('config-vrom') && !!document.getElementById('config-ram'), { timeout: 10_000 });
    await page.locator('#config-ram').selectOption('16384');

    {
      const fcp = page.waitForEvent('filechooser');
      await page.locator('#config-vrom').selectOption('__upload__');
      await (await fcp).setFiles(VROM_ABS);
    }
    await page.waitForFunction(() => {
      const s = document.getElementById('config-vrom') as HTMLSelectElement | null;
      return !!s && s.value !== '' && s.value !== '__upload__';
    }, { timeout: 30_000 });

    log('[rt] uploading A/UX HD (169 MB → OPFS)…');
    {
      const fcp = page.waitForEvent('filechooser');
      await page.locator('#config-hd0').selectOption('__upload__');
      await (await fcp).setFiles(HD_ABS);
    }
    await page.waitForFunction(() => {
      const s = document.getElementById('config-hd0') as HTMLSelectElement | null;
      return !!s && s.value !== '' && s.value !== '__upload__' && s.value !== '__create__';
    }, { timeout: 5 * 60 * 1000 });

    await page.waitForFunction(() => !!document.getElementById('config-video-mode'), { timeout: 5_000 });
    await page.locator('#config-video-mode').selectOption(VIDEO_MODE);

    // Start.  From here the machine free-runs under the real RAF loop — exactly the
    // path the web UI takes.  We touch nothing about scheduling.
    await page.click('#config-start-btn');
    await page.waitForFunction(() => (window as any).__gsBootReady === true, { timeout: 90_000 });

    // Stage the login reference where the boot ROM's screen.match can read it.
    const ref = Array.from(fs.readFileSync(LOGIN_REF));
    await page.evaluate(async (bytes) => {
      await (window as any).__gsTestShim.injectMedia([{ name: 'login-ref.png', data: new Uint8Array(bytes) }]);
    }, ref);

    // The machine is booting in the background.  Wait until the framebuffer IS the
    // A/UX login window (bitwise), failing hard if it never gets there.
    log('[rt] waiting for the A/UX login (real-time boot)…');
    await expect
      .poll(() => page.evaluate(() => (window as any).gsEval('screen.match', ['/tmp/login-ref.png'])),
            { timeout: 5 * 60 * 1000, intervals: [2000] })
      .toBe(true);
  });
});
