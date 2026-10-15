// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 in-browser A/B of the PowerPC translation tier (docs/internals/core/
// cpu/ppc/ppc_jit.md): the predecoded executor alone (jit.enabled=0)
// against it with the tier (=1), alternated in ONE browser session, turbo
// mode, as perf-predecode.spec.ts does for the interpreter executors.  The
// browser is where the tier is meant to run: the engine compiles the
// generated modules on its own background threads, which the Node
// measurements (scripts/bench/run.py --clock wall) cannot show.
//
// Workloads (the headless bench rows of the same names):
//   pm8500  604: System 7.6 at the desktop, 4 G instructions after power-on
//           (the Energy Saver dialog in front), mostly 68K code under the
//           ROM's emulator
//   pm6100  601, media-free: the ROM's 68K emulator in the Start Manager
//
// Results are `PERFBENCH label=<...> machine=<...> exec=<predecode|jit>
// pair=<n> mips=<...>` lines on stdout — tracked numbers, not a gate.
// PERF_PAIRS (default 3) alternating pairs, PERF_WINDOW_SECS (default 8)
// per measurement; PERF_MACHINES limits the rows (comma list).

import { test, expect, type Page } from "@playwright/test";
import * as path from "node:path";
import { gotoWeb2, stageOpfsFileStreaming } from "../helpers/web2-fs";
import { CONSOLE_OUTPUT, terminalRun as typeLine } from "../helpers/terminal";

const terminalRun = (page: Page, line: string) =>
  typeLine(page, line, { settleMs: 250 });

const DATA = path.resolve(__dirname, "../../data");
const LABEL = process.env.PERF_LABEL ?? "unlabeled";
const PAIRS = Number(process.env.PERF_PAIRS ?? 3);
const WINDOW_SECS = Number(process.env.PERF_WINDOW_SECS ?? 8);
const MACHINES = (process.env.PERF_MACHINES ?? "pm8500,pm6100").split(",");

const ROWS: Record<
  string,
  { rom: string; ram: string; hd?: string; settle: number }
> = {
  pm8500: {
    rom: "pm7500-pm8500-pm9500-96cd923d.rom",
    ram: "32 MB",
    hd: "systems/system_7_6_170mb_24ac.img",
    settle: 4_000_000_000,
  },
  pm6100: {
    rom: "pm6100-pm7100-pm8100-9feb69b3.rom",
    ram: "24 MB",
    settle: 200_000_000,
  },
};

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
    const text = await page.locator(CONSOLE_OUTPUT).innerText();
    const m = text.match(new RegExp(`${key}=(\\d+),(\\d+)`));
    if (m) return { instr: Number(m[1]), wallNs: Number(m[2]) };
  }
  throw new Error("terminal sample probe never answered");
}

async function probeString(page: Page, expr: string): Promise<string> {
  for (let attempt = 0; attempt < 30; attempt++) {
    const key = `ps${++probeSeq}`;
    await terminalRun(page, `echo "${key}=[${"$"}{${expr}}]"`);
    await page.waitForTimeout(400);
    const text = await page.locator(CONSOLE_OUTPUT).innerText();
    const m = text.match(new RegExp(`${key}=\\[([A-Za-z0-9_.-]+)\\]`));
    if (m) return m[1];
  }
  throw new Error("terminal string probe never answered");
}

async function measureOnce(page: Page): Promise<number> {
  const a = await probeSample(page);
  await page.waitForTimeout(WINDOW_SECS * 1000);
  const b = await probeSample(page);
  return (b.instr - a.instr) / ((b.wallNs - a.wallNs) / 1e9) / 1e6;
}

for (const machine of MACHINES) {
  test(`perf-jit: ${machine} predecoded vs translation tier (turbo, tracked numbers)`, async ({
    page,
  }) => {
    test.setTimeout(30 * 60 * 1000);
    const row = ROWS[machine];
    expect(row, `unknown machine ${machine}`).toBeTruthy();
    await gotoWeb2(page);
    if (row.hd)
      await stageOpfsFileStreaming(
        page,
        `/opfs/images/hd/${path.basename(row.hd)}`,
        path.join(DATA, row.hd),
      );
    const [romChooser] = await Promise.all([
      page.waitForEvent("filechooser"),
      page.getByRole("button", { name: "Load ROM..." }).click(),
    ]);
    await romChooser.setFiles(path.join(DATA, "roms", row.rom));

    await page.getByRole("button", { name: "New Machine..." }).click();
    const model = page.locator("#cfg-model");
    await expect(model.locator(`option[value="${machine}"]`)).toHaveCount(1, {
      timeout: 30_000,
    });
    await model.selectOption(machine);
    await page.locator("#cfg-opt-memory").selectOption({ label: row.ram });
    if (row.hd)
      await page
        .locator("#cfg-media-scsi-0")
        .selectOption({ label: path.basename(row.hd) });
    await page.getByRole("button", { name: "Start", exact: true }).click();
    await expect(
      page.locator(".toast .msg").filter({ hasText: "Machine started" }),
    ).toBeVisible({ timeout: 60_000 });

    await page.locator('button.ptab[data-tab="terminal"]').click();
    await expect(page.locator(".console")).toBeVisible({ timeout: 15_000 });
    expect(await probeString(page, "jit.available")).toBe("1");

    await terminalRun(page, 'scheduler.mode = "turbo"');
    expect(await probeString(page, "scheduler.mode")).toBe("turbo");
    await expect
      .poll(async () => (await probeSample(page)).instr, {
        timeout: 20 * 60 * 1000,
        intervals: [5_000],
      })
      .toBeGreaterThan(row.settle);

    const results: Record<string, number[]> = { predecode: [], jit: [] };
    for (let pair = 1; pair <= PAIRS; pair++) {
      for (const [exec, on] of [
        ["predecode", 0],
        ["jit", 1],
      ] as const) {
        await terminalRun(page, `jit.enabled = ${on}`);
        expect(await probeString(page, "jit.enabled")).toBe(String(on));
        await page.waitForTimeout(2_000);
        const mips = await measureOnce(page);
        results[exec].push(mips);
        console.log(
          `PERFBENCH label=${LABEL} machine=${machine} exec=${exec} pair=${pair} mips=${mips.toFixed(2)}`,
        );
      }
    }
    console.log(
      `PERFBENCH label=${LABEL} machine=${machine} jit compiles=${await probeString(page, "jit.compiles")} compile_us=${await probeString(page, "jit.compile_us")} deferred=${await probeString(page, "jit.deferred")}`,
    );
    for (const exec of ["predecode", "jit"] as const) {
      const sorted = [...results[exec]].sort((x, y) => x - y);
      console.log(
        `PERFBENCH label=${LABEL} machine=${machine} exec=${exec} median mips=${sorted[Math.floor(sorted.length / 2)].toFixed(2)}`,
      );
    }
    expect(Math.max(...results.predecode)).toBeGreaterThan(0.5);
    expect(Math.max(...results.jit)).toBeGreaterThan(0.5);
  });
}
