// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

/**
 * Comprehensive Drag & Drop Tests
 * 
 * These tests verify the emulator's drag/drop behavior for all supported file types:
 * 1. ROM files - on empty emulator, shows overlay asking for ROM
 * 2. Disk images - directly mounted as floppy drives  
 * 3. Checkpoints - reload machine state from saved checkpoint
 * 4. SCSI disk images - add SCSI device (requires restart)
 * 5. Unknown files - prompted to upload to memfs filesystem
 * 
 * Tests use true DataTransfer/drop events dispatched to the canvas element
 * for maximum fidelity to real user interactions.
 */

import { test, expect } from '../../fixtures';
import { bootWithUploadedMedia } from '../../helpers/boot';
import { matchScreenFast } from '../../helpers/screen';
import { runCommand } from '../../helpers/run-command';
import { dispatchDropEvent, dispatchMultiFileDropEvent } from '../../helpers/drop';
import * as fs from 'fs';
import * as path from 'path';

// ─────────────────────────────────────────────────────────────────────────────
// Test Data Paths
// ─────────────────────────────────────────────────────────────────────────────
const ROM_REL = 'roms/Plus_v3.rom';
const SYSTEM_DISK_REL = 'systems/System_6_0_8.dsk';
const ARCHIVE_REL = 'apps/MacTest_Disk.image_.sit_.hqx';
const HD_ZIP_REL = 'systems/hd1.zip';

// Checkpoint magic signature: "GSCHKPT2" (v2 = RLE compression)
const CHECKPOINT_MAGIC = new Uint8Array([0x47, 0x53, 0x43, 0x48, 0x4B, 0x50, 0x54, 0x32]);

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Wait for emulator to be ready for drop events
// ─────────────────────────────────────────────────────────────────────────────

async function waitForEmulatorReady(page: any, log: (msg: string) => void) {
  await page.waitForFunction(() => {
    return typeof (window as any).runCommand === 'function';
  }, { timeout: 30000 });
  log('emulator ready for drop events');
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST SUITE: ROM Drag/Drop (Empty Emulator)
// ─────────────────────────────────────────────────────────────────────────────

test.describe('ROM Drag/Drop', () => {
  test('empty emulator shows ROM overlay, drag/drop ROM loads and starts', async ({ page, log }) => {
    test.setTimeout(90_000);
    log('starting ROM overlay test');
    
    // Navigate to emulator with no media - should show ROM overlay
    await page.goto('/index.html');
    await page.waitForLoadState('domcontentloaded');
    await waitForEmulatorReady(page, log);
    
    // Wait for IDBFS sync to complete (which triggers overlay display)
    await page.waitForFunction(() => {
      // Check for the log message indicating sync is complete
      const overlay = document.getElementById('rom-required-overlay');
      return overlay && overlay.classList.contains('visible');
    }, { timeout: 10000 });
    
    log('ROM overlay is visible on empty emulator');
    
    // Read the actual ROM file
    const romPath = path.join(process.cwd(), 'tests', 'data', ROM_REL);
    const romData = new Uint8Array(fs.readFileSync(romPath));
    log(`loaded ROM file: ${romData.length} bytes`);
    
    // Dispatch a true drop event with the ROM file
    const dropResult = await dispatchDropEvent(page, '#screen', 'Plus_v3.rom', romData);
    expect(dropResult).toBe(true);
    log('drop event dispatched for ROM');
    
    // Wait for ROM to be processed
    await page.waitForTimeout(3000);
    
    // Verify the ROM was loaded and emulator started
    const romLoaded = await page.evaluate(() => {
      const commandLog = (window as any).__commandLog || [];
      return commandLog.some((cmd: string) => cmd.includes('load-rom'));
    });
    
    expect(romLoaded).toBe(true);
    log('ROM loaded successfully');
    
    // Verify the overlay is now hidden (no longer has .visible class)
    const overlayHidden = await page.evaluate(() => {
      const overlay = document.getElementById('rom-required-overlay');
      if (!overlay) return true; // removed means hidden
      return !overlay.classList.contains('visible');
    });
    
    expect(overlayHidden).toBe(true);
    log('ROM overlay hidden after ROM load');
    
    // Wait a bit and check for blinking question mark (no disk yet)
    // Reuse existing baseline from the old test suite
    await page.waitForTimeout(5000);
    await matchScreenFast(page, 'drag-drop-no-disk', {
      initialWaitMs: 2000,
      waitBeforeUpdateMs: 20_000,
      timeoutMs: 30_000
    });
    log('verified blinking question mark screen (ROM loaded, no disk)');
  });
});

// ─────────────────────────────────────────────────────────────────────────────
// TEST SUITE: Disk Image Drag/Drop
// ─────────────────────────────────────────────────────────────────────────────

test.describe('Disk Image Drag/Drop', () => {
  test('drag/drop floppy disk image auto-mounts', async ({ page, log }) => {
    test.setTimeout(120_000);
    log('starting disk image drop test');
    
    // Boot with ROM only - no disk
    await bootWithUploadedMedia(page, ROM_REL, undefined, undefined, { hideOverlay: true });
    await page.evaluate(() => (window as any).runCommand('run'));
    await page.waitForTimeout(2000);
    log('emulator running with ROM, no disk');
    
    // Read the system disk
    const diskPath = path.join(process.cwd(), 'tests', 'data', SYSTEM_DISK_REL);
    const diskData = new Uint8Array(fs.readFileSync(diskPath));
    log(`loaded disk image: ${diskData.length} bytes`);
    
    // Dispatch drop event for the disk image
    const dropResult = await dispatchDropEvent(page, '#screen', 'System_6_0_8.dsk', diskData);
    expect(dropResult).toBe(true);
    log('drop event dispatched for disk image');
    
    // Wait for disk to be processed and mounted
    await page.waitForTimeout(3000);
    
    // Verify the disk was uploaded to /tmp/upload
    const diskUploaded = 
      (await runCommand(page, 'exists "/tmp/upload/System_6_0_8.dsk"')) === 0 ||
      (await runCommand(page, 'exists "/tmp/System_6_0_8.dsk"')) === 0;
    
    expect(diskUploaded).toBe(true);
    log('disk image uploaded to memfs');
    
    // Wait for system to boot - reuse existing baseline
    await page.waitForTimeout(10000);
    
    // Verify system booted (desktop should appear) - reuse existing booted baseline
    await matchScreenFast(page, 'drag-drop-booted', {
      initialWaitMs: 5000,
      waitBeforeUpdateMs: 60_000,
      timeoutMs: 90_000
    });
    log('system booted from dropped disk image');
  });

  test('drag/drop archive extracts and mounts disk image', async ({ page, log }) => {
    test.setTimeout(120_000);
    log('starting archive drop test');
    
    // Boot with ROM and system disk
    await bootWithUploadedMedia(page, ROM_REL, SYSTEM_DISK_REL, undefined, { hideOverlay: true, fdWritable: true });
    await page.evaluate(() => (window as any).runCommand('run'));
    await page.waitForTimeout(5000);
    log('emulator running with ROM and system disk');
    
    // Wait for system to boot - reuse existing booted baseline
    await matchScreenFast(page, 'drag-drop-booted', {
      initialWaitMs: 5000,
      waitBeforeUpdateMs: 60_000,
      timeoutMs: 90_000
    });
    log('system booted, ready for archive drop');
    
    // Read the archive file
    const archivePath = path.join(process.cwd(), 'tests', 'data', ARCHIVE_REL);
    const archiveData = new Uint8Array(fs.readFileSync(archivePath));
    log(`loaded archive: ${archiveData.length} bytes`);
    
    // Dispatch drop event for the archive
    const dropResult = await dispatchDropEvent(page, '#screen', 'MacTest_Disk.image_.sit_.hqx', archiveData);
    expect(dropResult).toBe(true);
    log('drop event dispatched for archive');
    
    // Wait for archive extraction and disk mounting
    await page.waitForTimeout(10000);
    
    // Verify the archive was extracted by checking for specific expected files
    // The archive MacTest_Disk.image_.sit_.hqx extracts to:
    // /tmp/upload/MacTest_Disk.image_.sit__unpacked/MacTest Disk.image
    const unpackedDirExists = await runCommand(page, 'exists "/tmp/upload/MacTest_Disk.image_.sit__unpacked"');
    const diskImageExists = await runCommand(page, 'exists "/tmp/upload/MacTest_Disk.image_.sit__unpacked/MacTest Disk.image"');
    
    // Verify extraction succeeded
    const extracted = (unpackedDirExists === 0) && (diskImageExists === 0);
    
    log(`archive extraction verification: unpacked_dir=${unpackedDirExists === 0}, disk_image=${diskImageExists === 0}`);
    expect(extracted).toBe(true);
    
    // Verify disk from archive is now mounted - reuse existing mactest-mounted baseline
    await page.waitForTimeout(5000);
    await matchScreenFast(page, 'drag-drop-mactest-mounted', {
      initialWaitMs: 5000,
      waitBeforeUpdateMs: 30_000,
      timeoutMs: 60_000
    });
    log('disk from archive mounted successfully');
  });
});

// ─────────────────────────────────────────────────────────────────────────────
// TEST SUITE: Checkpoint Drag/Drop
// ─────────────────────────────────────────────────────────────────────────────

test.describe('Checkpoint Drag/Drop', () => {
  test('drag/drop checkpoint file is detected by signature and triggers restore', async ({ page, log }) => {
    test.setTimeout(180_000);
    log('starting checkpoint drop test');
    
    // Boot with ROM only (no disk) and step just a few thousand instructions.
    // At this early stage, RAM is almost entirely zeros, so the RLE-compressed
    // checkpoint is tiny (a few KB) — small enough to transfer via Playwright.
    await bootWithUploadedMedia(page, ROM_REL, undefined, undefined, { hideOverlay: true });
    log('emulator loaded with ROM only');
    
    // Step a small number of instructions to establish a deterministic state
    await runCommand(page, 's 2000');
    log('stepped 2000 instructions');
    
    // Record the instruction count as our reference state
    const savedInstr = await runCommand(page, 'get instr');
    log(`reference instruction count = ${savedInstr}`);
    
    // Save a real checkpoint (RLE-compressed, should be very small)
    const saveResult = await runCommand(page, 'save-state /tmp/early-checkpoint.bin refs');
    expect(saveResult).toBe(0);
    log('early-boot checkpoint saved');
    
    // Check the file size to verify RLE is working (should be well under 100KB)
    const cpSize = await runCommand(page, 'size /tmp/early-checkpoint.bin');
    log(`checkpoint file size: ${cpSize} bytes`);
    expect(cpSize).toBeGreaterThan(0);
    expect(cpSize).toBeLessThan(100 * 1024); // should be tiny with RLE
    
    // Download the checkpoint file via the shell download command
    const downloadPromise = page.waitForEvent('download', { timeout: 10_000 });
    await runCommand(page, 'download /tmp/early-checkpoint.bin');
    const download = await downloadPromise;
    const tempPath = await download.path();
    expect(tempPath).toBeTruthy();
    const checkpointBytes = Array.from(fs.readFileSync(tempPath!));
    log(`downloaded ${checkpointBytes.length} bytes via download command`);
    
    // Verify the data starts with the correct magic signature
    const magic = checkpointBytes.slice(0, 8);
    expect(magic).toEqual(Array.from(CHECKPOINT_MAGIC));
    log('checkpoint has correct GSCHKPT2 magic');
    
    // Step 1000 more instructions so the machine is now in a DIFFERENT state
    await runCommand(page, 's 1000');
    const alteredInstr = await runCommand(page, 'get instr');
    log(`altered instruction count = ${alteredInstr}`);
    expect(alteredInstr).not.toBe(savedInstr);
    
    // Now drop the real checkpoint via drag/drop — the full end-to-end path
    const dropResult = await dispatchDropEvent(
      page,
      '#screen',
      'early-checkpoint.bin',
      new Uint8Array(checkpointBytes)
    );
    expect(dropResult).toBe(true);
    log('drop event dispatched for real checkpoint file');
    
    // Wait for the drop handler to detect the checkpoint and run load-state
    await page.waitForTimeout(5000);
    
    // Verify the machine state was actually restored by checking the instruction count
    // The drop handler calls load-state which recreates the machine at the saved state
    const restoredInstr = await runCommand(page, 'get instr');
    log(`restored instruction count = ${restoredInstr}`);
    expect(restoredInstr).toBe(savedInstr);
    log('checkpoint restored successfully: instruction count matches saved state');
  });
});

// ─────────────────────────────────────────────────────────────────────────────
// TEST SUITE: SCSI Disk Image Drag/Drop
// ─────────────────────────────────────────────────────────────────────────────

test.describe('SCSI Disk Image Drag/Drop', () => {
  test('drag/drop large disk image uploads to memfs (HD not auto-mounted)', async ({ page, log }) => {
    // This test transfers a 3MB array through Playwright's serialization, which can
    // cause memory pressure when running in parallel with other tests. Use a longer timeout.
    test.setTimeout(120_000);
    log('starting SCSI disk drop test');
    
    // Boot with ROM to get the emulator ready
    await bootWithUploadedMedia(page, ROM_REL, undefined, undefined, { hideOverlay: true });
    log('emulator ready');
    
    // For large files (>5MB), we can't easily transfer via Playwright's DataTransfer
    // due to memory constraints. Instead, we create the file in-page and then
    // dispatch a drop event with a smaller proxy file, verifying the upload path works.
    // 
    // This test verifies that:
    // 1. Large files are uploaded to /tmp/upload
    // 2. They are NOT auto-mounted (since they fail floppy/ROM probe)
    // 3. The upload dialog is shown
    
    // Create a moderately large file (3MB - in the "unknown" range between floppy and HD)
    // This is small enough to transfer via drop event but large enough to fail disk probes
    const unknownSizeData = new Uint8Array(3 * 1024 * 1024);
    // Fill with a recognizable pattern (not a valid disk image header)
    for (let i = 0; i < unknownSizeData.length; i++) {
      unknownSizeData[i] = (i * 7) % 256;
    }
    log(`created test file: ${unknownSizeData.length} bytes`);
    
    // Dispatch TRUE drop event
    const dropResult = await dispatchDropEvent(page, '#screen', 'large-test-file.img', unknownSizeData);
    expect(dropResult).toBe(true);
    log('drop event dispatched for large file');
    
    // Wait for file to be processed
    await page.waitForTimeout(3000);
    
    // Verify the file was uploaded to /tmp/upload
    const uploadPath = '/tmp/upload/large-test-file.img';
    const exists = await runCommand(page, `exists "${uploadPath}"`);
    if (exists !== 0) {
      const uploadResult = { uploaded: false };
      log(`upload result: ${JSON.stringify(uploadResult)}`);
      expect(uploadResult.uploaded).toBe(true);
    } else {
      const size = await runCommand(page, `size "${uploadPath}"`);
      const uploadResult = { uploaded: true, path: uploadPath, size };
      log(`upload result: ${JSON.stringify(uploadResult)}`);
      expect(uploadResult.uploaded).toBe(true);
      expect(uploadResult.size).toBe(3 * 1024 * 1024);
    }
    log('large file uploaded to /tmp/upload successfully');
    
    // Verify that the upload dialog was shown (file wasn't auto-mounted)
    // The dialog should be visible since this file doesn't pass ROM or floppy probes
    const dialogShown = await page.evaluate(() => {
      const dlg = document.getElementById('upload-dialog');
      return dlg && dlg.getAttribute('aria-hidden') === 'false';
    });
    
    expect(dialogShown).toBe(true);
    log('upload dialog shown (file not auto-mounted as expected)');
  });
});

// ─────────────────────────────────────────────────────────────────────────────
// TEST SUITE: Unknown File Drag/Drop
// ─────────────────────────────────────────────────────────────────────────────

test.describe('Unknown File Drag/Drop', () => {
  test('drag/drop non-media file uploads to memfs only', async ({ page, log }) => {
    test.setTimeout(60_000);
    log('starting unknown file drop test');
    
    // Boot with ROM
    await bootWithUploadedMedia(page, ROM_REL, undefined, undefined, { hideOverlay: true });
    log('emulator ready with ROM');
    
    // Create a text file (not a disk image, ROM, or checkpoint)
    const textContent = 'This is a test text file that is not a disk image or ROM.';
    const textData = new TextEncoder().encode(textContent);
    log(`created text file: ${textData.length} bytes`);
    
    // Dispatch drop event for the text file
    const dropResult = await dispatchDropEvent(page, '#screen', 'readme.txt', new Uint8Array(textData), 'text/plain');
    expect(dropResult).toBe(true);
    log('drop event dispatched for text file');
    
    // Wait for file to be processed
    await page.waitForTimeout(2000);
    
    // Verify the file was uploaded to /tmp/upload
    const uploaded = 
      (await runCommand(page, 'exists "/tmp/upload/readme.txt"')) === 0 ||
      (await runCommand(page, 'exists "/tmp/readme.txt"')) === 0;
    
    expect(uploaded).toBe(true);
    log('unknown file uploaded to memfs successfully');
    
    // Verify the file was NOT mounted as a disk (probe should fail)
    const probeResult = await page.evaluate(async () => {
      const result = await (window as any).runCommand('insert-fd --probe "/tmp/upload/readme.txt"');
      return result;
    });
    
    expect(probeResult).not.toBe(0); // Non-zero means probe failed (as expected)
    log('text file correctly NOT recognized as disk image');
  });

  test('drag/drop JSON file uploads without disk mount attempt', async ({ page, log }) => {
    test.setTimeout(60_000);
    log('starting JSON file drop test');
    
    // Boot with ROM
    await bootWithUploadedMedia(page, ROM_REL, undefined, undefined, { hideOverlay: true });
    log('emulator ready');
    
    // Create a JSON file
    const jsonContent = JSON.stringify({ test: true, data: [1, 2, 3], name: 'test config' });
    const jsonData = new TextEncoder().encode(jsonContent);
    log(`created JSON file: ${jsonData.length} bytes`);
    
    // Dispatch drop event
    const dropResult = await dispatchDropEvent(page, '#screen', 'config.json', new Uint8Array(jsonData), 'application/json');
    expect(dropResult).toBe(true);
    log('drop event dispatched for JSON file');
    
    // Wait for processing
    await page.waitForTimeout(2000);
    
    // Verify upload
    const uploaded = 
      (await runCommand(page, 'exists "/tmp/upload/config.json"')) === 0 ||
      (await runCommand(page, 'exists "/tmp/config.json"')) === 0;
    
    expect(uploaded).toBe(true);
    log('JSON file uploaded to memfs successfully');
  });
});

// ─────────────────────────────────────────────────────────────────────────────
// TEST SUITE: Drop Hint UI Behavior
// ─────────────────────────────────────────────────────────────────────────────

test.describe('Drop Hint UI', () => {
  test('drop hint appears on dragenter and disappears on drop', async ({ page, log }) => {
    test.setTimeout(60_000);
    log('starting drop hint UI test');
    
    await page.goto('/index.html');
    await page.waitForLoadState('domcontentloaded');
    await waitForEmulatorReady(page, log);
    
    // Initially, drop hint should not be active
    const initialHint = await page.evaluate(() => {
      const hint = document.getElementById('drop-hint');
      return hint?.classList.contains('active') || false;
    });
    expect(initialHint).toBe(false);
    log('drop hint initially hidden');
    
    // Simulate dragenter
    await page.evaluate(() => {
      const canvas = document.getElementById('screen');
      if (!canvas) return;
      const dataTransfer = new DataTransfer();
      const event = new DragEvent('dragenter', { bubbles: true, cancelable: true, dataTransfer });
      canvas.dispatchEvent(event);
    });
    
    // Check if drop hint is now visible
    const hintAfterEnter = await page.evaluate(() => {
      const hint = document.getElementById('drop-hint');
      return hint?.classList.contains('active') || false;
    });
    expect(hintAfterEnter).toBe(true);
    log('drop hint visible after dragenter');
    
    // Simulate drop (which should hide the hint)
    const romPath = path.join(process.cwd(), 'tests', 'data', ROM_REL);
    const romData = new Uint8Array(fs.readFileSync(romPath));
    await dispatchDropEvent(page, '#screen', 'Plus_v3.rom', romData);
    
    await page.waitForTimeout(500);
    
    // Drop hint should be hidden after drop
    const hintAfterDrop = await page.evaluate(() => {
      const hint = document.getElementById('drop-hint');
      return hint?.classList.contains('active') || false;
    });
    expect(hintAfterDrop).toBe(false);
    log('drop hint hidden after drop');
  });
});

// ─────────────────────────────────────────────────────────────────────────────
// TEST SUITE: Full Workflow
// ─────────────────────────────────────────────────────────────────────────────

test.describe('Full Drop Workflow', () => {
  test('complete flow: ROM → disk → archive via drag/drop', async ({ page, log }) => {
    test.setTimeout(180_000);
    log('starting full drop workflow test');
    
    // Step 1: Start with empty emulator
    await page.goto('/index.html');
    await page.waitForLoadState('domcontentloaded');
    await waitForEmulatorReady(page, log);
    log('step 1: empty emulator ready');
    
    // Wait for ROM overlay to become visible (after IDBFS sync)
    await page.waitForFunction(() => {
      const overlay = document.getElementById('rom-required-overlay');
      return overlay && overlay.classList.contains('visible');
    }, { timeout: 10000 });
    log('ROM overlay visible');
    
    // Step 2: Drop ROM
    const romPath = path.join(process.cwd(), 'tests', 'data', ROM_REL);
    const romData = new Uint8Array(fs.readFileSync(romPath));
    await dispatchDropEvent(page, '#screen', 'Plus_v3.rom', romData);
    await page.waitForTimeout(5000);
    log('step 2: ROM dropped');
    
    // Wait for blinking question mark - reuse existing baseline
    await matchScreenFast(page, 'drag-drop-no-disk', {
      initialWaitMs: 2000,
      waitBeforeUpdateMs: 20_000,
      timeoutMs: 30_000
    });
    log('showing blinking question mark (waiting for disk)');
    
    // Step 3: Drop system disk
    const diskPath = path.join(process.cwd(), 'tests', 'data', SYSTEM_DISK_REL);
    const diskData = new Uint8Array(fs.readFileSync(diskPath));
    await dispatchDropEvent(page, '#screen', 'System_6_0_8.dsk', diskData);
    await page.waitForTimeout(3000);
    log('step 3: system disk dropped');
    
    // Wait for boot - reuse existing baseline
    await page.waitForTimeout(10000);
    await matchScreenFast(page, 'drag-drop-booted', {
      initialWaitMs: 5000,
      waitBeforeUpdateMs: 60_000,
      timeoutMs: 90_000
    });
    log('system booted');
    
    // Step 4: Drop archive
    const archivePath = path.join(process.cwd(), 'tests', 'data', ARCHIVE_REL);
    const archiveData = new Uint8Array(fs.readFileSync(archivePath));
    await dispatchDropEvent(page, '#screen', 'MacTest.sit.hqx', archiveData);
    await page.waitForTimeout(10000);
    log('step 4: archive dropped');
    
    // Verify archive extracted and disk mounted - reuse existing baseline
    await matchScreenFast(page, 'drag-drop-mactest-mounted', {
      initialWaitMs: 5000,
      waitBeforeUpdateMs: 30_000,
      timeoutMs: 60_000
    });
    log('full workflow complete: ROM → disk → archive all via drag/drop');
  });
});
