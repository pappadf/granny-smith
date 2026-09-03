// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 in-browser A/B of the two interpreter executors (docs/core/cpu/
// predecode.md): the switch cores (predecode.enabled=0) against the
// predecoded cores (=1), alternated in ONE browser session so the host,
// the JIT tier and the page are shared by both arms.  Turbo mode, the raw
// engine throughput within the RAF budget, is the number that isolates the
// executor; the accelerated governor's ladder cap would hide the gain.
//
// Workloads:
//   se30    68030 + PMMU, media-free: the ROM free-runs at the flashing-?
//           prompt (device-I/O bound: ~45 % of its instructions take a slow
//           path natively, so it understates the executor's share)
//   iicx    68030 + PMMU + 8•24 GC: System 6.0.8 booted from the Marathon
//           image and Marathon launched to its main menu — the application
//           workload of the performance proposal's bench
//   pm6100  PowerPC 601, media-free: the ROM parks in the 68K emulator's
//           SCSI scan (not in the default list: its terminal probes do not
//           answer in turbo mode in this harness)
//
// Results are `PERFBENCH label=<...> machine=<...> exec=<switch|predecode>
// pair=<n> mips=<...>` lines on stdout — tracked numbers, not a gate.
// PERF_PAIRS (default 3) alternating pairs, PERF_WINDOW_SECS (default 6)
// per measurement; PERF_MACHINES limits the rows (comma list).

import { test, expect, type Page } from "@playwright/test";
import * as path from "node:path";
import {
  gotoWeb2,
  stageOpfsFile,
  stageOpfsFileStreaming,
} from "../helpers/web2-fs";

const DATA = path.resolve(__dirname, "../../data");
const LABEL = process.env.PERF_LABEL ?? "unlabeled";
const PAIRS = Number(process.env.PERF_PAIRS ?? 3);
const WINDOW_SECS = Number(process.env.PERF_WINDOW_SECS ?? 6);
const MACHINES = (process.env.PERF_MACHINES ?? "se30,iicx").split(",");

const ROWS: Record<
  string,
  {
    rom: string;
    ram: string;
    hd?: string;
    vrom?: string;
    card?: string[];
    mode?: string[];
  }
> = {
  se30: { rom: "iix-iicx-se30-97221136.rom", ram: "8 MB" },
  iicx: {
    rom: "iix-iicx-se30-97221136.rom",
    ram: "8 MB",
    hd: "apps/marathon_8_24gc.img",
    // The 8•24 GC declares requires_vrom: its option only appears once the
    // vROM is in OPFS (the IIfx spec's pattern for the JMFB).
    vrom: "roms/824gc-v1.1-revb-d722b053.vrom",
    card: ["824gc", "8•24 GC"],
    mode: ["gc_640x480_8bpp", "gc_640x480"],
  },
  pm6100: { rom: "pm6100-pm7100-pm8100-9feb69b3.rom", ram: "24 MB" },
};

// Pick a <select> option whose value or label contains one of `needles`
// (first needle that matches wins; the option list may still be filling).
async function selectContaining(
  page: Page,
  selector: string,
  needles: string[],
): Promise<void> {
  const sel = page.locator(selector);
  await expect(sel).toBeVisible({ timeout: 30_000 });
  let value: string | null = null;
  for (let attempt = 0; attempt < 20 && value === null; attempt++) {
    value = await sel.evaluate((el: HTMLSelectElement, ns: string[]) => {
      for (const n of ns) {
        const o = Array.from(el.options).find(
          (x) => x.value.includes(n) || x.text.includes(n),
        );
        if (o) return o.value;
      }
      return null;
    }, needles);
    if (value === null) await page.waitForTimeout(500);
  }
  expect(
    value,
    `no option containing ${needles.join("|")} in ${selector}`,
  ).not.toBeNull();
  await sel.selectOption(value as string);
}

async function instrCount(page: Page): Promise<number> {
  return (await probeSample(page)).instr;
}

// The headless iicx-marathon row's launch choreography, typed into the shell.
async function launchMarathon(page: Page): Promise<void> {
  // Boot to the Finder: the headless row needs ~200 M instructions.
  await expect
    .poll(() => instrCount(page), { timeout: 240_000, intervals: [3_000] })
    .toBeGreaterThan(230_000_000);
  await terminalRun(page, 'machine.adb.mouse.move 237 157 "global"');
  await page.waitForTimeout(500);
  for (let i = 0; i < 2; i++) {
    await terminalRun(page, "machine.adb.mouse.click true");
    await terminalRun(page, "machine.adb.mouse.click false");
  }
  // Marathon's main menu is up ~85 M instructions after the double-click.
  const after = await instrCount(page);
  await expect
    .poll(() => instrCount(page), { timeout: 240_000, intervals: [3_000] })
    .toBeGreaterThan(after + 120_000_000);
}

async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator(".xterm");
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press("Enter");
  await page.waitForTimeout(250);
}

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
    const text = await page.locator(".xterm-rows").innerText();
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
    const text = await page.locator(".xterm-rows").innerText();
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
  test(`perf-predecode: ${machine} switch vs predecoded (turbo, tracked numbers)`, async ({
    page,
  }) => {
    test.setTimeout(20 * 60 * 1000);
    const row = ROWS[machine];
    expect(row, `unknown machine ${machine}`).toBeTruthy();
    await gotoWeb2(page);
    if (row.hd)
      await stageOpfsFileStreaming(
        page,
        `/opfs/images/hd/${path.basename(row.hd)}`,
        path.join(DATA, row.hd),
      );
    if (row.vrom)
      await stageOpfsFile(
        page,
        `/opfs/images/vrom/${path.basename(row.vrom)}`,
        path.join(DATA, row.vrom),
      );

    const [romChooser] = await Promise.all([
      page.waitForEvent("filechooser"),
      page.getByRole("button", { name: "Upload ROM..." }).click(),
    ]);
    await romChooser.setFiles(path.join(DATA, "roms", row.rom));

    await page.getByRole("button", { name: "New Machine..." }).click();
    const model = page.locator("#cfg-model");
    await expect(model.locator(`option[value="${machine}"]`)).toHaveCount(1, {
      timeout: 30_000,
    });
    await model.selectOption(machine);
    if (row.card) await selectContaining(page, "#cfg-card", row.card);
    if (row.mode) await selectContaining(page, "#cfg-video-mode", row.mode);
    await page.locator("#cfg-ram").selectOption(row.ram);
    if (row.hd)
      await selectContaining(page, "#cfg-hd", [path.basename(row.hd)]);
    await page.getByRole("button", { name: "Start Machine" }).click();
    await expect(
      page.locator(".toast .msg").filter({ hasText: "Machine started" }),
    ).toBeVisible({
      timeout: 60_000,
    });

    await page.locator('button.ptab[data-tab="terminal"]').click();
    await expect(page.locator(".xterm")).toBeVisible({ timeout: 15_000 });

    await terminalRun(page, 'scheduler.mode = "turbo"');
    expect(await probeString(page, "scheduler.mode")).toBe("turbo");
    // Let the ROM settle into its idle loop (and the wasm tier up) first —
    // or, with a disk, boot the System and launch the application.
    if (row.hd) await launchMarathon(page);
    else await page.waitForTimeout(8_000);

    const results: Record<string, number[]> = { switch: [], predecode: [] };
    for (let pair = 1; pair <= PAIRS; pair++) {
      for (const [exec, on] of [
        ["switch", 0],
        ["predecode", 1],
      ] as const) {
        await terminalRun(page, `predecode.enabled = ${on}`);
        expect(await probeString(page, "predecode.enabled")).toBe(String(on));
        await page.waitForTimeout(2_000);
        const mips = await measureOnce(page);
        results[exec].push(mips);
        console.log(
          `PERFBENCH label=${LABEL} machine=${machine} exec=${exec} pair=${pair} mips=${mips.toFixed(2)}`,
        );
      }
    }
    for (const exec of ["switch", "predecode"] as const) {
      const sorted = [...results[exec]].sort((x, y) => x - y);
      const median = sorted[Math.floor(sorted.length / 2)];
      console.log(
        `PERFBENCH label=${LABEL} machine=${machine} exec=${exec} median mips=${median.toFixed(2)}`,
      );
    }
    const s = results.switch.reduce((x, y) => x + y, 0) / PAIRS;
    const p = results.predecode.reduce((x, y) => x + y, 0) / PAIRS;
    console.log(
      `PERFBENCH label=${LABEL} machine=${machine} gain=${(((p - s) / s) * 100).toFixed(1)}% (mean ${s.toFixed(2)} -> ${p.toFixed(2)})`,
    );
    // Sanity only.
    expect(Math.max(...results.switch)).toBeGreaterThan(0.5);
    expect(Math.max(...results.predecode)).toBeGreaterThan(0.5);
  });
}
