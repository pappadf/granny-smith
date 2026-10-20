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

import { test, expect } from '../helpers/test';
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

  // This boot went straight to the core, which builds the machine without
  // running it, so the page still shows no machine -- and the Debug
  // toolbar's Continue/Stop/Restart are disabled with no machine (S1018-76).
  // Run it, as every page boot path does (bus/boot.ts prepareFreshMachine):
  // the run-state push puts the page's status at running.
  await gsCallInPage(page, 'scheduler.run');

  // Power-cycle via the Debug toolbar's Restart button — the UI path that
  // routes through bus/debug.ts restart() → machine.restart.
  await page.locator('button.ptab[data-tab="debug"]').click();
  const restart = page.getByRole('button', { name: 'Restart' });
  await expect(restart).toBeEnabled({ timeout: 30_000 });
  await restart.click();
  // Restart asks first while the machine runs (#242).
  const confirm = page.getByRole('dialog').getByRole('button', { name: 'Restart' });
  await expect(confirm).toBeVisible({ timeout: 60_000 });
  await confirm.click();
  const restarted = page.locator('.toast .msg').filter({ hasText: 'Machine restarted' });
  await expect(restarted).toBeVisible({ timeout: 60_000 });

  // The HD survived the power-cycle as the SAME open instance.
  expect(await gsEvalInPage(page, 'machine.created')).toBe(true);
  expect(await gsEvalInPage(page, 'machine.scsi.device[0].image.present')).toBe(true);
  expect(await gsEvalInPage(page, 'machine.scsi.device[0].image.filename')).toBe(file0);
  expect(await gsEvalInPage(page, 'machine.scsi.device[0].image.path')).toBe(stem0);
});
