// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the New Machine dialog's Storage, Start up from and Monitor
// sections build the machine they show.  (The Expansion Cards section has its own spec,
// display-card-config.)  Each test edits a section through the UI, starts
// the machine, and reads back what the core built through the object model.

import { test, expect, type Page } from '../helpers/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFileStreaming } from '../helpers/web2-fs';
import { gsEvalInPage } from '../helpers/web2-eval';

const ROMS = path.resolve(__dirname, '../../data/roms');
const OSX_DISK = path.resolve(
  __dirname,
  '../../data/systems/mac_os_x_10_0_3_2gb_g3_minimal_aqua.dmg',
);

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

// The G3's Open Firmware boots a partition, so "Start up from" offers a
// disk's volumes by name; the Mac OS X disk's (behind the GSDisk map, its
// partition 3) is chosen by itself, and the new machine's NVRAM carries
// Apple's Mac OS X patch and that partition as boot-device.
test('Start up from: a G3 disk by volume name, written to Open Firmware', async ({ page }) => {
  test.skip(!fs.existsSync(OSX_DISK), 'needs the Mac OS X 10.0.3 disk from gs-test-data');
  test.setTimeout(300_000);
  // Staged, then a reload: the worker's mount does not see main-thread
  // OPFS writes made after it came up.
  await gotoWeb2(page);
  await stageOpfsFileStreaming(page, '/opfs/images/hd/osx.dmg', OSX_DISK);
  await gotoWeb2(page);
  await uploadRom(page, 'pmg3dt-pmg3mt-78f57389.rom');
  await openNewMachine(page, 'pmg3dt');

  // The default SCSI hard disk out, a hard disk on the primary ATA bus in.
  await page
    .locator('.device-row[data-position="scsi:0"]')
    .getByRole('button', { name: /^Remove/ })
    .click({ timeout: 30_000 });
  await page.getByTestId('cfg-add-device-ata0').click();
  await page.locator('#cfg-add-type-ata0').selectOption('hd');
  await page.locator('#cfg-add-unit-ata0').selectOption('0');
  await page.getByRole('button', { name: 'Add', exact: true }).click();
  await page.locator('#cfg-media-ata0-0').selectOption('osx.dmg');

  const startup = page.locator('#cfg-startup');
  await expect(startup).toHaveValue('ata0:0:3', { timeout: 30_000 });
  await expect(startup.locator('option:checked')).toHaveText(/Macintosh HD$/);

  await start(page);

  expect(await gsEvalInPage(page, 'machine.nvram.getenv', ['boot-device'])).toBe(
    '/pci@80000000/mac-io@10/ide@20000/@0:3',
  );
  expect(await gsEvalInPage(page, 'machine.nvram.getenv', ['boot-command'])).toBe('0 bootr');
  expect(await gsEvalInPage(page, 'machine.nvram.getenv', ['use-nvramrc?'])).toBe('true');
});
