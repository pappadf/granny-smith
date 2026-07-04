// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Upload reproduction / regression guard, against the LOCAL build served with
// real COOP/COEP headers (so the engine is cross-origin isolated and the WASM
// module boots without the coi-serviceworker — see
// playwright.webkit-local.config.ts).
//
// Reported (Safari): the first attempt to upload a ROM/vROM failed with the
// toast "Upload failed: <name>" and this console error:
//
//   upload: OPFS write failed
//   UnknownError: The operation failed for an unknown transient reason
//                 (e.g. out of memory).
//
// Root cause: bus/opfs.ts::writeToOPFS staged the upload with
// FileSystemFileHandle.createWritable() ON THE MAIN THREAD, then persisted with a
// worker-side storage.cp. Safari's OPFS rejects main-thread createWritable()
// ("UnknownError"); on Chromium the worker couldn't see the main-thread write
// and stranded the file in /opfs/upload.
//
// Fix: bus/upload.ts::stageUpload now streams the staging write ON THE WORKER
// (Module.FS.open/write/close, proxied to the runtime thread whose WasmFS drives
// OPFS via createSyncAccessHandle — the browser-portable OPFS write path),
// slicing the file so nothing buffers the whole thing (works for any size,
// including large CD-ROM/HD images, with no size threshold).
//
// These tests assert uploads succeed, so they FAIL if the bug returns. On
// Chromium they PASS with the fix (before it, they failed — stranded in
// /opfs/upload). Playwright's Linux WebKitGTK has NO OPFS
// (navigator.storage.getDirectory is undefined), so the webkit project SKIPS
// here — run on macOS to cover Safari.

import { test, expect, type Page } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';

// A real, in-catalog SE/30 onboard-video VROM (32 KB). Named with a space +
// parentheses to match the reported "SE30 (1).vrom".
const SE30_VROM = path.resolve(__dirname, '../../data/roms/SE30.vrom');

// Boot the app as a first-time visitor. Skips on engines without OPFS (Linux
// WebKitGTK) — there the emulator can't boot at all, which is a different issue.
async function bootIsolated(page: Page): Promise<string[]> {
  const consoleLog: string[] = [];
  page.on('console', (m) => consoleLog.push(`[${m.type()}] ${m.text()}`));
  page.on('pageerror', (e) => consoleLog.push(`[pageerror] ${e.message}`));

  await page.goto('/index.html');
  const hasOpfs = await page.evaluate(() => typeof navigator.storage?.getDirectory === 'function');
  test.skip(!hasOpfs, 'engine has no OPFS (Linux WebKitGTK) — run on macOS Safari to reproduce');

  await page.waitForFunction(
    () => (window as unknown as { __gsReady?: boolean }).__gsReady === true,
    null,
    { timeout: 60_000 },
  );
  const cont = page.getByRole('button', { name: 'Continue' });
  if (await cont.isVisible().catch(() => false)) await cont.click();
  return consoleLog;
}

// Upload one file through the real Welcome "Upload ROM..." picker.
async function uploadViaPicker(page: Page, name: string, buffer: Buffer): Promise<void> {
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await chooser.setFiles({ name, mimeType: 'application/octet-stream', buffer });
}

// Read the whole OPFS tree (read-only) to prove where the upload landed.
async function opfsFiles(page: Page): Promise<string[]> {
  return page.evaluate(async () => {
    const out: string[] = [];
    const walk = async (dir: unknown, prefix: string): Promise<void> => {
      for await (const [n, h] of (
        dir as { entries(): AsyncIterable<[string, { kind: string }]> }
      ).entries()) {
        const p = `${prefix}/${n}`;
        if (h.kind === 'directory') await walk(h, p);
        else out.push(p);
      }
    };
    await walk(await navigator.storage.getDirectory(), '/opfs');
    return out.sort();
  });
}

// Wait for the async upload pipeline (stage → probe → storage.cp → cleanup) to
// land the file under `dir` AND clear the staging area, then assert no
// upload-failure toast fired. Polling both conditions tolerates the ordering of
// the copy vs. the cleanup rm while still catching a real leak (never clears).
async function expectPersisted(page: Page, dir: string, consoleLog: string[]): Promise<void> {
  await expect
    .poll(
      async () => {
        const files = await opfsFiles(page);
        const persisted = files.some((p) => p.startsWith(`${dir}/`));
        const stagingClear = !files.some((p) => p.startsWith('/opfs/upload/'));
        return persisted && stagingClear;
      },
      {
        timeout: 30_000,
        message: `upload never persisted to ${dir} (or was left in /opfs/upload).\nConsole:\n${consoleLog.join('\n')}`,
      },
    )
    .toBe(true);
  // "Upload failed: <name>" (staging write threw) / "Failed to save <name>"
  // (worker copy failed) — neither must appear.
  await expect(
    page.locator('.toast .msg').filter({ hasText: /Upload failed|Failed to save/ }),
  ).toHaveCount(0);
}

test('Upload ROM... persists a valid vROM to /opfs/images/vrom (not stranded / not "Upload failed")', async ({
  page,
}) => {
  const consoleLog = await bootIsolated(page);
  await uploadViaPicker(page, 'SE30 (1).vrom', fs.readFileSync(SE30_VROM));
  await expectPersisted(page, '/opfs/images/vrom', consoleLog);
});

// Large media must upload too — the streaming staging path slices the file, so
// no size threshold and no heap blow-up. 12 MB exercises multiple write chunks
// (STAGE_CHUNK_BYTES is 4 MB); the mechanism is identical for a 600 MB CD-ROM.
// A raw image that isn't a ROM/vROM/floppy size classifies as a hard disk.
test('Upload ROM... streams a large (multi-chunk) image to /opfs/images/hd', async ({ page }) => {
  const consoleLog = await bootIsolated(page);
  await uploadViaPicker(page, 'big-disk.img', Buffer.alloc(12 * 1024 * 1024, 0x11));
  await expectPersisted(page, '/opfs/images/hd', consoleLog);
});
