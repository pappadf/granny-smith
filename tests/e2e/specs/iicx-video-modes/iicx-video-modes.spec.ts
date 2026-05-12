// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// IIcx video-mode matrix end-to-end test — the WebGL/UI-flow sibling of
// tests/integration/iicx-video-modes.  Cold-boots the IIcx through the
// machine-configuration dialog for every (monitor, depth) tuple the JMFB
// catalog exposes, runs to the "Welcome to Macintosh" splash in
// "fast" (max-speed) scheduler mode at 100% zoom, and matches the
// post-shader canvas screenshot against a checked-in baseline.
//
// Boot-drive wait skip.  The Universal ROM's boot-drive-discovery wait
// at $40801610 burns ~16.8 s of wall-clock time polling the drive
// queue for a key-filtered drive type that never matches the floppy.
// We short-circuit it the same way the integration test does: poke
// $8805 (Cmd-Shift-Power) into $017A (KeyMap modifier word) once the
// wait routine has been entered, then clear $017A so System 7's INIT
// loader doesn't see Cmd-Shift held.  Wasm VBL is host-time-driven
// (vs cycle-driven in headless), so the 16.8 s saving is real wall
// time, not just emulated cycles.
//
// Match-at-Welcome strategy.  The JMFB is fully configured (resolution
// / depth / CLUT / stride) by the time the splash paints, so matching
// there rather than at Finder exercises the same plumbing and cuts the
// per-mode wall time substantially.  Because wasm VBL is host-time
// driven, Mac-OS state at any given cum_instr varies slightly between
// runs (different host throughput → different VBL count at the same
// cum_instr).  We absorb the variance two ways: (a) target a cum_instr
// deep inside the splash plateau so a few-VBL drift can't push us out,
// and (b) call `scheduler.stop` immediately after hitting the target
// so the canvas freezes on a deterministic frame.
//
// Why a separate test, not an extension of the headless integration run:
//
//   * The integration test compares `screen.save` PNGs — i.e. fb bytes
//     run through the live CLUT, no monitor model.  Kong's gamma-pre-
//     corrected CLUT shows through as a yellow tint in those refs.
//
//   * The WebGL renderer (em_video.c) applies a per-monitor CRT response
//     LUT in the fragment shader, undoing Apple's gamma pre-correction
//     so the user sees a neutral image.  The post-shader pixels differ
//     from the integration refs and need their own baselines.
//
// The 2 bpp and 4 bpp baselines are deliberately solid grey: em_video.c's
// FS_GRAY_STUB is the fragment program for those formats until the JMFB
// driver work fills out their unpackers.  The integration test still
// covers correctness at those depths via its software-side
// framebuffer_row_to_rgba path; once the shader unpackers land, these
// snapshots should be regenerated with --update-snapshots.
//
// Performance.  Each iteration goes through the full dialog flow because
// every Playwright test starts in a fresh browser context (see
// fixtures.ts) — OPFS is empty, so the ROM/VROM/FD uploads are paid
// once per video-mode within the same test() invocation.  Bundling all
// modes into ONE test() lets OPFS retain media across the page.goto
// reloads, and the scheduler's max-speed mode keeps the 250 M-instruction
// boot budget short enough for the whole matrix to fit in a single
// Playwright run.

import { test, expect } from '../../fixtures';
import { runCommand, waitForPrompt } from '../../helpers/run-command';
import * as fs from 'node:fs';
import * as path from 'node:path';

const TEST_DATA = path.join(process.cwd(), 'tests', 'data');
const ROM_REL = 'roms/IIcx.rom';
const VROM_REL = 'roms/Apple-341-0868.vrom';
const FD_REL = 'systems/System_7_0_1.image';

function readMedia(rel: string) {
  const fullPath = path.join(TEST_DATA, rel);
  return { name: path.basename(fullPath), buffer: fs.readFileSync(fullPath) };
}

// Mirrors the integration matrix (4 monitors × 4 depths = 16 modes).
// `bytes` is the post-shader pixel-byte budget; the screenshot dimensions
// at 100% zoom always equal (width, height).
interface VideoMode { id: string; width: number; height: number; }
const ALL_MODES: VideoMode[] = [
  { id: '13in_rgb_1bpp',  width:  640, height: 480 },
  { id: '13in_rgb_2bpp',  width:  640, height: 480 },
  { id: '13in_rgb_4bpp',  width:  640, height: 480 },
  { id: '13in_rgb_8bpp',  width:  640, height: 480 },
  { id: '12in_rgb_1bpp',  width:  512, height: 384 },
  { id: '12in_rgb_2bpp',  width:  512, height: 384 },
  { id: '12in_rgb_4bpp',  width:  512, height: 384 },
  { id: '12in_rgb_8bpp',  width:  512, height: 384 },
  { id: '15in_bw_1bpp',   width:  640, height: 870 },
  { id: '15in_bw_2bpp',   width:  640, height: 870 },
  { id: '15in_bw_4bpp',   width:  640, height: 870 },
  { id: '15in_bw_8bpp',   width:  640, height: 870 },
  { id: '21in_rgb_1bpp',  width: 1152, height: 870 },
  { id: '21in_rgb_2bpp',  width: 1152, height: 870 },
  { id: '21in_rgb_4bpp',  width: 1152, height: 870 },
  { id: '21in_rgb_8bpp',  width: 1152, height: 870 },
];

// IICX_VIDEO_MODES env can pick a comma-separated subset (e.g. for
// smoke-testing the dialog/reload flow without paying the full ~30 min
// matrix cost).  Default: full matrix.
const MODES: VideoMode[] = (process.env.IICX_VIDEO_MODES
  ? ALL_MODES.filter(m => process.env.IICX_VIDEO_MODES!.split(',').map(s => s.trim()).includes(m.id))
  : ALL_MODES);


// Drive the toolbar zoom-out button from the 200% default down to 100% so
// the captured canvas screenshot is at the framebuffer's native resolution
// (matches the integration reference dimensions).  Mirrors the workaround
// in kong-check.spec.ts; clicking is done after the modal config dialog
// has closed because the dialog otherwise intercepts pointer events.
async function zoomTo100(page: import('@playwright/test').Page) {
  await expect(page.locator('#zoom-level')).toHaveValue('200%', { timeout: 15_000 });
  for (let i = 0; i < 10; i++) await page.click('#zoom-out');
  await expect(page.locator('#zoom-level')).toHaveValue('100%');
}

test.describe('IIcx video-mode matrix via web UI', () => {
  // Run the matrix in a single test() so OPFS uploads persist across the
  // per-mode page.goto reloads (Playwright gives each test() its own
  // browser context; OPFS is per-context).
  test('boots every (monitor, depth) and matches baselines', async ({ page, log }) => {
    // ~3 minutes per mode worst case (Kong 8bpp at max speed):
    // 16 × 3 = 48 minutes upper bound; doubled for safety.
    test.setTimeout(90 * 60 * 1000);

    // The C side (em_main.c::install_background_checkpoint_handlers) writes
    // a quick checkpoint on `beforeunload`, so every page.goto after the
    // first iteration finds one in OPFS and main.js opens a "resume from
    // checkpoint?" prompt that intercepts pointer events.  This addInitScript
    // runs in every navigated page and auto-clicks the "Start fresh" button
    // the instant the checkpoint dialog appears, restoring the cold-boot
    // flow the test expects.
    await page.addInitScript(() => {
      const dismiss = (dlg: HTMLElement) => {
        const btn = dlg.querySelector('[data-checkpoint-fresh]') as HTMLElement | null;
        if (btn) btn.click();
      };
      const observe = () => {
        const root = document.body;
        if (!root) { document.addEventListener('DOMContentLoaded', observe); return; }
        new MutationObserver(() => {
          const dlg = document.getElementById('checkpoint-dialog');
          if (dlg && dlg.getAttribute('aria-hidden') === 'false') dismiss(dlg);
        }).observe(root, { childList: true, subtree: true, attributes: true,
          attributeFilter: ['aria-hidden'] });
        // Also dismiss synchronously if the dialog is already up at the
        // time the observer starts.
        const dlg = document.getElementById('checkpoint-dialog');
        if (dlg && dlg.getAttribute('aria-hidden') === 'false') dismiss(dlg);
      };
      observe();
    });

    let firstIteration = true;
    for (const mode of MODES) {
      log(`[video-modes] === ${mode.id} (${mode.width}x${mode.height}) ===`);
      await page.goto('/index.html');
      await page.waitForLoadState('domcontentloaded');

      // The ROM-upload dialog only appears on the first iteration (before
      // any ROM has been written to OPFS).  Subsequent reloads see the
      // ROM already present and jump straight to the config dialog.
      if (firstIteration) {
        log('[video-modes] uploading ROM (first iteration)');
        await page.waitForFunction(() => {
          const dlg = document.getElementById('rom-upload-dialog');
          return dlg && dlg.getAttribute('aria-hidden') === 'false';
        }, { timeout: 30_000 });
        const rom = readMedia(ROM_REL);
        await (await page.$('#rom-upload-file-input'))!.setInputFiles({
          name: rom.name, mimeType: 'application/octet-stream', buffer: rom.buffer,
        });
      }

      // Wait for the config dialog (appears in both first and subsequent
      // iterations once the ROM is staged).
      await page.waitForFunction(() => {
        const cfg = document.getElementById('config-dialog');
        return cfg && cfg.getAttribute('aria-hidden') === 'false';
      }, { timeout: 30_000 });

      // Pick the IIcx profile — drives the dialog to re-render the
      // IIcx-specific rows including the Video ROM picker and the
      // video-mode dropdown.
      await page.locator('#config-model').selectOption('iicx');
      await page.waitForFunction(() => !!document.getElementById('config-vrom'), { timeout: 10_000 });

      // VROM and FD: upload on the first iteration; on subsequent
      // iterations the previously-uploaded file shows up in the
      // dropdown's regular-options list (its OPFS path is the value)
      // and is already selected as the default.
      if (firstIteration) {
        log('[video-modes] uploading VROM + FD (first iteration)');
        const vrom = readMedia(VROM_REL);
        {
          const fcp = page.waitForEvent('filechooser');
          await page.locator('#config-vrom').selectOption('__upload__');
          const fc = await fcp;
          await fc.setFiles({ name: vrom.name, mimeType: 'application/octet-stream', buffer: vrom.buffer });
        }
        await page.waitForFunction(() => {
          const sel = document.getElementById('config-vrom') as HTMLSelectElement | null;
          return !!sel && sel.value !== '' && sel.value !== '__upload__';
        }, { timeout: 30_000 });

        const fd = readMedia(FD_REL);
        {
          const fcp = page.waitForEvent('filechooser');
          await page.locator('#config-fd0').selectOption('__upload__');
          const fc = await fcp;
          await fc.setFiles({ name: fd.name, mimeType: 'application/octet-stream', buffer: fd.buffer });
        }
        await page.waitForFunction(() => {
          const sel = document.getElementById('config-fd0') as HTMLSelectElement | null;
          return !!sel && sel.value !== '' && sel.value !== '__upload__';
        }, { timeout: 60_000 });
      } else {
        // The dropdown defaults to "(none)" even when media is present
        // in OPFS — select the previously-persisted file explicitly by
        // finding the first non-sentinel option in the list.  The
        // sentinel values are '', '__divider__', '__upload__',
        // '__create__'; everything else is a real OPFS path.
        const SENTINELS = ['', '__divider__', '__upload__', '__create__'];
        const pick = async (selector: string, label: string) => {
          const value: string = await page.evaluate(
            ([sel, sentinels]: [string, string[]]) => {
              const el = document.querySelector(sel) as HTMLSelectElement | null;
              if (!el) return '';
              for (const opt of Array.from(el.options)) {
                if (!sentinels.includes(opt.value) && !opt.disabled) return opt.value;
              }
              return '';
            },
            [selector, SENTINELS] as [string, string[]]);
          expect(value, `${label} should have a persisted OPFS option on reload`).not.toBe('');
          await page.locator(selector).selectOption(value);
          return value;
        };
        const vromPath = await pick('#config-vrom', 'VROM');
        const fdPath = await pick('#config-fd0', 'FD0');
        log(`[video-modes] re-selected persisted media: vrom=${vromPath} fd0=${fdPath}`);
      }

      // Pick this iteration's video mode.
      await page.waitForFunction(() => !!document.getElementById('config-video-mode'), { timeout: 5_000 });
      await page.locator('#config-video-mode').selectOption(mode.id);
      const selectedMode = await page.evaluate(() =>
        (document.getElementById('config-video-mode') as HTMLSelectElement).value);
      expect(selectedMode).toBe(mode.id);

      // Boot the configured machine.
      await page.click('#config-start-btn');
      await page.waitForFunction(() => (window as any).__gsBootReady === true, { timeout: 60_000 });

      // Confirm the JMFB landed on the requested resolution.
      expect(await page.evaluate(() => (window as any).gsEval('machine.id'))).toBe('iicx');
      expect(Number(await page.evaluate(() => (window as any).gsEval('screen.width')))).toBe(mode.width);
      expect(Number(await page.evaluate(() => (window as any).gsEval('screen.height')))).toBe(mode.height);

      // Bring the toolbar zoom down to 100% so the canvas-screenshot
      // dimensions match the framebuffer (and the reference baseline).
      await zoomTo100(page);

      // Engage max-speed scheduling — the explicit reason this test
      // diverges from kong-check.spec.ts, which runs at the slower
      // "live" default to keep playback realtime.
      await page.evaluate(() => (window as any).gsEval('scheduler.mode', ['max']));

      // Drive the boot to the Welcome splash via three bounded `run N`
      // stages with wait-skip pokes in between.  Mirrors the integration
      // test's `scheduler.run` sequence in test.script (40M past wait
      // routine entry → poke $8805 → 2M poll → clear → 15M to splash
      // plateau).  The 15M final stage sits deep enough into the splash
      // plateau to absorb the wasm-specific VBL drift described in the
      // file-header.
      await runCommand(page, 'run 40000000');
      await waitForPrompt(page);
      await page.evaluate(() => (window as any).gsEval('memory.poke.w', [0x017A, 0x8805]));
      await runCommand(page, 'run 2000000');
      await waitForPrompt(page);
      await page.evaluate(() => (window as any).gsEval('memory.poke.w', [0x017A, 0]));
      await runCommand(page, 'run 15000000');
      await waitForPrompt(page);

      // Compare against the per-mode baseline.  First run with
      // `--update-snapshots` seeds the file; subsequent runs verify.
      const png = await page.locator('#screen').screenshot();
      expect(png).toMatchSnapshot(`welcome-${mode.id}.png`, { maxDiffPixelRatio: 0.01 });

      firstIteration = false;
    }
  });
});
