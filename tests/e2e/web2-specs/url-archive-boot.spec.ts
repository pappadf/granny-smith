// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: URL-parameter boot from containers and from archive.org, onto a
// bare HFS volume.
//
// The worked example in docs/guide/web.md is
//
//   ?ROM=https://archive.org/download/<rom item>/<rom item>.zip/368CADFE%20-%20Mac%20IIci.ROM
//   &HD0=https://archive.org/download/AppleMacintoshSystem753/System7_5_3.img
//
// which exercises everything this feature is made of: case-insensitive
// names, a member path through a zip, archive.org routing (server-side zip
// extraction for the ROM, /cors/ for the disk image, whose plain /download/
// path carries no CORS header), and a bare HFS volume that only boots because
// scsi.attach_hd wraps it with a partition map and the GSDisk driver.
//
// No network: archive.org's two endpoints are routed to the same bytes on
// disk (the gs-test-data copies of those items), and the spec asserts the
// page asked for exactly the URLs archive.org serves with CORS.  The
// generic-host rows build a small zip in the spec and route it likewise.

import { test, expect, type Page, type Route } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import * as zlib from 'node:zlib';
import { gsEvalInPage } from '../helpers/web2-eval';

const IICI_ROM = path.resolve(__dirname, '../../data/roms/iici-368cadfe.rom');
const PLUS_ROM = path.resolve(__dirname, '../../data/roms/plus-v3-4d1f8172.rom');
const BARE_753 = path.resolve(__dirname, '../../data/systems/system_7_5_3_25mb_bare.img');

const ROM_ITEM = 'mac_rom_archive_-_as_of_8-19-2011';
const ROM_MEMBER_URL = `https://archive.org/download/${ROM_ITEM}/${ROM_ITEM}.zip/368CADFE%20-%20Mac%20IIci.ROM`;
const HD_URL = 'https://archive.org/download/AppleMacintoshSystem753/System7_5_3.img';
const HD_CORS_URL = 'https://archive.org/cors/AppleMacintoshSystem753/System7_5_3.img';

// A STORED (uncompressed) zip holding `entries`, in order.
function storedZip(entries: Array<{ name: string; data: Buffer }>): Buffer {
  const locals: Buffer[] = [];
  const centrals: Buffer[] = [];
  let offset = 0;
  for (const e of entries) {
    const name = Buffer.from(e.name, 'utf8');
    const crc = zlib.crc32(e.data);
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0);
    lh.writeUInt16LE(20, 4); // version needed
    lh.writeUInt16LE(0x0800, 6); // UTF-8 names
    lh.writeUInt16LE(0, 8); // stored
    lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(e.data.length, 18);
    lh.writeUInt32LE(e.data.length, 22);
    lh.writeUInt16LE(name.length, 26);
    locals.push(lh, name, e.data);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0);
    ch.writeUInt16LE(20, 4);
    ch.writeUInt16LE(20, 6);
    ch.writeUInt16LE(0x0800, 8);
    ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(e.data.length, 20);
    ch.writeUInt32LE(e.data.length, 24);
    ch.writeUInt16LE(name.length, 28);
    ch.writeUInt32LE(offset, 42);
    centrals.push(ch, name);
    offset += 30 + name.length + e.data.length;
  }
  const cd = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(cd.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, cd, end]);
}

// Fulfil `route` with `body`, as a CORS-enabled cross-origin server would.
function serve(route: Route, body: Buffer): Promise<void> {
  return route.fulfill({
    status: 200,
    contentType: 'application/octet-stream',
    headers: { 'Access-Control-Allow-Origin': '*' },
    body,
  });
}

async function waitReady(page: Page): Promise<void> {
  await page.waitForFunction(
    () => (window as { __gsReady?: boolean }).__gsReady === true,
    undefined,
    {
      timeout: 60_000,
    },
  );
}

// Read a Mac OS low-memory global through the object model (the core
// answers with a hex string, "0x8023").
async function macGlobal(page: Page, name: string): Promise<number> {
  const v = await gsEvalInPage(page, 'debug.mac.globals.read', [name]);
  if (typeof v === 'number') return v;
  return typeof v === 'string' ? Number(v) : -1;
}

test('archive.org ROM-in-zip + bare HD0 image boots the IIci off the wrapped volume', async ({
  page,
}) => {
  test.skip(!fs.existsSync(BARE_753), `no bare volume at ${BARE_753}`);
  test.setTimeout(300_000);
  const asked: string[] = [];
  await page.route('https://archive.org/**', (route) => {
    const url = route.request().url();
    asked.push(url);
    if (url === ROM_MEMBER_URL) return serve(route, fs.readFileSync(IICI_ROM));
    if (url === HD_CORS_URL) return serve(route, fs.readFileSync(BARE_753));
    return route.fulfill({ status: 404, body: 'not routed' });
  });

  const q = `?ROM=${encodeURIComponent(ROM_MEMBER_URL)}&HD0=${encodeURIComponent(HD_URL)}&speed=turbo`;
  await page.goto(`/index.html${q}`);
  await waitReady(page);

  // Straight into a running machine: no Welcome, no configuration dialog.
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'Booted iici from URL parameters' }),
  ).toBeVisible({ timeout: 120_000 });
  await expect(page.locator('.welcome-layer')).toHaveCount(0);

  // The page asked archive.org only for endpoints that carry CORS headers.
  expect(asked).toContain(ROM_MEMBER_URL);
  expect(asked).toContain(HD_CORS_URL);
  expect(asked).not.toContain(HD_URL);

  // The bare volume went in as a hard disk, grown by the wrapper's prefix.
  expect(await gsEvalInPage(page, 'machine.scsi.device[0].type')).toMatchObject({ enum: 'hd' });
  const size = fs.statSync(BARE_753).size;
  const images = (await gsEvalInPage(page, 'files.images.count')) as number;
  let wrapped = false;
  for (let i = 0; i < images; i++) {
    if ((await gsEvalInPage(page, `files.images[${i}].raw_size`)) === size + 96 * 512)
      wrapped = true;
  }
  expect(wrapped).toBe(true);

  // And the machine boots System 7.5.3 from it to the Finder: a boot drive,
  // an open System file, and an application heap apart from the system's.
  await expect
    .poll(() => macGlobal(page, 'BootDrive'), {
      timeout: 240_000,
      intervals: [2000],
    })
    .toBeGreaterThan(0);
  await expect
    .poll(async () => (await macGlobal(page, 'ApplZone')) !== (await macGlobal(page, 'SysZone')), {
      timeout: 240_000,
      intervals: [2000],
    })
    .toBe(true);
});

test('a zip member path picks that member, not the first file', async ({ page }) => {
  test.setTimeout(120_000);
  // The first entry is a decoy: the old behaviour (first file) would boot
  // the Plus.
  const zip = storedZip([
    { name: 'decoy/Plus.rom', data: fs.readFileSync(PLUS_ROM) },
    { name: 'Mac ROMs/IIci & friends.ROM', data: fs.readFileSync(IICI_ROM) },
  ]);
  await page.route('**/media/roms.zip', (route) => serve(route, zip));

  const rom = 'media/roms.zip/Mac%20ROMs/IIci%20%26%20friends.ROM';
  await page.goto(`/index.html?Rom=${encodeURIComponent(rom)}`);
  await waitReady(page);
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'Booted iici from URL parameters' }),
  ).toBeVisible({ timeout: 60_000 });
});

test('a member that is not in the zip is reported with what is there', async ({ page }) => {
  test.setTimeout(120_000);
  const zip = storedZip([{ name: 'Plus.rom', data: fs.readFileSync(PLUS_ROM) }]);
  await page.route('**/media/roms.zip', (route) => serve(route, zip));

  await page.goto(`/index.html?ROM=${encodeURIComponent('media/roms.zip/nope.rom')}`);
  await waitReady(page);
  await expect(
    page
      .locator('.toast .msg')
      .filter({ hasText: '"nope.rom" is not in roms.zip (it has Plus.rom)' }),
  ).toBeVisible({ timeout: 60_000 });
});

// The Lisa: the ProFile images in archive.org's "Apple Lisa Profile HD Disk
// Images for LisaEM and IDLE" item are raw 532-byte-block ProFile disks inside
// zips, so HD0= names the member and archive.org extracts it.  A fresh Lisa
// stops at the boot ROM's startup-device screen unless its parameter memory
// names the ProFile, which the page does for hd0 (bus/boot.ts setStartupDisk).
const LISA_ROM = path.resolve(__dirname, '../../data/roms/lisa2-revh-098917b2.rom');
const LOS_PROFILE = path.resolve(
  __dirname,
  '../../data/Lisa/LisaOfficeSystem-3.1/LOS-3.1-ProFile.image',
);
const LISA_ITEM =
  'apple-lisa-profile-hd-disk-images-for-lisaem-and-idle-lisa-office-system-3.1-lis';
const LISA_HD_URL = `https://archive.org/download/${LISA_ITEM}/IDLE_LOS3.1-after1stBoot.zip/profile.raw`;
// The boot ROM, as the "Lisa Software" item's two Rev H chip dumps: 341-0175-H
// holds the even bytes, 341-0176-H the odd.  The URL names the odd chip first
// on purpose: the page tries both interleavings and keeps the one whose
// checksum verifies.
const LISA_FW =
  'https://archive.org/download/lisa-software/Lisa%20Software.zip/Lisa%20Software/firmware';
const LISA_EVEN_URL = `${LISA_FW}/341-0175-H.BIN`;
const LISA_ODD_URL = `${LISA_FW}/341-0176-H.BIN`;

test('a Lisa boots Office System from archive.org: two ROM chips + a ProFile zip member', async ({
  page,
}) => {
  test.skip(!fs.existsSync(LOS_PROFILE), `no ProFile image at ${LOS_PROFILE}`);
  test.setTimeout(240_000);
  const rom = fs.readFileSync(LISA_ROM);
  const even = Buffer.from(rom.filter((_, i) => i % 2 === 0));
  const odd = Buffer.from(rom.filter((_, i) => i % 2 === 1));
  const asked: string[] = [];
  await page.route('https://archive.org/**', (route) => {
    const url = route.request().url();
    asked.push(url);
    if (url === LISA_EVEN_URL) return serve(route, even);
    if (url === LISA_ODD_URL) return serve(route, odd);
    if (url === LISA_HD_URL) return serve(route, fs.readFileSync(LOS_PROFILE));
    return route.fulfill({ status: 404, body: 'not routed' });
  });

  const q =
    `?ROM=${encodeURIComponent(LISA_ODD_URL)}&ROM=${encodeURIComponent(LISA_EVEN_URL)}` +
    `&HD0=${encodeURIComponent(LISA_HD_URL)}&speed=turbo`;
  await page.goto(`/index.html${q}`);
  await waitReady(page);
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'Booted lisa from URL parameters' }),
  ).toBeVisible({ timeout: 120_000 });
  expect(asked).toEqual([LISA_ODD_URL, LISA_EVEN_URL, LISA_HD_URL]);
  // The interleaved pair is the verified Rev H ROM.
  expect(await gsEvalInPage(page, 'machine.rom.id')).toBe('3f7b');
  expect(await gsEvalInPage(page, 'machine.hd.present')).toBe(true);

  // Booted from the ProFile, not parked at the ROM's startup-device screen:
  // the Office System runs out of RAM, the boot ROM lives at $FE0000.
  await expect
    .poll(
      async () => {
        const pc = Number(await gsEvalInPage(page, 'machine.cpu.pc'));
        return pc < 0xfe0000;
      },
      { timeout: 180_000, intervals: [3000] },
    )
    .toBe(true);
  await expect
    .poll(async () => Number(await gsEvalInPage(page, 'machine.cpu.pc')) < 0xfe0000, {
      timeout: 30_000,
      intervals: [2000],
    })
    .toBe(true);
});
