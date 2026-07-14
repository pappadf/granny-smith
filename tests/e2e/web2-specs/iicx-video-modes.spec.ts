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
// dialog, switch the scheduler to `fast`, drive three bounded scheduler.run
// stages through the Terminal panel (poking $017A to short-circuit the
// Universal ROM's ~17 s boot-drive-discovery wait), land deep in the
// "Welcome to Macintosh" splash plateau, freeze, and screenshot the canvas
// at 100% zoom. The plateau + maxDiffPixelRatio 0.01 absorb the wasm VBL
// pacing drift the legacy spec documents.
//
// One test() runs all modes so OPFS media (ROM/vROM/FD) persist across the
// per-mode page reloads; each reload after the first answers the
// checkpoint-resume prompt with "Start fresh" to keep the cold-boot flow.

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const IICX_ROM = path.join(DATA, 'roms', 'IIcx.rom');
const JMFB_VROM = path.join(DATA, 'roms', 'Apple-341-0868.vrom');
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

// Type one shell line into the Terminal panel's xterm. A trailing settle
// lets the async worker round-trip land before the next interaction.
async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(250);
}

// Read machine.cpu.instr_count / scheduler.running through the terminal.
// Each probe uses a fresh key so stale echoes can't satisfy the match. The
// disassembly prompt redraws the input line while running, so the echoed
// `key=…` value line is what we match, not the prompt.
let probeSeq = 0;
async function probeState(page: Page): Promise<{ instr: number; running: boolean } | null> {
  const key = `probe${++probeSeq}`;
  await terminalRun(page, `echo ${key}=\${machine.cpu.instr_count},\${scheduler.running}`);
  await page.waitForTimeout(400);
  const text = await page.locator('.xterm-rows').innerText();
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

// Poke a KeyMap modifier word. Can't be verified by read-back: on the IIcx
// (68030 PMMU) the shell's peek path translates low addresses differently
// from where poke lands, so peek.w always reads 0 here — yet the poke does
// reach the ROM's KeyMap read (the integration test relies on exactly this
// to skip the boot-drive wait). We issue it against the stable stopped
// prompt (runBudget leaves the machine halted), where a single submission
// is reliable.
async function pokeKeymap(page: Page, value: number): Promise<void> {
  await terminalRun(page, `machine.memory.poke.w 0x017A ${value}`);
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

test('IIcx video modes: post-shader canvas matches per-mode baselines', async ({ page }) => {
  test.setTimeout(30 * 60 * 1000);
  // The 15" portrait (640x870) and Kong (1152x870) canvases must fit the
  // viewport at 100% zoom for a 1:1 element screenshot.
  await page.setViewportSize({ width: 1500, height: 2000 });

  let first = true;
  for (const mode of MODES) {
    if (first) {
      await gotoWeb2(page);
      await stageOpfsFile(page, '/opfs/images/vrom/Apple-341-0868.vrom', JMFB_VROM);
      await stageOpfsFile(page, '/opfs/images/fd/System_7_0_1.image', FD_IMAGE);
      const [chooser] = await Promise.all([
        page.waitForEvent('filechooser'),
        page.getByRole('button', { name: 'Upload ROM...' }).click(),
      ]);
      await chooser.setFiles(IICX_ROM);
      first = false;
    } else {
      // Reload for a cold boot; the previous machine's beforeunload quick
      // checkpoint triggers the resume prompt — decline it.
      await page.reload();
      await page.waitForFunction(
        () => (window as { __gsReady?: boolean }).__gsReady === true,
        undefined,
        { timeout: 60_000 },
      );
      // The resume prompt only appears if a quick checkpoint was persisted
      // on the prior page's beforeunload; decline it if present.
      const fresh = page.getByRole('button', { name: 'Start fresh' });
      if (await fresh.isVisible().catch(() => false)) await fresh.click();
      else await expect(page.locator('.welcome-layer')).toBeVisible({ timeout: 15_000 });
    }

    // New Machine: IIcx + this iteration's mode + the System 7.0.1 floppy.
    await page.getByRole('button', { name: 'New Machine...' }).click();
    const model = page.locator('#cfg-model');
    await expect(model.locator('option[value="iicx"]')).toHaveCount(1, { timeout: 30_000 });
    await model.selectOption('iicx');
    const videoMode = page.locator('#cfg-video-mode');
    await expect(videoMode).toBeVisible({ timeout: 30_000 });
    await videoMode.selectOption(mode.id);
    await page.locator('#cfg-fd0').selectOption('System_7_0_1.image');
    await page.getByRole('button', { name: 'Start Machine' }).click();
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

    // Turbo (unthrottled) batching, then the three bounded run stages with
    // the boot-drive-wait skip pokes (same mechanics as the integration test).
    await page.getByRole('button', { name: 'turbo', exact: true }).click();
    await page.locator('button.ptab[data-tab="terminal"]').click();
    await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });
    // The machine auto-runs after boot; halt it so the bounded run stages
    // below start from a known stopped state.
    await terminalRun(page, 'scheduler.stop');
    await expect
      .poll(async () => (await currentState(page)).running, { timeout: 15_000, intervals: [500] })
      .toBe(false);
    await runBudget(page, 40_000_000);
    await pokeKeymap(page, 0x8805);
    await runBudget(page, 2_000_000);
    await pokeKeymap(page, 0);
    await runBudget(page, 15_000_000);

    const png = await page.locator('#screen').screenshot();
    expect(png).toMatchSnapshot(`welcome-${mode.id}.png`, { maxDiffPixelRatio: 0.01 });
  }
});
