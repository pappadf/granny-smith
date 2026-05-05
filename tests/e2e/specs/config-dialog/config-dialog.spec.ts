// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// E2E tests for the machine configuration dialog and unified upload pipeline.
// Covers: ROM upload dialog, config dialog interactions, media upload via
// file picker (simulated), and validation of uploaded files.

import { test, expect } from '../../fixtures';
import { runCommand } from '../../helpers/run-command';
import { bootWithUploadedMedia } from '../../helpers/boot';
import * as fs from 'fs';
import * as path from 'path';

const TEST_DATA = path.join(process.cwd(), 'tests', 'data');
const ROM_REL = 'roms/Plus_v3.rom';
const SE30_ROM_REL = 'roms/SE30.rom';
const FLOPPY_REL = 'systems/System_6_0_8.dsk';

// Read test file as Uint8Array
function readTestFile(relPath: string): Uint8Array {
  return new Uint8Array(fs.readFileSync(path.join(TEST_DATA, relPath)));
}

test.describe('Config Dialog', () => {

  test('ROM upload dialog appears on fresh start', async ({ page, log }) => {
    test.setTimeout(30_000);
    log('navigating without noui — should show ROM upload dialog');

    // Navigate without noui or rom param — triggers ROM upload dialog
    await page.goto('/index.html');
    await page.waitForLoadState('domcontentloaded');

    // Wait for the ROM upload dialog to appear
    await page.waitForFunction(() => {
      const dlg = document.getElementById('rom-upload-dialog');
      return dlg && dlg.getAttribute('aria-hidden') === 'false';
    }, { timeout: 15_000 });

    log('ROM upload dialog is visible');

    // Verify key elements exist
    const selectBtn = await page.$('#rom-upload-select-btn');
    expect(selectBtn).not.toBeNull();

    const fileInput = await page.$('#rom-upload-file-input');
    expect(fileInput).not.toBeNull();
  });

  test('ROM upload via dialog persists and shows config dialog', async ({ page, log }) => {
    test.setTimeout(60_000);
    log('navigating — will upload ROM via dialog');

    await page.goto('/index.html');
    await page.waitForLoadState('domcontentloaded');

    // Wait for ROM upload dialog
    await page.waitForFunction(() => {
      const dlg = document.getElementById('rom-upload-dialog');
      return dlg && dlg.getAttribute('aria-hidden') === 'false';
    }, { timeout: 15_000 });

    log('ROM upload dialog visible, injecting ROM via file input');

    // Simulate file selection on the hidden input by setting files via CDP
    const romData = readTestFile(ROM_REL);
    const fileInput = await page.$('#rom-upload-file-input');
    expect(fileInput).not.toBeNull();

    // Use Playwright's setInputFiles to set file on the hidden input
    await fileInput!.setInputFiles({
      name: 'Plus_v3.rom',
      mimeType: 'application/octet-stream',
      buffer: Buffer.from(romData),
    });

    log('ROM file set, waiting for config dialog');

    // Wait for ROM upload dialog to close and config dialog to appear
    await page.waitForFunction(() => {
      const romDlg = document.getElementById('rom-upload-dialog');
      const cfgDlg = document.getElementById('config-dialog');
      return (
        (!romDlg || romDlg.getAttribute('aria-hidden') === 'true') &&
        cfgDlg && cfgDlg.getAttribute('aria-hidden') === 'false'
      );
    }, { timeout: 30_000 });

    log('config dialog is visible');

    // Verify config dialog has expected elements
    const modelSelect = await page.$('#config-model');
    expect(modelSelect).not.toBeNull();

    const ramSelect = await page.$('#config-ram');
    expect(ramSelect).not.toBeNull();

    const startBtn = await page.$('#config-start-btn');
    expect(startBtn).not.toBeNull();

    // Verify the Plus model is available (uploaded ROM is Plus v3)
    const modelValue = await modelSelect!.inputValue();
    log(`selected model: ${modelValue}`);
    expect(modelValue).toBe('plus');
  });

  test('config dialog shows media dropdowns with upload option', async ({ page, log }) => {
    test.setTimeout(60_000);
    log('setting up config dialog via ROM upload');

    await page.goto('/index.html');
    await page.waitForLoadState('domcontentloaded');

    // Upload ROM to get to config dialog
    await page.waitForFunction(() => {
      const dlg = document.getElementById('rom-upload-dialog');
      return dlg && dlg.getAttribute('aria-hidden') === 'false';
    }, { timeout: 15_000 });

    const romData = readTestFile(ROM_REL);
    const fileInput = await page.$('#rom-upload-file-input');
    await fileInput!.setInputFiles({
      name: 'Plus_v3.rom',
      mimeType: 'application/octet-stream',
      buffer: Buffer.from(romData),
    });

    await page.waitForFunction(() => {
      const cfgDlg = document.getElementById('config-dialog');
      return cfgDlg && cfgDlg.getAttribute('aria-hidden') === 'false';
    }, { timeout: 30_000 });

    log('config dialog visible, checking floppy dropdowns');

    // Check that floppy selects exist and have the "Upload image..." option
    const fd0Select = await page.$('#config-fd0');
    expect(fd0Select).not.toBeNull();

    const hasUploadOption = await page.evaluate(() => {
      const sel = document.querySelector('#config-fd0') as HTMLSelectElement;
      if (!sel) return false;
      return Array.from(sel.options).some(o => o.value === '__upload__');
    });
    expect(hasUploadOption).toBe(true);
    log('floppy dropdown has Upload option');

    // Check SCSI HD selects exist
    const hd0Select = await page.$('#config-hd0');
    expect(hd0Select).not.toBeNull();

    const hdHasUpload = await page.evaluate(() => {
      const sel = document.querySelector('#config-hd0') as HTMLSelectElement;
      if (!sel) return false;
      return Array.from(sel.options).some(o => o.value === '__upload__');
    });
    expect(hdHasUpload).toBe(true);
    log('HD dropdown has Upload option');
  });

  test('config dialog start button boots the emulator', async ({ page, log }) => {
    test.setTimeout(90_000);
    log('full flow: ROM upload -> config -> start -> boot');

    await page.goto('/index.html');
    await page.waitForLoadState('domcontentloaded');

    // Upload ROM
    await page.waitForFunction(() => {
      const dlg = document.getElementById('rom-upload-dialog');
      return dlg && dlg.getAttribute('aria-hidden') === 'false';
    }, { timeout: 15_000 });

    const romData = readTestFile(ROM_REL);
    const fileInput = await page.$('#rom-upload-file-input');
    await fileInput!.setInputFiles({
      name: 'Plus_v3.rom',
      mimeType: 'application/octet-stream',
      buffer: Buffer.from(romData),
    });

    // Wait for config dialog
    await page.waitForFunction(() => {
      const cfgDlg = document.getElementById('config-dialog');
      return cfgDlg && cfgDlg.getAttribute('aria-hidden') === 'false';
    }, { timeout: 30_000 });

    log('config dialog visible, clicking Start');

    // Click Start
    await page.click('#config-start-btn');

    // Wait for full boot sequence to complete (bootFromConfig + __gsBootReady)
    await page.waitForFunction(() => (window as any).__gsBootReady === true, { timeout: 60_000 });

    log('boot sequence complete, verifying boot commands');

    // Verify rom load was issued
    const commandLog = await page.evaluate(() => {
      return (window as any).__commandLog || [];
    });

    const hasRomLoad = commandLog.some((cmd: string) => cmd.includes('rom load'));
    expect(hasRomLoad).toBe(true);
    log('rom load command was issued');

    // Verify the run was triggered. commandLog records the gsEval method name
    // with `.` → ` `, so `scheduler.run` shows up as 'scheduler run'.
    const hasRun = commandLog.some((cmd: string) => cmd === 'scheduler run' || cmd.startsWith('scheduler run '));
    expect(hasRun).toBe(true);
    log('run command was issued — emulator started');
  });

  test('fd validate command correctly identifies floppy images', async ({ page, log }) => {
    test.setTimeout(60_000);
    log('testing fd validate with various files');

    await bootWithUploadedMedia(page, ROM_REL, FLOPPY_REL, undefined, { hideOverlay: true });
    await runCommand(page, 'schedule max');

    // fd validate returns cmd_bool: true(1) = valid, false(0) = invalid
    log('validating uploaded floppy image');
    const fdResult = await runCommand(page, 'fd validate /tmp/fd0');
    expect(fdResult).toBe(1); // cmd_bool(true) = 1
    log(`fd validate /tmp/fd0 returned ${fdResult} (expected 1 = valid)`);

    // Validate the ROM file as floppy (should fail)
    log('validating ROM as floppy (should fail)');
    const romAsFd = await runCommand(page, 'fd validate /tmp/rom');
    expect(romAsFd).toBe(0); // cmd_bool(false) = 0
    log(`fd validate /tmp/rom returned ${romAsFd} (expected 0 = invalid)`);
  });

  test('hd validate command correctly identifies HD images', async ({ page, log }) => {
    test.setTimeout(60_000);
    log('testing hd validate');

    await bootWithUploadedMedia(page, ROM_REL, undefined, 'systems/hd1.zip', { hideOverlay: true });
    await runCommand(page, 'schedule max');

    // hd validate returns cmd_bool: true(1) = valid, false(0) = invalid
    log('validating uploaded HD image');
    const hdResult = await runCommand(page, 'hd validate /tmp/hd0');
    expect(hdResult).toBe(1); // cmd_bool(true)
    log(`hd validate returned ${hdResult}`);

    // Validate a floppy as HD (should fail — floppy-sized)
    log('creating floppy to test hd validate rejection');
    await runCommand(page, 'fd create /tmp/test-floppy.dsk');
    const floppyAsHd = await runCommand(page, 'hd validate /tmp/test-floppy.dsk');
    expect(floppyAsHd).toBe(0); // cmd_bool(false)
    log(`hd validate on floppy returned ${floppyAsHd} (expected 0 = invalid)`);
  });

  test('cdrom validate command accepts non-floppy images', async ({ page, log }) => {
    test.setTimeout(60_000);
    log('testing cdrom validate');

    await bootWithUploadedMedia(page, ROM_REL, undefined, 'systems/hd1.zip', { hideOverlay: true });
    await runCommand(page, 'schedule max');

    // cdrom validate returns cmd_bool: true(1) = valid, false(0) = invalid
    log('validating HD image as CD-ROM (placeholder accepts non-floppy)');
    const cdResult = await runCommand(page, 'cdrom validate /tmp/hd0');
    expect(cdResult).toBe(1); // cmd_bool(true)
    log(`cdrom validate returned ${cdResult}`);

    // cdrom validate should reject a floppy-sized image
    log('creating floppy to test cdrom validate rejection');
    await runCommand(page, 'fd create /tmp/test-cd-floppy.dsk');
    const floppyAsCd = await runCommand(page, 'cdrom validate /tmp/test-cd-floppy.dsk');
    expect(floppyAsCd).toBe(0); // cmd_bool(false)
    log(`cdrom validate on floppy returned ${floppyAsCd} (expected 0 = invalid)`);
  });

  test('unified commands work via shell dispatch', async ({ page, log }) => {
    test.setTimeout(60_000);
    log('testing unified fd/hd/scc/cdrom commands via shell');

    await bootWithUploadedMedia(page, ROM_REL, FLOPPY_REL, undefined, { hideOverlay: true });
    await runCommand(page, 'schedule max');

    // fd probe should work
    const probeResult = await runCommand(page, 'fd probe /tmp/fd0');
    expect(probeResult).toBe(0);
    log('fd probe passed');

    // fd probe on non-existent file should fail
    const probeMissing = await runCommand(page, 'fd probe /tmp/nonexistent');
    expect(probeMissing).not.toBe(0);
    log('fd probe on missing file correctly failed');

    // scc loopback query should work (returns 0 for query)
    const sccResult = await runCommand(page, 'scc loopback');
    expect(sccResult).toBe(0);
    log('scc loopback query passed');

    // hd loopback on/off should work
    const loopOn = await runCommand(page, 'hd loopback on');
    expect(loopOn).toBe(0);
    const loopOff = await runCommand(page, 'hd loopback off');
    expect(loopOff).toBe(0);
    log('hd loopback on/off passed');
  });
});
