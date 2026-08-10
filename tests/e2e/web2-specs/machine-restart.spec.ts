// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: the Restart button power-cycles the machine and media survives.
//
// Pins proposal-boot-vs-reset stage (C): web2's restart() calls the core's
// machine.restart, which rebuilds the recorded machine and TRANSFERS the
// open image handles across the teardown (§3.3) — no cached-config replay,
// no manual re-attachment in emulator.ts.  The storage-instance stem
// (image.path) must be identical before and after the restart: an equal
// stem proves the handle was transferred rather than reopened, which is
// the write-durability guarantee (a reopen would mint a fresh delta and
// discard every write made since the attach).

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';

const IICX_ROM = path.resolve(__dirname, '../../data/roms/iix-iicx-se30-97221136.rom');

// Type one shell line into the Terminal panel's xterm.
async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
}

// Echo an expression through the terminal under a unique key and return the
// printed value (fresh key per probe so stale echoes can't satisfy the
// match); values still starting with `$` are the input echo, not the result.
// Keystrokes can race a busy xterm render (a probe typed right after heavy
// output sometimes never lands), so each probe retries with a fresh key.
let probeSeq = 0;
async function terminalEval(page: Page, expr: string): Promise<string | null> {
  for (let attempt = 0; attempt < 3; attempt++) {
    const key = `mrs${++probeSeq}`;
    await terminalRun(page, `echo "${key}=\${${expr}}"`);
    for (let i = 0; i < 12; i++) {
      await page.waitForTimeout(400);
      const text = await page.locator('.xterm-rows').innerText();
      const values = [...text.matchAll(new RegExp(`${key}=(\\S+)`, 'g'))]
        .map((m) => m[1])
        .filter((v) => !v.startsWith('$'));
      if (values.length) return values[values.length - 1];
    }
  }
  return null;
}

test('Restart keeps the attached hard disk — same medium, same open instance', async ({
  page,
}) => {
  test.setTimeout(240_000);
  await gotoWeb2(page);

  // Upload the IIcx ROM via the Welcome button (no auto-boot).
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await chooser.setFiles(IICX_ROM);
  await expect(
    page.locator('.toast .msg').filter({ hasText: 'iix-iicx-se30-97221136.rom uploaded' }),
  ).toBeVisible({ timeout: 60_000 });

  // Boot from the Terminal panel and attach a scratch HD.
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });
  await terminalRun(page, 'machine.boot model="iicx" ram=8192 rom="/opfs/images/rom/97221136"');
  await page.waitForTimeout(3_000); // let the boot's terminal output settle
  expect(await terminalEval(page, 'machine.id')).toBe('iicx');
  await terminalRun(page, 'storage.hd_create("/tmp/restart-scratch.img", "20mb")');
  await page.waitForTimeout(1_000);
  await terminalRun(page, 'machine.scsi.attach_hd "/tmp/restart-scratch.img" 0');
  await page.waitForTimeout(3_000); // the persist copy prints; let the render settle
  expect(await terminalEval(page, 'machine.scsi.device[0].image.present')).toBe('true');
  const stem0 = await terminalEval(page, 'machine.scsi.device[0].image.path');
  const file0 = await terminalEval(page, 'machine.scsi.device[0].image.filename');
  expect(stem0).toBeTruthy();
  expect(file0).toBeTruthy();

  // Power-cycle via the Debug toolbar's Restart button — the UI path that
  // routes through bus/debug.ts restart() → machine.restart.
  await page.locator('button.ptab[data-tab="debug"]').click();
  await page.getByRole('button', { name: 'Restart' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine restarted' })).toBeVisible({
    timeout: 60_000,
  });

  // The HD survived the power-cycle as the SAME open instance.
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });
  expect(await terminalEval(page, 'machine.created')).toBe('true');
  expect(await terminalEval(page, 'machine.scsi.device[0].image.present')).toBe('true');
  expect(await terminalEval(page, 'machine.scsi.device[0].image.filename')).toBe(file0);
  expect(await terminalEval(page, 'machine.scsi.device[0].image.path')).toBe(stem0);
});
