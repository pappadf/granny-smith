// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: booting a machine that has NO NuBus slots must not carry a video
// card into its boot document.
//
// Reported on the v0.8.0 staging deploy:
//   Boot failed: machine.boot: model 'q660av' has no NuBus slots for
//   video_card 'se30'
//
// 'se30' is the SE/30 built-in-video GENERIC card kind
// (src/machines/glue/builtin_se30_video.c, `.id = "se30"`), offered as a
// selectable sibling in the SE/30's builtin video slot. The AV machines
// declare `.nubus_slots = NULL` (src/machines/av/q660av.c), so
// build_video_slots() reports an EMPTY slot list for them and `video_card=`
// has nothing legal to name — machine.c rejects any value outright.
//
// The trigger is a SECOND boot in one session, not the dialog: machine.boot's
// inheritance step (machine.c step 1) fills every field the document omits
// from the previous machine's built-from record. The dialog correctly sends no
// video_card for a slotless model, and the core then supplies the last one.

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';

const SE30_ROM = path.resolve(__dirname, '../../data/roms/iix-iicx-se30-97221136.rom');
const AV_ROM = path.resolve(__dirname, '../../data/roms/q840av-q660av-5bf10fd1.rom');

async function uploadRom(page: Page, file: string): Promise<void> {
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await chooser.setFiles(file);
}

// Wait for the machine to come up, harvesting toast text on every tick.
// Toasts auto-expire, so a boot rejection would otherwise vanish before the
// assertion notices and leave only a bare "still Stopped" timeout — the
// failure has to name the reason the app actually gave.
async function expectBooted(page: Page, model: string): Promise<void> {
  const running = page.locator('.gs-statusbar .sb-state .label');
  const seen = new Set<string>();
  const deadline = Date.now() + 60_000;
  for (;;) {
    for (const t of await page.locator('.toast .msg').allInnerTexts()) seen.add(t.trim());
    if ((await running.innerText().catch(() => '')) === 'Running') break;
    if (Date.now() > deadline) {
      throw new Error(
        `${model} never reached Running. Toasts seen:\n  ${[...seen].join('\n  ') || '(none)'}`,
      );
    }
    await page.waitForTimeout(200);
  }
  expect(
    await page.locator('#screen').evaluate((el) => (el as HTMLCanvasElement).width),
  ).toBeGreaterThan(0);
}

async function bootModel(page: Page, model: string): Promise<void> {
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const sel = page.locator('#cfg-model');
  await expect(sel.locator(`option[value="${model}"]`)).toHaveCount(1, { timeout: 60_000 });
  await sel.selectOption(model);
  await page.getByRole('button', { name: 'Start Machine' }).click();
}

test('a slotless model boots after a carded model ran in the same session', async ({ page }) => {
  test.setTimeout(240_000);
  const pageErrors: string[] = [];
  page.on('pageerror', (e) => pageErrors.push(e.message));

  await gotoWeb2(page);
  await uploadRom(page, SE30_ROM);
  await uploadRom(page, AV_ROM);

  // 1. Boot the SE/30. Its builtin slot has two sibling card kinds, so the
  // boot record captures a real video_card ('se30', the generic kind).
  await bootModel(page, 'se30');
  await expectBooted(page, 'se30');

  // 2. Shut it down and boot the slotless AV machine. Its boot document
  // carries no video_card — the core must not supply the SE/30's.
  await page.getByRole('button', { name: 'Shut down' }).click();
  await bootModel(page, 'q660av');
  await expectBooted(page, 'q660av');

  expect(pageErrors, `uncaught page errors:\n${pageErrors.join('\n')}`).toHaveLength(0);
});
