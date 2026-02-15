// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import type { Page } from '@playwright/test';

/**
 * Run an emulator command in the browser context and return its exit code.
 * 
 * Commands return 0 for success, non-zero for failure.
 * This allows tests to check command results programmatically.
 *
 * Usage: 
 * ```
 * const exitCode = await runCommand(page, 'insert-fd --probe /tmp/file.dsk');
 * expect(exitCode).toBe(0); // success
 * ```
 */
export async function runCommand(page: Page, cmd: string): Promise<number> {
  const result = await page.evaluate((c: string) => (window as any).runCommand(c), cmd);
  // Convert BigInt to Number if needed (Emscripten returns BigInt for uint64_t)
  return typeof result === 'bigint' ? Number(result) : result;
}

/**
 * Wait first for a short delay (to allow UI updates), then wait until
 * runCommand is ready and the scheduler reports idle via the 'status' shell command.
 *
 * This combines the previous pattern of `await page.waitForTimeout(10_000)` +
 * `waitForIdle(page, timeoutMs)` into a single helper.
 *
 * Default `initialWaitMs` matches existing tests (10_000) and default
 * `timeoutMs` matches the previous `waitForIdle` default (120_000).
 */
export async function waitForPrompt(page: Page, timeoutMs = 120_000, initialWaitMs = 10_000): Promise<void> {
  // initial short wait used throughout tests
  await page.waitForTimeout(initialWaitMs);

  // Wait for Module readiness
  try {
    await page.waitForFunction(() => {
      const w: any = window as any;
      return !!(w.runCommand && typeof w.runCommand === 'function');
    }, { timeout: timeoutMs });
  } catch (e) {
    throw new Error(`Timeout waiting for runCommand readiness after ${timeoutMs}ms`);
  }

  // Poll status command until scheduler is idle (returns 0)
  const pollIntervalMs = 100;
  const startTime = Date.now();
  while (true) {
    try {
      const status = await runCommand(page, 'status');
      if (status === 0) return; // Idle
    } catch {
      // Ignore errors during polling
    }
    
    if (Date.now() - startTime > timeoutMs) {
      throw new Error(`Timeout waiting for scheduler to become idle after ${timeoutMs}ms`);
    }
    
    await page.waitForTimeout(pollIntervalMs);
  }
}

/**
 * Trigger a filesystem sync and wait for it to complete.
 * Uses the 'sync' command to start, then polls 'sync status' until done.
 */
export async function waitForSync(page: Page, timeoutMs = 10_000): Promise<void> {
  // Trigger the sync
  await runCommand(page, 'sync');
  
  // Poll until sync completes (status returns 0)
  const pollIntervalMs = 200;
  const startTime = Date.now();
  
  while (true) {
    const status = await runCommand(page, 'sync status');
    if (status === 0) return; // Sync complete
    
    if (Date.now() - startTime > timeoutMs) {
      throw new Error(`Timeout waiting for sync to complete after ${timeoutMs}ms`);
    }
    
    await page.waitForTimeout(pollIntervalMs);
  }
}

/**
 * Wait for at least one complete checkpoint to exist.
 * Uses `load-state probe` which returns 0 when a valid (complete) checkpoint
 * is found in the checkpoint directory.
 */
export async function waitForCompleteCheckpoint(page: Page, timeoutMs = 15_000): Promise<void> {
  const pollIntervalMs = 200;
  const startTime = Date.now();

  while (true) {
    const result = await runCommand(page, 'load-state probe');
    if (result === 0) return;

    if (Date.now() - startTime > timeoutMs) {
      throw new Error(`Timeout waiting for complete checkpoint after ${timeoutMs}ms`);
    }

    await page.waitForTimeout(pollIntervalMs);
  }
}
