import { chromium } from 'playwright';
import fs from 'node:fs';
const PORT = 18090;
const b = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader-webgl', '--ignore-gpu-blocklist', '--disable-dev-shm-usage', '--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--use-webgpu-adapter=swiftshader'] });
const page = await b.newPage();
page.on('console', (m) => { const t = m.text(); if (/voodoo2|gpu|error/i.test(t)) console.log('[console]', t.slice(0, 400)); });
page.on('pageerror', (e) => console.log('[pageerror]', String(e)));
async function run(line) { await page.locator('.xterm').click(); await page.keyboard.type(line); await page.keyboard.press('Enter'); await page.waitForTimeout(300); }
async function overlayPixel() {
  return page.evaluate(() => {
    const c = document.getElementById('screen3d');
    const t = document.createElement('canvas'); t.width = c.width; t.height = c.height;
    const ctx = t.getContext('2d'); ctx.drawImage(c, 0, 0);
    const p = ctx.getImageData(300, 200, 1, 1).data; return [p[0], p[1], p[2], c.hidden];
  });
}
await page.goto(`http://localhost:${PORT}/index.html`);
await page.waitForFunction(() => window.__gsReady === true, undefined, { timeout: 60000 });
const cont = page.getByRole('button', { name: 'Continue' }); if (await cont.isVisible().catch(() => false)) await cont.click();
const [chooser] = await Promise.all([page.waitForEvent('filechooser'), page.getByRole('button', { name: 'Upload ROM...' }).click()]);
await chooser.setFiles('../data/roms/pm7500-pm8500-pm9500-96cd923d.rom');
await page.locator('.toast .msg').filter({ hasText: 'uploaded' }).waitFor({ timeout: 60000 });
await page.getByRole('button', { name: 'New Machine...' }).click();
await page.locator('#cfg-model option[value="pm7500"]').waitFor({ state: 'attached', timeout: 30000 });
await page.locator('#cfg-model').selectOption('pm7500');
await page.getByRole('button', { name: 'Start Machine' }).click();
await page.locator('.toast .msg').filter({ hasText: 'Machine started' }).waitFor({ timeout: 60000 });
await page.locator('button.ptab[data-tab="terminal"]').click();
await page.locator('.xterm').waitFor({ timeout: 15000 });
await run('machine.boot model="pm7500" ram=32768 rom="/opfs/images/rom/96CD923D" pci_card="voodoo2"');
await page.waitForTimeout(1500);
await run('scheduler.stop');
const pro = fs.readFileSync('diag-prologue.tmp.script', 'utf8');
await page.evaluate(async (data) => { const dir = await navigator.storage.getDirectory(); const up = await dir.getDirectoryHandle('upload', { create: true }); const fh = await up.getFileHandle('diag.script', { create: true }); const w = await fh.createWritable(); await w.write(data); await w.close(); }, pro);
await run('include "/opfs/upload/diag.script"');
await page.waitForTimeout(3000);
console.log((await page.locator('.xterm-rows').innerText()).split('\n').filter(l => /prologue|engaged/.test(l)).slice(-2).join(' | '));
console.log('before any present:', await overlayPixel());
await run('scheduler.run 5000000');
await page.waitForTimeout(3000);
console.log('after 5M instr:', await overlayPixel());
await run('echo "S ${machine.pci.slot[1].card.regs.gpu_stats}"');
await page.waitForTimeout(800);
console.log((await page.locator('.xterm-rows').innerText()).split('\n').filter(l => /engaged=|presents/.test(l)).slice(-3).join('\n'));
await b.close();
