// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: IIcx video-mode matrix — post-shader WebGL baselines.
//
// Port of the legacy iicx-video-modes e2e (retired with the legacy UI), the
// only test anywhere that asserts what the USER sees: the WebGL renderer
// (em_video.c) applies a per-monitor CRT-response LUT in the fragment
// shader — undoing Apple's gamma pre-correction (visible as Kong's yellow
// tint in the integration references) — so the post-shader canvas pixels
// differ from the framebuffer PNGs the headless sibling
// (tests/integration/iicx-video-modes) matches. That sibling still covers
// every (monitor, depth) tuple at the framebuffer level; this spec pins the
// shader/canvas side on a representative subset (one tuple per fragment
// program family: 1 bpp, 8 bpp CLUT, the B&W portrait monitor, and Kong's
// gamma-LUT case). Set IICX_VIDEO_MODES=comma,separated,ids to override —
// the full 16-mode legacy matrix remains available that way.
//
// Mechanics mirror the legacy spec: boot each mode through the New Machine
// dialog, stop the machine, switch the scheduler to turbo, run through the
// Terminal panel to an absolute instruction count in the middle of the
// "Welcome to Macintosh" splash plateau, freeze, and screenshot the canvas at
// 100% zoom. (The machine's PRAM skips the Universal ROM's boot-drive wait,
// which used to be short-circuited here by poking $017A.) The plateau +
// maxDiffPixelRatio 0.01 absorb the wasm VBL pacing drift the legacy spec
// documents.
//
// Each mode is its own test, and the file runs them in parallel: each test
// stages the media into its own context's OPFS and cold-boots.

import { test, expect, type Page } from '../helpers/test';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';
import { terminalRun as typeLine } from '../helpers/terminal';

// Keep web2's default pacing: the splash capture relies on the paced boot
// sitting at the splash until the spec stops the machine.
test.use({ gsSpeed: null });

// Output is read right after each line: type, submit, then settle.
const terminalRun = (page: Page, line: string) =>
  typeLine(page, line, { settleMs: 250 });

const DATA = path.resolve(__dirname, '../../data');
const IICX_ROM = path.join(DATA, 'roms', 'iix-iicx-se30-97221136.rom');
const JMFB_VROM = path.join(DATA, 'roms', 'mdc-8-24-revb-d1629664.vrom');
const FD_IMAGE = path.join(DATA, 'systems', 'System_7_0_1.image');

interface VideoMode {
  id: string;
  width: number;
  height: number;
}
const ALL_MODES: VideoMode[] = [
  { id: '13in_rgb_1bpp', width: 640, height: 480 },
  { id: '13in_rgb_2bpp', width: 640, height: 480 },
  { id: '13in_rgb_4bpp', width: 640, height: 480 },
  { id: '13in_rgb_8bpp', width: 640, height: 480 },
  { id: '12in_rgb_1bpp', width: 512, height: 384 },
  { id: '12in_rgb_2bpp', width: 512, height: 384 },
  { id: '12in_rgb_4bpp', width: 512, height: 384 },
  { id: '12in_rgb_8bpp', width: 512, height: 384 },
  { id: '15in_bw_1bpp', width: 640, height: 870 },
  { id: '15in_bw_2bpp', width: 640, height: 870 },
  { id: '15in_bw_4bpp', width: 640, height: 870 },
  { id: '15in_bw_8bpp', width: 640, height: 870 },
  { id: '21in_rgb_1bpp', width: 1152, height: 870 },
  { id: '21in_rgb_2bpp', width: 1152, height: 870 },
  { id: '21in_rgb_4bpp', width: 1152, height: 870 },
  { id: '21in_rgb_8bpp', width: 1152, height: 870 },
];
// Default subset: one tuple per fragment-program family.
const DEFAULT_IDS = ['13in_rgb_1bpp', '13in_rgb_8bpp', '15in_bw_1bpp', '21in_rgb_8bpp'];
const MODES: VideoMode[] = (process.env.IICX_VIDEO_MODES?.split(',').map((s) => s.trim()) ??
  DEFAULT_IDS)
  .map((id) => ALL_MODES.find((m) => m.id === id))
  .filter((m): m is VideoMode => !!m);

// Read machine.cpu.instr_count / scheduler.running through the terminal.
// Each probe uses a fresh key so stale echoes can't satisfy the match. The
// disassembly prompt redraws the input line while running, so the echoed
// `key=…` value line is what we match, not the prompt.
let probeSeq = 0;
async function probeState(page: Page): Promise<{ instr: number; running: boolean } | null> {
  const key = `probe${++probeSeq}`;
  await terminalRun(page, `echo "${key}=\${machine.cpu.instr_count},\${scheduler.running}"`);
  await page.waitForTimeout(400);
  const text = await page.locator('.console-output').innerText();
  const m = text.match(new RegExp(`${key}=(\\d+),(true|false)`));
  if (!m) return null;
  return { instr: Number(m[1]), running: m[2] === 'true' };
}

async function currentState(page: Page): Promise<{ instr: number; running: boolean }> {
  for (let i = 0; i < 30; i++) {
    const s = await probeState(page);
    if (s) return s;
  }
  throw new Error('terminal state probe never answered');
}

// Issue `scheduler.run <budget>` and wait until the run-stop event fired
// (instr_count advanced past floor and the scheduler halted). Re-issues the
// run if the submission was dropped (instr not advancing while stopped).
async function runBudget(page: Page, budget: number): Promise<void> {
  const before = await currentState(page);
  const floor = before.instr + budget;
  await terminalRun(page, `scheduler.run ${budget}`);
  let lastInstr = before.instr;
  let stalls = 0;
  await expect
    .poll(
      async () => {
        const s = await currentState(page);
        if (!s.running && s.instr >= floor) return true;
        // Dropped submission: stopped, below floor, not advancing → re-issue.
        if (!s.running && s.instr === lastInstr) {
          if (++stalls >= 3) {
            stalls = 0;
            await terminalRun(page, `scheduler.run ${floor - s.instr}`);
          }
        } else {
          stalls = 0;
        }
        lastInstr = s.instr;
        return false;
      },
      { timeout: 180_000, intervals: [2_000] },
    )
    .toBe(true);
}

// Pick `mode` in the dialog: its monitor (catalogue id), then the startup
// mode as WxHxD.  The mode ids here name the snapshot files.
const MONITOR: Record<string, string> = { '15in_bw': '15in_portrait' };
async function pickVideoMode(page: Page, mode: VideoMode): Promise<void> {
  const [, mon, depth] = /^(.*)_(\d+)bpp$/.exec(mode.id) ?? [];
  const monitor = page.locator('#cfg-monitor');
  await expect(monitor).toBeVisible({ timeout: 30_000 });
  await monitor.selectOption(MONITOR[mon] ?? mon);
  await page.locator('#cfg-video-mode').selectOption(`${mode.width}x${mode.height}x${depth}`);
}

// One test per mode, run side by side: each test's browser context has its
// own OPFS, so it stages the media and cold-boots its own machine (no reload
// or resume prompt between modes, as there was when one test ran them all).
test.describe.configure({ mode: 'parallel' });

for (const mode of MODES) {
  test(`IIcx video mode ${mode.id}: post-shader canvas matches its baseline`, async ({ page }) => {
    test.setTimeout(10 * 60 * 1000);
    // The 15" portrait (640x870) and Kong (1152x870) canvases must fit the
    // viewport at 100% zoom for a 1:1 element screenshot.
    await page.setViewportSize({ width: 1500, height: 2000 });

    await gotoWeb2(page);
    await stageOpfsFile(page, '/opfs/images/vrom/mdc-8-24-revb-d1629664.vrom', JMFB_VROM);
    await stageOpfsFile(page, '/opfs/images/fd/System_7_0_1.image', FD_IMAGE);
    const [chooser] = await Promise.all([
      page.waitForEvent('filechooser'),
      page.getByRole('button', { name: 'Load ROM...' }).click(),
    ]);
    await chooser.setFiles(IICX_ROM);

    // New Machine: IIcx + this test's mode + the System 7.0.1 floppy.
    await page.getByRole('button', { name: 'New Machine...' }).click();
    const model = page.locator('#cfg-model');
    await expect(model.locator('option[value="iicx"]')).toHaveCount(1, { timeout: 30_000 });
    await model.selectOption('iicx');
    await pickVideoMode(page, mode);
    await page.locator('#cfg-fd0').selectOption('System_7_0_1.image');
    await page.getByRole('button', { name: 'Start', exact: true }).click();
    await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
      timeout: 60_000,
    });

    // The JMFB must land on the requested resolution (worker resizes the
    // canvas), and 100% zoom makes the element screenshot 1:1.
    await expect
      .poll(() => page.locator('#screen').evaluate((el) => (el as HTMLCanvasElement).width), {
        timeout: 30_000,
      })
      .toBe(mode.width);
    const zoom = page.getByRole('textbox', { name: 'Zoom level' });
    await zoom.fill('100%');
    await zoom.press('Enter');

    // The machine auto-runs (paced) after boot; halt it FIRST so the bounded
    // run below starts from a known stopped state -- with no startup-drive
    // wait to sit in, a turbo stretch before the stop would run straight
    // past the splash. Then turbo (a mode change only; it does not resume).
    await page.locator('button.ptab[data-tab="terminal"]').click();
    await expect(page.locator('.console')).toBeVisible({ timeout: 15_000 });
    await terminalRun(page, 'scheduler.stop');
    await expect
      .poll(async () => (await currentState(page)).running, { timeout: 15_000, intervals: [500] })
      .toBe(false);
    await page.getByRole('button', { name: 'Max', exact: true }).click();
    // Run to 49 M instructions: the splash is up from ~45 M to ~52 M
    // (measured, headless, every mode here).
    const WELCOME_AT = 49_000_000;
    const { instr } = await currentState(page);
    expect(instr, 'the paced start already ran past the Welcome splash').toBeLessThan(
      WELCOME_AT - 1_000_000,
    );
    await runBudget(page, WELCOME_AT - instr);

    const png = await page.locator('#screen').screenshot();
    expect(png).toMatchSnapshot(`welcome-${mode.id}.png`, { maxDiffPixelRatio: 0.01 });
  });
}
