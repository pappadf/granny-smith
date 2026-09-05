// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Shared driver for the Voodoo2 WebGPU takeover e2e specs
// (voodoo2-webgpu.spec.ts, voodoo2-webgpu-fallback.spec.ts): boot a 7500
// with the card under a chosen raster backend through the shipped UI,
// probe the object model through the Terminal panel, and compose the
// aperture-driven script (helpers + bring-up replay + take-monitor +
// the takeover's drawing section) the takeover spec stages into OPFS.
// Two spec files because Playwright takes launchOptions per file, and
// the two need different browsers (with and without WebGPU).

import { expect, type Page } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { gotoWeb2 } from './web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const TNT_ROM = path.join(DATA, 'roms', 'pm7500-pm8500-pm9500-96cd923d.rom');
const STORED_ROM = '/opfs/images/rom/96CD923D';
const ROW = path.resolve(__dirname, '../../integration/tnt-pci-voodoo2');

// The base launch args of playwright.web2.config.ts (software WebGL for
// the app's probe) plus Chromium's software WebGPU adapter.  Flags land
// per file, so the rest of the suite runs untouched.
export const BASE_ARGS = [
  '--use-gl=angle',
  '--use-angle=swiftshader-webgl',
  '--ignore-gpu-blocklist',
  '--disable-dev-shm-usage',
];
export const WEBGPU_ARGS = [
  '--enable-unsafe-webgpu',
  '--enable-features=Vulkan,WebGPU',
  '--use-webgpu-adapter=swiftshader',
];

export async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(250);
}

// Read one expression back through the terminal with a fresh key per
// probe so the echoed input line (still holding the unexpanded ${...})
// can never satisfy the match — only the response can.
let probeSeq = 0;
export async function probe(page: Page, expr: string): Promise<string> {
  for (let attempt = 0; attempt < 30; attempt++) {
    const key = `v2g${++probeSeq}`;
    await terminalRun(page, `echo "${key}=[${'$'}{${expr}}]"`);
    await page.waitForTimeout(400);
    const text = await page.locator('.xterm-rows').innerText();
    const m = text.match(new RegExp(`${key}=\\[([A-Za-z0-9_.-]+)\\]`));
    if (m) return m[1];
  }
  throw new Error(`terminal probe never answered: ${expr}`);
}

// Boot the 7500 with the card seated under the requested backend and
// halt the guest, leaving the Terminal live.  The aperture is driven
// from there.
export async function bootWithCard(page: Page, rasterOption: string): Promise<void> {
  await gotoWeb2(page);
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
  await terminalRun(
    page,
    `machine.boot model="pm7500" ram=32768 rom="${STORED_ROM}" pci_card="voodoo2" pci_option="raster=${rasterOption}"`,
  );
  await page.waitForTimeout(1500);
  // Halt whatever the boot left running: the guest's own Open Firmware
  // would otherwise enumerate the bus underneath the config pokes.
  await terminalRun(page, 'scheduler.stop');
  expect(await probe(page, 'scheduler.running')).toBe('false');
  expect(await probe(page, 'machine.pci.slot[1].card.regs.fb_size')).toBe('4194304');
}

// The composed script: the row's own helpers, Memory Space and the BAR,
// the bring-up replay, the take-monitor sequence of the display row, and
// the takeover's drawing section.
export function composeScript(): string {
  const helpers = `
def swap32(v) {
    return (($v & 0xFF) << 24) | (($v & 0xFF00) << 8) | (($v >> 8) & 0xFF00) | (($v >> 24) & 0xFF)
}
def swap16(v) {
    return (($v & 0xFF) << 8) | (($v >> 8) & 0xFF)
}
def cfg_wr(reg, val) {
    machine.memory.poke.l(0xF2800000, swap32(0x00002000 | $reg))
    machine.memory.poke.l(0xF2C00000, swap32($val))
    machine.memory.poke.l(0xF2800000, 0)
}
def cfg_rd(reg) {
    machine.memory.poke.l(0xF2800000, swap32(0x00002000 | $reg))
    let v = swap32(machine.memory.peek.l(0xF2C00000))
    machine.memory.poke.l(0xF2800000, 0)
    return $v
}
cfg_wr(0x10, 0x81000000)
cfg_wr(0x04, 0x0002)
let BAR = machine.pci.slot[1].card.config.bar[0].base
def vreg_wr(off, val) {
    machine.memory.poke.l($BAR + $off, swap32($val))
}
def vreg_rd(off) {
    return swap32(machine.memory.peek.l($BAR + $off))
}
def idle3() {
    let ok = 0
    let polls = 0
    while $ok < 3 {
        $polls = $polls + 1
        assert $polls < 50 "idle contract: status[9] must clear within a bounded number of polls"
        if (vreg_rd(0x000) & 0x380) == 0 {
            $ok = $ok + 1
        } else {
            $ok = 0
        }
    }
}
`;
  // The bring-up replay, minus its one clock-dependent step: the beam-
  // counter check runs the scheduler, which this spec keeps halted (the
  // guest's firmware would otherwise enumerate the bus underneath us).
  const glideInit = fs
    .readFileSync(path.join(ROW, 'glide-init.script'), 'utf8')
    .replace(/# The beam advances:[\s\S]*?assert \$L0 != \$L1 "the beam counters must advance"\n/, '');
  if (glideInit.includes('the beam counters must advance')) throw new Error('beam step not stripped');
  const takeMonitor = `
# --- Take the monitor: the edge that engages GPU mode (§5.1). --------------
vreg_wr(0x220, (704 << 16) | 96)
vreg_wr(0x224, (523 << 16) | 2)
vreg_wr(0x208, (25 << 16) | 38)
vreg_wr(0x20C, (480 << 16) | 639)
vreg_wr(0x214, (vreg_rd(0x214) & 0xFFFFEEFF) | 0x1E000)
vreg_wr(0x210, vreg_rd(0x210) | 0x1)
assert machine.pci.slot[1].card.video.drives_monitor "the card drives the monitor"
# Reading the screen re-resolves the display chain, which is where the
# card sees the edge (the scheduler is halted, so no frame would).
let W = machine.screen.width
assert $W == 640 "the screen is the card's"
`;
  const drawGpu = fs.readFileSync(path.join(ROW, 'draw-webgpu.script'), 'utf8');
  return [helpers, glideInit, takeMonitor, drawGpu].join('\n');
}
