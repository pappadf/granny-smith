// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { test, expect } from '../../fixtures';
import { getLastTerminalLine } from '../../helpers/terminal';
import { bootWithMedia } from '../../helpers/boot';
import { matchScreenFast } from '../../helpers/screen';

// Basic smoke test: page loads, toolbar buttons exist, test hooks active.

test.describe('Granny Smith UI basic', () => {
  test('loads index and exposes test hooks', async ({ page }) => {
    await page.goto('/index.html');
    await expect(page.locator('.screen-toolbar')).toBeVisible();
    await expect(page.locator('#btn-run')).toBeVisible();
    
    // Test hook presence
    const hasHooks = await page.evaluate(() => !!(window as any).__gsTestShim);
    expect(hasHooks).toBeTruthy();
  });

  test('command logging via runCommand', async ({ page }) => {
    await page.goto('/index.html');
    // Wait for command bridge (async module load)
    await page.waitForFunction(() => typeof (window as any).runCommand === 'function');
    await page.evaluate(() => (window as any).__gsTest?.clearLog());
    await page.evaluate(() => (window as any).runCommand('run'));
    const log = await page.evaluate(() => (window as any).__gsTest?.commandLog);
    expect(log).toContain('run');
  });

  test('zoom out eventually reduces effective scale', async ({ page }) => {
    await page.goto('/index.html');
    const zoomInput = page.locator('#zoom-level');
    // Wait for JS module to initialize zoom from HTML default (100%) to configured (200%)
    await expect(zoomInput).toHaveValue('200%', { timeout: 15000 });
    const initial = parseInt((await zoomInput.inputValue()).replace('%',''),10);
    // Click zoom-out up to 10 times until value changes (due to integer scale locking)
    for (let i=0;i<10;i++) {
      await page.click('#zoom-out');
      const cur = parseInt((await zoomInput.inputValue()).replace('%',''),10);
      if (cur < initial) { expect(cur).toBeLessThan(initial); return; }
    }
    throw new Error('Zoom level did not decrease after multiple attempts');
  });

  test('zoom steps 10% down 200->100 (width 1024->512) then up 100->300 (width 512->1536)', async ({ page }) => {
    await page.goto('/index.html');
    const zoomInput = page.locator('#zoom-level');
    const canvas = page.locator('#screen');
    async function canvasWidth(){
      return await canvas.evaluate(el => parseInt(getComputedStyle(el).width, 10));
    }
    // Initial state
    await expect(zoomInput).toHaveValue('200%');
    await expect.poll(canvasWidth).toBe(1024);
    // Step down to 100% checking each 10% decrement
    for (let expected = 190; expected >= 100; expected -= 10) {
      await page.click('#zoom-out');
      await expect(zoomInput).toHaveValue(expected + '%');
      // Only assert width at boundary 100%
      if (expected === 100) {
        await expect.poll(canvasWidth).toBe(512);
      }
    }
    // Step up to 300% checking each 10% increment
    for (let expected = 110; expected <= 300; expected += 10) {
      await page.click('#zoom-in');
      await expect(zoomInput).toHaveValue(expected + '%');
      if (expected === 300) {
        await expect.poll(canvasWidth).toBe(1536);
      }
    }
  });

  test('play/pause toggles emulator and prompts show differing addresses', async ({ page }) => {
    // Boot directly with ROM (auto-runs); no manual inject or initial Run click needed.
    await bootWithMedia(page, 'roms/Plus_v3.rom');
    await page.waitForFunction(() => typeof (window as any).runCommand === 'function');

    // Disable idle checkpointing for this test to avoid state corruption
    await page.evaluate(() => (window as any).runCommand('checkpoint auto off'));

    // Allow brief execution time before first pause
    await page.waitForTimeout(1000);

    // Helper to retrieve the last non-empty terminal line via test hooks
    const getLastLine = () => getLastTerminalLine(page);

    // First click pauses (was running already)
    await page.click('#btn-run');
    // Wait until a prompt line appears (ends with '>')
    await page.waitForFunction(() => {
      try {
        const snap = (window as any).__gsTest?.getTerminalSnapshot(400) || '';
  const lines = snap.split('\n').map((l: string) => l.trim()).filter(Boolean);
        const last = lines.length ? lines[lines.length - 1] : '';
        return />\s*$/.test(last);
      } catch (e) { return false; }
    }, { timeout: 5000 });

    const firstPrompt = await getLastLine();
    expect(firstPrompt).toMatch(/>\s*$/);
    const firstAddrMatch = firstPrompt.match(/0x[0-9A-Fa-f]+|[0-9A-Fa-f]{3,}/);
    expect(firstAddrMatch, 'first prompt should include an address-like token').toBeTruthy();
    const firstAddr = firstAddrMatch ? firstAddrMatch[0] : '';

  // Run again
    await page.click('#btn-run');
    await page.waitForTimeout(1000);
    await page.click('#btn-run');
    await page.waitForFunction(() => {
      try {
        const snap = (window as any).__gsTest?.getTerminalSnapshot(400) || '';
  const lines = snap.split('\n').map((l: string) => l.trim()).filter(Boolean);
        const last = lines.length ? lines[lines.length - 1] : '';
        return />\s*$/.test(last);
      } catch (e) { return false; }
    }, { timeout: 5000 });

    const secondPrompt = await getLastLine();
    expect(secondPrompt).toMatch(/>\s*$/);
    const secondAddrMatch = secondPrompt.match(/0x[0-9A-Fa-f]+|[0-9A-Fa-f]{3,}/);
    expect(secondAddrMatch, 'second prompt should include an address-like token').toBeTruthy();
    const secondAddr = secondAddrMatch ? secondAddrMatch[0] : '';

    // Addresses ought to differ between two separate run sessions
    expect(firstAddr).not.toEqual(secondAddr);
  });

  test('ROM boot prompt shows blinking question mark', async ({ page, log }) => {
    test.setTimeout(60_000);

    log('[rom-boot-prompt] booting with ROM only');
    await bootWithMedia(page, 'roms/Plus_v3.rom');

    log('[rom-boot-prompt] waiting for boot and matchScreenFast: blinking question mark');
    // Verify we see the blinking question mark disk icon (waiting for boot disk)
    await matchScreenFast(page, 'rom-boot-prompt', {
      initialWaitMs: 2000,
      waitBeforeUpdateMs: 22_000,
      timeoutMs: 30_000
    });

    log('[rom-boot-prompt] test complete');
  });
});
