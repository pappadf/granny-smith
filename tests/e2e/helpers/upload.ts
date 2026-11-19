// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Getting a ROM into the page the way a user does: Welcome's "Load ROM..."
// button and the file picker it opens.  One copy of what most specs
// inlined.  Specs that test another route to the same outcome (a drop on
// the display, a URL) keep their own code.

import { expect, test, type FileChooser, type Page } from "@playwright/test";
import * as path from "node:path";

export interface UploadRomOptions {
  // Wait for the "added" toast: true for "<basename> added", or the text
  // to expect.  By default the upload is not awaited past the picker.
  toast?: boolean | string | RegExp;
  // How long the toast may take.
  timeoutMs?: number;
}

// What the picker can be handed: a path, paths, or in-memory files.
export type PickedFiles = Parameters<FileChooser["setFiles"]>[0];

// Click Welcome's "Load ROM..." and hand the picker `file`.
export async function uploadRomViaWelcome(
  page: Page,
  file: PickedFiles,
  opts: UploadRomOptions = {},
): Promise<void> {
  const [chooser] = await Promise.all([
    page.waitForEvent("filechooser"),
    page.getByRole("button", { name: "Load ROM..." }).click(),
  ]);
  await chooser.setFiles(file);
  if (!opts.toast) return;
  const text =
    opts.toast !== true ? opts.toast : `${typeof file === "string" ? path.basename(file) : ""} added`;
  await expect(page.locator(".toast .msg").filter({ hasText: text })).toBeVisible({
    timeout: opts.timeoutMs ?? 60_000,
  });
}

// Upload one in-memory file through the Welcome picker, which probes it
// against every media type rather than being told what it is.
export async function uploadViaPicker(page: Page, name: string, buffer: Buffer): Promise<void> {
  await uploadRomViaWelcome(page, { name, mimeType: "application/octet-stream", buffer });
}

// Boot the app as a first-time visitor, collecting the page's console.
// Skips on engines without OPFS (Linux WebKitGTK): there the emulator
// cannot boot at all, which is a different issue.
export async function bootIsolated(page: Page): Promise<string[]> {
  const consoleLog: string[] = [];
  page.on("console", (m) => consoleLog.push(`[${m.type()}] ${m.text()}`));
  page.on("pageerror", (e) => consoleLog.push(`[pageerror] ${e.message}`));

  await page.goto("/index.html");
  const hasOpfs = await page.evaluate(() => typeof navigator.storage?.getDirectory === "function");
  test.skip(!hasOpfs, "engine has no OPFS (Linux WebKitGTK); run on macOS Safari to reproduce");

  await page.waitForFunction(
    () => (window as unknown as { __gsReady?: boolean }).__gsReady === true,
    null,
    { timeout: 60_000 },
  );
  const cont = page.getByRole("button", { name: "Continue" });
  if (await cont.isVisible().catch(() => false)) await cont.click();
  return consoleLog;
}

// Every file in OPFS, as /opfs/... paths.
export async function opfsFiles(page: Page): Promise<string[]> {
  return page.evaluate(async () => {
    const out: string[] = [];
    const walk = async (dir: unknown, prefix: string): Promise<void> => {
      for await (const [n, h] of (
        dir as { entries(): AsyncIterable<[string, { kind: string }]> }
      ).entries()) {
        const p = `${prefix}/${n}`;
        if (h.kind === "directory") await walk(h, p);
        else out.push(p);
      }
    };
    await walk(await navigator.storage.getDirectory(), "/opfs");
    return out.sort();
  });
}
