// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import type { Page } from '@playwright/test';

/**
 * Run an emulator command in the browser context and return its exit code.
 *
 * Commands return 0 for success, non-zero for failure (or 1 for cmd_bool
 * "true" — same as the legacy shell). This allows tests to check command
 * results programmatically.
 *
 * Usage:
 * ```
 * const exitCode = await runCommand(page, 'fd probe /tmp/file.dsk');
 * expect(exitCode).toBe(0); // success
 * ```
 *
 * Implementation (M10c): the helper translates well-known shell-form
 * lines into typed `gsEval` calls, then maps the typed return back to
 * the legacy int convention. Anything that doesn't have a typed
 * wrapper yet falls through to the legacy `runCommand` bridge — that
 * fallback path goes away with M10e.
 */
export async function runCommand(page: Page, cmd: string): Promise<number> {
  const translated = translateToGsEval(cmd);
  if (translated) {
    const result: any = await page.evaluate(
      ({ method, args }: { method: string; args: any[] }) => (window as any).gsEval(method, args),
      { method: translated.method, args: translated.args }
    );
    return mapResult(result, translated.convention);
  }
  // Legacy fallback for shell-form lines that don't have a typed wrapper
  // (eval / br / s / x / log / set / print / mouse-button / scc / sync / …).
  const result = await page.evaluate((c: string) => (window as any).runCommand(c), cmd);
  return typeof result === 'bigint' ? Number(result) : result;
}

// === Shell-form → gsEval translator ========================================

type ReturnConvention =
  | 'cmd_int_bool'   // bool true → 0, false → 1 (legacy cmd_int convention)
  | 'cmd_bool'       // bool true → 1, false → 0 (legacy cmd_bool convention)
  | 'string_nonempty'; // V_STRING non-empty → 1, empty → 0 (mirrors fd validate)

type Translation = {
  method: string;
  args: unknown[];
  convention: ReturnConvention;
};

function mapResult(value: any, convention: ReturnConvention): number {
  if (value && typeof value === 'object' && 'error' in value) {
    // gsEval returns {error: "..."} for unresolved paths or bad args.
    return 1;
  }
  if (convention === 'cmd_int_bool') return value === true ? 0 : 1;
  if (convention === 'cmd_bool') return value === true ? 1 : 0;
  if (convention === 'string_nonempty')
    return typeof value === 'string' && value.length > 0 ? 1 : 0;
  return 0;
}

// Tokenize a shell-form line. Handles "double quoted" args and
// preserves the rest as literal whitespace-separated tokens. Backslash
// escapes inside quotes are decoded.
function tokenize(line: string): string[] {
  const out: string[] = [];
  let i = 0;
  while (i < line.length) {
    while (i < line.length && /\s/.test(line[i])) i++;
    if (i >= line.length) break;
    let token = '';
    if (line[i] === '"') {
      i++;
      while (i < line.length && line[i] !== '"') {
        if (line[i] === '\\' && i + 1 < line.length) {
          token += line[i + 1];
          i += 2;
        } else {
          token += line[i];
          i++;
        }
      }
      if (i < line.length) i++; // consume closing "
    } else {
      while (i < line.length && !/\s/.test(line[i])) {
        token += line[i];
        i++;
      }
    }
    out.push(token);
  }
  return out;
}

function parseInt10(s: string | undefined): number | null {
  if (s === undefined) return null;
  const n = Number(s);
  return Number.isFinite(n) ? n : null;
}

function parseBool(s: string | undefined): boolean {
  if (s === undefined) return false;
  return s === 'true' || s === 'on' || s === 'yes' || s === '1';
}

function translateToGsEval(line: string): Translation | null {
  const t = tokenize(line);
  if (t.length === 0) return null;
  const head = t[0];
  const tail = t.slice(1);

  // Single-token commands (no subcommand).
  if (head === 'status' && tail.length === 0)
    return { method: 'running', args: [], convention: 'cmd_bool' };
  if (head === 'run')
    return {
      method: 'run',
      args: tail.length >= 1 && parseInt10(tail[0]) !== null ? [parseInt10(tail[0])] : [],
      convention: 'cmd_int_bool',
    };
  if (head === 'cp' && tail.length >= 2)
    return { method: 'cp', args: tail, convention: 'cmd_int_bool' };
  if (head === 'file-copy' && tail.length === 2)
    return { method: 'cp', args: tail, convention: 'cmd_int_bool' };
  if (head === 'find-media' && tail.length >= 1)
    return { method: 'find_media', args: tail, convention: 'cmd_int_bool' };
  if (head === 'download' && tail.length === 1)
    return { method: 'download', args: tail, convention: 'cmd_int_bool' };
  if (head === 'schedule' && tail.length === 1)
    return { method: 'schedule', args: tail, convention: 'cmd_int_bool' };

  // peeler: --probe X | -o D X | <archive>
  if (head === 'peeler') {
    if (tail[0] === '--probe' && tail.length === 2)
      return { method: 'peeler_probe', args: [tail[1]], convention: 'cmd_int_bool' };
    if (tail[0] === '-o' && tail.length === 3)
      return { method: 'peeler', args: [tail[2], tail[1]], convention: 'cmd_int_bool' };
  }

  // setup --model X --ram Y
  if (head === 'setup') {
    let model = '';
    let ram: number | null = null;
    for (let i = 0; i < tail.length; i++) {
      if (tail[i] === '--model' && i + 1 < tail.length) model = tail[++i];
      else if (tail[i] === '--ram' && i + 1 < tail.length) ram = parseInt10(tail[++i]);
    }
    if (model)
      return {
        method: 'setup_machine',
        args: ram !== null ? [model, ram] : [model],
        convention: 'cmd_int_bool',
      };
  }

  // checkpoint subcommands (--probe / clear / --load / --save / --machine)
  if (head === 'checkpoint') {
    if (tail.length === 0) return null; // bare `checkpoint` queries auto state — leave on legacy
    if (tail[0] === '--probe' && tail.length === 1)
      return { method: 'checkpoint_probe', args: [], convention: 'cmd_int_bool' };
    if ((tail[0] === 'clear' || tail[0] === '--clear') && tail.length === 1)
      return { method: 'checkpoint_clear', args: [], convention: 'cmd_int_bool' };
    if (tail[0] === '--load')
      return {
        method: 'checkpoint_load',
        args: tail.length >= 2 ? [tail[1]] : [],
        convention: 'cmd_int_bool',
      };
    if (tail[0] === '--save' && tail.length >= 2)
      return {
        method: 'checkpoint_save',
        args: tail.length >= 3 ? [tail[1], tail[2]] : [tail[1]],
        convention: 'cmd_int_bool',
      };
    if (tail[0] === '--machine' && tail.length === 3)
      return {
        method: 'register_machine',
        args: [tail[1], tail[2]],
        convention: 'cmd_int_bool',
      };
  }

  // Subcommand-style: rom / vrom / fd / hd / cdrom / image
  if (tail.length >= 1) {
    const sub = tail[0];
    const subArgs = tail.slice(1);

    if (head === 'rom') {
      if (sub === 'probe')
        return {
          method: 'rom_probe',
          args: subArgs.length >= 1 ? [subArgs[0]] : [],
          convention: 'cmd_int_bool',
        };
      if (sub === 'checksum' && subArgs.length === 1)
        return { method: 'rom_checksum', args: subArgs, convention: 'string_nonempty' };
      if (sub === 'load' && subArgs.length === 1)
        return { method: 'rom_load', args: subArgs, convention: 'cmd_int_bool' };
      if (sub === 'validate' && subArgs.length === 1)
        return { method: 'rom_validate', args: subArgs, convention: 'cmd_bool' };
    }

    if (head === 'vrom') {
      if (sub === 'probe' && subArgs.length === 1)
        return { method: 'vrom_probe', args: subArgs, convention: 'cmd_int_bool' };
      if (sub === 'validate' && subArgs.length === 1)
        return { method: 'vrom_validate', args: subArgs, convention: 'cmd_bool' };
      if (sub === 'load' && subArgs.length === 1)
        return { method: 'vrom_load', args: subArgs, convention: 'cmd_int_bool' };
    }

    if (head === 'fd') {
      if (sub === 'probe' && subArgs.length === 1)
        return { method: 'fd_probe', args: subArgs, convention: 'cmd_int_bool' };
      if (sub === 'validate' && subArgs.length === 1)
        return { method: 'fd_validate', args: subArgs, convention: 'string_nonempty' };
      if (sub === 'insert' && subArgs.length >= 2) {
        const slot = parseInt10(subArgs[1]) ?? 0;
        const writable = subArgs.length >= 3 ? parseBool(subArgs[2]) : false;
        return {
          method: 'fd_insert',
          args: [subArgs[0], slot, writable],
          convention: 'cmd_int_bool',
        };
      }
    }

    if (head === 'hd') {
      if (sub === 'validate' && subArgs.length === 1)
        return { method: 'hd_validate', args: subArgs, convention: 'cmd_bool' };
      if (sub === 'attach' && subArgs.length === 2) {
        const id = parseInt10(subArgs[1]) ?? 0;
        return { method: 'hd_attach', args: [subArgs[0], id], convention: 'cmd_int_bool' };
      }
      if (sub === 'create' && subArgs.length === 2)
        return { method: 'hd_create', args: subArgs, convention: 'cmd_int_bool' };
    }

    if (head === 'cdrom') {
      if (sub === 'validate' && subArgs.length === 1)
        return { method: 'cdrom_validate', args: subArgs, convention: 'cmd_bool' };
      if (sub === 'attach' && subArgs.length === 1)
        return { method: 'cdrom_attach', args: subArgs, convention: 'cmd_int_bool' };
    }

    if (head === 'image') {
      if (sub === 'partmap' && subArgs.length === 1)
        return { method: 'partmap', args: subArgs, convention: 'cmd_int_bool' };
      if (sub === 'probe' && subArgs.length === 1)
        return { method: 'probe', args: subArgs, convention: 'cmd_int_bool' };
      if (sub === 'list')
        return {
          method: 'list_partitions',
          args: subArgs.length >= 1 ? subArgs : [],
          convention: 'cmd_int_bool',
        };
      if (sub === 'unmount' && subArgs.length === 1)
        return { method: 'unmount', args: subArgs, convention: 'cmd_int_bool' };
    }
  }

  return null;
}

/**
 * Wait first for a short delay (to allow UI updates), then wait until
 * runCommand is ready and the scheduler reports idle via `running()`.
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

  // Wait for Module readiness via either bridge.
  try {
    await page.waitForFunction(() => {
      const w: any = window as any;
      return typeof w.runCommand === 'function' && typeof w.gsEval === 'function';
    }, { timeout: timeoutMs });
  } catch (e) {
    throw new Error(`Timeout waiting for runCommand readiness after ${timeoutMs}ms`);
  }

  // Poll `running()` via gsEval until the scheduler is idle.
  const pollIntervalMs = 100;
  const startTime = Date.now();
  while (true) {
    try {
      const isRunning = await page.evaluate(() => (window as any).gsEval('running'));
      if (isRunning === false) return; // Idle
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
  // Trigger the sync (no typed wrapper yet — stays on the legacy bridge).
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
 * Uses `checkpoint_probe()` via gsEval.
 */
export async function waitForCompleteCheckpoint(page: Page, timeoutMs = 15_000): Promise<void> {
  const pollIntervalMs = 200;
  const startTime = Date.now();

  while (true) {
    const ok = await page.evaluate(() => (window as any).gsEval('checkpoint_probe'));
    if (ok === true) return;

    if (Date.now() - startTime > timeoutMs) {
      throw new Error(`Timeout waiting for complete checkpoint after ${timeoutMs}ms`);
    }

    await page.waitForTimeout(pollIntervalMs);
  }
}
