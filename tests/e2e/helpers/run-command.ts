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
 * Implementation: every shell-form line is translated to a typed `gsEval`
 * call. The legacy `window.runCommand` bridge is no longer reachable
 * through this helper; the only spec that still calls it directly is
 * the gs-eval regression test that asserts the legacy bridge keeps
 * working alongside the new wrapper.
 */
export async function runCommand(page: Page, cmd: string): Promise<number> {
  const translated = translateToGsEval(cmd);
  if (!translated) {
    throw new Error(
      `runCommand: shell form '${cmd}' has no typed translation — ` +
        `extend translateToGsEval() in run-command.ts or call window.runCommand directly.`
    );
  }
  const result: any = await page.evaluate(
    ({ method, args }: { method: string; args: any[] }) =>
      (window as any).gsEval(method, args),
    { method: translated.method, args: translated.args }
  );
  return mapResult(result, translated.convention);
}

// === Shell-form → gsEval translator ========================================

type ReturnConvention =
  | 'cmd_int_bool'   // bool true → 0, false → 1 (legacy cmd_int convention)
  | 'cmd_bool'       // bool true → 1, false → 0 (legacy cmd_bool convention)
  | 'string_nonempty' // V_STRING non-empty → 1, empty → 0 (mirrors fd validate)
  | 'void_or_error'  // any non-error → 0, error → 1 (legacy "did dispatch succeed")
  | 'pass_through';  // numeric value passed through unchanged (e.g. size/print)

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
  if (convention === 'void_or_error') return 0;
  if (convention === 'pass_through') {
    if (typeof value === 'number') return value;
    if (typeof value === 'bigint') return Number(value);
    return 0;
  }
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
  if (head === 'br' && tail.length === 1)
    return { method: 'break_set', args: [tail[0]], convention: 'cmd_int_bool' };
  if (head === 'stop' && tail.length === 0)
    return { method: 'stop', args: [], convention: 'cmd_int_bool' };
  if ((head === 's' || head === 'step') && tail.length <= 1) {
    const n = tail.length === 1 ? parseInt10(tail[0]) : null;
    return {
      method: 'step',
      args: tail.length === 1 && n !== null ? [n] : [],
      convention: 'cmd_int_bool',
    };
  }
  if (head === 'background-checkpoint' && tail.length === 1)
    return { method: 'background_checkpoint', args: [tail[0]], convention: 'cmd_int_bool' };
  if (head === 'exists' && tail.length === 1)
    // Legacy `exists` returned 0=exists, 1=missing — map_int_bool keeps that.
    return { method: 'path_exists', args: [tail[0]], convention: 'cmd_int_bool' };
  if (head === 'size' && tail.length === 1)
    // Legacy `size` returned the byte count — pass it through unchanged.
    return { method: 'path_size', args: [tail[0]], convention: 'pass_through' };

  // `screenshot checksum [top left bottom right]` — used by helpers/screen.ts
  // for fast frame matching. The typed wrapper takes either 0 or 4 args and
  // returns the polynomial hash as V_INT; pass it through unchanged.
  if (head === 'screenshot' && tail.length >= 1 && tail[0] === 'checksum') {
    if (tail.length === 1)
      return { method: 'screen.checksum', args: [], convention: 'pass_through' };
    if (tail.length === 5) {
      const t = parseInt10(tail[1]);
      const l = parseInt10(tail[2]);
      const b = parseInt10(tail[3]);
      const r = parseInt10(tail[4]);
      if (t !== null && l !== null && b !== null && r !== null)
        return { method: 'screen.checksum', args: [t, l, b, r], convention: 'pass_through' };
    }
  }

  // Debug ops: print/set/x. Targets accept the legacy syntax (`d5`, `pc`,
  // `z`, `0x1000.b`, `instr`, `$pc`, etc.) — passed through verbatim to
  // the print/set/examine wrappers, which forward to the legacy parser.
  if (head === 'print') {
    if (tail.length === 0)
      // Bare `print` — typed wrapper rejects empty target with V_ERR,
      // which mapResult turns into 1 (matches the test expectation).
      return { method: 'print_value', args: [''], convention: 'pass_through' };
    if (tail.length === 1)
      return { method: 'print_value', args: [tail[0]], convention: 'pass_through' };
    // `print instr` and similar multi-token forms with a single logical
    // target: rejoin them into one string.
    return {
      method: 'print_value',
      args: [tail.join(' ')],
      convention: 'pass_through',
    };
  }
  if (head === 'set') {
    if (tail.length === 2)
      return { method: 'set_value', args: tail, convention: 'cmd_int_bool' };
    if (tail.length === 0)
      // Bare `set` — typed wrapper rejects with V_ERR; legacy printed
      // a usage message and returned 0. The debug spec asserts `toBe(0)`
      // here, so we pass through cmd_int_bool which gives 1 on error.
      // Tests for `set` (no args) currently use `expect(exitCode).toBe(0)`
      // — use void_or_error to keep that contract.
      return { method: 'set_value', args: ['', ''], convention: 'void_or_error' };
  }
  if (head === 'x' && tail.length >= 1) {
    const args: unknown[] = [tail[0]];
    if (tail.length >= 2) {
      const c = parseInt10(tail[1]);
      if (c !== null) args.push(c);
    }
    return { method: 'examine', args, convention: 'cmd_int_bool' };
  }


  // Mouse: set-mouse [--global|--hw|--aux] x y, or x y first then mode.
  if (head === 'set-mouse') {
    let mode: string | null = null;
    const positional: string[] = [];
    for (const arg of tail) {
      if (arg === '--global') mode = 'global';
      else if (arg === '--hw') mode = 'hw';
      else if (arg === '--aux') mode = 'aux';
      else positional.push(arg);
    }
    if (positional.length === 2) {
      const x = parseInt10(positional[0]);
      const y = parseInt10(positional[1]);
      if (x !== null && y !== null) {
        const args: unknown[] = [x, y];
        if (mode) args.push(mode);
        return { method: 'mouse.move', args, convention: 'cmd_int_bool' };
      }
    }
  }

  // Mouse: mouse-button [--global|--hw] up|down
  if (head === 'mouse-button') {
    let mode: string | null = null;
    let downStr: string | null = null;
    for (const arg of tail) {
      if (arg === '--global') mode = 'global';
      else if (arg === '--hw') mode = 'hw';
      else if (arg === 'down' || arg === 'up') downStr = arg;
    }
    if (downStr !== null) {
      const args: unknown[] = [downStr === 'down'];
      if (mode) args.push(mode);
      return { method: 'mouse.click', args, convention: 'cmd_int_bool' };
    }
  }

  // AppleTalk shares (typed methods return V_NONE — use void_or_error)
  if (head === 'atalk-share-add' && tail.length === 2)
    return {
      method: 'network.appletalk.shares.add',
      args: tail,
      convention: 'void_or_error',
    };
  if (head === 'atalk-share-remove' && tail.length === 1)
    return {
      method: 'network.appletalk.shares.remove',
      args: tail,
      convention: 'void_or_error',
    };

  // logpoint <spec...> — pack everything into a single spec string.
  // Keep `logpoint list` / `logpoint clear` on the typed list/clear methods.
  if (head === 'logpoint') {
    if (tail.length === 1 && tail[0] === 'list')
      return { method: 'logpoint_list_dump', args: [], convention: 'cmd_int_bool' };
    if (tail.length === 1 && tail[0] === 'clear')
      return { method: 'logpoint_clear', args: [], convention: 'cmd_int_bool' };
    if (tail.length >= 1)
      return {
        method: 'logpoint_set',
        args: [tail.join(' ')],
        convention: 'cmd_int_bool',
      };
  }

  // log <cat> <level>  (positional shorthand) or
  // log <cat> level=N file=... stdout=... ts=...  (full named-arg spec).
  // log_set's second arg now accepts either an integer level or the full
  // spec string (forwarded verbatim to the legacy `log` parser).
  if (head === 'log' && tail.length >= 2) {
    const cat = tail[0];
    if (tail.length === 2) {
      const lvl = parseInt10(tail[1]);
      if (lvl !== null)
        return { method: 'log_set', args: [cat, lvl], convention: 'cmd_int_bool' };
    }
    return {
      method: 'log_set',
      args: [cat, tail.slice(1).join(' ')],
      convention: 'cmd_int_bool',
    };
  }

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

  // checkpoint subcommands (--probe / clear / --load / --save / --machine /
  // auto on|off). `checkpoint auto on|off` writes the auto_checkpoint
  // attribute; bare `checkpoint` is no longer a valid form (the legacy
  // bridge that handled the auto-state query is gone).
  if (head === 'checkpoint') {
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
    if (tail[0] === 'auto' && tail.length === 2 && (tail[1] === 'on' || tail[1] === 'off'))
      return {
        method: 'auto_checkpoint',
        args: [tail[1] === 'on'],
        convention: 'void_or_error',
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
      if (sub === 'create' && subArgs.length >= 1)
        return {
          method: 'fd_create',
          args: subArgs.length >= 2 ? [subArgs[0], subArgs[1]] : [subArgs[0]],
          convention: 'cmd_int_bool',
        };
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
      if (sub === 'loopback' && subArgs.length === 1 && (subArgs[0] === 'on' || subArgs[0] === 'off'))
        return { method: 'scsi.loopback', args: [subArgs[0] === 'on'], convention: 'void_or_error' };
    }

    if (head === 'cdrom') {
      if (sub === 'validate' && subArgs.length === 1)
        return { method: 'cdrom_validate', args: subArgs, convention: 'cmd_bool' };
      if (sub === 'attach' && subArgs.length === 1)
        return { method: 'cdrom_attach', args: subArgs, convention: 'cmd_int_bool' };
      if (sub === 'eject')
        return {
          method: 'cdrom_eject',
          args: subArgs.length >= 1 ? [parseInt10(subArgs[0]) ?? 3] : [],
          convention: 'cmd_int_bool',
        };
      if (sub === 'info')
        return {
          method: 'cdrom_info',
          args: subArgs.length >= 1 ? [parseInt10(subArgs[0]) ?? 3] : [],
          convention: 'cmd_int_bool',
        };
    }

    if (head === 'scc') {
      // `scc loopback` (query) → bare-read attribute (legacy returns 0 = ok
      // regardless of state); `scc loopback on|off` → setter, returns the
      // new value (true → cmd_int_bool 0 = success).
      if (sub === 'loopback') {
        if (subArgs.length === 0)
          return { method: 'scc.loopback', args: [], convention: 'void_or_error' };
        if (subArgs.length === 1 && (subArgs[0] === 'on' || subArgs[0] === 'off'))
          return { method: 'scc.loopback', args: [subArgs[0] === 'on'], convention: 'void_or_error' };
      }
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
