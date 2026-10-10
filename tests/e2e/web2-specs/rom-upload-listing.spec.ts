// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: a ROM uploaded from the Welcome page appears in an already-open
// Filesystem tab.
//
// The Filesystem tab caches each directory listing it has shown, and used to
// drop those caches only for its own mutations.  An upload through the
// Welcome page's "Load ROM..." stored the ROM (and said "uploaded") while an
// expanded /opfs/images/rom kept showing its old listing — the ROM looked lost
// until the tab was switched away and back.  The upload bumps images.revision;
// the tab now drops its /opfs/images listings when that changes.

import { test, expect } from '../helpers/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile, openFilesystemTab, row, expand } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data/roms');
const PLUS_ROM = path.join(DATA, 'plus-v3-4d1f8172.rom');
const TNT_ROM = path.join(DATA, 'pm7500-pm8500-pm9500-96cd923d.rom');
const TNT_ID = '96cd923d-c241cd82bf90797a';

test('a Welcome-page ROM upload shows up in an open Filesystem tab', async ({ page }) => {
  test.setTimeout(120_000);
  await gotoWeb2(page);
  // One ROM already in the store, so /opfs/images/rom has a listing to cache.
  await stageOpfsFile(page, '/opfs/images/rom/4d1f8172', PLUS_ROM);
  await openFilesystemTab(page);
  await expand(page, 'images', 'rom');
  await expand(page, 'rom', '4d1f8172');
  await expect(row(page, TNT_ID)).toHaveCount(0);

  // Upload from the Welcome page, as a user does.
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Load ROM...' }).click(),
  ]);
  await chooser.setFiles({ name: 'tnt (2).rom', mimeType: 'application/octet-stream', buffer: fs.readFileSync(TNT_ROM) });
  await expect(page.locator('.toast .msg').filter({ hasText: 'tnt (2).rom added' })).toBeVisible({ timeout: 30_000 });

  // The open tree shows it without collapsing, re-expanding or switching tabs.
  await expect(row(page, TNT_ID)).toBeVisible({ timeout: 10_000 });
  await expect(row(page, '4d1f8172')).toBeVisible();
});
