// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 in-browser run of the interactive Mac OS X scenario
// (tests/bench/osx-interactive.script; docs/guide/BENCHMARKS.md,
// "Interactive scenarios"): Mac OS X 10.0.3 with the Aqua look on the beige
// G3 at the Finder -- open and close the File menu, minimize the Finder
// window into the Dock, restore it -- under the predecoded executor and the
// translation tier (with and without jit.supervisor).  The browser is where
// the tier is meant to run: the engine compiles its modules on background
// threads, which scripts/bench/scenario.py's Node runs charge to the cold
// round instead.
//
// Each configuration gets a fresh page (fresh translation state) that boots
// from URL parameters -- the ROM and the disk served by routes, the disk's
// sidecar configuration as config= -- settles at the Finder in turbo mode
// and plays the scenario on the running machine (MODE=live: two rounds,
// cold then warm).  No checkpoints: writing one of this 256 MB machine
// blocks the emulator's thread longer than the page's 3 s heartbeat watch,
// which then marks the emulator dead -- so the automatic checkpoint is
// turned off too, as soon as the machine exists.
//
// Results are `PERFSCEN label=<...> config=<...> run=<n> phase=<...>
// ms=<...> instr=<...>` lines on stdout -- tracked numbers, not a gate.
// PERF_RUNS (default 1) passes over the configurations; PERF_CONFIGS limits
// them (comma list of predecode, jit, jit-sup).  Needs gs-test-data at or
// after df4ebcf (the Aqua disk).  It boots Mac OS X once per configuration
// and run, so it runs only when asked:
//
//   cd tests/e2e && PERF_OSX_INTERACTIVE=1 npx playwright test \
//     --config=playwright.web2.config.ts --project=serial perf-osx-interactive.spec.ts

import { test, expect, type Page } from "@playwright/test";
import * as fs from "node:fs";
import * as path from "node:path";
import { stageOpfsText } from "../helpers/web2-fs";
import { gsEvalInPage } from "../helpers/web2-eval";
import { CONSOLE_OUTPUT, terminalRun as typeLine } from "../helpers/terminal";

const terminalRun = (page: Page, line: string) =>
  typeLine(page, line, { settleMs: 250 });

const DATA = path.resolve(__dirname, "../../data");
const ROM = path.join(DATA, "roms/pmg3dt-pmg3mt-78f57389.rom");
const DISK = path.join(DATA, "systems/mac_os_x_10_0_3_2gb_g3_minimal_aqua.dmg");
const SIDECAR = path.join(
  DATA,
  "systems/mac_os_x_10_0_3_2gb_g3_minimal_aqua.json",
);
const SCENARIO = path.resolve(__dirname, "../../bench/osx-interactive.script");

const LABEL = process.env.PERF_LABEL ?? "unlabeled";
const RUNS = Number(process.env.PERF_RUNS ?? 1);
const CONFIG_SETUP: Record<string, string> = {
  predecode: "predecode.enabled = 1\njit.enabled = 0\n",
  jit: "predecode.enabled = 1\njit.enabled = 1\njit.supervisor = 0\n",
  "jit-sup": "predecode.enabled = 1\njit.enabled = 1\njit.supervisor = 1\n",
};
const CONFIGS = (process.env.PERF_CONFIGS ?? "predecode,jit,jit-sup").split(
  ",",
);

// The link a user would open: the G3, 256 MB, the disk's own configuration
// (ATA, Apple's Gossamer nvramrc patch, the 17" monitor)
function link(): string {
  const cfg = JSON.parse(fs.readFileSync(SIDECAR, "utf8"));
  cfg.model = "pmg3dt";
  const config = Buffer.from(JSON.stringify(cfg)).toString("base64url");
  return `/index.html?rom=url-g3.rom&model=pmg3dt&memory=256MB&config=${config}&hd0=url-osx-aqua.dmg`;
}

// An attribute's value as a string, or null while it cannot be read (the
// URL boot creates the machine only after it has imported the disk)
async function attr(page: Page, p: string): Promise<string | null> {
  const v = (await gsEvalInPage(page, p).catch(() => null)) as unknown;
  if (v === null || typeof v !== "object") return v === null ? null : String(v);
  const o = v as { enum?: string; error?: string };
  return o.error ? null : (o.enum ?? JSON.stringify(v));
}

async function bootFromLink(page: Page): Promise<void> {
  for (const [suffix, file] of [
    ["url-g3.rom", ROM],
    ["url-osx-aqua.dmg", DISK],
  ]) {
    const body = fs.readFileSync(file);
    await page.route(`**/${suffix}`, (route) =>
      route.fulfill({
        status: 200,
        contentType: "application/octet-stream",
        body,
      }),
    );
  }
  await page.goto(link());
  await page.waitForFunction(
    () => (window as { __gsReady?: boolean }).__gsReady === true,
    undefined,
    {
      timeout: 120_000,
    },
  );
  // The machine exists once the scheduler answers; only then do the
  // terminal's commands find it
  await expect
    .poll(() => attr(page, "scheduler.mode"), {
      timeout: 600_000,
      intervals: [2_000],
    })
    .not.toBeNull();
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator(".console")).toBeVisible({ timeout: 15_000 });
  await terminalRun(page, "checkpoint.auto = false");
  await terminalRun(page, 'scheduler.mode = "turbo"');
  await expect
    .poll(() => attr(page, "checkpoint.auto"), { timeout: 15_000 })
    .toBe("false");
  await expect
    .poll(() => attr(page, "scheduler.mode"), { timeout: 15_000 })
    .toBe("turbo");
}

// Wait for `marker` (or a failure) in the terminal; return the whole text
// with the terminal's line wrapping undone.  On a timeout: the terminal and
// the screen, for where it stopped.
async function waitFor(
  page: Page,
  marker: string,
  timeoutMs: number,
): Promise<string> {
  try {
    await expect(page.locator(CONSOLE_OUTPUT)).toContainText(
      new RegExp(`${marker}|ASSERT FAILED|failed`),
      {
        timeout: timeoutMs,
      },
    );
  } catch (e) {
    console.log(
      `PERFSCEN timeout; terminal:\n${await page.locator(CONSOLE_OUTPUT).innerText({ timeout: 5_000 })}`,
    );
    await page.screenshot({
      path: test.info().outputPath("timeout.png"),
      timeout: 10_000,
    });
    throw e;
  }
  return (await page.locator(CONSOLE_OUTPUT).innerText()).replace(/\n/g, "");
}

test.skip(
  !process.env.PERF_OSX_INTERACTIVE,
  "opt-in: PERF_OSX_INTERACTIVE=1 (boots Mac OS X per configuration)",
);
test.skip(
  !fs.existsSync(DISK),
  "the Aqua disk is not in this checkout's test data (gs-test-data df4ebcf or later)",
);

test("perf-osx-interactive: Mac OS X menu, minimize and restore (turbo, tracked numbers)", async ({
  browser,
}) => {
  test.setTimeout(3 * 60 * 60 * 1000);
  const scenario = fs.readFileSync(SCENARIO, "utf8");
  for (let run = 1; run <= RUNS; run++) {
    for (const config of CONFIGS) {
      const setup = CONFIG_SETUP[config];
      expect(setup, `unknown configuration ${config}`).toBeTruthy();
      const page = await browser.newPage();
      await bootFromLink(page);
      await stageOpfsText(
        page,
        "/opfs/upload/osx-interactive.script",
        scenario,
      );
      const body = setup
        .trim()
        .split("\n")
        .map((l) => `    ${l}`)
        .join("\n");
      // Settle at the Finder (the scenario's MODE=boot: 10 G + 15 G slots
      // from power-on, here from wherever the link's boot has got to), then
      // play the scenario where it stands
      await stageOpfsText(
        page,
        "/opfs/upload/osx-driver.script",
        `let MODE = "live"\nlet CKPT = ""\nlet TEST_DATA = "/opfs"\ndef scen_setup() {\n${body}\n}\n` +
          `scheduler.run 25000000000\n` +
          `assert machine.screen.width == 1024 "not at the Finder's 1024 x 768"\n` +
          `include "/opfs/upload/osx-interactive.script"\n`,
      );
      await terminalRun(page, 'include "/opfs/upload/osx-driver.script"');
      const text = await waitFor(
        page,
        '"phase":"restore2"[^}]*}',
        60 * 60 * 1000,
      );
      expect(text).not.toContain("ASSERT FAILED");
      for (const m of text.matchAll(/@@SCEN (\{[^}]*\})/g)) {
        const rec = JSON.parse(m[1]) as {
          phase: string;
          ns: number;
          instr: number;
        };
        console.log(
          `PERFSCEN label=${LABEL} config=${config} run=${run} phase=${rec.phase} ms=${(rec.ns / 1e6).toFixed(0)} instr=${rec.instr}`,
        );
      }
      await page.close();
    }
  }
});
