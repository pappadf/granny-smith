// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// IIfx A/UX 3.0.1 boot-to-login end-to-end test — the WebGL/UI-flow sibling of
// tests/integration/iifx-aux3-boot.  Cold-boots the IIfx through the machine-
// configuration dialog (model + 16 MB RAM + JMFB Video ROM + the A/UX HD + a
// 13" RGB 8bpp video mode) and matches the A/UX graphical login window against a
// checked-in baseline, pixel-for-pixel.
//
// Why 8bpp specifically: 8-bit colour is the configuration that exposed two
// distinct IIfx bugs, so the e2e pins it (1bpp is exercised by the headless
// integration test):
//   1. jmfb.c PRAM seeding — selecting ANY video mode makes the JMFB factory seed
//      slot-PRAM AND stamp the boot-ROM PRAM validity tokens; a regression there
//      (the token stamp suppressing the ROM's default startup-device PRAM init)
//      left D3=0 at SCSILoad → no boot driver → Mac-OS no-boot floppy.  See
//      notes/iifx-debug/117.
//   2. CPU instruction-fetch fault handling — 8bpp's larger framebuffer raises
//      memory pressure so A/UX exec'ing /etc/init demand-pages init's text page
//      (crt0 at user VA $148) from disk.  f_trap routed that PMMU instruction-
//      fetch fault through the non-retry exception_bus_error, whose same-PC
//      double-fault→HALT heuristic falsely fired when the page wasn't resident on
//      the first RTE retry → HALT → GLU reset → ROM POST hang.  The fix routes
//      PMMU instruction-fetch faults through exception_bus_error_retry.  See
//      memory project_iifx_aux_8bpp_scc_iop_hang.  This pins both fixes.
//
// Two choices make the match exact (zero tolerance):
//   1. Drive with a fixed instruction budget: scheduler.run N.  Every target
//      now runs identical VBL frame-units (trigger_vbl + one VBL-period run, via
//      scheduler_run_frame) — only the *pacing* differs — so the guest execution
//      sequence is deterministic and the framebuffer after a fixed budget is
//      bit-reproducible regardless of host timing.  scheduler.mode max just makes
//      the RAF loop batch many frame-units per tick (faster); the run_stop_event
//      still halts at exactly N instructions = exactly LOGIN_FRAME_UNITS frames,
//      so this lands on the same frame the headless integration test does.
//   2. Match the emulator FRAMEBUFFER via screen.save, not the WebGL canvas.  The
//      canvas is GPU-dependent; the framebuffer PNG is the same deterministic
//      artifact the headless integration test compares.

import { test, expect } from '../../fixtures';
import * as path from 'node:path';

const TEST_DATA = path.join(process.cwd(), 'tests', 'data');
// Absolute paths — setInputFiles/setFiles stream from disk (the 169 MB HD
// exceeds Playwright's 50 MB in-memory buffer cap, so a path is required).
const ROM_ABS = path.join(TEST_DATA, 'roms', '4147DD77-IIfx.rom');
const VROM_ABS = path.join(TEST_DATA, 'roms', '341-0868.vrom');
const HD_ABS = path.join(TEST_DATA, 'aux', 'aux_3.0.1', 'hd160-with-aux-301.img');
const VIDEO_MODE = '13in_rgb_8bpp'; // 640x480 8bpp (256 colours)
// Instruction budget to the A/UX login.  One VBL frame-unit is 166,251
// instructions; at 8bpp the boot reaches the login window later than 1bpp (the
// bigger framebuffer means more demand-paging during exec) and is bit-stable
// well before 4000 frame-units (verified identical at 4000 and 4500).  4500
// frame-units × 166,251 = 748,129,500 instructions — a whole number of
// frame-units, so the run_stop_event lands exactly on a frame boundary.
const LOGIN_FRAME_UNITS = 4500;
const LOGIN_INSTRUCTIONS = LOGIN_FRAME_UNITS * 166251; // 748,129,500

test.describe('IIfx A/UX 3.0.1 boot to login via web UI', () => {
  test('boots the A/UX HD to the graphical login and matches the baseline', async ({ page, log }) => {
    test.setTimeout(8 * 60 * 1000);

    // Auto-dismiss the "resume from checkpoint?" dialog (a quick checkpoint is
    // written on beforeunload) so we always cold-boot.
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

    // ROM upload dialog → stage the IIfx ROM.
    await page.waitForFunction(() => {
      const d = document.getElementById('rom-upload-dialog');
      return d && d.getAttribute('aria-hidden') === 'false';
    }, { timeout: 30_000 });
    await (await page.$('#rom-upload-file-input'))!.setInputFiles(ROM_ABS);

    // Config dialog → IIfx profile, 16 MB RAM, JMFB Video ROM, A/UX HD, video mode.
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

    log('[login] uploading A/UX HD (169 MB → OPFS)…');
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

    // Boot, then halt the host-time-driven run so we control execution.
    await page.click('#config-start-btn');
    await page.waitForFunction(() => (window as any).__gsBootReady === true, { timeout: 90_000 });
    await page.evaluate(() => (window as any).gsEval('scheduler.stop'));
    await page.evaluate(() => (window as any).gsEval('machine.rtc.time', ['2026-04-26T21:30:36']));
    // Max speed: let the RAF loop batch many frame-units per tick so the fixed
    // budget completes quickly.  Determinism is unaffected — batching only
    // changes how many whole frame-units a tick runs, not the guest sequence.
    await page.evaluate(() => (window as any).gsEval('scheduler.mode', ['max']));

    expect(await page.evaluate(() => (window as any).gsEval('machine.id'))).toBe('iifx');
    expect(Number(await page.evaluate(() => (window as any).gsEval('machine.screen.width')))).toBe(640);
    expect(Number(await page.evaluate(() => (window as any).gsEval('machine.screen.height')))).toBe(480);

    // Deterministically drive Mac OS → A/UX Startup → kernel → root mount → init →
    // fsck → the CommandShell login window.  scheduler.run with a fixed budget
    // starts the RAF loop; the run_stop_event halts it at exactly the budget.
    // Wait for the run to finish (scheduler.running flips false), then dump the
    // framebuffer to a PNG.
    log('[login] running to A/UX login…');
    await page.evaluate((n) => (window as any).gsEval('scheduler.run', [n]), LOGIN_INSTRUCTIONS);
    await expect
      .poll(() => page.evaluate(() => (window as any).gsEval('scheduler.running')),
            { timeout: 5 * 60 * 1000, intervals: [1000] })
      .toBe(false);
    await page.evaluate(() => (window as any).gsEval('machine.screen.save', ['/tmp/aux-login.png']));
    const png = await page.evaluate(() => Array.from((window as any).__Module.FS.readFile('/tmp/aux-login.png') as Uint8Array));

    expect(Buffer.from(png)).toMatchSnapshot('aux-login.png', { maxDiffPixels: 0, threshold: 0 });
  });
});
