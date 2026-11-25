// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The screen-recorder specs' shared steps: boot a Plus and run it, start a
// recording from the display toolbar, wait for frames, read an OPFS file's
// first bytes.  See docs/internals/core/record/record.md.

import { expect, type Page } from "./test";
import * as path from "node:path";
import { gotoWeb2, stageOpfsFile } from "./web2-fs";
import { gsCallInPage, gsEvalInPage } from "./web2-eval";
import { ROMS } from "./paths";

const PLUS_ROM = path.join(ROMS, "plus-v3-4d1f8172.rom");
const ROM_PATH = "/opfs/images/rom/plus-v3-4d1f8172.rom";

// Boot a Plus and run it, so the page shows a running machine.
// `beforeRun` runs between the boot and the first instruction (a recording
// started there holds the boot beep).
export async function bootPlus(
  page: Page,
  beforeRun?: () => Promise<void>,
): Promise<void> {
  await gotoWeb2(page);
  await stageOpfsFile(page, ROM_PATH, PLUS_ROM);
  await gsCallInPage(page, "machine.boot", {
    model: "plus",
    ram: 4096,
    rom: ROM_PATH,
  });
  // A hidden page writes a background checkpoint too; not these tests' subject
  await gsCallInPage(page, "checkpoint.auto", [false]);
  await beforeRun?.();
  await gsCallInPage(page, "scheduler.run");
  await expect(page.locator(".gs-statusbar")).toHaveAttribute(
    "data-state",
    "running",
    {
      timeout: 30_000,
    },
  );
}

// Start a recording from the record button's menu; the path being written.
export async function startFromToolbar(page: Page): Promise<string> {
  const btn = page.locator(".record-btn");
  await expect(btn).toBeEnabled();
  await btn.click();
  await page
    .locator(".context-menu .item", { hasText: "Start Recording" })
    .click();
  await expect(page.getByTestId("recording")).toBeVisible({ timeout: 15_000 });
  await expect(btn).toHaveAttribute("data-recording", "");
  return (await gsEvalInPage(page, "recorder.path")) as string;
}

// Wait until the recording has `n` more frames than now.
export async function recordFrames(page: Page, n: number): Promise<number> {
  const from = (await gsEvalInPage(page, "recorder.frames")) as number;
  await expect
    .poll(async () => (await gsEvalInPage(page, "recorder.frames")) as number, {
      timeout: 60_000,
    })
    .toBeGreaterThan(from + n);
  return (await gsEvalInPage(page, "recorder.frames")) as number;
}

// The first bytes of an OPFS file, read by the page itself.
export async function opfsHead(
  page: Page,
  file: string,
  n = 4,
): Promise<number[]> {
  return page.evaluate(
    async ({ file, n }) => {
      const rel = file.replace(/^\/opfs\/?/, "").split("/");
      const name = rel.pop() as string;
      let dir = await navigator.storage.getDirectory();
      for (const part of rel) dir = await dir.getDirectoryHandle(part);
      const f = await (await dir.getFileHandle(name)).getFile();
      return Array.from(new Uint8Array(await f.slice(0, n).arrayBuffer()));
    },
    { file, n },
  );
}
