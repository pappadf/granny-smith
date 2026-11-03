// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// MacTest Pro (680x0) on a Macintosh IIci, in the browser at Max speed.
//
// The headless iici-mactest-pro row runs the same CD on the same machine
// (24AC in slot $C, 8*24 GC in slot $D) but steps the scheduler from its
// script, so every click lands on a fixed instruction.  Here the machine
// runs live under the page's own Max (turbo) pacing, and the clicks, the
// floppy and the waits happen in wall-clock time, as a user's would: the
// question is whether all nine tests still pass when nothing is pinned to
// the instruction count.
//
// The screens are compared against the headless row's goldens (staged into
// OPFS) with machine.screen.matches; each step also saves a page screenshot
// under test-results/ for a look.

import { test, expect, type Page } from '../helpers/test';
import * as path from 'node:path';
import * as fs from 'node:fs';
import { gotoWeb2, stageOpfsFile, stageOpfsFileStreaming } from '../helpers/web2-fs';
import { gsEvalInPage, gsCallInPage } from '../helpers/web2-eval';

const DATA = path.resolve(__dirname, '../../data');
const ROM = path.join(DATA, 'roms/iici-368cadfe.rom');
const CD = path.join(DATA, 'apps/mactest_pro_68k_feb_1996.iso');
const GOLDENS = path.resolve(__dirname, '../../integration/iici-mactest-pro');
const OUT = path.resolve(__dirname, 'test-results/mactest-max');

async function typeLine(page: Page, line: string): Promise<void> {
  await page.locator('.console').click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
}

const sleep = (page: Page, ms: number) => page.waitForTimeout(ms);

async function shot(page: Page, name: string): Promise<void> {
  fs.mkdirSync(OUT, { recursive: true });
  await page.screenshot({ path: path.join(OUT, `${name}.png`) });
  await gsEvalInPage(page, 'machine.screen.save', [`/opfs/upload/mtmax/${name}.png`]);
  // Copy the guest's own frame out of OPFS for a look at full size.
  const b64 = await page.evaluate(async (n: string) => {
    let dir = await navigator.storage.getDirectory();
    for (const part of ['upload', 'mtmax']) dir = await dir.getDirectoryHandle(part);
    const f = await (await dir.getFileHandle(`${n}.png`)).getFile();
    const bytes = new Uint8Array(await f.arrayBuffer());
    let s = '';
    for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
    return btoa(s);
  }, name);
  fs.writeFileSync(path.join(OUT, `${name}-guest.png`), Buffer.from(b64, 'base64'));
}

async function matches(page: Page, golden: string): Promise<boolean> {
  return (await gsEvalInPage(page, 'machine.screen.matches', [`/opfs/upload/mtmax/g/${golden}`])) === true;
}

// Poll until the guest screen equals `golden`, up to `ms` of wall time.
async function waitMatch(page: Page, golden: string, ms: number): Promise<number> {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    if (await matches(page, golden)) return Date.now() - t0;
    await sleep(page, 500);
  }
  return -1;
}

// Poll until the screen checksum holds for `quietMs`.
async function waitStable(page: Page, quietMs: number, maxMs: number): Promise<void> {
  const t0 = Date.now();
  let last = await gsEvalInPage(page, 'machine.screen.checksum');
  let since = Date.now();
  while (Date.now() - t0 < maxMs) {
    await sleep(page, 250);
    const now = await gsEvalInPage(page, 'machine.screen.checksum');
    if (now !== last) {
      last = now;
      since = Date.now();
    } else if (Date.now() - since >= quietMs) return;
  }
}

// Clicks run inside the worker as one shell command each: the headless
// row's click_fixed / double_click_fixed (tests/integration/lib/mac.script,
// staged into OPFS and included), paced in guest ticks.  Driving the mouse
// from the page instead does not work at Max: each object-model call from
// the page waits for a turbo slice boundary (~16 ms of wall time, some 18
// guest ticks at the speed reached here), so the two clicks of a double
// click arrive well outside DoubleTime and the Finder sees two single
// clicks.  The machine runs live, at Max, between the commands.
async function shellDone(page: Page): Promise<void> {
  await sleep(page, 200);
  await expect(page.locator('.console.busy')).toHaveCount(0, { timeout: 60_000 });
}

// Stepping the scheduler from the shell leaves the machine stopped when the
// command returns; a bare scheduler.run hands it back to live running.
async function runLive(page: Page): Promise<void> {
  await typeLine(page, 'scheduler.run');
  await shellDone(page);
}

async function click(page: Page, x: number, y: number): Promise<void> {
  await typeLine(page, `click_fixed(${x}, ${y})`);
  await shellDone(page);
  await runLive(page);
}

async function doubleClick(page: Page, x: number, y: number): Promise<void> {
  await typeLine(page, `double_click_fixed(${x}, ${y})`);
  await shellDone(page);
  await runLive(page);
}

test('MacTest Pro (68k) on a IIci with a 24AC and an 8*24 GC passes at Max speed', async ({ page }) => {
  test.skip(!fs.existsSync(CD), 'tests/data/apps/mactest_pro_68k_feb_1996.iso not fetched');
  test.setTimeout(30 * 60 * 1000);

  await gotoWeb2(page);
  await stageOpfsFile(page, '/opfs/images/rom/368CADFE', ROM);
  // Apple's declaration ROMs for the two cards, where the page offers card
  // ROMs from (without them each card runs the substitute ROM, which
  // MacTest Pro does not know).
  for (const v of ['display-card-24ac-d8daab87.vrom', '824gc-v1.1-revb-d722b053.vrom'])
    await stageOpfsFile(page, `/opfs/images/vrom/${v}`, path.join(DATA, 'roms', v));
  await stageOpfsFileStreaming(page, '/opfs/images/cd/mactest68k.iso', CD);
  await stageOpfsFile(page, '/opfs/upload/mtmax/mac.script', path.resolve(__dirname, '../../integration/lib/mac.script'));
  for (const g of ['finder.png', 'run-folder.png', 'main-window.png', 'insert-disk.png', 'passed.png'])
    await stageOpfsFile(page, `/opfs/upload/mtmax/g/${g}`, path.join(GOLDENS, g));
  // The worker's mount sees main-thread OPFS writes after a reload.
  await gotoWeb2(page);

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.console')).toBeVisible({ timeout: 15_000 });
  await typeLine(
    page,
    'machine.boot model="iici" ram=8192 slots="12=display_card_24ac;13=824gc" rom="/opfs/images/rom/368CADFE"',
  );
  await sleep(page, 3_000);
  await typeLine(page, 'machine.rtc.time = "2026-10-01T12:00:00"');
  await typeLine(page, 'machine.scsi.attach_cdrom "/opfs/images/cd/mactest68k.iso" 3');
  await sleep(page, 1_000);
  await typeLine(page, 'include "/opfs/upload/mtmax/mac.script"');
  await shellDone(page);
  // A terminal boot leaves the machine stopped: run it live, at Max.
  await typeLine(page, 'scheduler.run');
  const t0 = Date.now();
  const log = (s: string) => console.log(`mactest-max +${((Date.now() - t0) / 1000).toFixed(1)}s ${s}`);

  // The Finder: the current application is "Finder".
  await expect
    .poll(
      async () =>
        (await gsEvalInPage(page, 'machine.cpu.mmu.peek', [0x910, 4])) === 0x0646696e &&
        (await gsEvalInPage(page, 'machine.cpu.mmu.peek', [0x914, 2])) === 0x6465,
      { timeout: 300_000, intervals: [1000] },
    )
    .toBe(true);
  await waitStable(page, 3_000, 120_000);
  log(`Finder; matches finder.png: ${await matches(page, 'finder.png')}`);
  await shot(page, '1-finder');

  await doubleClick(page, 67, 190); // "Run MacTest Pro (sys 7)"
  await waitStable(page, 3_000, 60_000);
  log(`run folder; matches run-folder.png: ${await matches(page, 'run-folder.png')}`);
  await shot(page, '2-run-folder');

  await doubleClick(page, 140, 105); // the "Mac II Family" configuration
  const mainMs = await waitMatch(page, 'main-window.png', 300_000);
  log(`main window after ${mainMs} ms`);
  await shot(page, '3-main-window');
  expect(mainMs, 'MacTest Pro main window (nine tests)').toBeGreaterThanOrEqual(0);

  await click(page, 107, 103); // Test
  const promptMs = await waitMatch(page, 'insert-disk.png', 300_000);
  log(`floppy prompt after ${promptMs} ms`);
  await shot(page, '4-insert-disk');
  expect(promptMs, 'the floppy test asks for a disk').toBeGreaterThanOrEqual(0);

  await gsCallInPage(page, 'files.fd_create', ['/opfs/upload/mtmax/blank-hd.dsk', true]);
  await gsCallInPage(page, 'machine.floppy.drive[0].insert', ['/opfs/upload/mtmax/blank-hd.dsk', true]);
  log('blank HD floppy inserted');

  const passMs = await waitMatch(page, 'passed.png', 20 * 60 * 1000);
  log(`passed after ${passMs} ms; instr_count=${await gsEvalInPage(page, 'scheduler.instr_count')}`);
  await shot(page, '5-end');
  expect(passMs, '"All selected tests passed."').toBeGreaterThanOrEqual(0);
});
