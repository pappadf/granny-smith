// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// IIcx end-to-end boot test exercising the WEB UI machine-config dialog.
//
// Sister to tests/e2e/specs/iicx-boot, which boots via URL parameters
// (rom=, vrom=, fd0=, model=).  This variant goes through every
// surface a real user touches:
//
//   1. Open / (no URL params) → ROM upload dialog appears.
//   2. setInputFiles on the ROM picker → uploads IIcx.rom → config
//      dialog appears.
//   3. Change "Machine:" select to Macintosh IIcx (the IIcx ROM is
//      Universal so it offers SE/30, IIcx, IIx — picking iicx is
//      what the test is about).
//   4. Use the "Video ROM:" dropdown to upload Apple-341-0868.vrom
//      via "Upload image..." (filechooser event).  This is the row
//      that needs_vrom = true on the IIcx profile makes the dialog
//      render in the first place — the regression we're protecting.
//   5. Use the "Internal FD0:" dropdown to upload System_7_0_1.image.
//   6. Click Start → bootFromConfig runs (machine.boot iicx →
//      vrom.load → rom.load → fd insert → scheduler.run).
//   7. Wait until cpu.instr_count advances by 200 M (the System 7.0.1
//      floppy reaches a stable Finder desktop at ~140 M instructions
//      per a headless screen.match probe; 200 M gives ~40% margin).
//   8. Capture the canvas and match against the same finder.png
//      baseline as the URL-driven iicx-boot test (the underlying
//      VRAM contents are identical regardless of the boot path).

import { test, expect } from '../../fixtures';
import * as fs from 'fs';
import * as path from 'path';

const TEST_DATA = path.join(process.cwd(), 'tests', 'data');
const ROM_REL = 'roms/IIcx.rom';
const VROM_REL = 'roms/Apple-341-0868.vrom';
const FD_REL = 'systems/System_7_0_1.image';

function readMedia(relPath: string): { name: string; buffer: Buffer; } {
  const abs = path.join(TEST_DATA, relPath);
  return { name: path.basename(abs), buffer: fs.readFileSync(abs) };
}

// Run the emulator until cpu.instr_count has advanced by `delta`
// instructions (scheduler.run returns immediately in the browser
// because the emulator runs on a worker thread).
async function runForInstructions(page: import('@playwright/test').Page, log: (m: string) => void, delta: number, opts?: { timeoutMs?: number; pollMs?: number; }) {
  const timeoutMs = opts?.timeoutMs ?? 600_000;
  const pollMs = opts?.pollMs ?? 1500;
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

test.describe('IIcx Boot via web UI config dialog', () => {
  test('boots to Finder desktop after configuring IIcx through the dialog', async ({ page, log }) => {
    test.setTimeout(360_000);

    // --- 1. Cold start: navigate to the page with no URL params. ---
    // Each Playwright test gets a fresh browser context (see fixtures.ts)
    // so OPFS is empty — the ROM upload dialog appears unprompted.
    log('[iicx-web] navigating to /index.html (no URL params)');
    await page.goto('/index.html');
    await page.waitForLoadState('domcontentloaded');

    // --- 2. ROM upload dialog: upload IIcx.rom. ---
    log('[iicx-web] waiting for ROM upload dialog');
    await page.waitForFunction(() => {
      const dlg = document.getElementById('rom-upload-dialog');
      return dlg && dlg.getAttribute('aria-hidden') === 'false';
    }, { timeout: 30_000 });

    log('[iicx-web] uploading IIcx.rom');
    const rom = readMedia(ROM_REL);
    const romInput = await page.$('#rom-upload-file-input');
    await romInput!.setInputFiles({ name: rom.name, mimeType: 'application/octet-stream', buffer: rom.buffer });

    // --- 3. Config dialog: switch model to Macintosh IIcx. ---
    log('[iicx-web] waiting for config dialog');
    await page.waitForFunction(() => {
      const cfg = document.getElementById('config-dialog');
      return cfg && cfg.getAttribute('aria-hidden') === 'false';
    }, { timeout: 30_000 });

    log('[iicx-web] selecting Macintosh IIcx as the active model');
    const modelSel = page.locator('#config-model');
    await modelSel.selectOption('iicx');

    // Wait for the dialog to re-render with the IIcx-specific rows
    // (the Video ROM row only appears when the chosen profile reports
    // needs_vrom = true).
    await page.waitForFunction(() => !!document.getElementById('config-vrom'), { timeout: 10_000 });

    // --- 4. Upload Apple-341-0868.vrom via the "Video ROM:" picker. ---
    log('[iicx-web] uploading Apple-341-0868.vrom via the Video ROM picker');
    const vrom = readMedia(VROM_REL);
    {
      const fcPromise = page.waitForEvent('filechooser');
      await page.locator('#config-vrom').selectOption('__upload__');
      const fc = await fcPromise;
      await fc.setFiles({ name: vrom.name, mimeType: 'application/octet-stream', buffer: vrom.buffer });
    }
    // The dialog inserts the new option once the upload pipeline
    // finishes and selects it.  Wait for that to happen.
    await page.waitForFunction(() => {
      const sel = document.getElementById('config-vrom') as HTMLSelectElement | null;
      return !!sel && sel.value !== '' && sel.value !== '__upload__';
    }, { timeout: 30_000 });

    // --- 5. Upload the System 7.0.1 floppy via FD0 picker. ---
    log('[iicx-web] uploading System_7_0_1.image via the FD0 picker');
    const fd = readMedia(FD_REL);
    {
      const fcPromise = page.waitForEvent('filechooser');
      await page.locator('#config-fd0').selectOption('__upload__');
      const fc = await fcPromise;
      await fc.setFiles({ name: fd.name, mimeType: 'application/octet-stream', buffer: fd.buffer });
    }
    await page.waitForFunction(() => {
      const sel = document.getElementById('config-fd0') as HTMLSelectElement | null;
      return !!sel && sel.value !== '' && sel.value !== '__upload__';
    }, { timeout: 60_000 });

    // --- 6. Click Start → bootFromConfig runs. ---
    log('[iicx-web] clicking Start');
    await page.click('#config-start-btn');
    await page.waitForFunction(() => (window as any).__gsBootReady === true, { timeout: 60_000 });

    // Confirm the machine actually came up as IIcx.
    const machineId = await page.evaluate(() => (window as any).gsEval('machine.id'));
    expect(machineId).toBe('iicx');
    log(`[iicx-web] machine.id = ${machineId}`);

    // --- 7. Run to Finder desktop (~140 M instructions; budget 200 M). ---
    log('[iicx-web] running 200M instructions to reach the Finder desktop');
    await runForInstructions(page, log, 200_000_000, { timeoutMs: 240_000 });
    await page.waitForTimeout(2000); // let the canvas/rAF flush

    // --- 8. Snapshot the canvas — same baseline as the URL-driven
    //     iicx-boot test, since the underlying VRAM contents are
    //     identical regardless of how the machine got configured.
    log('[iicx-web] capturing Finder desktop screenshot');
    const finder = await page.locator('#screen').screenshot();
    expect(finder).toMatchSnapshot('iicx-finder.png', { maxDiffPixelRatio: 0.01 });
  });
});
