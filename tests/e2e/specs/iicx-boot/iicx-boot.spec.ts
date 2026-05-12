// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// IIcx end-to-end boot test (browser/canvas-level screenshots).
//
// Mirrors the integration test at tests/integration/iicx-floppy: boot the
// IIcx with the System 7.0.1 floppy, land on the Finder desktop, then
// open "About This Macintosh" via the Apple menu.  Unlike the integration
// test (which uses screen.save against the C-side framebuffer), this
// e2e test captures pixels at the BROWSER/CANVAS level via
// page.locator('#screen').screenshot() and compares against committed
// baselines using Playwright's toMatchSnapshot.
//
// Why a separate test: the integration variant proves the core JMFB
// framebuffer is correct; this one proves the WebGL renderer + canvas
// scaling produce the right pixels in the browser end of the pipeline.

import { test, expect } from '../../fixtures';
import { bootWithMedia } from '../../helpers/boot';

const ROM_REL = 'roms/IIcx.rom';
const VROM_REL = 'roms/Apple-341-0868.vrom';
const FD_REL = 'systems/System_7_0_1.image';

// Run the emulator until cpu.instr_count has advanced by `delta`
// instructions.  In the browser, scheduler.run returns immediately
// because the worker thread runs the schedule asynchronously, so we
// have to poll the instruction count to know when execution has
// actually progressed enough.
async function runForInstructions(page: import('@playwright/test').Page, log: (m: string) => void, delta: number, opts?: { timeoutMs?: number; pollMs?: number; }) {
  const timeoutMs = opts?.timeoutMs ?? 600_000;
  const pollMs = opts?.pollMs ?? 1000;
  const startCount = (Number(await page.evaluate(() => (window as any).gsEval('cpu.instr_count'))) >>> 0);
  const target = startCount + delta;
  const start = Date.now();
  let last = startCount;
  while (Date.now() - start < timeoutMs) {
    const cur = Number(await page.evaluate(() => (window as any).gsEval('cpu.instr_count'))) >>> 0;
    if (cur >= target) {
      log(`[run-instr] reached ${cur - startCount} instructions in ${Date.now() - start}ms`);
      return cur;
    }
    last = cur;
    await page.waitForTimeout(pollMs);
  }
  throw new Error(`runForInstructions: only advanced ${last - startCount}/${delta} in ${timeoutMs}ms`);
}

// IIcx 13" RGB native: 640x480.  The browser canvas may be scaled by the
// page's CSS, but page.locator('#screen').screenshot() returns the pixels
// as rendered in the viewport — that's what we want to compare.

// NOTE on the WebGL renderer bug fixed alongside this test:
// The renderer (em_video.c) only re-uploads pixels when the producer
// marks display.fb_dirty, and previously the producer set it only on
// JMFB *register* writes (CSR/VBASE/RowWords/CLUT).  Direct VRAM
// writes — the mainline path for QuickDraw / Finder painting — never
// notified the renderer, so the canvas froze on the first post-
// PrimaryInit frame.  jmfb.c's card_on_vbl now sets fb_dirty each VBL
// so the renderer re-uploads ~60 times/sec.  Without that fix, the
// browser canvas stayed on the "Welcome to Macintosh" splash even
// though VRAM contained the fully-painted Finder desktop (verified by
// the C-side screen.save vs page.locator('#screen').screenshot()
// divergence).
test.describe('IIcx Boot (browser/canvas-level)', () => {
  test('boots to floppy/? icon (no boot media)', async ({ page, log }) => {
    test.setTimeout(360_000);

    log('[iicx-boot] booting with IIcx ROM only (no floppy/HD)');
    await bootWithMedia(page, ROM_REL, undefined, undefined, 'max', 'iicx', VROM_REL);

    // Reach the SCSI_WAIT_BSY/floppy-icon screen.  In the browser,
    // scheduler runs in a worker thread, so we have to poll for
    // instruction progress to know when execution has caught up.
    log('[iicx-boot] running 335M instructions to reach the floppy/? icon');
    await runForInstructions(page, log, 335_000_000, { timeoutMs: 240_000 });

    // Give the canvas/rAF pipeline time to flush the latest pixels to
    // the DOM canvas before we screenshot it.
    await page.waitForTimeout(2000);

    log('[iicx-boot] capturing floppy/? icon screenshot');
    const shot = await page.locator('#screen').screenshot();
    expect(shot).toMatchSnapshot('iicx-floppy-icon.png', { maxDiffPixelRatio: 0.01 });
  });

  test('boots to Finder desktop with System 7.0.1 floppy', async ({ page, log }) => {
    test.setTimeout(360_000);

    log('[iicx-boot] booting with IIcx ROM + Apple-341-0868.vrom + System 7.0.1 floppy');
    await bootWithMedia(page, ROM_REL, FD_REL, undefined, 'max', 'iicx', VROM_REL);

    // Headless probe: the System 7.0.1 floppy reaches a stable Finder
    // desktop at ~140 M instructions (screen.match pins to the first
    // post-paint frame); 200 M gives ~40% headroom for browser-side
    // wall-clock jitter without burning extra wall time.
    log('[iicx-boot] running 200M instructions to reach Finder desktop');
    await runForInstructions(page, log, 200_000_000, { timeoutMs: 240_000 });

    // Let the canvas/rAF pipeline catch up.
    await page.waitForTimeout(2000);

    log('[iicx-boot] capturing Finder desktop screenshot');
    const finderShot = await page.locator('#screen').screenshot();
    expect(finderShot).toMatchSnapshot('iicx-finder.png', { maxDiffPixelRatio: 0.01 });
  });
});
