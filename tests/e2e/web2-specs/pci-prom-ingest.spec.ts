// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: configuring a machine whose display comes ONLY from a PCI slot.
//
// Regression guard for four bugs that all shipped, and that the component
// tests could not have caught — every one of them lived in a seam the unit
// mocks replace (real OPFS, the real worker, the real wasm startup, and the
// real machine.boot validation):
//
//   1. The Display Card picker offered "Control / Chaos on-board video" — a
//      soldered stand-in that the core refuses to stage into a socket, and
//      whose presence suppressed the ask for the card's expansion ROM.  The
//      component fixture omitted the fixed slot the real profile carries, so
//      the mocked test passed while the UI was wrong.
//   2. Uploading a .prom died with "Failed to save": /opfs/images/prom was
//      missing from the startup mkdir list and files.cp does not create
//      parent directories.  MockOpfs seeds every category directory, so no
//      mocked test could see it.
//   3. A stored .prom was not re-offered on reload — the startup enumeration
//      that offers persisted vROMs had no PROM counterpart.
//   4. Start Machine failed with "unknown video-mode id '14in_rgb_8bpp'":
//      the Video Mode picker was populated from a PCI card's monitors, but
//      video_mode is validated against the NuBus catalog.
//
// The lesson those share is that a picker that LOOKS right proves nothing.
// So this spec drives the whole configuration and requires a machine that
// actually booted — every assertion below the upload is downstream of a real
// machine.boot document being accepted by the core.

import { test, expect, type Page } from '../helpers/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';
import { gsCallInPage, gsEvalInPage } from '../helpers/web2-eval';

const DATA = path.resolve(__dirname, '../../data');
const TNT_ROM = path.join(DATA, 'roms', 'pm7500-pm8500-pm9500-96cd923d.rom');
const MACH64_PROM = path.join(DATA, 'roms', 'mach64-gx-104-437584e0.prom');

// The ROM is stored under its content id (its header sum plus the ConfigInfo
// 64-bit sum), the .prom under its id (the PCIR vendor and device ids plus
// the FCode header's own checksum) — both content-addressed, so the upload
// filename never matters.
const STORED_ROM = '/opfs/images/rom/96cd923d-c241cd82bf90797a';
const STORED_PROM = '/opfs/images/prom/1002-4758-c6e8';

// Upload a host file through the shipped generic ingest path — the Welcome
// "Load ROM..." button, which probes the file against every media type
// rather than being told what it is.  This is the exact action that produced
// "Failed to save".
async function uploadViaWelcome(page: Page, hostFile: string): Promise<void> {
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Load ROM...' }).click(),
  ]);
  await chooser.setFiles(hostFile);
}

async function openNewMachine(page: Page): Promise<void> {
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator('option[value="pm9500"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption('pm9500');
}

test('a 9500 boots its factory PCI display card once its ROM is uploaded', async ({ page }) => {
  test.setTimeout(240_000);
  await gotoWeb2(page);

  await uploadViaWelcome(page, TNT_ROM);
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'pm7500-pm8500-pm9500-96cd923d.rom added' }),
  ).toBeVisible({ timeout: 60_000 });

  // --- Before the .prom: the machine states what it needs. -----------------
  await openNewMachine(page);

  // The 9500 has no built-in video, so with no expansion ROM there is no
  // display card it can take.  The dialog says the machine will start with
  // no screen, offers the card disabled with the ROM it needs -- an expansion
  // ROM (.prom), not a video ROM -- and never offers the emulator's
  // Control/Chaos stand-in, which is not hardware the 9500 ever had.
  await expect(page.locator('.config-form')).toContainText('It will start with no screen', {
    timeout: 30_000,
  });
  await expect(page.locator('.config-form')).toContainText('expansion ROM (.prom)');
  await expect(page.locator('.config-form')).not.toContainText('on-board video');

  // --- Upload the card's expansion ROM. ------------------------------------
  await page.locator('.back-link').click();
  await uploadViaWelcome(page, MACH64_PROM);

  // The toast naming the card is the "we identified it" signal.  Before the
  // store directory existed this read "Failed to save ..." instead.
  await expect(
    page.locator('.toast .msg').filter({ hasText: "PCI expansion ROM for 'mach64_gx'" }),
  ).toBeVisible({ timeout: 60_000 });

  // --- The factory card is in the default configuration now. --------------
  // The 9500 shipped with the Apple Accelerated PCI Graphics Card in slot A1;
  // with its expansion ROM offered, the dialog opens with it seated (and the
  // emulator's Control/Chaos stand-in nowhere).
  await openNewMachine(page);
  await expect(page.locator('.item-row[data-card="mach64_gx"]')).toHaveCount(1, { timeout: 30_000 });
  await expect(page.locator('.config-form')).not.toContainText('It will start with no screen');
  await expect(page.locator('.config-form')).not.toContainText('on-board video');

  // --- Start it.  This is the assertion the other three bugs hid behind. ---
  // Everything above is a picker rendering the right strings; only this
  // proves the boot document the dialog builds is one the core accepts.
  const start = page.getByRole('button', { name: 'Start', exact: true });
  await expect(start).toBeEnabled();
  await start.click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 120_000,
  });
  // A boot the core rejected leaves the Welcome layer up and raises a
  // "Boot failed: ..." toast, so assert both directions.
  await expect(page.locator('.toast .msg').filter({ hasText: 'Boot failed' })).toHaveCount(0);
  await expect(page.locator('.welcome-layer')).toHaveCount(0);
});

test('an uploaded .prom is still offered after a reload', async ({ page }) => {
  test.setTimeout(240_000);
  await gotoWeb2(page);

  await uploadViaWelcome(page, TNT_ROM);
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'pm7500-pm8500-pm9500-96cd923d.rom added' }),
  ).toBeVisible({ timeout: 60_000 });
  await uploadViaWelcome(page, MACH64_PROM);
  await expect(
    page.locator('.toast .msg').filter({ hasText: "PCI expansion ROM for 'mach64_gx'" }),
  ).toBeVisible({ timeout: 60_000 });

  // Reload: the ingest-time offer is gone with the old page, so from here the
  // file is visible to the core ONLY if the wasm startup enumerated the store
  // — the pass that exists for vROMs and had no PROM counterpart.
  await page.reload();
  await page.waitForFunction(() => (window as { __gsReady?: boolean }).__gsReady === true, undefined, {
    timeout: 60_000,
  });
  const cont = page.getByRole('button', { name: 'Continue' });
  if (await cont.isVisible().catch(() => false)) await cont.click();

  // Both files survived the reload, content-addressed.
  expect(await gsEvalInPage(page, 'files.path_size', [STORED_PROM])).toBe(32768);

  // An "(auto)" boot — a document with pci_card= but NO prom= pick. Strict
  // resolution refuses it unless the stored file was offered at startup, so
  // this is the reload guard rather than a second copy of the UI test.
  await gsCallInPage(page, 'machine.boot', {
    model: 'pm9500',
    ram: 32768,
    rom: STORED_ROM,
    pci_card: 'mach64_gx',
  });
  await expect.poll(() => gsEvalInPage(page, 'machine.id'), { timeout: 30_000 }).toBe('pm9500');
  // The card really is seated in socket A1, not merely a boot that survived.
  expect(await gsEvalInPage(page, 'machine.pci.slot[1].card.name')).toContain('Mach64 GX');
});
