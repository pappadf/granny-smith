// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The perf specs' shared measurements (perf-bench, perf-headbase, perf-jit,
// perf-predecode): an {instr_count, host_wall_ns} sample read through the
// terminal, a MIPS window over two samples, and the iicx-marathon boot and
// launch choreography.  They read through the Terminal panel on purpose:
// the sample is taken in ONE expansion inside the worker.

import { expect, type Page } from "@playwright/test";
import { terminalEcho, terminalEval, terminalRun } from "./terminal";

// {instr_count, host_wall_ns} sampled in ONE expansion inside the worker.
export async function probeSample(page: Page): Promise<{ instr: number; wallNs: number }> {
  for (let attempt = 0; attempt < 30; attempt++) {
    const m = await terminalEcho(
      page,
      "${machine.cpu.instr_count},${scheduler.host_wall_ns}",
      "(\\d+),(\\d+)",
    );
    if (m) return { instr: Number(m[1]), wallNs: Number(m[2]) };
  }
  throw new Error("terminal sample probe never answered");
}

// The guest's instruction count.
export async function instrCount(page: Page): Promise<number> {
  return (await probeSample(page)).instr;
}

// MIPS over one window of `windowSecs` on a running machine.
export async function measureOnce(page: Page, windowSecs: number): Promise<number> {
  const a = await probeSample(page);
  await page.waitForTimeout(windowSecs * 1000);
  const b = await probeSample(page);
  return (b.instr - a.instr) / ((b.wallNs - a.wallNs) / 1e9) / 1e6;
}

// Boot to the Finder and let it go idle: the Finder is the application
// (ApplZone moved off SysZone), then another 150 M instructions for the
// desktop to draw and the startup disk activity to finish.
export async function settleFinder(page: Page): Promise<void> {
  await expect
    .poll(
      () =>
        terminalEval(
          page,
          'debug.mac.globals.read("ApplZone") != debug.mac.globals.read("SysZone")',
        ),
      { timeout: 600_000, intervals: [5_000] },
    )
    .toMatch(/^(true|1)$/);
  const at = await instrCount(page);
  await expect
    .poll(() => instrCount(page), { timeout: 240_000, intervals: [3_000] })
    .toBeGreaterThan(at + 150_000_000);
}

// The headless iicx-marathon row's launch choreography, typed into the shell.
export async function launchMarathon(page: Page): Promise<void> {
  // Boot to the Finder: the headless row needs ~200 M instructions.
  await expect
    .poll(() => instrCount(page), { timeout: 240_000, intervals: [3_000] })
    .toBeGreaterThan(230_000_000);
  await terminalRun(page, 'machine.adb.mouse.move 237 157 "global"', { settleMs: 250 });
  await page.waitForTimeout(500);
  for (let i = 0; i < 2; i++) {
    await terminalRun(page, "machine.adb.mouse.click true", { settleMs: 250 });
    await terminalRun(page, "machine.adb.mouse.click false", { settleMs: 250 });
  }
  // Marathon's main menu is up ~85 M instructions after the double-click.
  const after = await instrCount(page);
  await expect
    .poll(() => instrCount(page), { timeout: 240_000, intervals: [3_000] })
    .toBeGreaterThan(after + 120_000_000);
}
