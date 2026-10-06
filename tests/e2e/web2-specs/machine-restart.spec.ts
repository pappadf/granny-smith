// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Restart button power-cycles the machine and media survives.
//
// Pins that web2's restart() calls the core's machine.restart, which power-
// cycles the machine WITHOUT tearing it down — no cached-config replay, no
// manual re-attachment in emulator.ts.  The storage-instance stem
// (image.path) must be identical before and after the restart: an equal
// stem proves the medium is the same open instance rather than a reopen,
// which is the write-durability guarantee (a reopen would mint a fresh delta
// and discard every write made since the attach).

import { test, expect } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';
import { gsCallInPage, gsEvalInPage } from '../helpers/web2-eval';

const IICX_ROM = path.resolve(__dirname, '../../data/roms/iix-iicx-se30-97221136.rom');

test('Restart keeps the attached hard disk — same medium, same open instance', async ({
  page,
}) => {
  test.setTimeout(240_000);
  await gotoWeb2(page);

  // Upload the IIcx ROM via the Welcome button (no auto-boot).
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Load ROM...' }).click(),
  ]);
  await chooser.setFiles(IICX_ROM);
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'iix-iicx-se30-97221136.rom added' }),
  ).toBeVisible({ timeout: 60_000 });

  // Boot and attach a scratch HD.
  await gsCallInPage(page, 'machine.boot', { model: 'iicx', ram: 8192, rom: '/opfs/images/rom/97221136' });
  await expect.poll(() => gsEvalInPage(page, 'machine.id'), { timeout: 30_000 }).toBe('iicx');
  await gsCallInPage(page, 'files.hd_create', ['/tmp/restart-scratch.img', '20mb']);
  await gsCallInPage(page, 'machine.scsi.attach_hd', ['/tmp/restart-scratch.img', 0]);
  expect(await gsEvalInPage(page, 'machine.scsi.device[0].image.present')).toBe(true);
  const stem0 = await gsEvalInPage(page, 'machine.scsi.device[0].image.path');
  const file0 = await gsEvalInPage(page, 'machine.scsi.device[0].image.filename');
  expect(stem0).toBeTruthy();
  expect(file0).toBeTruthy();

  // Power-cycle via the Debug toolbar's Restart button — the UI path that
  // routes through bus/debug.ts restart() → machine.restart.
  await page.locator('button.ptab[data-tab="debug"]').click();
  await page.getByRole('button', { name: 'Restart' }).click();
  // Restart asks first while the page shows the machine running or paused
  // (#242); this boot went straight to the core, so it may not -- answer
  // the question if it comes.
  const confirm = page.getByRole('dialog').getByRole('button', { name: 'Restart' });
  const restarted = page.locator('.toast .msg').filter({ hasText: 'Machine restarted' });
  await expect(confirm.or(restarted).first()).toBeVisible({ timeout: 60_000 });
  if (await confirm.isVisible()) await confirm.click();
  await expect(restarted).toBeVisible({ timeout: 60_000 });

  // The HD survived the power-cycle as the SAME open instance.
  expect(await gsEvalInPage(page, 'machine.created')).toBe(true);
  expect(await gsEvalInPage(page, 'machine.scsi.device[0].image.present')).toBe(true);
  expect(await gsEvalInPage(page, 'machine.scsi.device[0].image.filename')).toBe(file0);
  expect(await gsEvalInPage(page, 'machine.scsi.device[0].image.path')).toBe(stem0);
});
