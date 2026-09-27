// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Printing in the browser: the LaserWriter bridge's OPEN / FEED / FINISH go
// over the shared-memory ring to the platen worker, and the finished PDF
// comes back to the page as a download.  The guest side is the integration
// suite's appletalk-print script (Chooser -> LaserWriter -> Finder Print
// Directory on System 6.0.8), driven here from the terminal.

import { test, expect } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile, stageOpfsText } from '../helpers/web2-fs';
import { terminalRun } from '../helpers/terminal';

const DATA = path.resolve(__dirname, '../../data');
const PLUS_ROM = path.join(DATA, 'roms', 'plus-v3-4d1f8172.rom');
const SYSTEM_HD = path.join(DATA, 'systems', 'system_6_0_8_20mb_8_24gc.img');

const PRINT_SCRIPT = `machine.boot model="plus" rom="/opfs/images/rom/4D1F8172"
scheduler.stop
scheduler.mode = "turbo"
machine.scsi.attach_hd "/opfs/images/hd/print-hd.img" 0
machine.rtc.pram.boot_device = 0
machine.rtc.time = "2026-04-26T21:30:36"
scheduler.run 30000000
appletalk.printer.enabled = true
assert appletalk.printer.status == "status: idle" "printer is not idle before printing"
assert appletalk.printer.documents == 0 "a document exists before any print"
machine.adb.mouse.move 25 10
scheduler.run 2000000
machine.adb.mouse.click true
scheduler.run 4000000
machine.adb.mouse.move 25 123
scheduler.run 4000000
machine.adb.mouse.click false
scheduler.run 60000000
machine.adb.mouse.move 165 135
scheduler.run 2000000
machine.adb.mouse.click true
scheduler.run 2000000
machine.adb.mouse.click false
scheduler.run 20000000
machine.adb.mouse.move 187 206
scheduler.run 2000000
machine.adb.mouse.click true
scheduler.run 2000000
machine.adb.mouse.click false
scheduler.run 60000000
machine.adb.mouse.move 300 88
scheduler.run 2000000
machine.adb.mouse.click true
scheduler.run 2000000
machine.adb.mouse.click false
scheduler.run 20000000
machine.adb.mouse.move 72 47
scheduler.run 2000000
machine.adb.mouse.click true
scheduler.run 2000000
machine.adb.mouse.click false
scheduler.run 40000000
machine.adb.mouse.move 470 52
scheduler.run 2000000
machine.adb.mouse.click true
scheduler.run 80000
machine.adb.mouse.click false
scheduler.run 80000
machine.adb.mouse.click true
scheduler.run 80000
machine.adb.mouse.click false
scheduler.run 40000000
machine.adb.mouse.move 53 10
scheduler.run 2000000
machine.adb.mouse.click true
scheduler.run 4000000
machine.adb.mouse.move 100 203
scheduler.run 4000000
machine.adb.mouse.click false
scheduler.run 60000000
machine.adb.mouse.move 452 45
scheduler.run 2000000
machine.adb.mouse.click true
scheduler.run 2000000
machine.adb.mouse.click false
scheduler.run 100000000
scheduler.run 200000000
scheduler.run 300000000
echo "printer: documents=\${appletalk.printer.documents} pages=\${appletalk.printer.last_pages} outcome='\${appletalk.printer.last_outcome}' status='\${appletalk.printer.status}' jobs=\${appletalk.printer.stats.jobs} bytes=\${appletalk.printer.stats.bytes} aborts=\${appletalk.printer.stats.aborts}"
assert appletalk.printer.documents >= 1 "no document was produced"
assert appletalk.printer.last_pages == 1 "the printed document is not one page"
assert appletalk.printer.last_outcome == "ok" "the job did not finish cleanly"
assert appletalk.printer.status == "status: idle" "the printer is not idle after the job"
assert appletalk.printer.stats.jobs >= 1 "the finished job was not counted"
assert appletalk.printer.stats.aborts == 0 "a job was counted as cut off"
assert appletalk.printer.stats.bytes > 1000 "the job's PostScript was not counted"
echo done
`;

// The driver closes the connection the instant its EOF write is acknowledged,
// before the worker's FINISHED comes back (host time, against a guest in
// turbo mode); the bridge keeps the job past that close (appletalk_printer.c,
// the detached job) so the document is counted and downloaded.
test('a print job from the guest ends as a PDF download', async ({ page }) => {
  test.setTimeout(12 * 60 * 1000);
  await gotoWeb2(page);
  await stageOpfsFile(page, '/opfs/images/hd/print-hd.img', SYSTEM_HD);
  await stageOpfsText(page, '/opfs/upload/print.script', PRINT_SCRIPT);

  await stageOpfsFile(page, '/opfs/images/rom/4D1F8172', PLUS_ROM);

  const logs: string[] = [];
  page.on('console', (m) => logs.push(`[${m.type()}] ${m.text()}`));
  page.on('pageerror', (e) => logs.push(`[pageerror] ${e.message}`));

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });

  // The download is the page's side of the finished job; the script's own
  // assertions cover the bridge's side.
  const download = page.waitForEvent('download', { timeout: 10 * 60 * 1000 });
  download.catch(() => {}); // reported below, after the script's own verdict
  await terminalRun(page, 'include "/opfs/upload/print.script"');
  await expect(page.locator('.xterm-rows')).toContainText(/printer: documents=|failed/, {
    timeout: 10 * 60 * 1000,
  });
  const text = await page.locator('.xterm-rows').innerText();
  const line = text.split('\n').find((l) => l.includes('printer: documents='));
  const platen = await page.evaluate(() => {
    const p = (window as unknown as { __platen?: { memory: WebAssembly.Memory; ctrl: number } })
      .__platen;
    if (!p) return 'no attach';
    const w = new Uint32Array(p.memory.buffer, p.ctrl, 16);
    return `ctrl: ${Array.from(w).join(' ')}`;
  });
  console.log(line, '\n', platen, '\n', logs.filter((l) => /platen|printer|laserwriter|pap|download/i.test(l)).join('\n'));
  expect(line).toContain('documents=1');
  expect(line).toContain("outcome='ok'");
  const dl = await download;
  expect(dl.suggestedFilename()).toMatch(/^\d{5}-.*\.pdf$/);
});
