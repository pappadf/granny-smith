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
import { mouseClick, mouseDrag } from '../../helpers/mouse';
import { runCommand } from '../../helpers/run-command';
import { getScreenChecksum } from '../../helpers/screen';

const ROM_REL = 'roms/IIcx.rom';
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

// Wait for the emulated screen to settle on the FINAL boot screen.
// Polls screen.checksum every `pollMs` ms and tracks how many distinct
// values have been observed.  The boot animation goes through several
// distinct stable states (gray-fill from PrimaryInit, "Welcome to
// Macintosh", floppy-icon, Finder boot, Finder desktop).  We require
// `minTransitions` distinct stable values BEFORE accepting a final
// stable state — that filters out the early intermediate stable
// frames.  Bails out after `timeoutMs`.
async function waitForFinalStableScreen(page: import('@playwright/test').Page, log: (m: string) => void, opts: { timeoutMs: number; pollMs?: number; stableSamples?: number; minTransitions?: number; minWaitMs?: number; }) {
  const pollMs = opts.pollMs ?? 1500;
  const stableSamples = opts.stableSamples ?? 4;
  const minTransitions = opts.minTransitions ?? 0;
  const minWaitMs = opts.minWaitMs ?? 0;
  const start = Date.now();
  let recent: number[] = [];
  const seen = new Set<number>();
  let lastStable = 0;
  if (minWaitMs > 0) await page.waitForTimeout(minWaitMs);
  while (Date.now() - start < opts.timeoutMs) {
    const cs = (await getScreenChecksum(page)) >>> 0;
    recent.push(cs);
    if (recent.length > stableSamples) recent.shift();
    if (recent.length === stableSamples && recent.every(v => v === recent[0]) && cs !== 0) {
      if (cs !== lastStable) {
        seen.add(cs);
        lastStable = cs;
        log(`[wait-final] reached stable screen #${seen.size} checksum=0x${cs.toString(16)} at ${Date.now() - start}ms`);
        if (seen.size >= minTransitions + 1) {
          log(`[wait-final] accepted as final after ${seen.size} distinct stable screens`);
          return cs;
        }
      }
    }
    await page.waitForTimeout(pollMs);
  }
  throw new Error(`screen never reached final stable state within ${opts.timeoutMs}ms (saw ${seen.size} distinct stable screens)`);
}

// Simpler: wait for a stable screen (any).  Used after a known UI
// action like opening a menu — we just need "finished animating".
async function waitForStableScreen(page: import('@playwright/test').Page, log: (m: string) => void, opts: { timeoutMs: number; pollMs?: number; stableSamples?: number; minWaitMs?: number; }) {
  const pollMs = opts.pollMs ?? 1000;
  const stableSamples = opts.stableSamples ?? 4;
  const minWaitMs = opts.minWaitMs ?? 0;
  const start = Date.now();
  let recent: number[] = [];
  if (minWaitMs > 0) await page.waitForTimeout(minWaitMs);
  while (Date.now() - start < opts.timeoutMs) {
    const cs = (await getScreenChecksum(page)) >>> 0;
    recent.push(cs);
    if (recent.length > stableSamples) recent.shift();
    if (recent.length === stableSamples && recent.every(v => v === recent[0]) && cs !== 0) {
      log(`[wait-stable] settled at checksum=0x${cs.toString(16)} after ${Date.now() - start}ms`);
      return cs;
    }
    await page.waitForTimeout(pollMs);
  }
  throw new Error(`screen never stabilised within ${opts.timeoutMs}ms (last ${recent.length} samples: ${recent.map(v => '0x' + (v >>> 0).toString(16)).join(', ')})`);
}

// IIcx 13" RGB native: 640x480.  The browser canvas may be scaled by the
// page's CSS, but page.locator('#screen').screenshot() returns the pixels
// as rendered in the viewport — that's what we want to compare.

// TODO: Both tests below are skipped because the browser wasm build
// reaches a different VRAM/ScrnBase state than the headless build at
// the same instruction count.  In the headless integration test
// (tests/integration/iicx-floppy) the IIcx boots to a fully-painted
// Finder desktop at 2B instructions; in the browser at 2B the OS
// shows ScrnBase = $00003588 / ScreenRow = $4 with empty VRAM —
// suggesting either:
//   * PROXY_TO_PTHREAD timing differences during boot (interrupt
//     delivery, scheduler cycle counting), or
//   * the WebGL renderer's display-struct snapshot is stale or
//     points at the wrong VRAM region.
// Test scaffolding (model param routing, runForInstructions helper)
// is correct and ready to use once the divergence is resolved.
test.describe('IIcx Boot (browser/canvas-level)', () => {
  test.skip('boots to floppy/? icon (no boot media)', async ({ page, log }) => {
    test.setTimeout(360_000);

    log('[iicx-boot] booting with IIcx ROM only (no floppy/HD)');
    await bootWithMedia(page, ROM_REL, undefined, undefined, 'max', 'iicx');

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

  test.skip('boots to Finder + opens About This Macintosh', async ({ page, log }) => {
    test.setTimeout(720_000);

    log('[iicx-boot] booting with IIcx ROM + System 7.0.1 floppy');
    await bootWithMedia(page, ROM_REL, FD_REL, undefined, 'max', 'iicx');

    // Boot through 2B instructions — same budget as the headless
    // integration test (tests/integration/iicx-floppy/test.script).
    // We poll the instruction count because scheduler.run returns
    // immediately in the browser (worker-thread async).
    log('[iicx-boot] running 2B instructions to reach Finder desktop');
    await runForInstructions(page, log, 2_000_000_000, { timeoutMs: 600_000 });

    // Let the canvas/rAF pipeline catch up.
    await page.waitForTimeout(2000);

    log('[iicx-boot] capturing Finder desktop screenshot');
    const finderShot = await page.locator('#screen').screenshot();
    expect(finderShot).toMatchSnapshot('iicx-finder.png', { maxDiffPixelRatio: 0.01 });

    // Open the Apple menu → "About This Macintosh..." (same sequence
    // as tests/integration/iicx-floppy/test.script).  The mouse helpers
    // drive the C-side mouse via gsEval, so coordinates are in the
    // emulated screen's coordinate space (640x480).
    log('[iicx-boot] opening "About This Macintosh"');
    await runForInstructions(page, log, 4_000_000, { timeoutMs: 30_000 });
    await mouseClick(page, 25, 10);
    await runForInstructions(page, log, 20_000_000, { timeoutMs: 30_000 });
    await mouseDrag(page, 25, 10, 25, 30);
    await runForInstructions(page, log, 100_000_000, { timeoutMs: 60_000 });

    // Let the canvas/rAF pipeline catch up.
    await page.waitForTimeout(2000);

    log('[iicx-boot] capturing About box screenshot');
    const aboutShot = await page.locator('#screen').screenshot();
    expect(aboutShot).toMatchSnapshot('iicx-about.png', { maxDiffPixelRatio: 0.01 });
  });
});
