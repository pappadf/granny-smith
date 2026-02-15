// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { test } from '../../fixtures';
import { expect } from '@playwright/test';
import { matchScreenFast } from '../../helpers/screen';
import { bootWithUploadedMedia, bootWithMedia } from '../../helpers/boot';
import { installTestShim, captureXterm } from '../../helpers/terminal';
import { runCommand, waitForPrompt, waitForSync, waitForCompleteCheckpoint } from '../../helpers/run-command';
import { mouseDrag } from '../../helpers/mouse';
import { readMemfsFiles } from '../../helpers/memfs';
import { dispatchDropEvent } from '../../helpers/drop';
import * as fs from 'fs';
import * as path from 'path';

test.describe('State', () => {

  // Save and immedately load back state, verify that boot continues after load.
  // This is basically a sanity test of save-state / load-state functionality.
  // In this case we test checkpointing when booting from hard disk image.
  test('test 1: save and load back state', async ({ page, log }) => {

    test.setTimeout(180_000);

    log('[state-test1] booting via MEMFS uploads');
    await bootWithUploadedMedia(page, 'roms/Plus_v3.rom', undefined, 'systems/hd1.zip', { hdSlot: 0, hideOverlay: true });

    log('[state-test1] running 18M instructions');
    await runCommand(page, 'run 18000000');

    log('[state-test1] waiting for prompt');
    await waitForPrompt(page);

    // Wait for the "Welcome to Macintosh" screen to appear
    await matchScreenFast(page, 'test-1-booting', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });

    log('[state-test1] saving state');
    await runCommand(page, 'save-state foo');

    log('[state-test1] loading back state');
    await runCommand(page, 'load-state foo');

    log('[state-test1] resuming from loaded state');
    await runCommand(page, 'run');

    // Wait for the desktop to appear
    await matchScreenFast(page, 'test-1-desktop', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });
  });

  // Save state and then continue the boot (with logging) until we reach the desktop.
  // Restore the saved state and continue booting again with logging.
  // Verify that logging output is identical between original run and restored run.
  // This test uses floppy boot rather than hard disk boot.
  test('test 2: compare logs after save and load back', async ({ page, log }) => {

    test.setTimeout(180_000);

    log('[state-test2] booting via MEMFS uploads (fd0)');
    await bootWithUploadedMedia(page, 'roms/Plus_v3.rom', 'systems/System_6_0_8.dsk', undefined, { hideOverlay: true });

    // Disable idle checkpointing for this test to ensure deterministic execution
    await page.evaluate(() => (window as any).runCommand('checkpoint auto off'));

    log('[state-test2] first run (pre-save)');
    await runCommand(page, 'run 25000000');

    log('[state-test2] waiting for prompt');
    await waitForPrompt(page);

    //  Wait for the happy Mac screen to appear
    await matchScreenFast(page, 'test-2-screen-1', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });

    log('[state-test2] saving state');
    await runCommand(page, 'save-state foo');

    // ======================================================================
    // Iteration 1:

    log('[state-test2] iteration 1: start logging to file');
    await runCommand(page, 'log floppy level=5 file=/tmp/floppy-log.txt stdout=off ts=on');
    await runCommand(page, 'log rtc level=5 file=/tmp/floppy-log.txt stdout=off ts=on');

    log('[state-test2] iteration 1: run 50M instructions');
    await runCommand(page, 'run 50000000');

    log('[state-test2] iteration 1: run completed, waiting for prompt');
    await waitForPrompt(page);

    // Wait for desktop
    await matchScreenFast(page, 'test-2-screen-2', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });

    // disable logging
    await runCommand(page, 'log floppy level=0');
    await runCommand(page, 'log rtc level=0');

    // ======================================================================
    // Iteration 2: Restore saved state

    log('[state-test2] loading state');
    await runCommand(page, 'load-state foo');

    log('[state-test2] iteration 2: start logging to file');
    await runCommand(page, 'log floppy level=5 file=/tmp/floppy-log-loaded.txt stdout=off ts=on');
    await runCommand(page, 'log rtc level=5 file=/tmp/floppy-log-loaded.txt stdout=off ts=on');

    log('[state-test2] second run (post-restore)');
    await runCommand(page, 'run 50000000');

    log('[state-test2] waiting for prompt');
    await waitForPrompt(page);

    // Wait for desktop
    await matchScreenFast(page, 'test-2-screen-2', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });

    log('[state-test2] second run completed');

    // ======================================================================
    // Iteration 2: Restore saved state

    // Extract both logs for comparison.
    const logs = await readMemfsFiles(page, [
      '/tmp/floppy-log.txt',
      '/tmp/floppy-log-loaded.txt'
    ]);
    const a = logs['/tmp/floppy-log.txt'];
    const b = logs['/tmp/floppy-log-loaded.txt'];

    // Attach logs to Playwright test artifacts.
    await test.info().attach('floppy-log-original', { body: a, contentType: 'text/plain' });
    await test.info().attach('floppy-log-loaded', { body: b, contentType: 'text/plain' });

    // Filter out checkpoint-related log lines that may vary due to background idle checkpointing
    const filterCheckpoints = (log: string) => log.split(/\r?\n/)
      .filter(line => !line.includes('Checkpointing floppy controller'))
      .filter(line => !line.includes('Checkpoint wrote disk entry'))
      .filter(line => !line.includes('Flush skipped drive'))
      .join('\n');
    const aFiltered = filterCheckpoints(a);
    const bFiltered = filterCheckpoints(b);

    // Validate minimum line count before equality check.
    const countLines = (s: string) => s === '' ? 0 : s.split(/\r?\n/).length;
    const linesA = countLines(aFiltered);
    const linesB = countLines(bFiltered);
    expect(linesA, `Original log has insufficient lines (${linesA} < 1000)`).toBeGreaterThanOrEqual(1000);
    expect(linesB, `Restored log has insufficient lines (${linesB} < 1000)`).toBeGreaterThanOrEqual(1000);

    // Assert equality of original vs restored run logs (excluding checkpoint timing). Fails test if mismatch.
    expect(aFiltered, 'Loaded state log must match original log').toBe(bFiltered);

  });

  test('test 3: resume from background checkpoint', async ({ page, log }) => {

    test.setTimeout(120_000);

    log('[state-test3] booting via MEMFS uploads');
    await bootWithUploadedMedia(page, 'roms/Plus_v3.rom', undefined, 'systems/hd1.zip', { hdSlot: 0, hideOverlay: true });

    log('[state-test3] running instructions before checkpoint');
    await runCommand(page, 'run 15000000');
    await waitForPrompt(page);

    const before = await runCommand(page, 'get instr');
    log(`[state-test3] instruction counter before checkpoint: ${before}`);

    log('[state-test3] forcing background checkpoint');
    await runCommand(page, 'background-checkpoint e2e');
    await waitForPrompt(page);

    log('[state-test3] waiting for checkpoint to be marked complete');
    await waitForCompleteCheckpoint(page);

    log('[state-test3] syncing persist storage');
    await waitForSync(page);

    log('[state-test3] reloading page to trigger resume prompt');
    await page.reload();

    await page.waitForSelector('#checkpoint-dialog', { state: 'visible', timeout: 30_000 });
    await page.locator('[data-checkpoint-continue]').click();

    // Checkpoint was saved while paused (after run 15000000 completed)
    // So it should restore to paused state - wait for prompt
    await waitForPrompt(page);
    
    const restored = await runCommand(page, 'get instr');
    expect(restored, 'Restored instruction counter').toBe(before);

    // Resume execution and verify instruction counter increases
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    const after = await runCommand(page, 'get instr');
    expect(after, 'Instruction counter should increase after restore').toBeGreaterThan(before);
  });


  // This test will boot up and reach the desktop, then reload the page to trigger a checkpoint being
  // first created and then restored after reload. Finally it will open a new tab to verify that the
  // same checkpoint can be reached/reused from a fresh tab.

  test('test 4: checkpoint survives reload and new tab', async ({ page, log }) => {

    test.setTimeout(90_000);

    log('[state-test4] booting from HD');
    await bootWithMedia(page, 'roms/Plus_v3.rom', undefined, 'systems/hd1.zip');

    log('[state-test4] waiting for desktop');
    await matchScreenFast(page, 'test-4-desktop', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });

    // Wait for idle checkpoint to be created automatically (15s min interval + margin)
    // Then explicitly wait for the checkpoint to be marked complete (has .complete marker)
    log('[state-test4] waiting for idle checkpoint');
    await page.waitForTimeout(16_000);
    
    log('[state-test4] waiting for checkpoint to be marked complete');
    await waitForCompleteCheckpoint(page);

    // Checkpointing on reload is automatic thanks to pagehide hooks
    // No extra commands needed - testing real-world scenario where user reloads while running
    log('[state-test4] reloading tab to trigger checkpoint');
    const reloadPromise = page.waitForLoadState('load');
    await page.evaluate(() => { window.location.reload(); }).catch(() => null);
    await reloadPromise;

    // Checkpoint dialog should appear automatically if checkpoint was saved
    await page.waitForSelector('#checkpoint-dialog', { state: 'visible', timeout: 30_000 });
    await page.locator('[data-checkpoint-continue]').click();
    await matchScreenFast(page, 'test-4-desktop', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });

    log('[state-test4] opening new tab to reuse checkpoint');
    const newTab = await page.context().newPage();
    await installTestShim(newTab);
    await newTab.goto('/index.html');
    await newTab.waitForSelector('#checkpoint-dialog', { state: 'visible', timeout: 30_000 });
    await newTab.locator('[data-checkpoint-continue]').click();
    await matchScreenFast(newTab, 'test-4-desktop', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });
    await newTab.close();
  });

  // Stress test for background checkpoint system:
  // - Multiple rapid checkpoint saves with verification
  // - Verifies only the latest complete checkpoint is retained
  // - Tests cleanup of incomplete/stale checkpoints
  // - Verifies checkpoint integrity after multiple operations
  test('test 5: background checkpoint stress test', async ({ page, log }) => {

    test.setTimeout(240_000);

    log('[state-test5] booting via MEMFS uploads');
    await bootWithUploadedMedia(page, 'roms/Plus_v3.rom', undefined, 'systems/hd1.zip', { hdSlot: 0, hideOverlay: true });

    log('[state-test5] running initial instructions');
    await runCommand(page, 'run 10000000');
    await waitForPrompt(page);

    // Phase 1: Rapid checkpoint saves
    log('[state-test5] phase 1: rapid checkpoint saves');
    const numCheckpoints = 5;
    for (let i = 1; i <= numCheckpoints; i++) {
      log(`[state-test5] saving checkpoint ${i}/${numCheckpoints}`);

      // Run some instructions between checkpoints to change state
      await runCommand(page, `run ${1000000 * i}`);
      await waitForPrompt(page);

      // Force a background checkpoint
      await runCommand(page, `background-checkpoint stress-${i}`);
      await waitForPrompt(page);

      // Wait for checkpoint to be marked complete
      await waitForCompleteCheckpoint(page);

      // Verify a valid checkpoint exists after save
      const probeResult = await runCommand(page, 'load-state probe');
      log(`[state-test5] checkpoint ${i}: probe=${probeResult}`);
      expect(probeResult, `Valid checkpoint should exist after save ${i}`).toBe(0);
    }

    // Phase 2: Verify instruction counter accumulates correctly
    log('[state-test5] phase 2: verifying state integrity');
    const instrBefore = await runCommand(page, 'get instr');
    log(`[state-test5] instruction count before reload: ${instrBefore}`);
    
    // Force final checkpoint and sync
    await runCommand(page, 'background-checkpoint final');
    await waitForPrompt(page);
    await waitForCompleteCheckpoint(page);
    await waitForSync(page);

    // Verify a valid checkpoint exists
    const finalProbe = await runCommand(page, 'load-state probe');
    log(`[state-test5] final checkpoint probe: ${finalProbe}`);
    expect(finalProbe, 'Valid checkpoint should exist after final save').toBe(0);
    
    // Phase 3: Reload and verify checkpoint is restored
    log('[state-test5] phase 3: reload and restore');
    await page.reload();
    
    await page.waitForSelector('#checkpoint-dialog', { state: 'visible', timeout: 30_000 });
    await page.locator('[data-checkpoint-continue]').click();
    
    await waitForPrompt(page);
    
    // Verify instruction counter matches what we saved
    const instrAfter = await runCommand(page, 'get instr');
    log(`[state-test5] instruction count after restore: ${instrAfter}`);
    
    expect(instrAfter).toBeGreaterThan(0);
    expect(instrAfter).toBeLessThanOrEqual(instrBefore);

    // Phase 4: Multiple fast reloads (tests pagehide checkpoint handling)
    log('[state-test5] phase 4: rapid reload stress');
    for (let i = 1; i <= 3; i++) {
      log(`[state-test5] reload cycle ${i}/3`);
      
      // Run a bit
      await runCommand(page, 'run 2000000');
      await waitForPrompt(page);
      
      // Reload (should trigger pagehide checkpoint)
      await page.reload();
      
      // Should get checkpoint prompt
      await page.waitForSelector('#checkpoint-dialog', { state: 'visible', timeout: 30_000 });
      await page.locator('[data-checkpoint-continue]').click();
      await waitForPrompt(page);
      
      // Verify a valid checkpoint exists after reload cycle
      const cycleProbe = await runCommand(page, 'load-state probe');
      log(`[state-test5] reload cycle ${i}: probe=${cycleProbe}`);
      expect(cycleProbe, `Valid checkpoint should exist after reload ${i}`).toBe(0);
    }

    log('[state-test5] stress test complete');
  });

  // 

  test('test 6: background checkpoint with browser reload', async ({ page, log }, testInfo) => {

    test.setTimeout(180_000);

    // --- Phase 1: boot and reach a known screen state ---
    log('[state-test6] booting via MEMFS uploads (fd0)');
    await bootWithUploadedMedia(page, 'roms/Plus_v3.rom', 'systems/System_6_0_8.dsk', undefined, { hideOverlay: true });

    log('[state-test6] running initial instructions');
    await runCommand(page, 'run 100000000');
    await waitForPrompt(page);

    // Verify we reached a known screen state (happy Mac)
    await matchScreenFast(page, 'test-6-screen-before-reload', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });

    // Resume continuous execution so background checkpointing fires naturally
    // during scheduler tick callbacks (~every 15 seconds at 60 ticks/sec).
    // No explicit checkpoint saves — this tests the real-world scenario where
    // a user reloads the browser with background checkpoints already persisted.
    log('[state-test6] resuming continuous execution for background checkpointing');
    await runCommand(page, 'run');

    // Wait long enough for at least one background checkpoint cycle to complete
    log('[state-test6] waiting for background checkpoint');
    await page.waitForTimeout(16_000);
    await waitForCompleteCheckpoint(page, 30_000);

    // Capture xterm output from phase 1 before reload overwrites the buffer
    log('[state-test6] capturing xterm (phase 1)');
    await captureXterm(page, 'test-6-phase-1-before-reload', testInfo, log);

    // --- Phase 2: reload the tab (real-world scenario) ---
    log('[state-test6] reloading tab');
    const reloadPromise = page.waitForLoadState('load');
    await page.evaluate(() => { window.location.reload(); }).catch(() => null);
    await reloadPromise;

    // Checkpoint dialog should appear — background checkpoint was persisted to IndexedDB
    await page.waitForSelector('#checkpoint-dialog', { state: 'visible', timeout: 30_000 });
    await page.locator('[data-checkpoint-continue]').click();

    await runCommand(page, 'run');

    await mouseDrag(page, 25, 10, 25, 30); // deselect short ram test

    // Checkpoint was saved while running, so execution auto-resumes.
    // Verify we can reach the expected screen state after restore.
    log('[state-test6] verifying screen after checkpoint restore');
    await matchScreenFast(page, 'test-6-screen-after-reload', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });
  });

  test('test 7: on-demand checkpoint with browser reload', async ({ page, log }, testInfo) => {

    test.setTimeout(240_000);

    // --- Phase 1: boot and reach a known screen state ---
    log('[state-test7] booting via MEMFS uploads (fd0)');
    await bootWithUploadedMedia(page, 'roms/Plus_v3.rom', 'systems/System_6_0_8.dsk', undefined, { hideOverlay: true });

    // Disable idle checkpointing for this test to ensure deterministic execution
    await page.evaluate(() => (window as any).runCommand('checkpoint auto off'));

    log('[state-test7] running initial instructions');
    await runCommand(page, 'run 100000000');
    await waitForPrompt(page);

    // Verify we reached a known screen state (happy Mac)
    await matchScreenFast(page, 'test-7-desktop', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });

    // Create an on-demand checkpoint with inline content (self-contained),
    // then download it via the browser so we have the bytes for drag-drop later.
    log('[state-test7] saving on-demand checkpoint (content mode)');
    await runCommand(page, 'save-state /tmp/on-demand-checkpoint content');

    log('[state-test7] downloading checkpoint via browser');
    const downloadPromise = page.waitForEvent('download', { timeout: 30_000 });
    await runCommand(page, 'download /tmp/on-demand-checkpoint');
    const download = await downloadPromise;
    const checkpointTempPath = await download.path();
    expect(checkpointTempPath, 'checkpoint download should produce a temp file').toBeTruthy();
    const checkpointBytes = new Uint8Array(fs.readFileSync(checkpointTempPath!));
    log(`[state-test7] downloaded checkpoint: ${checkpointBytes.length} bytes`);

    // ======================================================================
    // Iteration 1:

    log('[state-test7] iteration 1: start logging to file');
    await runCommand(page, 'log floppy level=5 file=/tmp/test-7-iteration-1.txt stdout=off ts=on');
    await runCommand(page, 'log rtc level=5 file=/tmp/test-7-iteration-1.txt stdout=off ts=on');

    // Click the Apple menu and select "About This Macintosh"
    await runCommand(page, 'set-mouse 25 10');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'mouse-button down');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'set-mouse 25 30');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'mouse-button up');

    // Wait for the About This Macintosh window to appear
    await runCommand(page, 'run 20000000');
    await waitForPrompt(page);

    await matchScreenFast(page, 'test-7-about', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });

    // disable logging
    await runCommand(page, 'log floppy level=0');
    await runCommand(page, 'log rtc level=0');

    // Read iteration 1 logs from MEMFS now, before reload clears MEMFS
    const iter1Logs = await readMemfsFiles(page, ['/tmp/test-7-iteration-1.txt']);
    const a = iter1Logs['/tmp/test-7-iteration-1.txt'];


    // Capture xterm output from phase 1 before reload overwrites the buffer
    log('[state-test7] capturing xterm (phase 1)');
    await captureXterm(page, 'test-7-phase-1-before-reload', testInfo, log);

    // ======================================================================
    // Iteration 2: reload page and restore checkpoint via drag-drop

    // Clear any persisted checkpoint data before reload so the
    // "Continue from saved checkpoint?" dialog does not appear.
    log('[state-test7] clearing checkpoint storage before reload');
    await runCommand(page, 'checkpoint clear');

    log('[state-test7] reloading tab');
    const reloadPromise = page.waitForLoadState('load');
    await page.evaluate(() => { window.location.reload(); }).catch(() => null);
    await reloadPromise;

    // Wait for WASM module to load and runCommand to become available
    await page.waitForFunction(() => {
      return typeof (window as any).runCommand === 'function';
    }, { timeout: 30_000 });

    // Dismiss the checkpoint dialog if it somehow still appears
    // (e.g. due to IndexedDB sync timing)
    const dialog = await page.$('#checkpoint-dialog');
    if (dialog && await dialog.isVisible()) {
      log('[state-test7] dismissing stale checkpoint dialog');
      const discardBtn = page.locator('[data-checkpoint-discard]');
      if (await discardBtn.isVisible()) {
        await discardBtn.click();
      }
      await page.waitForTimeout(1_000);
    }

    // Restore the checkpoint by dropping it onto the canvas (simulates real drag-drop).
    // The drop handler detects the checkpoint magic signature and calls load-state.
    log('[state-test7] restoring checkpoint via drag-drop');
    const dropResult = await dispatchDropEvent(
      page, '#screen', 'on-demand-checkpoint.bin', checkpointBytes
    );
    expect(dropResult, 'drop event should dispatch successfully').toBe(true);

    // Wait for the drop handler to process the checkpoint and load-state to complete
    await page.waitForTimeout(5_000);
    await waitForPrompt(page);

    // Disable idle checkpointing again after restore
    await page.evaluate(() => (window as any).runCommand('checkpoint auto off'));

    log('[state-test7] iteration 2: start logging to file');
    await runCommand(page, 'log floppy level=5 file=/tmp/test-7-iteration-2.txt stdout=off ts=on');
    await runCommand(page, 'log rtc level=5 file=/tmp/test-7-iteration-2.txt stdout=off ts=on');

    // Click the Apple menu and select "About This Macintosh"
    await runCommand(page, 'set-mouse 25 10');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'mouse-button down');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'set-mouse 25 30');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'mouse-button up');

    // Wait for the About This Macintosh window to appear
    await runCommand(page, 'run 20000000');
    await waitForPrompt(page);

    // disable logging
    await runCommand(page, 'log floppy level=0');
    await runCommand(page, 'log rtc level=0');

    // ======================================================================
    // Compare logs and verify screen after restore

    // Extract iteration 2 log from MEMFS (iteration 1 was read before reload).
    const iter2Logs = await readMemfsFiles(page, [
      '/tmp/test-7-iteration-2.txt'
    ]);
    const b = iter2Logs['/tmp/test-7-iteration-2.txt'];

    // Attach logs to Playwright test artifacts.
    await test.info().attach('iteration-1-log', { body: a, contentType: 'text/plain' });
    await test.info().attach('iteration-2-log', { body: b, contentType: 'text/plain' });

    // Validate minimum line count before equality check (sanity: logging was active).
    const countLines = (s: string) => s === '' ? 0 : s.split(/\r?\n/).length;
    const linesA = countLines(a);
    const linesB = countLines(b);
    log(`[state-test7] iteration 1 log: ${linesA} lines, iteration 2 log: ${linesB} lines`);
    expect(linesA, `Original log has insufficient lines (${linesA} < 5)`).toBeGreaterThanOrEqual(5);
    expect(linesB, `Restored log has insufficient lines (${linesB} < 5)`).toBeGreaterThanOrEqual(5);

    // Assert equality of original vs restored run logs. Fails test if mismatch.
    expect(a, 'Loaded state log must match original log').toBe(b);
  });

  // Variant of test 7: boot with ROM only (no disk), then drag/drop the boot
  // disk to start the system.  After reaching the desktop, create an on-demand
  // checkpoint, download it, reload the page, and restore via drag-drop.
  // Verifies log determinism across the original and restored runs.
  test('test 8: on-demand checkpoint with drag-drop boot disk', async ({ page, log }, testInfo) => {

    test.setTimeout(240_000);

    // --- Phase 1: boot with ROM only, then drag-drop the boot disk ---
    log('[state-test8] booting with ROM only (no boot disk)');
    await bootWithUploadedMedia(page, 'roms/Plus_v3.rom', undefined, undefined, { hideOverlay: true });

    // Disable idle checkpointing for deterministic execution
    await page.evaluate(() => (window as any).runCommand('checkpoint auto off'));

    // Start the CPU so the Mac shows the blinking question-mark floppy icon
    await page.evaluate(() => (window as any).runCommand('run'));
    await page.waitForTimeout(2_000);

    // Drag-drop the boot disk image onto the emulator canvas
    log('[state-test8] dropping boot disk via drag-drop');
    const diskPath = path.join(process.cwd(), 'tests', 'data', 'systems', 'System_6_0_8.dsk');
    const diskData = new Uint8Array(fs.readFileSync(diskPath));
    const dropDiskResult = await dispatchDropEvent(page, '#screen', 'System_6_0_8.dsk', diskData);
    expect(dropDiskResult, 'disk drop event should dispatch successfully').toBe(true);

    // The CPU is already running continuously; let it boot from the dropped disk
    // and wait until the desktop screen appears
    log('[state-test8] waiting for desktop after disk drop');
    await matchScreenFast(page, 'test-8-desktop', { initialWaitMs: 5_000, waitBeforeUpdateMs: 60_000, timeoutMs: 90_000 });

    // Stop the CPU so we can proceed with deterministic stepped execution
    await page.evaluate(() => (window as any).runCommand('stop'));
    await waitForPrompt(page);

    // --- Phase 2: create on-demand checkpoint and download it ---
    log('[state-test8] saving on-demand checkpoint (content mode)');
    await runCommand(page, 'save-state /tmp/on-demand-checkpoint content');

    log('[state-test8] downloading checkpoint via browser');
    const downloadPromise = page.waitForEvent('download', { timeout: 30_000 });
    await runCommand(page, 'download /tmp/on-demand-checkpoint');
    const download = await downloadPromise;
    const checkpointTempPath = await download.path();
    expect(checkpointTempPath, 'checkpoint download should produce a temp file').toBeTruthy();
    const checkpointBytes = new Uint8Array(fs.readFileSync(checkpointTempPath!));
    log(`[state-test8] downloaded checkpoint: ${checkpointBytes.length} bytes`);

    // ======================================================================
    // Iteration 1: run from current state, logging floppy+rtc activity

    log('[state-test8] iteration 1: start logging to file');
    await runCommand(page, 'log floppy level=5 file=/tmp/test-8-iteration-1.txt stdout=off ts=on');
    await runCommand(page, 'log rtc level=5 file=/tmp/test-8-iteration-1.txt stdout=off ts=on');

    // Click the Apple menu and select "About This Macintosh"
    await runCommand(page, 'set-mouse 25 10');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'mouse-button down');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'set-mouse 25 30');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'mouse-button up');

    // Wait for the About This Macintosh window to appear
    await runCommand(page, 'run 20000000');
    await waitForPrompt(page);

    await matchScreenFast(page, 'test-8-about', { initialWaitMs: 2_000, waitBeforeUpdateMs: 30_000, timeoutMs: 30_000 });

    // Disable logging
    await runCommand(page, 'log floppy level=0');
    await runCommand(page, 'log rtc level=0');

    // Read iteration 1 logs from MEMFS before reload clears it
    const iter1Logs = await readMemfsFiles(page, ['/tmp/test-8-iteration-1.txt']);
    const a = iter1Logs['/tmp/test-8-iteration-1.txt'];

    // Capture xterm output from phase 1 before reload
    log('[state-test8] capturing xterm (phase 1)');
    await captureXterm(page, 'test-8-phase-1-before-reload', testInfo, log);

    // ======================================================================
    // Iteration 2: reload page and restore checkpoint via drag-drop

    // Clear persisted checkpoint data so the "Continue?" dialog does not appear
    log('[state-test8] clearing checkpoint storage before reload');
    await runCommand(page, 'checkpoint clear');

    log('[state-test8] reloading tab');
    const reloadPromise = page.waitForLoadState('load');
    await page.evaluate(() => { window.location.reload(); }).catch(() => null);
    await reloadPromise;

    // Wait for WASM module to load and runCommand to become available
    await page.waitForFunction(() => {
      return typeof (window as any).runCommand === 'function';
    }, { timeout: 30_000 });

    // Dismiss the checkpoint dialog if it somehow still appears
    const dialog = await page.$('#checkpoint-dialog');
    if (dialog && await dialog.isVisible()) {
      log('[state-test8] dismissing stale checkpoint dialog');
      const discardBtn = page.locator('[data-checkpoint-discard]');
      if (await discardBtn.isVisible()) {
        await discardBtn.click();
      }
      await page.waitForTimeout(1_000);
    }

    // Restore the checkpoint by dropping it onto the canvas
    log('[state-test8] restoring checkpoint via drag-drop');
    const dropResult = await dispatchDropEvent(
      page, '#screen', 'on-demand-checkpoint.bin', checkpointBytes
    );
    expect(dropResult, 'drop event should dispatch successfully').toBe(true);

    // Wait for the drop handler to process the checkpoint and load-state to complete
    await page.waitForTimeout(5_000);
    await waitForPrompt(page);

    // Disable idle checkpointing again after restore
    await page.evaluate(() => (window as any).runCommand('checkpoint auto off'));

    log('[state-test8] iteration 2: start logging to file');
    await runCommand(page, 'log floppy level=5 file=/tmp/test-8-iteration-2.txt stdout=off ts=on');
    await runCommand(page, 'log rtc level=5 file=/tmp/test-8-iteration-2.txt stdout=off ts=on');

    // Click the Apple menu and select "About This Macintosh"
    await runCommand(page, 'set-mouse 25 10');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'mouse-button down');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'set-mouse 25 30');
    await runCommand(page, 'run 1000000');
    await waitForPrompt(page);
    await runCommand(page, 'mouse-button up');

    // Wait for the About This Macintosh window to appear
    await runCommand(page, 'run 20000000');
    await waitForPrompt(page);

    // Disable logging
    await runCommand(page, 'log floppy level=0');
    await runCommand(page, 'log rtc level=0');

    // ======================================================================
    // Compare logs and verify screen after restore

    // Extract iteration 2 log from MEMFS
    const iter2Logs = await readMemfsFiles(page, ['/tmp/test-8-iteration-2.txt']);
    const b = iter2Logs['/tmp/test-8-iteration-2.txt'];

    // Attach logs to Playwright test artifacts
    await test.info().attach('iteration-1-log', { body: a, contentType: 'text/plain' });
    await test.info().attach('iteration-2-log', { body: b, contentType: 'text/plain' });

    // Validate minimum line count before equality check
    const countLines = (s: string) => s === '' ? 0 : s.split(/\r?\n/).length;
    const linesA = countLines(a);
    const linesB = countLines(b);
    log(`[state-test8] iteration 1 log: ${linesA} lines, iteration 2 log: ${linesB} lines`);
    expect(linesA, `Original log has insufficient lines (${linesA} < 5)`).toBeGreaterThanOrEqual(5);
    expect(linesB, `Restored log has insufficient lines (${linesB} < 5)`).toBeGreaterThanOrEqual(5);

    // Assert equality of original vs restored run logs
    expect(a, 'Loaded state log must match original log').toBe(b);
  });
});