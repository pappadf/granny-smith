// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// End-to-end coverage of the web2 Filesystem tab, exercised as a real user
// would: descend into a disk image, multi-select files, copy them out, delete
// them, then copy them again. No emulated machine is booted — the Filesystem
// tab, the object-model shell and OPFS are all live at module-ready.
//
// Everything is driven through the real UI (tab switch, twistie expands,
// shift/ctrl selection clicks, context menu, styled confirm dialog). The only
// synthetic step is the drag *gesture* (treeDrag) — Playwright/CDP cannot
// drive native HTML5 drag-and-drop — but the handlers, bus/fsOps, the worker
// and OPFS it triggers are all real.

import { test, expect } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile, row, expand, collapse, treeDrag } from '../helpers/web2-fs';

const IMAGE_HOST = path.resolve(__dirname, '../../data/systems/System_6_0_8.dsk');
const IMAGE = 'System_6_0_8.dsk';
const FD_DIR = `/opfs/images/fd/${IMAGE}`;

// Two real files at the HFS root of System_6_0_8.dsk ("System Folder" is the
// lone directory; these are files with data forks).
const FILE_A = 'TeachText';
const FILE_B = 'Read Me';

test('Filesystem tab: descend image, multi-select, copy out, delete, copy again', async ({ page }) => {
  await gotoWeb2(page);

  // Precondition: a real HFS floppy sitting in OPFS (the fixture input — the
  // same navigator.storage write the shipped upload path performs).
  await stageOpfsFile(page, FD_DIR, IMAGE_HOST);

  // Open the Filesystem tab.
  await page.locator('button.ptab[data-tab="filesystem"]').click();
  await expect(row(page, '/opfs')).toBeVisible();

  // Descend /opfs → images → fd → the image. (/opfs is expanded by default.)
  // A floppy has no partition map, so the view collapses its synthetic single
  // partition and the HFS root entries appear directly under the image node.
  await expand(page, 'images', 'fd');
  await expand(page, 'fd', IMAGE);
  await expand(page, IMAGE, FILE_A);
  await expect(row(page, 'System Folder')).toBeVisible();
  await expect(row(page, FILE_B)).toBeVisible();

  // --- Multi-select two files inside the (read-only) image -----------------
  await row(page, FILE_A).click();
  await row(page, FILE_B).click({ modifiers: ['ControlOrMeta'] });

  // --- Copy them out of the image into fd (the OPFS folder holding it) ------
  // The drag gesture is synthetic; copyOutOfImage → worker → OPFS is real.
  await treeDrag(page, row(page, FILE_A), row(page, 'fd'));
  await expect(page.locator('.toast .msg').filter({ hasText: 'Copied 2 items' })).toBeVisible();
  // Both files now exist twice: inside the (still-expanded) image and as the
  // fresh copies under fd.
  await expect(row(page, FILE_A)).toHaveCount(2);
  await expect(row(page, FILE_B)).toHaveCount(2);

  // Collapse the image so only the copies (direct children of fd) remain —
  // labels are now unambiguous.
  await collapse(page, IMAGE);
  await expect(row(page, FILE_A)).toHaveCount(1);
  await expect(row(page, FILE_B)).toHaveCount(1);

  // --- Delete the two copies via the real context menu + confirm dialog -----
  await row(page, FILE_A).click();
  await row(page, FILE_B).click({ modifiers: ['ControlOrMeta'] });
  await row(page, FILE_A).click({ button: 'right' });
  await page.locator('.context-menu .item').filter({ hasText: 'Delete 2 items' }).click();
  await page.locator('.btn.danger').filter({ hasText: 'Delete' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Deleted 2 items' })).toBeVisible();
  await expect(row(page, FILE_A)).toHaveCount(0);
  await expect(row(page, FILE_B)).toHaveCount(0);

  // --- Copy them again ------------------------------------------------------
  // Regression guard for the WasmFS/OPFS coherence fix: copying the same paths
  // out again after a delete used to fail with an I/O error.
  await expand(page, IMAGE, FILE_A); // re-expand the image to reach the sources
  await row(page, FILE_A).click();
  await row(page, FILE_B).click({ modifiers: ['ControlOrMeta'] });
  await treeDrag(page, row(page, FILE_A), row(page, 'fd'));
  await expect(row(page, FILE_A)).toHaveCount(2);
  await expect(row(page, FILE_B)).toHaveCount(2);

  // And the copies are really back under fd (image collapsed → unambiguous).
  await collapse(page, IMAGE);
  await expect(row(page, FILE_A)).toHaveCount(1);
  await expect(row(page, FILE_B)).toHaveCount(1);
});
