// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Tab-completion E2E (M9 — proposal §4.6).
// Drives `window.tabComplete(line, cursor)` directly so the assertions
// don't have to wrestle with terminal keystroke timing. Each case
// describes one branch of the completer's decision tree.

import { test, expect } from '../../fixtures';
import { bootWithMedia } from '../../helpers/boot';

async function complete(page: any, line: string, cursor?: number): Promise<string[]> {
  return await page.evaluate(
    ({ line, cursor }: { line: string; cursor: number }) => {
      const fn = (window as any).tabComplete;
      return fn ? fn(line, cursor) : null;
    },
    { line, cursor: cursor ?? line.length }
  );
}

test.describe('Tab completion', () => {
  test.beforeEach(async ({ page }) => {
    // Boot a real machine so the object root has children attached
    // (cpu/memory/scsi/floppy/...) — the tree-walking completer needs
    // them to produce mid-path suggestions.
    await bootWithMedia(page, 'roms/Plus_v3.rom');
    await page.waitForFunction(() => typeof (window as any).tabComplete === 'function');
  });

  test('line-start lists root children and root methods', async ({ page }) => {
    const all = await complete(page, '');
    expect(Array.isArray(all)).toBeTruthy();
    // Top-level objects attached to the root.
    expect(all).toEqual(expect.arrayContaining(['cpu', 'memory', 'storage']));
    // Root methods (registered as members on emu_root_class_real in
    // M8 slice 3 / 4): help, quit, cp, peeler, …
    expect(all).toEqual(expect.arrayContaining(['help', 'quit']));
  });

  test('line-start prefix narrows the suggestions', async ({ page }) => {
    const matches = await complete(page, 'cp');
    // `cpu` (root child) and `cp` (root method) both start with "cp".
    expect(matches).toEqual(expect.arrayContaining(['cpu', 'cp']));
    // Anything that doesn't start with `cp` must be filtered out.
    expect(matches.every((m: string) => m.toLowerCase().startsWith('cp'))).toBeTruthy();
  });

  test('mid-path lists members of the resolved object', async ({ page }) => {
    const matches = await complete(page, 'cpu.');
    // CPU has pc / sr / d0..d7 / a0..a7 etc. Composed names carry the
    // head so the terminal performs one full-token replace.
    expect(matches).toEqual(expect.arrayContaining(['cpu.pc', 'cpu.sr']));
    // Every candidate must keep the head.
    expect(matches.every((m: string) => m.startsWith('cpu.'))).toBeTruthy();
  });

  test('mid-path prefix narrows by tail', async ({ page }) => {
    const matches = await complete(page, 'cpu.p');
    expect(matches).toEqual(expect.arrayContaining(['cpu.pc']));
    // No `cpu.sr` (does not start with `p`).
    expect(matches.every((m: string) => m.startsWith('cpu.p'))).toBeTruthy();
  });

  test('method-arg position dispatches V_BOOL → on/off/true/false', async ({ page }) => {
    // sound.mute(muted: V_BOOL) — proposal §4.6 says method-arg
    // position dispatches by arg_decl[i].
    const matches = await complete(page, 'sound.mute ');
    expect(matches).toEqual(expect.arrayContaining(['on', 'off', 'true', 'false']));
  });

  test('cursor inside $(...) returns no suggestions', async ({ page }) => {
    // `echo $(cpu.|)` — cursor between the `.` and `)`. Inside the
    // expression, completion is deliberately suppressed (proposal §4.6).
    const line = 'echo $(cpu.)';
    const matches = await complete(page, line, line.length - 1);
    expect(matches).toEqual([]);
  });

  test('cursor inside "..." returns no suggestions', async ({ page }) => {
    const line = 'echo "cpu.';
    const matches = await complete(page, line, line.length);
    expect(matches).toEqual([]);
  });

  test('cursor inside ${...} returns no suggestions', async ({ page }) => {
    const line = 'echo "x=${cpu.';
    const matches = await complete(page, line, line.length);
    expect(matches).toEqual([]);
  });

  test('legacy commands still tab-complete', async ({ page }) => {
    // The legacy registry is union-merged with the root tree at the
    // line-start position; until the M10 cutover deletes the registry,
    // names like `eval` must still appear when their prefix is typed.
    const matches = await complete(page, 'eva');
    expect(matches).toEqual(expect.arrayContaining(['eval']));
  });
});
