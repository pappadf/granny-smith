// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 in-browser head-vs-base benchmark: the browser variant of the CPU
// benchmarks (docs/guide/BENCHMARKS.md, metric B).  Two WASM builds -- this checkout's (served by the Playwright web
// server) and a base build's dist/ (PERF_BASE_DIST, served here on a second
// port) -- run in two pages of ONE browser session, and their turbo-mode
// MIPS are measured alternately, head and base taking turns while the other
// is stopped, so the host, the browser and the JIT see both alike.  A JS
// port of gs-headless --calibrate's kernels runs in the page as well.
//
// Workloads (the native rows of the same name, reached the same way):
//   se30    System 7.5.3 from the bare volume at the Finder desktop, idle
//   iicx    Marathon at its main menu on a IIcx with the 8•24 GC
//   pm6100  the 601: a media-free ROM boot 200 M instructions in, the ROM's
//           68K emulator running the Start Manager (pm6100-68kemu)
//   pm8500  the 604: the System 7.6 desktop 4 G instructions after power-on
//           (pm8500-finder)
// The PowerPC rows are reached by instruction count, as the native rows
// are, with the browser's clock (the native rows pin the RTC).  The windows
// run on from there (turbo, seconds each), so they measure a later stretch
// than the native rows' windows: the pm6100 the ROM's wait for a boot disk,
// the pm8500 the desktop.  No G3 row: its boot naps in stretches, and
// windows of this length run into them.
// After the pairs each page reports predecode.enabled and its counters.
//
// Report only: `PERFBENCH label=<...> machine=<...> binary=<head|base>
// pair=<n> mips=<...>` lines and a ratio per machine; the assertions are
// sanity checks.  Skipped without PERF_BASE_DIST (scripts/bench/build-base.sh
// --wasm builds one).  PERF_PAIRS (default 3), PERF_WINDOW_SECS (default 6),
// PERF_MACHINES (default se30,iicx).

import { test, expect, type Page, type BrowserContext } from '@playwright/test';
import { spawn, type ChildProcess } from 'node:child_process';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { stageOpfsFile, stageOpfsFileStreaming } from '../helpers/web2-fs';
import { CONSOLE_OUTPUT, terminalRun as typeLine } from '../helpers/terminal';

const terminalRun = (page: Page, line: string) => typeLine(page, line, { settleMs: 250 });

const DATA = path.resolve(__dirname, '../../data');
const LABEL = process.env.PERF_LABEL ?? 'unlabeled';
const PAIRS = Number(process.env.PERF_PAIRS ?? 3);
const WINDOW_SECS = Number(process.env.PERF_WINDOW_SECS ?? 6);
const MACHINES = (process.env.PERF_MACHINES ?? 'se30,iicx').split(',');
const BASE_DIST = process.env.PERF_BASE_DIST ?? '';
const HEAD_URL = 'http://localhost:18090/index.html?speed=turbo';
const BASE_PORT = 18091;
const BASE_URL = `http://localhost:${BASE_PORT}/index.html?speed=turbo`;

type Row = {
  rom: string;
  ram: string;
  hd?: string; // none: a media-free ROM boot
  settle: 'finder' | 'marathon' | 'instr';
  instr?: number; // settle 'instr': run to this instruction count
  vrom?: string;
  card?: string;
  monitor?: string;
  mode?: string;
};

const ROWS: Record<string, Row> = {
  se30: {
    rom: 'iix-iicx-se30-97221136.rom',
    ram: '8 MB',
    hd: 'systems/system_7_5_3_25mb_bare.img',
    settle: 'finder',
  },
  iicx: {
    rom: 'iix-iicx-se30-97221136.rom',
    ram: '8 MB',
    hd: 'apps/marathon_8_24gc.img',
    settle: 'marathon',
    vrom: 'roms/824gc-v1.1-revb-d722b053.vrom',
    card: '824gc',
    monitor: '13in_rgb',
    mode: '640x480x8',
  },
  pm6100: {
    rom: 'pm6100-pm7100-pm8100-9feb69b3.rom',
    ram: '24 MB',
    settle: 'instr',
    instr: 200_000_000,
  },
  pm8500: {
    rom: 'pm7500-pm8500-pm9500-96cd923d.rom',
    ram: '32 MB',
    hd: 'systems/system_7_6_170mb_24ac.img',
    settle: 'instr',
    instr: 4_000_000_000,
  },
};

// --- terminal probes ----------------------------------------------------------

let probeSeq = 0;

// {instr_count, host_wall_ns} sampled in ONE expansion inside the worker.
async function probeSample(page: Page): Promise<{ instr: number; wallNs: number }> {
  for (let attempt = 0; attempt < 30; attempt++) {
    const key = `pb${++probeSeq}`;
    await terminalRun(page, `echo "${key}=\${machine.cpu.instr_count},\${scheduler.host_wall_ns}"`);
    await page.waitForTimeout(400);
    const text = await page.locator(CONSOLE_OUTPUT).innerText();
    const m = text.match(new RegExp(`${key}=(\\d+),(\\d+)`));
    if (m) return { instr: Number(m[1]), wallNs: Number(m[2]) };
  }
  throw new Error('terminal sample probe never answered');
}

async function probeString(page: Page, expr: string): Promise<string> {
  for (let attempt = 0; attempt < 30; attempt++) {
    const key = `ps${++probeSeq}`;
    await terminalRun(page, `echo "${key}=[${'$'}{${expr}}]"`);
    await page.waitForTimeout(400);
    const text = await page.locator(CONSOLE_OUTPUT).innerText();
    const m = text.match(new RegExp(`${key}=\\[([A-Za-z0-9_.-]+)\\]`));
    if (m) return m[1];
  }
  throw new Error('terminal string probe never answered');
}

async function instrCount(page: Page): Promise<number> {
  return (await probeSample(page)).instr;
}

// --- reaching the state ------------------------------------------------------------

// The Finder is the application, then 150 M more instructions to go idle.
async function settleFinder(page: Page): Promise<void> {
  await expect
    .poll(() => probeString(page, 'debug.mac.globals.read("ApplZone") != debug.mac.globals.read("SysZone")'), {
      timeout: 600_000,
      intervals: [5_000],
    })
    .toMatch(/^(true|1)$/);
  const at = await instrCount(page);
  await expect
    .poll(() => instrCount(page), { timeout: 240_000, intervals: [3_000] })
    .toBeGreaterThan(at + 150_000_000);
}

// The native iicx-marathon row's launch choreography, typed into the shell.
async function launchMarathon(page: Page): Promise<void> {
  await expect
    .poll(() => instrCount(page), { timeout: 240_000, intervals: [3_000] })
    .toBeGreaterThan(230_000_000);
  await terminalRun(page, 'machine.adb.mouse.move 237 157 "global"');
  await page.waitForTimeout(500);
  for (let i = 0; i < 2; i++) {
    await terminalRun(page, 'machine.adb.mouse.click true');
    await terminalRun(page, 'machine.adb.mouse.click false');
  }
  const after = await instrCount(page);
  await expect
    .poll(() => instrCount(page), { timeout: 240_000, intervals: [3_000] })
    .toBeGreaterThan(after + 120_000_000);
}

// Load web2 at `url`, stage the row's media, configure and start the machine,
// switch to turbo and run it to the row's state.
async function bootRow(page: Page, url: string, machine: string, row: Row): Promise<void> {
  await page.goto(url);
  await page.waitForFunction(
    () => {
      const w = window as { __gsReady?: boolean; __gsBootError?: string };
      return w.__gsReady === true || typeof w.__gsBootError === 'string';
    },
    undefined,
    { timeout: 60_000 },
  );
  const bootError = await page.evaluate(() => (window as { __gsBootError?: string }).__gsBootError);
  if (bootError) throw new Error(`emulator did not start at ${url}: ${bootError}`);
  const cont = page.getByRole('button', { name: 'Continue' });
  if (await cont.isVisible().catch(() => false)) await cont.click();

  if (row.hd)
    await stageOpfsFileStreaming(page, `/opfs/images/hd/${path.basename(row.hd)}`, path.join(DATA, row.hd));
  if (row.vrom)
    await stageOpfsFile(page, `/opfs/images/vrom/${path.basename(row.vrom)}`, path.join(DATA, row.vrom));

  const [romChooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Load ROM...' }).click(),
  ]);
  await romChooser.setFiles(path.join(DATA, 'roms', row.rom));

  await page.getByRole('button', { name: 'New Machine...' }).click();
  const model = page.locator('#cfg-model');
  await expect(model.locator(`option[value="${machine}"]`)).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption(machine);
  await page.locator('#cfg-opt-memory').selectOption({ label: row.ram });
  if (row.card) {
    await page.getByTestId('cfg-add-card').click({ timeout: 30_000 });
    await page.locator('#cfg-add-card').selectOption(row.card);
    const slot = await page.locator('#cfg-add-card-slot').inputValue();
    await page.getByRole('button', { name: 'Add', exact: true }).click();
    await page.locator('#cfg-display').selectOption(slot);
    if (row.monitor) await page.locator('#cfg-monitor').selectOption(row.monitor);
    if (row.mode) await page.locator('#cfg-video-mode').selectOption(row.mode);
  }
  if (row.hd) await page.locator('#cfg-media-scsi-0').selectOption({ label: path.basename(row.hd) });
  await page.getByRole('button', { name: 'Start', exact: true }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({ timeout: 60_000 });

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.console')).toBeVisible({ timeout: 15_000 });
  await terminalRun(page, 'scheduler.mode = "turbo"');
  expect(await probeString(page, 'scheduler.mode')).toBe('turbo');
  if (row.settle === 'finder') await settleFinder(page);
  else if (row.settle === 'marathon') await launchMarathon(page);
  else
    await expect
      .poll(() => instrCount(page), { timeout: 1_200_000, intervals: [5_000] })
      .toBeGreaterThan(row.instr ?? 0);
}

// One measured window on the page in front, its machine running.
async function measureOnce(page: Page): Promise<number> {
  await page.bringToFront();
  await terminalRun(page, 'scheduler.run');
  await page.waitForTimeout(2_000);
  const a = await probeSample(page);
  await page.waitForTimeout(WINDOW_SECS * 1000);
  const b = await probeSample(page);
  await terminalRun(page, 'scheduler.stop');
  return (b.instr - a.instr) / ((b.wallNs - a.wallNs) / 1e9) / 1e6;
}

// --- the calibration kernels, ported from src/platform/headless/calibrate.c ---
// Same kernels and seeds (version 1); a JS number is not a uint64, so the LCG
// keeps its 64-bit state in a BigInt and the integer chains use 32-bit math.
// Comparable across browser runs, not with the native score.
async function calibrateInPage(page: Page): Promise<{ alu: number; chase: number; interp: number }> {
  return page.evaluate(() => {
    let lcg = 0n;
    const next = () => {
      lcg = (lcg * 6364136223846793005n + 1442695040888963407n) & 0xffffffffffffffffn;
      return Number(lcg >> 32n) >>> 0;
    };
    const time = (fn: (n: number) => void) => {
      let n = 1 << 16;
      let t = 0;
      for (;;) {
        const t0 = performance.now();
        fn(n);
        t = performance.now() - t0;
        if (t >= 10) break;
        n *= 4;
      }
      n = Math.ceil((n * 170) / t);
      let best = Infinity;
      for (let i = 0; i < 3; i++) {
        const t0 = performance.now();
        fn(n);
        best = Math.min(best, ((performance.now() - t0) * 1e6) / n);
      }
      return best;
    };
    let sink = 0;
    const alu = time((n) => {
      let a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
      for (let i = 0; i < n; i++) {
        a = ((a + i) ^ (a << 3)) | 0;
        b = ((b + i) ^ (b >>> 5)) | 0;
        c = ((c + i) ^ (c << 7)) | 0;
        d = ((d + i) ^ (d >>> 11)) | 0;
        e = ((e + i) ^ (e << 13)) | 0;
        f = ((f + i) ^ (f >>> 17)) | 0;
        g = ((g + i) ^ (g << 19)) | 0;
        h = ((h + i) ^ (h >>> 23)) | 0;
      }
      sink ^= a + b + c + d + e + f + g + h;
    });
    const N = 1 << 18;
    const nxt = new Uint32Array(N);
    for (let i = 0; i < N; i++) nxt[i] = i;
    lcg = 0x6368617365n;
    for (let i = N - 1; i > 0; i--) {
      const j = next() % i;
      const t = nxt[i];
      nxt[i] = nxt[j];
      nxt[j] = t;
    }
    const chase = time((n) => {
      let p = 0;
      for (let i = 0; i < n; i++) p = nxt[p];
      sink ^= p;
    });
    const P = 1 << 16;
    const prog = new Uint8Array(P);
    lcg = 0x696e74657270n;
    for (let i = 0; i < P; i++) prog[i] = next() >>> 26;
    const r = new Uint32Array(16);
    const mem = new Uint32Array(1024);
    let pc = 0;
    let flag = 0;
    const interp = time((n) => {
      for (let i = 0; i < n; i++) {
        const op = prog[pc & (P - 1)];
        pc = (pc + 1) >>> 0;
        const ra = (op + pc) & 15;
        const rb = (op * 7 + pc) & 15;
        const A = r[ra];
        const B = r[rb];
        switch (op) {
          case 0: r[ra] = A + B; break;
          case 1: r[ra] = A - B; break;
          case 2: r[ra] = A ^ B; break;
          case 3: r[ra] = A & (B | 1); break;
          case 4: r[ra] = A | B; break;
          case 5: r[ra] = A << (B & 7); break;
          case 6: r[ra] = A >>> (B & 7); break;
          case 7: r[ra] = Math.imul(B, 2654435761 | 0); break;
          case 8: r[ra] = mem[B & 1023]; break;
          case 9: mem[B & 1023] = A; break;
          case 10: r[ra] = A + mem[pc & 1023]; break;
          case 11: mem[pc & 1023] ^= A; break;
          case 12: flag = A < B ? 1 : 0; break;
          case 13: flag = A === B ? 1 : 0; break;
          case 14: if (flag) pc += 3; break;
          case 15: if (!flag) pc += 1; break;
          case 16: r[ra] = ~A; break;
          case 17: r[ra] = -B; break;
          case 18: r[ra] = A + 1; break;
          case 19: r[ra] = A - 1; break;
          case 20: r[ra] = (A << 8) | (A >>> 24); break;
          case 21: r[ra] = (A >>> 8) | (A << 24); break;
          case 22: r[ra] = B & 0xffff; break;
          case 23: r[ra] = (B << 16) >> 16; break;
          case 24: r[ra] = (B << 24) >> 24; break;
          case 25: r[ra] = B >>> 16; break;
          case 26: flag = A >>> 31; break;
          case 27: flag = A & 1; break;
          case 28: r[ra] = flag ? B : A; break;
          case 29: r[ra] = A + flag; break;
          case 30: mem[(A + 1) & 1023] = B; break;
          case 31: r[ra] = mem[(A + 1) & 1023]; break;
          case 32: r[ra] = A + B + flag; break;
          case 33: r[ra] = A - B - flag; break;
          case 34: r[ra] = B; r[rb] = A; break;
          case 35: r[ra] = B & 0xff; break;
          case 36: r[ra] = B & 0xffff; break;
          case 37: r[ra] = A ^ 0x5a5a5a5a; break;
          case 38: r[ra] = A + 0x01010101; break;
          case 39: r[ra] = (A & 0xffff0000) | (B & 0xffff); break;
          case 40: flag = A > B ? 1 : 0; break;
          case 41: flag = A !== 0 ? 1 : 0; break;
          case 42: if (flag) pc -= 2; break;
          case 43: r[ra] = Math.imul(A, 5) >>> 1; break;
          case 44: r[ra] = Math.floor(B / ((A & 7) + 1)); break;
          case 45: r[ra] = B % ((A & 15) + 1); break;
          case 46: mem[(pc + A) & 1023] += 1; break;
          case 47: r[ra] = mem[(pc + B) & 1023] ^ A; break;
          case 48: r[ra] = (A >>> 1) | (flag << 31); break;
          case 49: r[ra] = (A << 1) | flag; break;
          case 50: { let v = A, c = 0; while (v) { v &= v - 1; c++; } flag = c & 1; } break;
          case 51: r[ra] = Math.clz32(A); break;
          case 52: r[ra] = B + 4; break;
          case 53: r[ra] = B - 4; break;
          case 54: r[ra] = A & ~B; break;
          case 55: r[ra] = A | 0x80000000; break;
          case 56: r[ra] = A & 0x7fffffff; break;
          case 57: r[ra] = (A ^ B) >>> 3; break;
          case 58: r[ra] = A + (B << 2); break;
          case 59: r[ra] = A + (B >>> 2); break;
          case 60: flag = (A & B) === 0 ? 1 : 0; break;
          case 61: pc += A & 3; break;
          case 62: r[ra] = pc; break;
          default: r[ra] = Math.imul(A, 3) + B; break;
        }
        pc >>>= 0;
      }
      sink ^= r[0];
    });
    (window as { __calibSink?: number }).__calibSink = sink;
    return { alu, chase, interp };
  });
}

// --- the test --------------------------------------------------------------------

let baseServer: ChildProcess | undefined;

test.beforeAll(async () => {
  if (!BASE_DIST) return;
  baseServer = spawn('python3', [path.join(__dirname, '..', 'test_server.py'), '--root', BASE_DIST, '--port', String(BASE_PORT)], {
    stdio: 'ignore',
  });
  // Up when it answers.
  for (let i = 0; i < 50; i++) {
    try {
      const r = await fetch(BASE_URL);
      if (r.ok) return;
    } catch {
      // not yet
    }
    await new Promise((res) => setTimeout(res, 200));
  }
  throw new Error(`the base build's server did not start on ${BASE_PORT}`);
});

test.afterAll(() => {
  baseServer?.kill();
});

for (const machine of MACHINES) {
  test(`perf-headbase: ${machine} head vs base (turbo, report only)`, async ({ browser }) => {
    test.skip(!BASE_DIST || !fs.existsSync(path.join(BASE_DIST, 'index.html')), 'PERF_BASE_DIST names no base build');
    test.setTimeout(30 * 60 * 1000);
    const row = ROWS[machine];
    expect(row, `unknown machine ${machine}`).toBeTruthy();

    const context: BrowserContext = await browser.newContext();
    const pages: Record<'head' | 'base', Page> = {
      head: await context.newPage(),
      base: await context.newPage(),
    };
    // One machine runs at a time: boot head, stop it, boot base, stop it.
    for (const side of ['head', 'base'] as const) {
      await pages[side].bringToFront();
      await bootRow(pages[side], side === 'head' ? HEAD_URL : BASE_URL, machine, row);
      await terminalRun(pages[side], 'scheduler.stop');
    }

    const results: Record<'head' | 'base', number[]> = { head: [], base: [] };
    for (let pair = 1; pair <= PAIRS; pair++) {
      // Alternate which side goes first, so slow drift hits both alike.
      const order = pair % 2 ? (['head', 'base'] as const) : (['base', 'head'] as const);
      for (const side of order) {
        const mips = await measureOnce(pages[side]);
        results[side].push(mips);
        console.log(`PERFBENCH label=${LABEL} machine=${machine} binary=${side} pair=${pair} mips=${mips.toFixed(2)}`);
      }
    }
    const median = (v: number[]) => [...v].sort((x, y) => x - y)[Math.floor(v.length / 2)];
    for (const side of ['head', 'base'] as const) {
      const pd = (attr: string) => probeString(pages[side], `predecode.${attr}`);
      console.log(
        `PERFBENCH label=${LABEL} machine=${machine} binary=${side} predecode=${await pd('enabled')} ` +
          `lookups=${await pd('lookups')} generic=${await pd('generic_steps')} instr=${await instrCount(pages[side])}`,
      );
    }
    const h = median(results.head);
    const b = median(results.base);
    console.log(
      `PERFBENCH label=${LABEL} machine=${machine} median head=${h.toFixed(2)} base=${b.toFixed(2)} ratio=${(h / b).toFixed(3)}`,
    );
    // The calibration kernels last, in a page of their own: run in the head
    // page beforehand they left that page measurably slower than base's.
    const calibPage = await context.newPage();
    await calibPage.goto('about:blank');
    const calib = await calibrateInPage(calibPage);
    console.log(
      `PERFBENCH label=${LABEL} calib alu_ns=${calib.alu.toFixed(3)} chase_ns=${calib.chase.toFixed(3)} interp_ns=${calib.interp.toFixed(3)}`,
    );
    await context.close();
    // Sanity only.
    expect(Math.max(...results.head)).toBeGreaterThan(0.5);
    expect(Math.max(...results.base)).toBeGreaterThan(0.5);
  });
}
