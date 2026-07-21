// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the card-driven video configuration in the New Machine dialog.
//
// Regression guard for two coupled bugs (proposal-web2-card-centric-config.md):
//   1. Video ROMs were listed by raw filename, so the dialog couldn't speak in
//      cards. Now the dialog probes each vROM (machine.vrom.identify → card_id)
//      and offers the *card* by its name, never a ".vrom" filename.
//   2. The dialog never set machine.nubus.video_card, so the slot's default
//      card (the IIcx's 8•24) always booted — even when the user picked the
//      24AC. Now selecting the 24AC card actually instantiates it.
//
// Driven entirely through the shipped UI + OPFS. Both IIcx video-card vROMs are
// staged so the card picker has a real choice; we assert it offers the cards by
// *name* (never a ".vrom" filename), pick the 24AC, and start successfully.
//
// Scope note: this verifies the web2 UI contract — that the dialog speaks in
// cards and the chosen card boots. That the selected id actually instantiates
// the 24AC (vs. the slot-default 8•24) in slot $9 is proven at the core level
// by the iicx-display-card-24ac integration test, which asserts
// machine.nubus.slot[9].card.name == "Apple Macintosh Display Card 24AC".
// (web2 has no window.gsEval — that is a legacy-app global; web2 reaches the
// core only through typed UI paths, so we don't probe the object model here.)

import { test, expect } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';

const IICX_ROM = path.resolve(__dirname, '../../data/roms/iix-iicx-se30-97221136.rom');
const VROM_8_24 = path.resolve(__dirname, '../../data/roms/mdc-8-24-revb-d1629664.vrom');
const VROM_24AC = path.resolve(__dirname, '../../data/roms/display-card-24ac-d8daab87.vrom');

test('New Machine dialog: pick the 24AC card by name and boot it', async ({ page }) => {
  test.setTimeout(120_000);
  await gotoWeb2(page);

  // Stage both IIcx video-card vROMs under their canonical names so the card
  // picker has a real choice (8•24 vs 24AC). Done before the ROM upload so the
  // upload's image-revision bump re-scans and identifies them in one pass.
  await stageOpfsFile(page, '/opfs/images/vrom/mdc-8-24-revb-d1629664.vrom', VROM_8_24);
  await stageOpfsFile(page, '/opfs/images/vrom/display-card-24ac-d8daab87.vrom', VROM_24AC);

  // Upload the IIcx ROM via the Welcome "Upload ROM..." button. The persist
  // bumps the image revision, which re-scans the config slide (ROM + the two
  // staged vROMs).
  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await romChooser.setFiles(IICX_ROM);

  // Open the dialog and select the IIcx (the Universal ROM also lights up
  // se30 / iix, so the pick is explicit).
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator('option[value="iicx"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption('iicx');

  // The Display Card picker speaks in *cards*, not vROM filenames: two options,
  // one of which is the 24AC, and never a raw ".vrom" name.
  const card = page.locator('#cfg-card');
  await expect(card).toBeVisible({ timeout: 30_000 });
  await expect(card.locator('option')).toHaveCount(2);
  await expect(card.locator('option', { hasText: '24AC' })).toHaveCount(1);
  await expect(card.locator('option', { hasText: '.vrom' })).toHaveCount(0);

  // Pick the 24AC card and start.
  await card.selectOption('display_card_24ac');
  const start = page.getByRole('button', { name: 'Start Machine' });
  await expect(start).toBeEnabled();
  await start.click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 60_000,
  });
  await expect(page.locator('.welcome-layer')).toHaveCount(0);
});
