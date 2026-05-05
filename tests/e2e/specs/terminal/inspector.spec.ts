// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Object inspector E2E (M11 — proposal §7).
// Drives the panel through `window.__gsInspector` so the assertions
// don't depend on layout pixel offsets.

import { test, expect } from '../../fixtures';
import { bootWithMedia } from '../../helpers/boot';

test.describe('Object inspector', () => {
  test.beforeEach(async ({ page }) => {
    // The panel reads the live tree, so it needs a booted machine.
    await bootWithMedia(page, 'roms/Plus_v3.rom');
    await page.waitForFunction(
      () => typeof (window as any).__gsInspector === 'object'
    );
  });

  test('expanded panel lists root children', async ({ page }) => {
    await page.evaluate(() => (window as any).__gsInspector.expand());
    await page.evaluate(() => (window as any).__gsInspector.refresh());
    const names = await page.evaluate(() => (window as any).__gsInspector.getTreeNames());
    // Plus boots cpu/memory/storage as a minimum — the tree must show
    // at least these.
    expect(names).toEqual(expect.arrayContaining(['cpu', 'memory', 'storage']));
  });

  test('selecting a path renders its attributes and methods', async ({ page }) => {
    await page.evaluate(() => (window as any).__gsInspector.expand());
    await page.evaluate(() => (window as any).__gsInspector.refresh());
    await page.evaluate(() => (window as any).__gsInspector.select('cpu'));
    const detail = await page.evaluate(() =>
      (window as any).__gsInspector.getDetailText()
    );
    // The selected heading is the path; cpu's attribute table must
    // include `pc` and `sr`.
    expect(detail).toContain('cpu');
    expect(detail).toContain('pc');
    expect(detail).toContain('sr');
  });

  test('refresh re-reads attribute values after a write', async ({ page }) => {
    await page.evaluate(() => (window as any).__gsInspector.expand());
    await page.evaluate(() => (window as any).__gsInspector.select('sound'));
    const before = await page.evaluate(() =>
      (window as any).__gsInspector.getDetailText()
    );
    // Toggle sound.enabled via gsEval, then refresh and verify the
    // panel picked up the new value.
    await page.evaluate(async () => {
      const cur = await (window as any).gsEval('sound.enabled');
      await (window as any).gsEval('sound.enabled', [!cur]);
    });
    await page.evaluate(() => (window as any).__gsInspector.refresh());
    const after = await page.evaluate(() =>
      (window as any).__gsInspector.getDetailText()
    );
    // The text changed somewhere — same heading, but the boolean flipped.
    expect(after).not.toBe(before);
    // Restore for hygiene.
    await page.evaluate(async () => {
      const cur = await (window as any).gsEval('sound.enabled');
      await (window as any).gsEval('sound.enabled', [!cur]);
    });
  });
});
