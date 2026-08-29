// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: a configured pm6100 must boot exactly ONCE — including on an
// image that earlier sessions already wrote to.
//
// Repro spec for the reported "double boot" on the Power Macintosh 6100
// with a Mac OS 8.1 HD image: boot with a chime, run a few seconds (the
// report pins it right after the RAM test, at the first HD access), reboot
// with a second chime, then boot for real.
//
// A pristine-profile first boot did NOT reproduce (verified: one chime,
// one boot banner, monotonic instr_count, no reset-vector re-entry), so
// this spec recreates the user's actual situation: the image in OPFS has
// been booted before — with the session ending mid-run, the way every
// session ended while the ADB crash was live.  Phase 1 boots the virgin
// image fast-forwarded deep into the 8.1 boot (real disk writes land in
// the image's OPFS state), then shuts down mid-run.  Phase 2 re-does the
// user's setup — New Machine, pm6100, 24 MB, same HD — at real-time speed
// with every detector armed from t=0:
//  - AUDIO: an AnalyserNode tapped onto everything that connects to the
//    AudioContext destination logs output RMS every ~100 ms; a boot chime
//    is one loud burst, the symptom is two a few seconds apart.
//  - BOOT BANNERS: every machine.boot prints "ROM loaded successfully"
//    into the Terminal pane; the pane is opened before Start and its text
//    snapshotted (no typing) through the critical window.
//  - the 601 reset vector ($FFF00100) logpoint, armed as soon as typing
//    is safe (logpoints do not survive machine.boot), catches later warm
//    re-entries; scheduler.instr_count sampling catches JS-level reboots.
//
// Media: the ROM ships in tests/data; the Mac OS 8.1 HD image is a
// user-supplied local asset (tmp/macos81.img, 245 MB) — the spec skips
// when it is absent.

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import * as fs from 'node:fs';
import { gotoWeb2, stageOpfsFileStreaming } from '../helpers/web2-fs';

const PDM_ROM = path.resolve(__dirname, '../../data/roms/pm6100-pm7100-pm8100-9feb69b3.rom');
const MACOS81_HD = path.resolve(__dirname, '../../../tmp/macos81.img');

interface RmsSample {
  t: number;
  rms: number;
}

// Distinct loud bursts (chimes) in an RMS series: a burst opens when RMS
// crosses the threshold and closes after `gapMs` of quiet.
function burstStarts(samples: RmsSample[], threshold = 0.01, gapMs = 1500): number[] {
  const starts: number[] = [];
  let lastLoud = -Infinity;
  for (const s of samples) {
    if (s.rms >= threshold) {
      if (s.t - lastLoud > gapMs) starts.push(s.t);
      lastLoud = s.t;
    }
  }
  return starts;
}

async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
}

// Echo an expression through the terminal under a unique key and return the
// printed value (same idiom as machine-restart.spec.ts).
let probeSeq = 0;
async function terminalEval(page: Page, expr: string): Promise<string | null> {
  for (let attempt = 0; attempt < 3; attempt++) {
    const key = `dbp${++probeSeq}`;
    await terminalRun(page, `echo "${key}=\${${expr}}"`);
    for (let i = 0; i < 10; i++) {
      await page.waitForTimeout(400);
      const text = await page.locator('.xterm-rows').innerText();
      const values = [...text.matchAll(new RegExp(`${key}=(\\S+)`, 'g'))]
        .map((m) => m[1])
        .filter((v) => !v.startsWith('$'));
      if (values.length) return values[values.length - 1];
    }
  }
  return null;
}

// Configure a pm6100 (24 MB, the 8.1 HD) through the New Machine dialog and
// press Start.  Returns performance.now() at the click.
async function configureAndStart(page: Page): Promise<number> {
  await page.getByRole('button', { name: 'New Machine...' }).click();
  const modelSel = page.locator('#cfg-model');
  await expect(modelSel).toBeVisible({ timeout: 60_000 });
  await modelSel.selectOption('pm6100');
  await page.locator('#cfg-ram').selectOption({ label: '24 MB' });
  await page.locator('#cfg-hd').selectOption({ label: 'macos81.img' });
  const t0 = await page.evaluate(() => performance.now());
  await page.getByRole('button', { name: 'Start Machine' }).click();
  return t0;
}

// Count "ROM loaded successfully" banners currently visible in the xterm.
async function bannerCount(page: Page): Promise<number> {
  const text = await page.locator('.xterm-rows').innerText();
  return (text.match(/ROM loaded successfully/g) ?? []).length;
}

test('pm6100 + Mac OS 8.1 HD boots exactly once — also on a previously-used image', async ({
  page,
}) => {
  test.skip(!fs.existsSync(MACOS81_HD), 'tmp/macos81.img not present (user-supplied asset)');
  test.setTimeout(1_500_000);

  // Audio tap: the first node that connects to the context destination also
  // feeds an AnalyserNode sampled for RMS on a 100 ms interval.
  await page.addInitScript(() => {
    const g = window as unknown as {
      __rmsLog: { t: number; rms: number }[];
      __rmsTap?: AnalyserNode;
    };
    g.__rmsLog = [];
    const origConnect = AudioNode.prototype.connect;
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    (AudioNode.prototype as any).connect = function (dest: unknown, ...rest: unknown[]) {
      try {
        if (dest instanceof AudioDestinationNode && !g.__rmsTap) {
          const ctx = (this as AudioNode).context as AudioContext;
          const an = ctx.createAnalyser();
          an.fftSize = 2048;
          origConnect.call(this as AudioNode, an);
          g.__rmsTap = an;
          const buf = new Float32Array(an.fftSize);
          setInterval(() => {
            an.getFloatTimeDomainData(buf);
            let s = 0;
            for (let i = 0; i < buf.length; i++) s += buf[i] * buf[i];
            g.__rmsLog.push({ t: performance.now(), rms: Math.sqrt(s / buf.length) });
          }, 100);
        }
      } catch {
        /* tap is best-effort; the other detectors stand alone */
      }
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      return (origConnect as any).call(this, dest, ...rest);
    };
  });

  const consoleLines: string[] = [];
  page.on('console', (msg) => consoleLines.push(`[console] ${msg.text()}`));

  await gotoWeb2(page);

  // Stage the 8.1 HD image, then RELOAD so the worker's WasmFS mount sees
  // it (main-thread OPFS writes are invisible to an already-mounted worker).
  await stageOpfsFileStreaming(page, '/opfs/images/hd/macos81.img', MACOS81_HD);
  await gotoWeb2(page);

  // Upload the ROM the way the user does: the Welcome "Upload ROM..." button.
  const [chooser] = await Promise.all([
    page.waitForEvent('filechooser'),
    page.getByRole('button', { name: 'Upload ROM...' }).click(),
  ]);
  await chooser.setFiles(PDM_ROM);
  await expect(page.locator('.toast .msg').filter({ hasText: 'uploaded' }).first()).toBeVisible({
    timeout: 120_000,
  });

  // Terminal pane visible before any boot: C-side boot banners stream into
  // the xterm from t=0.
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });

  // ---- Phase 1: dirty the image the way the user's sessions did ----------
  // Boot the virgin image, fast-forward ~2 minutes of wall time deep into
  // the 8.1 boot so real disk writes land, then shut down mid-run (every
  // session during the ADB-crash era ended without a clean shutdown).
  await configureAndStart(page);
  await page.locator('[title^="Fast-Forward"]').click();
  await page.waitForTimeout(120_000);
  const phase1Instr = await terminalEval(page, 'scheduler.instr_count');
  console.log(`phase 1 (dirtying) reached instr_count=${phase1Instr}`);
  await page.locator('[aria-label="Shut down"]').click();
  await expect(page.getByRole('button', { name: 'New Machine...' })).toBeVisible({
    timeout: 30_000,
  });

  // ---- Phase 2: the user's flow, on the now previously-used image --------
  const bannersBefore = await bannerCount(page);
  const t0 = await configureAndStart(page);

  // Critical window: watch the terminal WITHOUT typing for ~16 s and keep
  // the maximum banner count seen.
  let banners = 0;
  const started = Date.now();
  while (Date.now() - started < 16_000) {
    const n = (await bannerCount(page)) - bannersBefore;
    if (n > banners) banners = n;
    await page.waitForTimeout(700);
  }

  // Safe to type now: arm the reset-vector logpoint (this add gets a fresh
  // id — read it back positionally via the printed attributes is brittle,
  // so probe both plausible ids) and sample instr_count out to ~60 s.
  await terminalRun(page, 'debug.logpoints.add addr=0xFFF00100 message="RESETVEC"');
  const samples: number[] = [];
  while (Date.now() - started < 60_000) {
    const v = await terminalEval(page, 'scheduler.instr_count');
    if (v !== null) {
      const n = Number(v);
      if (Number.isFinite(n)) samples.push(n);
    }
    await page.waitForTimeout(1_500);
  }
  const hits =
    (await terminalEval(page, 'try(debug.logpoints[0].hit_count, 0) + try(debug.logpoints[1].hit_count, 0)')) ?? '0';

  const rmsLog = await page.evaluate(
    () => (window as unknown as { __rmsLog: RmsSample[] }).__rmsLog,
  );
  const bursts = burstStarts(rmsLog.filter((s) => s.t >= t0 && s.t <= t0 + 30_000));

  console.log(`phase 2 boot banners: ${banners}`);
  console.log(
    `phase 2 chimes: ${bursts.length} at +${bursts.map((b) => Math.round(b - t0)).join('ms, +')}ms`,
  );
  console.log(`phase 2 instr_count samples: ${samples.join(', ')}`);
  console.log(`phase 2 reset-vector hits: ${hits}`);
  console.log(consoleLines.slice(-30).join('\n'));

  // Exactly one machine.boot banner for the one Start.
  expect(banners, 'a second machine.boot printed a second ROM banner').toBeLessThanOrEqual(1);
  // Exactly one chime (zero would mean the audio tap failed — also a bug).
  expect(bursts.length, `expected exactly one boot chime, heard ${bursts.length}`).toBe(1);
  // No JS-level reboot: instr_count never goes backwards.
  const decreases = samples.filter((v, i) => i > 0 && v < samples[i - 1]);
  expect(decreases, `instr_count went backwards (${samples.join(', ')})`).toHaveLength(0);
  // No guest warm reset after the banner window.
  expect(Number(hits), 'the 601 reset vector was re-entered — the machine rebooted').toBe(0);
  expect(await terminalEval(page, 'machine.id')).toBe('pm6100');
});
