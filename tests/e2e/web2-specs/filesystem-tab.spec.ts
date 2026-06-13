// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// End-to-end coverage of the web2 Filesystem tab — every operation this branch
// added — exercised as a real user would. No emulated machine is booted: the
// Filesystem tab, the object-model shell and OPFS are all live at module-ready.
//
// Everything is driven through the real UI (tab switch, twistie expands,
// shift/ctrl selection clicks, context menu, rename + confirm dialogs, host
// downloads). The only synthetic step is the drag *gesture* (treeDrag /
// dropFileOnRow) — Playwright/CDP cannot drive native HTML5 drag-and-drop — but
// the handlers, bus/fsOps, the worker and OPFS it triggers are all real.

import { test, expect, type Download } from '@playwright/test';
import * as path from 'node:path';
import {
  gotoWeb2,
  stageOpfsFile,
  stageOpfsText,
  mkdirOpfs,
  openFilesystemTab,
  row,
  expand,
  collapse,
  treeDrag,
  dropFileOnRow,
} from '../helpers/web2-fs';

const IMAGE_HOST = path.resolve(__dirname, '../../data/systems/System_6_0_8.dsk');
const ARCHIVE_HOST = path.resolve(__dirname, '../../data/apps/MacTest_Disk.image_.sit_.hqx');
const IMAGE = 'System_6_0_8.dsk';
const FD = `/opfs/images/fd/${IMAGE}`;
const UPLOAD = '/opfs/upload';

// Real files at the HFS root of System_6_0_8.dsk. "System Folder" is the lone
// directory; the rest are files with data forks. The view sorts directories
// first then files alpha, so the file run is: Apple HD SC Setup, Desktop,
// Installer, Installer Script, Read Me, TeachText.
const FILE_A = 'TeachText';
const FILE_B = 'Read Me';

// Descend /opfs → images → fd → the image. A floppy has no partition map, so
// the view collapses its synthetic single partition and the HFS root entries
// appear directly under the image node. (/opfs is expanded by default.)
async function descendIntoImage(page: import('@playwright/test').Page): Promise<void> {
  await expand(page, 'images', 'fd');
  await expand(page, 'fd', IMAGE);
  await expand(page, IMAGE, FILE_A);
}

test.beforeEach(async ({ page }) => {
  await gotoWeb2(page);
});

test('descend image, ctrl-multi-select, copy out, delete, copy again', async ({ page }) => {
  await stageOpfsFile(page, FD, IMAGE_HOST);
  await openFilesystemTab(page);
  await descendIntoImage(page);
  await expect(row(page, 'System Folder')).toBeVisible();

  // Multi-select two files inside the (read-only) image with ctrl-click.
  await row(page, FILE_A).click();
  await row(page, FILE_B).click({ modifiers: ['ControlOrMeta'] });

  // Copy them out of the image into fd (the OPFS folder holding it).
  await treeDrag(page, row(page, FILE_A), row(page, 'fd'));
  await expect(page.locator('.toast .msg').filter({ hasText: 'Copied 2 items' })).toBeVisible();
  await expect(row(page, FILE_A)).toHaveCount(2); // inside the image + the copy under fd
  await expect(row(page, FILE_B)).toHaveCount(2);

  // Collapse the image so only the copies (direct children of fd) remain.
  await collapse(page, IMAGE);
  await expect(row(page, FILE_A)).toHaveCount(1);
  await expect(row(page, FILE_B)).toHaveCount(1);

  // Delete the two copies via the context menu + styled confirm dialog.
  await row(page, FILE_A).click();
  await row(page, FILE_B).click({ modifiers: ['ControlOrMeta'] });
  await row(page, FILE_A).click({ button: 'right' });
  await page.locator('.context-menu .item').filter({ hasText: 'Delete 2 items' }).click();
  await page.locator('.btn.danger').filter({ hasText: 'Delete' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Deleted 2 items' })).toBeVisible();
  await expect(row(page, FILE_A)).toHaveCount(0);
  await expect(row(page, FILE_B)).toHaveCount(0);

  // Copy them again — regression guard for the WasmFS/OPFS coherence fix
  // (a copy of the same paths after a delete used to fail with an I/O error).
  await expand(page, IMAGE, FILE_A);
  await row(page, FILE_A).click();
  await row(page, FILE_B).click({ modifiers: ['ControlOrMeta'] });
  await treeDrag(page, row(page, FILE_A), row(page, 'fd'));
  await expect(row(page, FILE_A)).toHaveCount(2);
  await collapse(page, IMAGE);
  await expect(row(page, FILE_A)).toHaveCount(1);
  await expect(row(page, FILE_B)).toHaveCount(1);
});

test('shift-range multi-select drives a bulk copy out of an image', async ({ page }) => {
  await stageOpfsFile(page, FD, IMAGE_HOST);
  await openFilesystemTab(page);
  await descendIntoImage(page);

  // Click an anchor, then shift-click three rows down: Installer, Installer
  // Script, Read Me (a contiguous sibling run).
  await row(page, 'Installer').click();
  await row(page, 'Read Me').click({ modifiers: ['Shift'] });
  await expect(page.locator('.tree-row.selected')).toHaveCount(3);

  await treeDrag(page, row(page, 'Installer'), row(page, 'fd'));
  await expect(page.locator('.toast .msg').filter({ hasText: 'Copied 3 items' })).toBeVisible();
  await collapse(page, IMAGE);
  await expect(row(page, 'Installer')).toHaveCount(1);
  await expect(row(page, 'Installer Script')).toHaveCount(1);
  await expect(row(page, 'Read Me')).toHaveCount(1);
});

test('move files within OPFS by dragging into a folder', async ({ page }) => {
  await mkdirOpfs(page, `${UPLOAD}/dest`);
  await stageOpfsText(page, `${UPLOAD}/f1.bin`, 'one');
  await stageOpfsText(page, `${UPLOAD}/f2.bin`, 'two');
  await openFilesystemTab(page);
  await expand(page, 'upload', 'dest');

  await row(page, 'f1.bin').click();
  await row(page, 'f2.bin').click({ modifiers: ['ControlOrMeta'] });
  await treeDrag(page, row(page, 'f1.bin'), row(page, 'dest'));
  await expect(page.locator('.toast .msg').filter({ hasText: 'Moved 2 items' })).toBeVisible();

  // Gone from upload (dest still collapsed) ...
  await expect(row(page, 'f1.bin')).toHaveCount(0);
  await expect(row(page, 'f2.bin')).toHaveCount(0);
  // ... and present inside dest.
  await expand(page, 'dest', 'f1.bin');
  await expect(row(page, 'f1.bin')).toHaveCount(1);
  await expect(row(page, 'f2.bin')).toHaveCount(1);
});

test('rename then delete a single OPFS file', async ({ page }) => {
  await stageOpfsText(page, `${UPLOAD}/oldname.bin`, 'x');
  await openFilesystemTab(page);
  await expand(page, 'upload', 'oldname.bin');

  // Rename via context menu → RenameDialog.
  await row(page, 'oldname.bin').click({ button: 'right' });
  await page.locator('.context-menu .item').filter({ hasText: 'Rename' }).click();
  await page.locator('#rename-input').fill('newname.bin');
  await page.getByRole('button', { name: 'Rename' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: "Renamed to 'newname.bin'" })).toBeVisible();
  await expect(row(page, 'oldname.bin')).toHaveCount(0);
  await expect(row(page, 'newname.bin')).toHaveCount(1);

  // Single-item delete via context menu + confirm dialog.
  await row(page, 'newname.bin').click({ button: 'right' });
  await page.locator('.context-menu .item').filter({ hasText: 'Delete' }).click();
  await expect(page.locator('.modal-backdrop')).toBeVisible();
  await expect(page.getByText("Delete 'newname.bin'?")).toBeVisible();
  await page.locator('.btn.danger').filter({ hasText: 'Delete' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: "Deleted 'newname.bin'" })).toBeVisible();
  await expect(row(page, 'newname.bin')).toHaveCount(0);
});

test('download an OPFS file (bulk) and a file from inside an image', async ({ page }) => {
  await stageOpfsFile(page, FD, IMAGE_HOST);
  await stageOpfsText(page, `${UPLOAD}/doc1.txt`, 'aaa');
  await stageOpfsText(page, `${UPLOAD}/doc2.txt`, 'bbb');
  await openFilesystemTab(page);

  // Bulk download two OPFS files ("Download files" menu label → one download
  // per file).
  await expand(page, 'upload', 'doc1.txt');
  const downloads: Download[] = [];
  page.on('download', (d) => downloads.push(d));
  await row(page, 'doc1.txt').click();
  await row(page, 'doc2.txt').click({ modifiers: ['ControlOrMeta'] });
  await row(page, 'doc1.txt').click({ button: 'right' });
  await page.locator('.context-menu .item').filter({ hasText: 'Download files' }).click();
  await expect.poll(() => downloads.map((d) => d.suggestedFilename()).sort()).toEqual([
    'doc1.txt',
    'doc2.txt',
  ]);

  // Download a single file from inside the (read-only) image — copied to a
  // scratch OPFS path first, then saved.
  await descendIntoImage(page);
  const inImage = page.waitForEvent('download');
  await row(page, FILE_A).click({ button: 'right' });
  await page.locator('.context-menu .item').filter({ hasText: 'Download' }).click();
  // The app sets download="TeachText"; Chromium MIME-sniffs the extension-less
  // data fork and appends ".txt", so assert the prefix.
  expect((await inImage).suggestedFilename()).toMatch(/^TeachText/);
});

test('unpack a Mac archive into a sibling folder', async ({ page }) => {
  await stageOpfsFile(page, `${UPLOAD}/archive.hqx`, ARCHIVE_HOST);
  await openFilesystemTab(page);
  await expand(page, 'upload', 'archive.hqx');

  await row(page, 'archive.hqx').click({ button: 'right' });
  await page.locator('.context-menu .item').filter({ hasText: 'Unpack' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Unpacked' })).toBeVisible();
  await expect(row(page, 'archive_unpacked')).toBeVisible();
});

test('external file drop uploads into an OPFS folder', async ({ page }) => {
  await openFilesystemTab(page);

  await dropFileOnRow(page, row(page, 'upload'), 'dropped.txt', 'hello world');
  await expect(page.locator('.toast .msg').filter({ hasText: 'saved to' })).toBeVisible();
  await expand(page, 'upload', 'dropped.txt');
  await expect(row(page, 'dropped.txt')).toHaveCount(1);
});

test('inside a read-only image, the context menu offers only Download', async ({ page }) => {
  await stageOpfsFile(page, FD, IMAGE_HOST);
  await openFilesystemTab(page);
  await descendIntoImage(page);

  await row(page, FILE_A).click({ button: 'right' });
  const menu = page.locator('.context-menu');
  await expect(menu.locator('.item').filter({ hasText: 'Download' })).toBeVisible();
  await expect(menu.locator('.item').filter({ hasText: 'Delete' })).toHaveCount(0);
  await expect(menu.locator('.item').filter({ hasText: 'Rename' })).toHaveCount(0);
  await expect(menu.locator('.item').filter({ hasText: 'Unpack' })).toHaveCount(0);
});
