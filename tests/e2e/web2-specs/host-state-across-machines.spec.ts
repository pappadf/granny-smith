// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: what belongs to the page, not to a machine.
//
//   - `?speed=` is the host's pacing setting: it reaches the core before
//     anything boots (the `pacing` object needs no machine), and every machine
//     the page then builds runs under it.
//   - The page's picture of the screen follows the machine that is running:
//     the display announces its geometry when it is attached, so a second
//     machine with another screen size, and a third back at the first size,
//     each get the canvas their own screen needs -- nothing is remembered
//     from the machine before.

import { test, expect, type Page } from "@playwright/test";
import * as path from "node:path";
import { gotoWeb2, stageOpfsFile } from "../helpers/web2-fs";
import { gsEvalInPage } from "../helpers/web2-eval";

const DATA = path.resolve(__dirname, "../../data");
const ROM = path.join(DATA, "roms", "iix-iicx-se30-97221136.rom");
const ROM_OPFS = "/opfs/images/rom/97221136";

async function waitReady(page: Page): Promise<void> {
  await page.waitForFunction(
    () => {
      const w = window as { __gsReady?: boolean; __gsBootError?: string };
      return w.__gsReady === true || typeof w.__gsBootError === "string";
    },
    undefined,
    { timeout: 60_000 },
  );
  const bootError = await page.evaluate(
    () => (window as { __gsBootError?: string }).__gsBootError,
  );
  if (bootError) throw new Error(`emulator did not start: ${bootError}`);
  const cont = page.getByRole("button", { name: "Continue" });
  if (await cont.isVisible().catch(() => false)) await cont.click();
}

async function boot(page: Page, model: string): Promise<void> {
  const r = await gsEvalInPage(page, "machine.boot", {
    model,
    ram: 8192,
    rom: ROM_OPFS,
  });
  expect(r, `machine.boot ${model}`).not.toEqual(
    expect.objectContaining({ error: expect.anything() }),
  );
  await expect
    .poll(async () => gsEvalInPage(page, "machine.id"), { timeout: 30_000 })
    .toBe(model);
}

// An enum attribute's name (the core answers {enum, index}).
async function enumAt(page: Page, path: string): Promise<unknown> {
  const v = await gsEvalInPage(page, path);
  return v && typeof v === "object" && "enum" in v
    ? (v as { enum: unknown }).enum
    : v;
}

// The screen canvas's laid-out aspect ratio (CSS size follows the machine's
// screen; the backing store belongs to the worker).
async function canvasAspect(page: Page): Promise<number> {
  return page.evaluate(() => {
    const c = document.getElementById("screen") as HTMLCanvasElement | null;
    if (!c) return 0;
    const w = parseFloat(c.style.width);
    const h = parseFloat(c.style.height);
    return h > 0 ? w / h : 0;
  });
}

test("?speed= reaches the core before a machine, and the machine runs under it", async ({
  page,
}) => {
  test.setTimeout(3 * 60 * 1000);
  await page.goto("/index.html?speed=turbo");
  await waitReady(page);

  // No machine yet: the setting is the host's.
  await expect
    .poll(async () => enumAt(page, "pacing.mode"), { timeout: 30_000 })
    .toBe("turbo");

  await stageOpfsFile(page, ROM_OPFS, ROM);
  await boot(page, "se30");
  expect(await enumAt(page, "scheduler.mode")).toBe("turbo");
  expect(await enumAt(page, "pacing.mode")).toBe("turbo");
});

test("the screen follows each machine the page builds", async ({ page }) => {
  test.setTimeout(3 * 60 * 1000);
  await gotoWeb2(page);
  await stageOpfsFile(page, ROM_OPFS, ROM);

  const SE30 = 512 / 342;
  const IICX = 640 / 480;

  await boot(page, "se30");
  await expect
    .poll(() => canvasAspect(page), { timeout: 30_000 })
    .toBeCloseTo(SE30, 2);

  await boot(page, "iicx");
  await expect
    .poll(() => canvasAspect(page), { timeout: 30_000 })
    .toBeCloseTo(IICX, 2);

  // Back to the first machine's size: not a cached value from the last one.
  await boot(page, "se30");
  await expect
    .poll(() => canvasAspect(page), { timeout: 30_000 })
    .toBeCloseTo(SE30, 2);
});
