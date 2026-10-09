// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the New Machine dialog's Storage and Monitor sections build the
// machine they show.  (The Expansion Cards section has its own spec,
// display-card-config.)  Each test edits a section through the UI, starts
// the machine, and reads back what the core built through the object model.

import { test, expect, type Page } from '../helpers/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';
import { gsEvalInPage } from '../helpers/web2-eval';

const ROMS = path.resolve(__dirname, '../../data/roms');

async function uploadRom(page: Page, file: string): Promise<void> {
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Load ROM...' }).click(),
  ]);
  await chooser.setFiles(path.join(ROMS, file));
  await expect(page.locator('.toast .msg').filter({ hasText: `${file} added` })).toBeVisible({
    timeout: 60_000,
  });
}

async function openNewMachine(page: Page, model: string): Promise<void> {
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const select = page.locator('#cfg-model');
  await expect(select.locator(`option[value="${model}"]`)).toHaveCount(1, { timeout: 30_000 });
  await select.selectOption(model);
}

async function start(page: Page): Promise<void> {
  const button = page.getByRole('button', { name: 'Start', exact: true });
  await expect(button).toBeEnabled();
  await button.click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 120_000,
  });
  await expect(page.locator('.welcome-layer')).toHaveCount(0);
}

test('Storage: a device added and one removed are what the machine is built with', async ({
  page,
}) => {
  test.setTimeout(180_000);
  await gotoWeb2(page);
  await uploadRom(page, 'iix-iicx-se30-97221136.rom');
  await openNewMachine(page, 'iicx');

  // The IIcx's default storage: a hard disk at ID 0 and a CD-ROM drive at 3.
  const rows = page.locator('.device-row');
  await expect(rows).toHaveCount(2, { timeout: 30_000 });
  await expect(page.locator('.device-row[data-position="scsi:3"]')).toHaveAttribute(
    'data-device-type',
    'cd',
  );

  // Move the CD-ROM drive's place: remove it, add one at ID 5.
  await page
    .locator('.device-row[data-position="scsi:3"]')
    .getByRole('button', { name: /^Remove/ })
    .click();
  await expect(rows).toHaveCount(1);
  await page.getByTestId('cfg-add-device-scsi').click();
  await page.locator('#cfg-add-type-scsi').selectOption('cd');
  await page.locator('#cfg-add-unit-scsi').selectOption('5');
  await page.getByRole('button', { name: 'Add', exact: true }).click();
  await expect(page.locator('.device-row[data-position="scsi:5"]')).toHaveAttribute(
    'data-device-type',
    'cd',
  );

  await start(page);

  // The core built the drive where the dialog put it, and none where it was.
  const storage = (await gsEvalInPage(page, 'machine.storage')) as {
    bus: string;
    unit: number;
    type: string;
  }[];
  const cds = storage.filter((d) => d.type === 'cd');
  expect(cds.map((d) => `${d.bus}:${d.unit}`)).toEqual(['scsi:5']);
  expect(await gsEvalInPage(page, 'machine.scsi.device[5].type')).toMatchObject({ enum: 'cdrom' });
});

test('Monitor: the connected card gets the monitor and startup mode; built-in video none', async ({
  page,
}) => {
  test.setTimeout(180_000);
  await gotoWeb2(page);
  await uploadRom(page, 'iici-368cadfe.rom');
  await openNewMachine(page, 'iici');

  // Seat an 8•24 (on its substitute ROM: no vROM is uploaded here).
  await page.getByTestId('cfg-add-card').click({ timeout: 30_000 });
  await page.locator('#cfg-add-card').selectOption('mdc_8_24');
  const slot = await page.locator('#cfg-add-card-slot').inputValue();
  await page.getByRole('button', { name: 'Add', exact: true }).click();
  await expect(page.locator(`.item-row[data-slot="${slot}"]`)).toHaveCount(1);

  // Connect the monitor to it: a 12" RGB at 8 bpp.
  await page.locator('#cfg-display').selectOption(slot);
  await page.locator('#cfg-monitor').selectOption('12in_rgb');
  await page.locator('#cfg-video-mode').selectOption('512x384x8');

  await start(page);

  // NuBus slot ids name the slot number in hex after "nubus_".
  const n = parseInt(slot.replace(/^nubus_/, ''), 16);
  expect(await gsEvalInPage(page, `machine.nubus.slot[${n}].card.monitor`)).toBe('12in_rgb');
  expect(await gsEvalInPage(page, `machine.nubus.slot[${n}].card.sense`)).toBe(2);
  // The built-in video has no monitor: its sense lines are open.
  expect(await gsEvalInPage(page, 'machine.nubus.slot[11].card.monitor')).toBe('none');
  // The startup mode is the slot's PRAM record: 8 bpp.
  const savedMode = await gsEvalInPage(page, 'machine.rtc.pram.peek', [0x46 + (n - 9) * 8 + 2]);
  expect(Number(savedMode)).toBe(0x83);
});
