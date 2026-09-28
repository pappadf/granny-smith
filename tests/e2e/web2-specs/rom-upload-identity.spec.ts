// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: a CPU ROM is stored under its content id, and a damaged dump of a
// known ROM is refused with the reason instead of being stored.
//
// A ROM's id is the checksum field(s) it carries (rom.identify).  For a 4 MiB
// Old World PowerPC ROM that is the header sum plus the ConfigInfo 64-bit sum,
// because the header sum covers only the first 3 MiB: before content ids a
// dump damaged in its last megabyte verified, was stored under the good
// ROM's name and overwrote it.  Now the damaged copy still identifies as the
// TNT ROM it claims to be, fails its own 64-bit sum, and the upload says so —
// it must not fall through to the permissive hard-disk probe either.

import { test, expect, type Page } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';

const TNT_ROM = path.resolve(__dirname, '../../data/roms/pm7500-pm8500-pm9500-96cd923d.rom');
const STORED_ROM = '/opfs/images/rom/96cd923d-c241cd82bf90797a';

// Boot the app as a first-time visitor.
async function bootIsolated(page: Page): Promise<void> {
  await page.goto('/index.html');
  const hasOpfs = await page.evaluate(() => typeof navigator.storage?.getDirectory === 'function');
  test.skip(!hasOpfs, 'engine has no OPFS');
  await page.waitForFunction(
    () => (window as unknown as { __gsReady?: boolean }).__gsReady === true,
    null,
    { timeout: 60_000 },
  );
  const cont = page.getByRole('button', { name: 'Continue' });
  if (await cont.isVisible().catch(() => false)) await cont.click();
}

// Upload one file through the Welcome "Upload ROM..." picker, which probes it
// against every media type rather than being told what it is.
async function uploadViaPicker(page: Page, name: string, buffer: Buffer): Promise<void> {
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await chooser.setFiles({ name, mimeType: 'application/octet-stream', buffer });
}

// Every file in OPFS, as /opfs/... paths.
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

test('a ROM is stored by content id; a damaged dump of it is refused, not stored', async ({ page }) => {
  test.setTimeout(120_000);
  await bootIsolated(page);
  const good = fs.readFileSync(TNT_ROM);

  // The good dump lands under its full content id.
  await uploadViaPicker(page, 'tnt.rom', good);
  await expect
    .poll(async () => (await opfsFiles(page)).includes(STORED_ROM), { timeout: 30_000 })
    .toBe(true);

  // One byte flipped in the PowerPC megabyte: the header sum still verifies,
  // the ConfigInfo 64-bit sum does not.
  const damaged = Buffer.from(good);
  damaged[0x3f0000] ^= 0xff;
  await uploadViaPicker(page, 'tnt-damaged.rom', damaged);
  await expect(
    page.locator('.toast .msg').filter({ hasText: /tnt-damaged\.rom.*PowerPC section does not verify.*damaged/ }),
  ).toBeVisible({ timeout: 30_000 });

  // Nothing new was stored: not over the good ROM, not as a hard disk, and the
  // staging area is clean.
  await expect
    .poll(async () => (await opfsFiles(page)).some((p) => p.startsWith('/opfs/upload/')), { timeout: 30_000 })
    .toBe(false);
  const files = await opfsFiles(page);
  expect(files.filter((p) => p.startsWith('/opfs/images/rom/'))).toEqual([STORED_ROM]);
  expect(files.filter((p) => p.startsWith('/opfs/images/hd/'))).toEqual([]);
  const stored = await page.evaluate(async (p) => {
    let dir = await navigator.storage.getDirectory();
    const parts = p.replace('/opfs/', '').split('/');
    for (const d of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(d);
    const file = await (await dir.getFileHandle(parts[parts.length - 1])).getFile();
    return new Uint8Array(await file.slice(0x3f0000, 0x3f0001).arrayBuffer())[0];
  }, STORED_ROM);
  expect(stored).toBe(good[0x3f0000]);
});
