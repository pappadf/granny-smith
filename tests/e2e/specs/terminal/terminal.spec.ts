// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { test, expect } from '../../fixtures';

test.describe('Terminal interactions', () => {
  test('collapse toggle changes state', async ({ page }) => {
    await page.goto('/index.html');
    const panel = page.locator('#terminal-panel');
    await expect(panel).toHaveAttribute('data-collapsed', 'true');
    await page.click('#terminal-toggle');
    await expect(panel).toHaveAttribute('data-collapsed', 'false');
    await page.click('#terminal-toggle');
    await expect(panel).toHaveAttribute('data-collapsed', 'true');
  });

  test('runCommand helper reachable', async ({ page }) => {
    await page.goto('/index.html');
    await page.waitForFunction(() => typeof (window as any).runCommand === 'function');
    const ok = await page.evaluate(() => typeof (window as any).runCommand === 'function');
    expect(ok).toBeTruthy();
  });
});
