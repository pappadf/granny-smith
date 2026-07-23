// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Accelerated scheduler button (proposal-scheduler-accelerated-
// mode.md §9, last bullet) — the third toolbar mode switches the core to
// `accelerated`, instruction throughput rises above Real-Time (the adaptive
// governor climbing under the real RAF loop), and the *timebase* stays locked
// to real wall-clock while it does.
//
// The two rates that pin the mode's contract, both read through the shipped
// Terminal panel (web2 has no window.gsEval):
//   - scheduler.cycles per wall second: cycles ARE the guest timebase (VBL,
//     VIA φ2 and sound all divide them down), so this must sit at the
//     machine's clock rate in Real-Time AND in Accelerated — and it is what
//     separates Accelerated from Fast-Forward, where time itself runs fast.
//   - machine.cpu.instr_count per wall second: in Real-Time this is
//     cycles/CPI; Accelerated raises it while cycles/s stays put.
//
// The machine is an SE/30 with no media, free-running at the ROM's insert-
// disk prompt under the real RAF loop — a busy-wait, so instructions retire
// at full rate without booting an OS. All bounds are generous: terminal
// round-trips add ±hundreds of ms to the wall-clock windows, and CI hosts
// pace RAF with their own jitter.

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const SE30_ROM = path.join(DATA, 'roms', 'iix-iicx-se30-97221136.rom');

// SE/30: 15.6672 MHz, authentic CPI 4.
const SE30_HZ = 15_667_200;

// Type one shell line into the Terminal panel's xterm. A trailing settle
// lets the async worker round-trip land before the next interaction.
async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(250);
}

// Probe the cycle and instruction counters with a fresh key per probe so
// stale terminal echoes can't satisfy the match (same pattern as
// iicx-video-modes.spec.ts).
let probeSeq = 0;
async function probeCounters(
  page: Page,
): Promise<{ cycles: number; instr: number }> {
  for (let attempt = 0; attempt < 30; attempt++) {
    const key = `rate${++probeSeq}`;
    await terminalRun(
      page,
      `echo "${key}=\${scheduler.cycles},\${machine.cpu.instr_count}"`,
    );
    await page.waitForTimeout(400);
    const text = await page.locator('.xterm-rows').innerText();
    const m = text.match(new RegExp(`${key}=(\\d+),(\\d+)`));
    if (m) return { cycles: Number(m[1]), instr: Number(m[2]) };
  }
  throw new Error('terminal counter probe never answered');
}

// Read a string expression through the terminal (mode readbacks). The value
// pattern is strict word characters so the echoed *input* line (which still
// shows the unexpanded ${...}) can never satisfy the match — only the
// response can.
async function probeString(page: Page, expr: string): Promise<string> {
  for (let attempt = 0; attempt < 30; attempt++) {
    const key = `str${++probeSeq}`;
    await terminalRun(page, `echo "${key}=[${'$'}{${expr}}]"`);
    await page.waitForTimeout(400);
    const text = await page.locator('.xterm-rows').innerText();
    const m = text.match(new RegExp(`${key}=\\[([A-Za-z0-9_.-]+)\\]`));
    if (m) return m[1];
  }
  throw new Error('terminal string probe never answered');
}

// Wall-clock rate measurement across `seconds` of free-running emulation.
// The window is timed around the probes themselves so terminal latency lands
// inside the measured interval rather than skewing it.
async function measureRates(
  page: Page,
  seconds: number,
): Promise<{ cyclesPerSec: number; instrPerSec: number }> {
  const a = await probeCounters(page);
  const ta = Date.now();
  await page.waitForTimeout(seconds * 1000);
  const b = await probeCounters(page);
  const dt = (Date.now() - ta) / 1000;
  return {
    cyclesPerSec: (b.cycles - a.cycles) / dt,
    instrPerSec: (b.instr - a.instr) / dt,
  };
}

test('Accelerated toolbar mode: faster CPU, real-time timebase', async ({
  page,
}) => {
  test.setTimeout(10 * 60 * 1000);
  await gotoWeb2(page);

  // SE/30 ROM via the Welcome "Upload ROM..." button; built-in video, so no
  // vROM staging and no video-mode plumbing.
  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await romChooser.setFiles(SE30_ROM);

  // New Machine: SE/30, 8 MB, no media — free-runs at the flashing-? insert
  // prompt, a busy-wait that retires instructions at full rate.
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator('option[value="se30"]')).toHaveCount(1, {
    timeout: 30_000,
  });
  await model.selectOption('se30');
  await page.locator('#cfg-ram').selectOption('8 MB');
  await page.getByRole('button', { name: 'Start Machine' }).click();
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'Machine started' }),
  ).toBeVisible({
    timeout: 60_000,
  });

  // Terminal up (the typed path to the object model).
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });

  // --- Real-Time baseline ---------------------------------------------------
  // Default mode is live/paced; the cycle rate must sit at the SE/30's clock
  // and instructions at cycles/CPI (authentic 4).
  expect(await probeString(page, 'scheduler.mode')).toBe('paced');
  const live = await measureRates(page, 6);
  expect(live.cyclesPerSec).toBeGreaterThan(SE30_HZ * 0.5);
  expect(live.cyclesPerSec).toBeLessThan(SE30_HZ * 1.5);
  expect(live.cyclesPerSec / live.instrPerSec).toBeGreaterThan(3.6); // ~CPI 4
  expect(live.cyclesPerSec / live.instrPerSec).toBeLessThan(4.4);

  // --- Switch to Accelerated -------------------------------------------------
  await page.getByRole('button', { name: 'accelerated', exact: true }).click();
  await expect
    .poll(async () => probeString(page, 'scheduler.mode'), { timeout: 30_000 })
    .toBe('accelerated');
  expect(await probeString(page, 'scheduler.speed_auto')).toBe('true');

  // The adaptive governor climbs one quantized rung per ~2 s dwell as long as
  // the host shows headroom; wait until throughput clears 1.4x the Real-Time
  // baseline (first rung is 1.5x, so this is one climb with margin).
  await expect
    .poll(async () => (await measureRates(page, 3)).instrPerSec, {
      timeout: 120_000,
    })
    .toBeGreaterThan(live.instrPerSec * 1.4);

  // --- The §9 property, measured in one window -------------------------------
  // CPU-bound throughput up, timebase unchanged: instructions per real second
  // beat Real-Time while cycles per real second stay at the machine's clock.
  const accel = await measureRates(page, 6);
  expect(accel.instrPerSec).toBeGreaterThan(live.instrPerSec * 1.4);
  expect(accel.cyclesPerSec).toBeGreaterThan(live.cyclesPerSec * 0.7);
  expect(accel.cyclesPerSec).toBeLessThan(live.cyclesPerSec * 1.3);

  // --- Status-bar readout ----------------------------------------------------
  // The core pushes the governed multiplier (onSchedulerSpeed); the status bar
  // shows it only in Accelerated mode. It must now read the climbed speed
  // (> 1x — matching the throughput rise above).
  const speed = page.locator('.gs-statusbar .sb-speed .label');
  await expect(speed).toBeVisible({ timeout: 10_000 });
  const shown = parseFloat((await speed.innerText()).replace('×', ''));
  expect(shown).toBeGreaterThan(1);

  // --- Back to Real-Time ------------------------------------------------------
  await page.getByRole('button', { name: 'real-time', exact: true }).click();
  await expect
    .poll(async () => probeString(page, 'scheduler.mode'), { timeout: 30_000 })
    .toBe('paced');
  // Leaving Accelerated hides the readout.
  await expect(speed).toBeHidden({ timeout: 10_000 });
});
