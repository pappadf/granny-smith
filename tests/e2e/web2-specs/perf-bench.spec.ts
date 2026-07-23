// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 in-browser throughput benchmark (perf proposal P5 validation / P12
// tracked number).  Boots an SE/30 with no media — the ROM free-runs at the
// flashing-? insert prompt, a busy-wait that retires instructions at full
// rate without any test data — then measures two numbers:
//
//   1. PRIMARY — Accelerated mode, engaged through the web2 toolbar exactly
//      as a user would: wait for the adaptive governor to plateau, then
//      measure effective instructions/second and the multiplier in force.
//      This is the user-facing deliverable (and what P11's knob re-tune
//      moves).
//   2. SECONDARY — turbo mode (via the shell): raw engine throughput within
//      the fixed TURBO_HOST_HEADROOM RAF budget.  Finer-grained for build-
//      flag A/Bs, and still meaningful when every variant saturates the
//      accelerated governor's ladder cap.
//
// Measurement: one `echo` samples machine.cpu.instr_count and
// scheduler.host_wall_ns in the SAME expansion inside the worker, so the
// instruction and clock samples are atomic and terminal round-trip latency
// never enters the measured interval.
//
// Results are emitted as `PERFBENCH label=<...> mode=<...> ...` lines on
// stdout — a tracked number, not a gate (assertions are sanity-only).  Label
// comes from PERF_LABEL in the environment (defaults to "unlabeled").

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const SE30_ROM = path.join(DATA, 'roms', 'iix-iicx-se30-97221136.rom');

const LABEL = process.env.PERF_LABEL ?? 'unlabeled';
const WINDOWS = Number(process.env.PERF_WINDOWS ?? 3);
const WINDOW_SECS = Number(process.env.PERF_WINDOW_SECS ?? 8);

// Type one shell line into the Terminal panel's xterm (same pattern as
// scheduler-accelerated.spec.ts).
async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(250);
}

// Atomically sample {instr_count, host_wall_ns} inside the worker via a
// single echo expansion, keyed so stale terminal echoes can't match.
let probeSeq = 0;
async function probeSample(
  page: Page,
): Promise<{ instr: number; wallNs: number }> {
  for (let attempt = 0; attempt < 30; attempt++) {
    const key = `pb${++probeSeq}`;
    await terminalRun(
      page,
      `echo "${key}=\${machine.cpu.instr_count},\${scheduler.host_wall_ns}"`,
    );
    await page.waitForTimeout(400);
    const text = await page.locator('.xterm-rows').innerText();
    const m = text.match(new RegExp(`${key}=(\\d+),(\\d+)`));
    if (m) return { instr: Number(m[1]), wallNs: Number(m[2]) };
  }
  throw new Error('terminal sample probe never answered');
}

// Read a string/number expression through the terminal (strict value pattern
// so the echoed input line can't satisfy the match).
async function probeString(page: Page, expr: string): Promise<string> {
  for (let attempt = 0; attempt < 30; attempt++) {
    const key = `ps${++probeSeq}`;
    await terminalRun(page, `echo "${key}=[${'$'}{${expr}}]"`);
    await page.waitForTimeout(400);
    const text = await page.locator('.xterm-rows').innerText();
    const m = text.match(new RegExp(`${key}=\\[([A-Za-z0-9_.-]+)\\]`));
    if (m) return m[1];
  }
  throw new Error('terminal string probe never answered');
}

// Measure `windows` intervals of WINDOW_SECS and report MIPS per window plus
// the live multiplier alongside each sample.
async function measureWindows(
  page: Page,
  modeLabel: string,
  windows: number,
): Promise<number[]> {
  const mipsPerWindow: number[] = [];
  for (let w = 1; w <= windows; w++) {
    const a = await probeSample(page);
    await page.waitForTimeout(WINDOW_SECS * 1000);
    const b = await probeSample(page);
    const mips = (b.instr - a.instr) / ((b.wallNs - a.wallNs) / 1e9) / 1e6;
    const speed = await probeString(page, 'scheduler.speed');
    mipsPerWindow.push(mips);
    console.log(
      `PERFBENCH label=${LABEL} mode=${modeLabel} window=${w} mips=${mips.toFixed(2)} speed=${speed}`,
    );
  }
  const sorted = [...mipsPerWindow].sort((x, y) => x - y);
  const median = sorted[Math.floor(sorted.length / 2)];
  console.log(
    `PERFBENCH label=${LABEL} mode=${modeLabel} median mips=${median.toFixed(2)}`,
  );
  return mipsPerWindow;
}

test('perf-bench: accelerated + turbo throughput (tracked numbers)', async ({
  page,
}) => {
  test.setTimeout(15 * 60 * 1000);
  await gotoWeb2(page);

  // SE/30 ROM via the Welcome "Upload ROM..." button; built-in video.
  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await romChooser.setFiles(SE30_ROM);

  // New Machine: SE/30, 8 MB, no media.
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
  ).toBeVisible({ timeout: 60_000 });

  // Terminal up (the typed path to the object model).
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });

  // --- PRIMARY: Accelerated via the web2 toolbar ----------------------------
  await page.getByRole('button', { name: 'accelerated', exact: true }).click();
  await expect
    .poll(async () => probeString(page, 'scheduler.mode'), { timeout: 30_000 })
    .toBe('accelerated');
  expect(await probeString(page, 'scheduler.speed_auto')).toBe('true');

  // Wait for the adaptive governor to plateau: the multiplier must hold the
  // same value across two consecutive 4 s samples (the ladder dwells ~2 s per
  // rung while climbing), or 90 s elapses.
  let lastSpeed = '';
  let stable = 0;
  const deadline = Date.now() + 90_000;
  while (Date.now() < deadline && stable < 2) {
    await page.waitForTimeout(4_000);
    const s = await probeString(page, 'scheduler.speed');
    stable = s === lastSpeed ? stable + 1 : 0;
    lastSpeed = s;
  }
  console.log(`PERFBENCH label=${LABEL} governor plateau speed=${lastSpeed}`);

  const accel = await measureWindows(page, 'accel', WINDOWS);

  // --- SECONDARY: turbo engine throughput -----------------------------------
  await terminalRun(page, 'scheduler.mode = "turbo"');
  expect(await probeString(page, 'scheduler.mode')).toBe('turbo');
  await page.waitForTimeout(3_000);
  const turbo = await measureWindows(page, 'turbo', WINDOWS);

  // P12 observability: the status bar's live MIPS readout (pushed from the
  // core ~1 Hz) must be visible while running and read a plausible number.
  const mipsChip = page.locator('.gs-statusbar .sb-mips .label');
  await expect(mipsChip).toBeVisible({ timeout: 15_000 });
  const shownMips = parseFloat(await mipsChip.innerText());
  console.log(`PERFBENCH label=${LABEL} statusbar mips=${shownMips}`);
  expect(shownMips).toBeGreaterThan(0.5);

  // Sanity only — this spec reports tracked numbers, it does not gate.
  expect(Math.max(...accel)).toBeGreaterThan(0.5);
  expect(Math.max(...turbo)).toBeGreaterThan(0.5);
});
