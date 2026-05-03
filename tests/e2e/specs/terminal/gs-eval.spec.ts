// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gsEval E2E (M10a — proposal §7.2 / object-model-impl-plan.md M10a).
// Verifies that the new JS wrapper around `gs_eval` lands alongside the
// legacy `runCommand` path. Migration of callers happens in M10b; this
// test only confirms the wrapper resolves attribute reads, attribute
// writes, and zero-arg method calls without disturbing the existing
// command bridge.

import { test, expect } from '../../fixtures';
import { bootWithMedia } from '../../helpers/boot';

test.describe('gsEval bridge', () => {
  test.beforeEach(async ({ page }) => {
    // The wrapper needs the object root populated, which only happens
    // after a machine boots (gs_classes_install runs from system_create).
    await bootWithMedia(page, 'roms/Plus_v3.rom');
    await page.waitForFunction(() => typeof (window as any).gsEval === 'function');
  });

  test('attribute read returns the current value as JSON', async ({ page }) => {
    const pc = await page.evaluate(async () => await (window as any).gsEval('cpu.pc'));
    // Plus boots with cpu.pc inside ROM space; the value formats as a
    // hex string per VAL_HEX (gs_api JSON serializer rule).
    expect(typeof pc).toBe('string');
    expect(pc).toMatch(/^0x[0-9a-f]+$/i);
  });

  test('attribute write round-trips through node_set', async ({ page }) => {
    // sound.enabled is a writable V_BOOL mirror of the mute gate.
    const round = await page.evaluate(async () => {
      const ge = (window as any).gsEval;
      const before = await ge('sound.enabled');
      const w = await ge('sound.enabled', [!before]);
      const after = await ge('sound.enabled');
      // Restore the original so the next test isn't perturbed.
      await ge('sound.enabled', [before]);
      return { before, w, after };
    });
    // Setter returns V_NONE (formatted as null); reader returns the new bool.
    expect(typeof round.before).toBe('boolean');
    expect(round.w).toBeNull();
    expect(round.after).toBe(!round.before);
  });

  test('zero-arg method call dispatches via node_call', async ({ page }) => {
    // `time()` is a root method (M8 slice 3). Result is a positive
    // monotonic-ish wall-clock number; we only assert it parses.
    const t = await page.evaluate(async () => await (window as any).gsEval('time'));
    expect(typeof t === 'number' || typeof t === 'string').toBeTruthy();
  });

  test('unresolvable path returns an error object', async ({ page }) => {
    const r = await page.evaluate(async () => await (window as any).gsEval('not.a.thing'));
    expect(r).toEqual({ error: expect.stringContaining('did not resolve') });
  });
});
