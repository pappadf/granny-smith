// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: checkpoint save → reload → resume, driven entirely through the
// shipped UI. Replaces the browser-level coverage of the legacy
// tests/e2e/specs/state/ suite (the in-session save/load and log-determinism
// halves of that suite live in tests/integration/checkpoint*).
//
// What only a browser test can pin, and what this spec covers:
//   1. The per-machine checkpoint identity round-trip: machineId.ts mints an
//      id into localStorage, machine.register roots /opfs/checkpoints/<id>-
//      <created>/, and after a reload the SAME identity must be re-registered
//      so checkpoint.probe finds the previous session's state.checkpoint.
//   2. The resume prompt flow: maybeOfferBackgroundCheckpoint → the
//      CheckpointResumePrompt modal → Resume loads the checkpoint and the
//      machine comes back live; Start fresh clears it (and a second reload
//      must NOT re-prompt — checkpoint.clear really cleared).
//   3. The SE/30 profile regression (legacy state test 10): restoring after a
//      reload must recreate the machine from the checkpoint's own profile,
//      not a default-model fallback. A Plus-layout restore of SE/30 state
//      (68030, dual VIA, VRAM) corrupts or fails outright, so we assert the
//      restored machine identifies as se30 through the Terminal panel.
//
// Determinism note: we do NOT rely on the ~15 s tick-auto checkpoint or the
// beforeunload handler. The Checkpoints panel's "Create Checkpoint" button
// runs checkpoint.snapshot on the worker, which writes state.checkpoint
// (tmp+rename) before its toast appears — so the reload always finds a
// complete checkpoint, with no timing dependence.

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import { gotoWeb2, stageOpfsFile } from '../helpers/web2-fs';

const PLUS_ROM = path.resolve(__dirname, '../../data/roms/plus-v3-4d1f8172.rom');
const SE30_ROM = path.resolve(__dirname, '../../data/roms/iix-iicx-se30-97221136.rom');
const SE30_VROM = path.resolve(__dirname, '../../data/roms/builtin-se30-video-4f71ff1a.vrom');

// Upload a ROM via the Welcome "Upload ROM..." button (the shipped path; the
// persist bumps the image revision so the config slide re-scans).
async function uploadRom(page: Page, romPath: string): Promise<void> {
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await chooser.setFiles(romPath);
}

// Open the New Machine dialog, pick a model, and start it.
async function startMachine(page: Page, modelId: string): Promise<void> {
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator(`option[value="${modelId}"]`)).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption(modelId);
  const start = page.getByRole('button', { name: 'Start Machine' });
  await expect(start).toBeEnabled();
  await start.click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 60_000,
  });
  await expect(page.locator('.welcome-layer')).toHaveCount(0);
  await expect(statusLabel(page)).toHaveText('Running');
}

function statusLabel(page: Page) {
  return page.locator('.gs-statusbar .sb-state .label');
}

// Save a checkpoint through the Checkpoints panel's header button. The
// gsEval('checkpoint.snapshot') behind it completes before the toast shows,
// so state.checkpoint is durably in OPFS when this returns.
async function createCheckpoint(page: Page): Promise<void> {
  await page.locator('button.ptab[data-tab="checkpoints"]').click();
  await page.getByRole('button', { name: 'Create Checkpoint' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: /Checkpoint '.*' created/ })).toBeVisible(
    { timeout: 30_000 },
  );
  // The view rescans OPFS after the snapshot — the machine's checkpoint row
  // must be listed (pins CheckpointsView against real OPFS, not the mock).
  await expect(page.locator('.checkpoints-view .tbody .tr')).toHaveCount(1, { timeout: 10_000 });
}

// Reload the app and wait for the module bridge to come back.
async function reloadWeb2(page: Page): Promise<void> {
  await page.reload();
  await page.waitForFunction(() => (window as { __gsReady?: boolean }).__gsReady === true, undefined, {
    timeout: 60_000,
  });
}

const resumeModal = (page: Page) =>
  page.locator('.modal, [role="dialog"]').filter({ hasText: 'Continue from saved checkpoint?' });

// Drive the Terminal panel: type a shell line and return once the given
// pattern shows up in the xterm buffer (DOM renderer — rows are readable).
async function terminalExpect(page: Page, line: string, pattern: RegExp): Promise<void> {
  await page.locator('button.ptab[data-tab="terminal"]').click();
  const term = page.locator('.xterm');
  await expect(term).toBeVisible({ timeout: 15_000 });
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
  await expect(page.locator('.xterm-rows')).toContainText(pattern, { timeout: 15_000 });
}

test.describe('checkpoint save → reload → resume', () => {
  test('Plus: checkpoint survives a reload and Resume brings the machine back live', async ({
    page,
  }) => {
    test.setTimeout(180_000);
    await gotoWeb2(page);
    await uploadRom(page, PLUS_ROM);
    await startMachine(page, 'plus');

    // Let the ROM run a little so there is real machine state to capture.
    await page.waitForTimeout(2_000);
    await createCheckpoint(page);

    await reloadWeb2(page);

    // The same localStorage machine identity re-registers, checkpoint.probe
    // finds state.checkpoint, and the resume prompt appears.
    await expect(resumeModal(page)).toBeVisible({ timeout: 30_000 });
    await page.getByRole('button', { name: 'Resume' }).click();

    // Snapshot captured a running machine → the restore auto-resumes.
    await expect(
      page.locator('.toast .msg').filter({ hasText: 'Resumed from saved checkpoint' }),
    ).toBeVisible({ timeout: 60_000 });
    await expect(page.locator('.welcome-layer')).toHaveCount(0);
    await expect(statusLabel(page)).toHaveText('Running', { timeout: 15_000 });
  });

  test('Start fresh discards the checkpoint and a second reload does not re-prompt', async ({
    page,
  }) => {
    test.setTimeout(180_000);
    await gotoWeb2(page);
    await uploadRom(page, PLUS_ROM);
    await startMachine(page, 'plus');
    await page.waitForTimeout(2_000);
    await createCheckpoint(page);

    await reloadWeb2(page);
    await expect(resumeModal(page)).toBeVisible({ timeout: 30_000 });
    await page.getByRole('button', { name: 'Start fresh' }).click();
    await expect(
      page.locator('.toast .msg').filter({ hasText: 'Starting fresh (checkpoint discarded)' }),
    ).toBeVisible({ timeout: 30_000 });
    // Cold-boot path: the Welcome layer is back.
    await expect(page.locator('.welcome-layer')).toHaveCount(1);

    // checkpoint.clear must have actually removed state.checkpoint: a second
    // reload boots straight to Welcome with no prompt.
    await reloadWeb2(page);
    await expect(page.locator('.welcome-layer')).toHaveCount(1, { timeout: 30_000 });
    await expect(resumeModal(page)).toHaveCount(0);
  });

  test('SE/30: restore after reload keeps the SE/30 profile (regression: Plus fallback)', async ({
    page,
  }) => {
    test.setTimeout(240_000);
    await gotoWeb2(page);

    // The SE/30's onboard video card requires a vROM; stage it before the ROM
    // upload so the config slide's re-scan identifies both in one pass.
    await stageOpfsFile(page, '/opfs/images/vrom/builtin-se30-video-4f71ff1a.vrom', SE30_VROM);
    await uploadRom(page, SE30_ROM);
    await startMachine(page, 'se30');

    await page.waitForTimeout(2_000);
    await createCheckpoint(page);

    await reloadWeb2(page);
    await expect(resumeModal(page)).toBeVisible({ timeout: 30_000 });
    await page.getByRole('button', { name: 'Resume' }).click();
    await expect(
      page.locator('.toast .msg').filter({ hasText: 'Resumed from saved checkpoint' }),
    ).toBeVisible({ timeout: 60_000 });
    await expect(statusLabel(page)).toHaveText('Running', { timeout: 15_000 });

    // The restored machine must identify as an SE/30 — system_restore()
    // falling back to the default (Plus) profile is the historical failure
    // this pins (legacy state test 10). The Terminal panel is the shipped
    // typed path to the object model (web2 has no window.gsEval).
    await terminalExpect(page, 'machine.id', /se30/);
  });
});
