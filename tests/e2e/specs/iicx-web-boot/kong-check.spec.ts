// One-shot visual check: Kong (21" RGB) at 8 bpp should render neutral
// gray, not yellow-tinted, with the CRT response LUT in place.

import { test, expect } from '../../fixtures';
import * as fs from 'node:fs';
import * as path from 'node:path';

const TEST_DATA = path.join(process.cwd(), 'tests', 'data');
function readMedia(rel: string) {
  const fullPath = path.join(TEST_DATA, rel);
  const buffer = fs.readFileSync(fullPath);
  return { name: path.basename(fullPath), buffer };
}

async function runForInstructions(page: any, delta: number) {
  const startCount = (Number(await page.evaluate(() => (window as any).gsEval('cpu.instr_count'))) >>> 0);
  const target = startCount + delta;
  const start = Date.now();
  while (Date.now() - start < 300_000) {
    const cur = Number(await page.evaluate(() => (window as any).gsEval('cpu.instr_count'))) >>> 0;
    if (cur >= target) return cur;
    await page.waitForTimeout(500);
  }
  throw new Error(`runForInstructions: did not reach ${delta}`);
}

test('Kong 8bpp neutralization check', async ({ page }) => {
  test.setTimeout(360_000);
  await page.goto('/index.html');
  await page.waitForLoadState('domcontentloaded');
  await page.waitForFunction(() => {
    const dlg = document.getElementById('rom-upload-dialog');
    return dlg && dlg.getAttribute('aria-hidden') === 'false';
  }, { timeout: 30_000 });
  const rom = readMedia('roms/IIcx.rom');
  await (await page.$('#rom-upload-file-input'))!.setInputFiles({ name: rom.name, mimeType: 'application/octet-stream', buffer: rom.buffer });
  await page.waitForFunction(() => {
    const cfg = document.getElementById('config-dialog');
    return cfg && cfg.getAttribute('aria-hidden') === 'false';
  }, { timeout: 30_000 });
  await page.locator('#config-model').selectOption('iicx');
  await page.waitForFunction(() => !!document.getElementById('config-vrom'), { timeout: 10_000 });
  const vrom = readMedia('roms/Apple-341-0868.vrom');
  {
    const fcp = page.waitForEvent('filechooser');
    await page.locator('#config-vrom').selectOption('__upload__');
    const fc = await fcp;
    await fc.setFiles({ name: vrom.name, mimeType: 'application/octet-stream', buffer: vrom.buffer });
  }
  await page.waitForFunction(() => {
    const sel = document.getElementById('config-vrom') as HTMLSelectElement | null;
    return !!sel && sel.value !== '' && sel.value !== '__upload__';
  }, { timeout: 30_000 });
  const fd = readMedia('systems/System_7_0_1.image');
  {
    const fcp = page.waitForEvent('filechooser');
    await page.locator('#config-fd0').selectOption('__upload__');
    const fc = await fcp;
    await fc.setFiles({ name: fd.name, mimeType: 'application/octet-stream', buffer: fd.buffer });
  }
  await page.waitForFunction(() => {
    const sel = document.getElementById('config-fd0') as HTMLSelectElement | null;
    return !!sel && sel.value !== '' && sel.value !== '__upload__';
  }, { timeout: 60_000 });

  // Pick Kong 8 bpp.
  await page.waitForFunction(() => !!document.getElementById('config-video-mode'), { timeout: 5_000 });
  const dropdownOptions = await page.evaluate(() => {
    const sel = document.getElementById('config-video-mode') as HTMLSelectElement;
    return Array.from(sel.options).map(o => ({ value: o.value, label: o.textContent }));
  });
  console.log('[kong-debug] dropdown count:', dropdownOptions.length, 'sample:', JSON.stringify(dropdownOptions[14]));
  await page.locator('#config-video-mode').selectOption('21in_rgb_8bpp');
  const selectedValue = await page.evaluate(() => (document.getElementById('config-video-mode') as HTMLSelectElement).value);
  console.log('[kong-debug] selected value:', selectedValue);

  await page.click('#config-start-btn');
  await page.waitForFunction(() => (window as any).__gsBootReady === true, { timeout: 60_000 });

  // Kong (1152×870) at the 200% default produces a 2304×1740 screenshot
  // that overflows analysis tooling.  Drive the toolbar zoom-out button
  // down to 100% now that the modal dialogs are out of the way.
  await expect(page.locator('#zoom-level')).toHaveValue('200%', { timeout: 15_000 });
  for (let i = 0; i < 10; i++) {
    await page.click('#zoom-out');
  }
  await expect(page.locator('#zoom-level')).toHaveValue('100%');

  const pendingMode = await page.evaluate(() => (window as any).gsEval('nubus.video_mode'));
  console.log('[kong-debug] post-boot nubus.video_mode:', pendingMode);

  const machineId = await page.evaluate(() => (window as any).gsEval('machine.id'));
  expect(machineId).toBe('iicx');
  const w = await page.evaluate(() => (window as any).gsEval('screen.width'));
  const h = await page.evaluate(() => (window as any).gsEval('screen.height'));
  console.log('[kong-debug] screen', w, 'x', h);

  await runForInstructions(page, 250_000_000);
  await page.waitForTimeout(2000);


  const png = await page.locator('#screen').screenshot();
  fs.writeFileSync('/tmp/kong-8bpp-actual.png', png);
});
