// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the New Machine dialog survives a floppy filename that exists in
// both /opfs/images/fd/ and the legacy /opfs/images/fdhd/.
//
// Regression guard. BrowserOpfs.scanImages('fd') folds the fdhd migration
// directory into the fd listing, so one name can arrive twice. The dialog's
// floppy dropdown keyed its <option>s by that name, and the repeat threw
// Svelte's each_key_duplicate *during the render flush* — which aborted the
// flush wholesale and left the dialog frozen on "Scanning ROMs…" even though
// the ROM scan itself had completed. The failure therefore pointed at
// /opfs/images/rom while the cause sat in /opfs/images/fd; copying disk images
// into fd out of a browsed CD-ROM was enough to trigger it.
//
// Staging writes the two files straight to OPFS (the fixture), then the ROM
// upload runs through the shipped UI so its image-revision bump drives the
// real re-scan.

import { test, expect } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsText } from '../helpers/web2-fs';

const IICX_ROM = path.resolve(__dirname, '../../data/roms/iix-iicx-se30-97221136.rom');
const DUP = 'Install 1.img';

test('a floppy name present in both fd and fdhd does not freeze the dialog', async ({ page }) => {
  const pageErrors: string[] = [];
  page.on('pageerror', (e) => pageErrors.push(e.message));
  await gotoWeb2(page);

  await stageOpfsText(page, `/opfs/images/fdhd/${DUP}`, 'legacy copy');
  await stageOpfsText(page, `/opfs/images/fd/${DUP}`, 'current copy');

  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await romChooser.setFiles(IICX_ROM);

  await page.getByRole('button', { name: 'New Machine...' }).click();

  // The scan completes: the model dropdown fills and the "Scanning ROMs…"
  // placeholder is gone.
  await expect(page.locator('#cfg-model').locator('option[value="iicx"]')).toHaveCount(1, {
    timeout: 30_000,
  });
  await expect(page.locator('.form-help', { hasText: 'Scanning ROMs' })).toHaveCount(0);

  // The duplicate collapses to one offer, and nothing threw.
  await expect(
    page.locator('select[id^="cfg-fd"]').first().locator('option', { hasText: DUP }),
  ).toHaveCount(1);
  expect(pageErrors).toEqual([]);
});
