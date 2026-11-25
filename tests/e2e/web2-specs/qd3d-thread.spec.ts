// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the QuickDraw 3D Accelerator Card's worker-thread raster backend
// in the BROWSER (the voodoo2-thread.spec.ts idiom).
//
// The card renders on a pthread of its own -- a Web Worker created on
// demand under Emscripten -- while the emulator keeps running; a frame's
// pixels reach memory when the frame completes, after the producer has
// fenced the worker.  Nothing native can see whether that Worker starts in
// the browser or whether the fence returns, so this spec boots a 7500 with
// the card seated, asks which backend it got, then pokes a one-scanline
// frame into RAM (the tnt-qd3d device row's encoding, cut down to one
// triangle), starts it through the card's BAR, lets the machine run until
// the frame completes and reads the pixels back.
//
// Everything goes through the shipped Terminal panel.

import { test, expect, type Page } from '../helpers/test';
import { gotoWeb2 } from '../helpers/web2-fs';
import { terminalRun as typeLine, terminalEval } from '../helpers/terminal';
import { uploadRomViaWelcome } from '../helpers/upload';
import { STORED_ROM, TNT_ROM, cfgWrite, hex } from '../helpers/tnt';

const terminalRun = (page: Page, line: string) =>
  typeLine(page, line, { settleMs: 250 });

// The card's own BAR and the frame in RAM are big-endian and go out as
// they are; only Bandit 1's config ports take swapped literals (cfgWrite).
const put = (page: Page, addr: number, value: number) =>
  terminalRun(page, `machine.memory.poke.l(${hex(addr)}, ${hex(value)})`);

const BAR = 0x81000000;
const PROG = 0x100000; // control block: Y and width
const OBJ = 0x110000; // one Gouraud triangle
const SCAN = 0x100040; // control block: ScanlineStart
const DESC = 0x200000; // output descriptors
const OUT = 0x300000; // the scanline's 16 pixels

test('the QuickDraw 3D card renders on a Web Worker, and a frame lands when it completes', async ({ page }) => {
  test.setTimeout(6 * 60 * 1000);
  await gotoWeb2(page);

  await uploadRomViaWelcome(page, TNT_ROM);
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'pm7500-pm8500-pm9500-96cd923d.rom added' }),
  ).toBeVisible({ timeout: 60_000 });
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator('option[value="pm7500"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption('pm7500');
  await page.getByRole('button', { name: 'Start', exact: true }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 60_000,
  });
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.console')).toBeVisible({ timeout: 15_000 });

  // Re-boot with the card seated, let the ROM bring up the memory
  // controller (before it has, the bus master's view of RAM is not the
  // CPU's), then halt.
  await terminalRun(page, `machine.boot model="pm7500" ram=32768 rom="${STORED_ROM}" pci_card="qd3d"`);
  await page.waitForTimeout(1500);
  await terminalRun(page, 'scheduler.stop');
  await terminalRun(page, 'scheduler.run 50000000');
  await page.waitForTimeout(1000);
  expect(await terminalEval(page, 'scheduler.running')).toBe('false');
  expect(await terminalEval(page, 'machine.pci.slot[1].card.renderer.frames')).toBe('0');

  // "thread" means pthread_create succeeded on Emscripten's Workers; a
  // fallback to "sw" is exactly the failure this spec exists to catch.
  expect(await terminalEval(page, 'machine.pci.slot[1].card.renderer.raster')).toBe('thread');

  // The BAR at $81000000, memory space and bus mastering on.
  await cfgWrite(page, 0x10, BAR);
  await cfgWrite(page, 0x04, 0x0016);

  // The frame: Y (scanline 0, no next) and a 16-pixel width; a red opaque
  // triangle (0,0) (16,0) (0,2) -- on scanline 0's centre it covers
  // pixels 0..11 -- at depth 0.5; then ScanlineStart, and the end.
  const cb = [PROG, OBJ, 0x20000004, 0xffff0004, 0x20000030, 15, 0, 0, 0];
  for (let i = 1; i < cb.length; i++) await put(page, PROG + 4 * (i - 1), cb[i]);
  const tri = [SCAN, 0x68200000, 0x00000000, 0x00800000, 0x00000010, 0x3f000000, 0x3f000000, 0x3f000000,
    0xffff0000, 0xffff0000, 0xffff0000];
  for (let i = 0; i < tri.length; i++) await put(page, OBJ + 4 * i, tri[i]);
  const scan = [0, 0x20000028, 0, 0, 0, 0, 0, 0];
  for (let i = 0; i < scan.length; i++) await put(page, SCAN + 4 * i, scan[i]);
  // One WRITE of 64 bytes, then STOP; the output pre-filled.
  for (const [a, v] of [[DESC, 0x40], [DESC + 4, OUT], [DESC + 8, 0], [DESC + 12, 0], [DESC + 16, 0x08000000],
    [OUT, 0xeeeeeeee], [OUT + 60, 0xeeeeeeee]] as const)
    await put(page, a, v);

  // Reset the channel, background grey, mode 0, the descriptor list, run
  // with interrupts, go.
  await put(page, BAR + 0x40, 0x1000);
  await put(page, BAR + 0x40, 0);
  await put(page, BAR, 0x20000008);
  await put(page, BAR, 0xff404040);
  await put(page, BAR + 0x4c, DESC);
  await put(page, BAR + 0x40, 0xc000);
  await put(page, BAR, 0x20000020);
  await put(page, BAR, PROG);

  // Walked at the go write; the pixels wait for the frame's completion.
  expect(await terminalEval(page, 'machine.pci.slot[1].card.renderer.frames')).toBe('1');
  expect(await terminalEval(page, 'machine.pci.slot[1].card.renderer.objects')).toBe('1');
  expect(await terminalEval(page, 'machine.pci.slot[1].card.renderer.irq')).toBe('false');
  expect(Number(await terminalEval(page, `machine.memory.peek.l(${hex(OUT)})`))).toBe(0xeeeeeeee);

  await terminalRun(page, 'scheduler.run 200000');
  await page.waitForTimeout(1000);
  expect(await terminalEval(page, 'machine.pci.slot[1].card.renderer.irq')).toBe('true');
  // Pixel 0 under the triangle, red; pixel 15 past it, the background.
  expect(Number(await terminalEval(page, `machine.memory.peek.l(${hex(OUT)})`))).toBe(0x00ff0000);
  expect(Number(await terminalEval(page, `machine.memory.peek.l(${hex(OUT + 60)})`))).toBe(0x00404040);
});
