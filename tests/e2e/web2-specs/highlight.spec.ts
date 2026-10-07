// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the console colours its input from shell.highlight.  An
// unresolved path segment and everything after it are `unknown`; a command
// entry keeps its colours.  Latency: with the machine running in turbo, the
// 95th percentile of 200 consecutive shell.highlight round trips stays
// within 30 ms (the budget the console's 30 ms request delay assumes).

import { test, expect, type Page } from "@playwright/test";
import * as path from "node:path";
import { gotoWeb2 } from "../helpers/web2-fs";
import { CONSOLE_INPUT, focusTerminal } from "../helpers/terminal";

const SE30_ROM = path.join(
  path.resolve(__dirname, "../../data"),
  "roms",
  "iix-iicx-se30-97221136.rom",
);

async function bootSE30(page: Page): Promise<void> {
  const [romChooser] = await Promise.all([
    page.waitForEvent("filechooser"),
    page.getByRole("button", { name: "Load ROM..." }).click(),
  ]);
  await romChooser.setFiles(SE30_ROM);
  await page.getByRole("button", { name: "New Machine..." }).click();
  const model = page.locator("#cfg-model");
  await expect(model.locator('option[value="se30"]')).toHaveCount(1, {
    timeout: 30_000,
  });
  await model.selectOption("se30");
  await page.locator("#cfg-opt-memory").selectOption("8 MB");
  await page.getByRole("button", { name: "Start", exact: true }).click();
  await expect(
    page.locator(".toast .msg").filter({ hasText: "Machine started" }),
  ).toBeVisible({
    timeout: 60_000,
  });
}

test("the console highlights paths, and the round trip fits the budget", async ({
  page,
}) => {
  test.setTimeout(180_000);
  await gotoWeb2(page);
  await bootSE30(page);
  await page.locator('button.ptab[data-tab="terminal"]').click();

  // Acceptance: `flopy` and `drive` are unknown; `machine` resolves.
  await focusTerminal(page);
  await page.keyboard.type("machine.flopy.drive");
  const input = page.locator(CONSOLE_INPUT);
  await expect(input.locator(".hl-unknown")).toHaveText(["flopy", "drive"], {
    timeout: 10_000,
  });
  await expect(input.locator(".hl-object")).toHaveText(["machine"]);

  // A correct path, an enum and a command entry that keeps its colours.
  await page.keyboard.press("Control+a");
  await page.keyboard.type("scheduler.mode = turbo");
  await expect(input.locator(".hl-enum")).toHaveText("turbo", {
    timeout: 10_000,
  });
  await expect(input.locator(".hl-attribute")).toHaveText("mode");
  await page.keyboard.press("Enter");
  const entry = page.locator(".console-output .entry.command").last();
  await expect(entry).toHaveText("scheduler.mode = turbo", { timeout: 10_000 });
  await expect(entry.locator(".hl-enum")).toHaveText("turbo");

  // Latency with the machine running flat out.
  await page.keyboard.type("scheduler.run");
  await page.keyboard.press("Enter");
  await expect
    .poll(
      () =>
        page.evaluate(() =>
          (
            window as unknown as {
              __gsEvalForTests: (p: string) => Promise<unknown>;
            }
          ).__gsEvalForTests("scheduler.running"),
        ),
      {
        timeout: 15_000,
      },
    )
    .toBe(true);
  const times = await page.evaluate(async () => {
    const ev = (
      window as unknown as {
        __gsEvalForTests: (p: string, a?: unknown[]) => Promise<unknown>;
      }
    ).__gsEvalForTests;
    const lines = [
      "machine.floppy.drive[0].insert /opfs/a.img writable=true",
      "let x = $pc + 0x10 # step",
      'if $x > 3 { echo "n=${$x:d}" }',
      "debug.breakpoints.add 0x40800000 space=physical",
    ];
    const out: number[] = [];
    for (let i = 0; i < 200; i++) {
      const t0 = performance.now();
      await ev("shell.highlight", [lines[i % lines.length]]);
      out.push(performance.now() - t0);
    }
    return out;
  });
  const sorted = [...times].sort((a, b) => a - b);
  const p95 = sorted[Math.floor(sorted.length * 0.95) - 1];
  const p50 = sorted[Math.floor(sorted.length * 0.5)];
  console.log(
    `shell.highlight round trip under turbo: p50 ${p50.toFixed(2)} ms, p95 ${p95.toFixed(2)} ms`,
  );
  expect(p95).toBeLessThanOrEqual(30);
});
