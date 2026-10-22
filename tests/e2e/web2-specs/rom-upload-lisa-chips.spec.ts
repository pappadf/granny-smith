// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: a Lisa / Macintosh XL boot ROM uploaded as its two byte-wide chip
// dumps (#353).  Picked together through the Welcome "Load ROM..." picker the
// two 8 KB chips are interleaved, in whichever order verifies, and stored as
// the one 16 KB ROM; a chip picked on its own is refused as half a ROM.
// Before, each chip fell through to the permissive hard-disk probe and was
// stored as a tiny "hard disk".

import { test, expect, type Page } from '../helpers/test';
import { gsEvalInPage } from '../helpers/web2-eval';
import * as fs from 'node:fs';
import * as path from 'node:path';

const LISA_ROM = path.resolve(__dirname, '../../data/roms/lisa2-revh-098917b2.rom');
const STORED_ROM = '/opfs/images/rom/3f7b';

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

// Upload files through the Welcome "Load ROM..." picker, which probes them
// against every media type rather than being told what they are.
async function uploadViaPicker(page: Page, picked: Array<{ name: string; buffer: Buffer }>) {
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Load ROM...' }).click(),
  ]);
  await chooser.setFiles(picked.map((f) => ({ ...f, mimeType: 'application/octet-stream' })));
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

test('a Lisa ROM picked as its two chips is stored as one ROM; one chip alone is refused', async ({
  page,
}) => {
  test.skip(!fs.existsSync(LISA_ROM), `no Lisa ROM at ${LISA_ROM}`);
  test.setTimeout(120_000);
  await bootIsolated(page);
  const rom = fs.readFileSync(LISA_ROM);
  // 341-0175-H holds the even bytes, 341-0176-H the odd.
  const even = Buffer.from(rom.filter((_, i) => i % 2 === 0));
  const odd = Buffer.from(rom.filter((_, i) => i % 2 === 1));

  // One chip alone: refused with what it is, and nothing stored.
  await uploadViaPicker(page, [{ name: '341-0175-H.BIN', buffer: even }]);
  await expect(
    page
      .locator('.toast .msg')
      .filter({ hasText: /341-0175-H\.BIN.*one of the two chips of a Lisa.*high-byte chip/ }),
  ).toBeVisible({ timeout: 30_000 });
  expect((await opfsFiles(page)).filter((p) => p.startsWith('/opfs/images/'))).toEqual([]);

  // Both, the odd chip first: interleaved into the verified Rev H ROM.
  await uploadViaPicker(page, [
    { name: '341-0176-H.BIN', buffer: odd },
    { name: '341-0175-H.BIN', buffer: even },
  ]);
  await expect
    .poll(async () => (await opfsFiles(page)).includes(STORED_ROM), { timeout: 30_000 })
    .toBe(true);
  await expect
    .poll(async () => (await opfsFiles(page)).some((p) => p.startsWith('/opfs/upload/')), {
      timeout: 30_000,
    })
    .toBe(false);
  const files = await opfsFiles(page);
  expect(files.filter((p) => p.startsWith('/opfs/images/'))).toEqual([STORED_ROM]);
  const id = await gsEvalInPage(page, 'machine.rom.identify', [STORED_ROM]);
  expect(id).toMatchObject({ recognised: true, intact: true, id: '3f7b', compatible: ['lisa'] });
});
