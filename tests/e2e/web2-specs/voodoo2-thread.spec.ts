// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Voodoo2's worker-thread raster backend in the BROWSER
// (proposal-voodoo2-raster-thread §5.7, phase 2).
//
// The wasm build runs the emulator on one Web Worker (PROXY_TO_PTHREAD)
// and, since this spec's commit, the Voodoo2 rasteriser on a second one
// preallocated at link time (-sPTHREAD_POOL_SIZE=2).  Nothing in the
// native gates can see whether that second pthread actually starts under
// Emscripten's pthreads-on-Workers, whether the producer's fences
// (pthread_cond_wait on the proxied main thread) return, or whether the
// pool is sized so pthread_create does not stall — so this spec boots a
// 7500 with the card seated, asks the card which backend it got, and then
// draws through the aperture and reads the result back: the pixel comes
// through an LFB read (a fence), the counters through v2_observe (a
// fence).  The values are the unit row's (tests/integration/
// tnt-pci-voodoo2): one 136-pixel right triangle, red at vertex A.
//
// Everything goes through the shipped Terminal panel (web2 has no
// window.gsEval), the scheduler-accelerated.spec.ts idiom.

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const TNT_ROM = path.join(DATA, 'roms', 'pm7500-pm8500-pm9500-96cd923d.rom');
const STORED_ROM = '/opfs/images/rom/96CD923D';

// Bandit 1's config ports are little-endian and machine.memory.poke is a
// raw big-endian bus view (the tnt-pci-voodoo2 row's idiom), so the
// literals are byte-swapped here, at the spec level.
function swap32(v: number): number {
  return (((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >>> 8) & 0xff00) | ((v >>> 24) & 0xff)) >>> 0;
}
const hex = (v: number): string => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(8, '0');

async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(250);
}

// Read one expression back through the terminal with a fresh key per probe
// so the echoed input line (still holding the unexpanded ${...}) can never
// satisfy the match — only the response can.
let probeSeq = 0;
async function probe(page: Page, expr: string): Promise<string> {
  for (let attempt = 0; attempt < 30; attempt++) {
    const key = `v2p${++probeSeq}`;
    await terminalRun(page, `echo "${key}=[${'$'}{${expr}}]"`);
    await page.waitForTimeout(400);
    const text = await page.locator('.xterm-rows').innerText();
    const m = text.match(new RegExp(`${key}=\\[([A-Za-z0-9_.-]+)\\]`));
    if (m) return m[1];
  }
  throw new Error(`terminal probe never answered: ${expr}`);
}

// One PCI config-space dword write through Bandit 1's ports: socket A1 is
// device 13, one-hot IDSEL $00200000.
async function cfgWrite(page: Page, reg: number, value: number): Promise<void> {
  await terminalRun(page, `machine.memory.poke.l(0xF2800000, ${hex(swap32(0x00002000 | reg))})`);
  await terminalRun(page, `machine.memory.poke.l(0xF2C00000, ${hex(swap32(value))})`);
  await terminalRun(page, `machine.memory.poke.l(0xF2800000, 0)`);
}

const BAR = 0x81000000;
async function vregWrite(page: Page, off: number, value: number): Promise<void> {
  await terminalRun(page, `machine.memory.poke.l(${hex(BAR + off)}, ${hex(swap32(value))})`);
}

test('the Voodoo2 rasterises on a second Web Worker, and the shadow is authoritative when read', async ({
  page,
}) => {
  test.setTimeout(6 * 60 * 1000);
  await gotoWeb2(page);

  // The TNT ROM via the Welcome "Upload ROM..." button, stored under its
  // checksum; then a machine of any kind so the Terminal panel is live.
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await chooser.setFiles(TNT_ROM);
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'pm7500-pm8500-pm9500-96cd923d.rom uploaded' }),
  ).toBeVisible({ timeout: 60_000 });
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator('option[value="pm7500"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption('pm7500');
  await page.getByRole('button', { name: 'Start Machine' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 60_000,
  });
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });

  // Re-boot with the card seated (the typed document, no option: the
  // BROWSER DEFAULT is what this spec pins).  The machine stays halted —
  // the guest never runs; the aperture is driven from here.
  await terminalRun(page, `machine.boot model="pm7500" ram=32768 rom="${STORED_ROM}" pci_card="voodoo2"`);
  await page.waitForTimeout(1500);
  // Halt whatever the boot left running: the guest's own Open Firmware
  // would otherwise enumerate the bus underneath the config pokes below.
  await terminalRun(page, 'scheduler.stop');
  expect(await probe(page, 'scheduler.running')).toBe('false');
  // The card is seated: its 4 MB framebuffer (the name carries a space,
  // which the probe's strict value pattern deliberately excludes).
  expect(await probe(page, 'machine.pci.slot[1].card.regs.fb_size')).toBe('4194304');

  // --- The backend the browser build installs. -----------------------------
  // "thread" means pthread_create succeeded on Emscripten's Worker pool
  // and the worker is running; a fallback to "sw" is exactly the failure
  // this spec exists to catch.
  expect(await probe(page, 'machine.pci.slot[1].card.regs.raster')).toBe('thread');

  // --- Map the aperture and open the gates, as the driver would. -----------
  await cfgWrite(page, 0x10, BAR);
  await cfgWrite(page, 0x04, 0x0002);
  await cfgWrite(page, 0x40, 0x00000003);
  expect(await probe(page, 'machine.pci.slot[1].card.config.bar[0].base')).toBe(hex(BAR).toLowerCase());
  // LFB reads on (fbiInit1[3], gated by initEnable[0] just opened) and the
  // front-buffer layout the unit row uses.
  await vregWrite(page, 0x218, (150 << 11) | 0x400000);
  await vregWrite(page, 0x214, 0x00201102 | 0x8);
  await vregWrite(page, 0x20c, (480 << 16) | 639);

  // --- One gouraud triangle: A=(50,50) B=(50,66) C=(66,50) in 12.4. --------
  // 136 pixels under the fill convention; red 255 at A, dither off, rgb
  // write mask on (fbzMode $200 with clipping off).
  await vregWrite(page, 0x110, 0x00000200);
  await vregWrite(page, 0x104, 0x00000000);
  await vregWrite(page, 0x008, 0x00000320);
  await vregWrite(page, 0x00c, 0x00000320);
  await vregWrite(page, 0x010, 0x00000320);
  await vregWrite(page, 0x014, 0x00000420);
  await vregWrite(page, 0x018, 0x00000420);
  await vregWrite(page, 0x01c, 0x00000320);
  await vregWrite(page, 0x020, 0x000ff000);
  await vregWrite(page, 0x024, 0x00000000);
  await vregWrite(page, 0x028, 0x00000000);
  await vregWrite(page, 0x040, 0x00000000);
  await vregWrite(page, 0x060, 0x00000000);
  await vregWrite(page, 0x080, 0x80000000);

  // --- Observed through the fences. ----------------------------------------
  // fbiPixelsOut is executor-owned: the read retires the queue on the
  // second Worker and mirrors the count.  fbiTrianglesOut counts at
  // submission on the producer and needs no fence (the seam's contract).
  expect(await probe(page, 'machine.pci.slot[1].card.regs.read(0x15C)')).toBe('136');
  expect(await probe(page, 'machine.pci.slot[1].card.regs.read(0x25C)')).toBe('1');
  // The pixel at vertex A, through an LFB read: 5-6-5 red $F800 in the
  // card's little-endian domain, so the big-endian halfword view reads
  // $00F8 = 248.  (LFB offset = y<<11 | x<<1.)
  const lfbA = BAR + 0x400000 + (50 << 11) + (50 << 1);
  expect(Number(await probe(page, `machine.memory.peek.w(${hex(lfbA)})`))).toBe(0xf8);
  // ...and just left of A is untouched.
  expect(Number(await probe(page, `machine.memory.peek.w(${hex(lfbA - 2)})`))).toBe(0);
});
