// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: configure an Apple Lisa 2 with the installed SCO Xenix 3.0 ProFile
// hard disk through the real machine-configuration dialog, then start it.
//
// Driven entirely through the shipped UI:
//   1. Upload the Lisa 2 ROM via the Welcome "Upload ROM..." button (file picker).
//   2. Open "New Machine..." and select the Lisa model the ROM identifies as.
//   3. Upload the installed Xenix ProFile image into the hard-disk slot (file
//      picker) and select it.
//   4. Start the machine.
//
// The Lisa-specific assertion: a Lisa profile advertises hd_bus='profile', so
// the config dialog labels the hard-disk row "ProFile" (not "SCSI HD") and
// initEmulator routes the attach to profile.attach rather than scsi.attach_hd.
// A successful start (the "Machine started" toast + the Welcome screen giving
// way to the running display) confirms the whole path: machine.boot lisa →
// rom.load → profile.attach → scheduler.run.
//
// The ROM and the Xenix ProFile image are the same artifacts the
// lisa-xenix-install / lisa-xenix-boot integration tests use, staged under
// tests/data.

import { test, expect } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';

const LISA_ROM = path.resolve(__dirname, '../../data/Lisa/roms/098917B2-LisaH.rom');
const XENIX_HD = path.resolve(__dirname, '../../data/Lisa/Xenix-3.0/Xenix-3.0-ProFile.image');
const XENIX_HD_NAME = 'Xenix-3.0-ProFile.image';

test('configure a Lisa 2 with the Xenix ProFile and boot', async ({ page }) => {
  test.setTimeout(120_000);
  await gotoWeb2(page);

  // --- 1. Upload the Lisa 2 ROM via the Welcome "Upload ROM..." button. ------
  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await romChooser.setFiles(LISA_ROM);

  // --- 2. Open the New Machine config dialog and pick the Lisa model. --------
  // The upload pipeline persists the ROM and bumps the image revision, which
  // re-scans the (kept-mounted) config slide; wait for the 'lisa' model the
  // rev-H ROM identifies as to appear, then select it.
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator('option[value="lisa"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption('lisa');

  // Lisa's hard disk is the parallel-port ProFile, so the HD row is labelled
  // "ProFile" (hd_bus='profile'), not "SCSI HD".
  await expect(page.locator('label[for="cfg-hd"]')).toHaveText('ProFile');

  // --- 3. Upload the installed Xenix ProFile image into the ProFile slot. ----
  // Selecting the "Upload image..." sentinel opens a file picker; after the
  // upload the slot resets to (none), so pick the now-listed image by name.
  const hd = page.locator('#cfg-hd');
  const [hdChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    hd.selectOption('Upload image...'),
  ]);
  await hdChooser.setFiles(XENIX_HD);
  await expect(hd.locator('option', { hasText: XENIX_HD_NAME })).toHaveCount(1, { timeout: 30_000 });
  await hd.selectOption(XENIX_HD_NAME);

  // --- 4. Start the machine. -------------------------------------------------
  const start = page.getByRole('button', { name: 'Start Machine' });
  await expect(start).toBeEnabled();
  await start.click();

  // initEmulator ran the full boot sequence (machine.boot lisa → rom.load →
  // profile.attach → scheduler.run) and the Welcome screen is gone.
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 30_000,
  });
  await expect(page.locator('.welcome-layer')).toHaveCount(0);
});

// The ProFile hard-disk row offers "Create blank image..." just like the SCSI
// row, but it must open the *ProFile* creator (532-byte/block raw image, 5 MB
// or 10 MB) rather than the SCSI drive-catalog dialog.
test('create a blank ProFile from the config dialog', async ({ page }) => {
  test.setTimeout(120_000);
  await gotoWeb2(page);

  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await romChooser.setFiles(LISA_ROM);

  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator('option[value="lisa"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption('lisa');
  await expect(page.locator('label[for="cfg-hd"]')).toHaveText('ProFile');

  // Selecting the "Create blank image..." sentinel on the ProFile row opens the
  // create dialog. It must be the ProFile creator, offering 5 MB / 10 MB — NOT
  // the SCSI drive catalog.
  const hd = page.locator('#cfg-hd');
  await hd.selectOption('Create blank image...');
  const dlg = page.getByRole('dialog', { name: 'Create Blank ProFile' });
  await expect(dlg).toBeVisible();
  await expect(dlg.getByText('5 MB ProFile')).toBeVisible();
  await expect(dlg.getByText('10 MB Widget')).toBeVisible();

  // Create the 10 MB Widget; the dialog closes and the new image is selected in
  // the ProFile row (storage.profile_create wrote a raw 19448×532 image, which
  // the OPFS rescan now lists).
  await dlg.getByText('10 MB Widget').click();
  await dlg.getByRole('button', { name: 'Create' }).click();
  await expect(dlg).toBeHidden();
  await expect(hd).toHaveValue(/^blank_profile_10MB_\d+\.image$/, { timeout: 30_000 });
});
