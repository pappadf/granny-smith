// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The virtual ImageWriter's documents reach the page from the core: the
// printer rasterises each job and writes its PDF itself (src/core/printer),
// and the platform hands the bytes over the download path marked as a
// document (em_main.c printer_sink_document), which the page opens in the
// print viewer (bus/download.ts) -- the same viewer the LaserWriter uses,
// titled with the printer's name.
//
// The job is a small synthetic stream -- a line of draft text and a graphics
// band -- fed to the printer directly, so the test needs no guest driver
// (the integration rows lisa-imagewriter and mac-imagewriter print from
// real ones).

import { test, expect } from "@playwright/test";
import * as path from "node:path";
import { gotoWeb2, stageOpfsFile, stageOpfsText } from "../helpers/web2-fs";
import { terminalRun } from "../helpers/terminal";

const DATA = path.resolve(__dirname, "../../data");
const ROM = path.join(DATA, "roms", "iix-iicx-se30-97221136.rom");
// Pica, a line of text, then 64 graphics columns of a diagonal, and a form feed
const JOB =
  "\x1bNHello from the ImageWriter\r\n" +
  "\x1bG0064" +
  "\x01\x02\x04\x08\x10\x20\x40\x7f".repeat(8) +
  "\r\n\x0c";

test("an ImageWriter job opens in the print viewer", async ({ page }) => {
  test.setTimeout(3 * 60 * 1000);
  // The viewer path is the desktop browser's; force it on in headless Chromium
  await page.addInitScript(() => {
    Object.defineProperty(Navigator.prototype, "pdfViewerEnabled", {
      get: () => true,
    });
  });
  await gotoWeb2(page);
  await stageOpfsFile(page, "/opfs/images/rom/97221136", ROM);
  await stageOpfsText(page, "/opfs/upload/job.iw", JOB);

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator(".console")).toBeVisible({ timeout: 15_000 });
  await terminalRun(
    page,
    'machine.boot model="iicx" ram=8192 rom="/opfs/images/rom/97221136"',
  );
  await page.waitForTimeout(2_000);

  await terminalRun(
    page,
    'machine.imagewriter.feed_file "/opfs/upload/job.iw"',
  );
  await terminalRun(page, "machine.imagewriter.eject");

  const viewer = page.locator('.modal-card.wide[role="dialog"]');
  await expect(viewer).toBeVisible({ timeout: 60_000 });
  await expect(viewer.locator(".modal-title")).toHaveText(
    /^ImageWriter II: Print \(1 page\)$/,
  );
  await expect(viewer.locator("iframe.pdf-frame")).toHaveAttribute(
    "src",
    /^blob:/,
  );
  await expect(viewer.locator("a[download]")).toHaveAttribute(
    "download",
    "imagewriter2-00001-Print.pdf",
  );
  await viewer.getByRole("button", { name: "Close" }).click();
  await expect(viewer).toBeHidden();
});
