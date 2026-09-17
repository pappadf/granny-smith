// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: does GUI-mode Setup draw in the BROWSER?
//
// Headless, tmp/nt-textmode-installed.img boots into Windows NT Setup's wizard
// and draws it in full -- 1,782 BitBLT operations.  The same disk, the same
// floppy and the same emulator source, run in the web UI, was reported
// stopping at wall E28: a black screen carrying one grey "< Back" button.
// That appearance has a known cause in this project -- src/pci.c's comment for
// VGA_APERTURE_MIRROR records GUI Setup coming up as "a black void with one
// button" when the moved VGA window resolved to raw VRAM instead of the
// decoded legacy-window mirror -- but nothing in the served binary explains
// why the browser would differ: the wasm carries the engine's strings and its
// build banner is current.
//
// So this drives the browser through exactly what the user does by hand -- the
// New Machine dialog, then the two Open Firmware lines typed on the emulated
// ADB keyboard at the on-screen prompt -- and films the canvas.  Two runs
// differing only in where the emulator runs is the one comparison that can
// place the fault.
//
// It is a REPRODUCTION, not an assertion of correctness: while the bug is live
// it is expected to fail, and its job is to say WHERE it stopped.  Frames land
// in tmp/repro-web/ whatever the outcome.

import { test, expect, type Page } from '@playwright/test';
import * as path from 'node:path';
import * as fs from 'node:fs';
import { gotoWeb2 } from '../helpers/web2-fs';

const REPO = path.resolve(__dirname, '../../..');
const ANS = path.join(REPO, 'local/gs-docs/projects/windows-nt-ppc-ans');
const ROM = path.join(ANS, 'roms/ans-2.26NT-ad405e01.rom');
const FLOPPY = path.join(REPO, 'tmp/boot-floppy.img');
const DISK = process.env.REPRO_DISK ?? path.join(REPO, 'tmp/nt-textmode-installed.img');
const OUT = path.join(REPO, 'tmp/repro-web');

// The config's own args, plus unlimited storage: the default profile grants an
// origin 2 GB, and a 512 MB image costs that twice over (the writable's swap
// copy, then the file) before the HTTP cache is counted.
test.use({
  launchOptions: {
    args: [
      '--use-gl=angle',
      '--use-angle=swiftshader-webgl',
      '--ignore-gpu-blocklist',
      '--disable-dev-shm-usage',
      '--unlimited-storage',
    ],
  },
});

// Stage a host file into OPFS, sparsely, through a sync access handle.
//
// Getting a 512 MB image into this container's browser took four tries, and
// the constraint turned out to be neither quota nor the JS heap:
//
//   * helpers/web2-fs's base64 stagers take the tab out ("Target crashed").
//   * fetch().body.pipeTo(writable) deadlocks, renderer idle, no error.
//   * The UI's own upload validates by media type, and our FAT12 PC boot
//     floppy is not a valid Mac floppy, so the dropdown reverts to "(none)".
//   * Any single createWritable() session dies at about 400 MB with
//     "Failed to write data to data pipe" AND DISCARDS THE WHOLE FILE; a
//     sync access handle instead SHORT-WRITES silently, landing 442 MB of a
//     512 MB image.  Quota is not the limit -- it grows with usage, and was
//     still 2 GB clear when both failed.
//
// So only the bytes that are not zero are written.  The installed disk holds
// its data in the first 120 MB (88 non-zero MB of 512), setSize() gives the
// file its real length, and the result is byte-identical to the host image
// while costing well under the ceiling.  Every write's return value is
// checked, because a short write is how this fails quietly.
async function stage(page: Page, opfsPath: string, hostFile: string): Promise<void> {
  const BLOCK = 1024 * 1024;
  const size = fs.statSync(hostFile).size;
  const fd = fs.openSync(hostFile, 'r');
  try {
    await page.evaluate(
      async ({ p, size }: { p: string; size: number }) => {
        const src = `
          let h = null;
          self.onmessage = async (e) => {
            const m = e.data;
            try {
              if (m.op === 'open') {
                const rel = m.path.replace(/^\\/opfs\\/?/, '').split('/').filter(Boolean);
                const name = rel.pop();
                let dir = await navigator.storage.getDirectory();
                for (const part of rel) dir = await dir.getDirectoryHandle(part, { create: true });
                const fh = await dir.getFileHandle(name, { create: true });
                h = await fh.createSyncAccessHandle();
                h.truncate(0);
                self.postMessage({ ok: true });
              } else if (m.op === 'write') {
                const bytes = new Uint8Array(m.buf);
                const n = h.write(bytes, { at: m.at });
                self.postMessage({ ok: n === bytes.length, wrote: n, want: bytes.length });
              } else if (m.op === 'close') {
                // Extend to the real length by writing the last byte, not with
                // truncate(): a grow-truncate is ignored here, before the writes
                // or after them -- the file came back 120 MB either way, the end
                // of the last block that carried data.
                if (h.getSize() < m.size) h.write(new Uint8Array(1), { at: m.size - 1 });
                h.flush();
                const n = h.getSize();
                h.close();
                h = null;
                self.postMessage({ ok: true, size: n });
              }
            } catch (err) {
              self.postMessage({ ok: false, err: String(err) });
            }
          };
        `;
        const w = new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })));
        const g = window as unknown as {
          __call?: (m: unknown, t?: Transferable[]) => Promise<Record<string, unknown>>;
          __worker?: Worker;
        };
        g.__worker = w;
        // A worker that fails to load fires onerror, never onmessage: without
        // this the whole run hangs to the test timeout with nothing to show.
        g.__call = (m: unknown, t: Transferable[] = []) =>
          new Promise((res, rej) => {
            const timer = setTimeout(() => rej(new Error('stage worker silent for 30s')), 30_000);
            w.onerror = (ev) => {
              clearTimeout(timer);
              rej(new Error(`stage worker failed to load: ${ev.message}`));
            };
            w.onmessage = (ev) => {
              clearTimeout(timer);
              res(ev.data);
            };
            w.postMessage(m, t);
          });
        const r = await g.__call({ op: 'open', path: p, size });
        if (!r.ok) throw new Error(`open ${p}: ${r.err}`);
      },
      { p: opfsPath, size },
    );

    const buf = Buffer.alloc(BLOCK);
    const zero = Buffer.alloc(BLOCK);
    let written = 0;
    for (let at = 0; at < size; at += BLOCK) {
      const n = fs.readSync(fd, buf, 0, Math.min(BLOCK, size - at), at);
      if (buf.subarray(0, n).equals(zero.subarray(0, n))) continue; // already zero in OPFS
      written += n;
      const r = await page.evaluate(
        async ({ data, at }: { data: string; at: number }) => {
          const g = window as unknown as {
            __call: (m: unknown, t?: Transferable[]) => Promise<Record<string, unknown>>;
          };
          const bin = atob(data);
          const bytes = new Uint8Array(bin.length);
          for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
          return g.__call({ op: 'write', buf: bytes.buffer, at }, [bytes.buffer]);
        },
        { data: buf.subarray(0, n).toString('base64'), at },
      );
      if (!r.ok) throw new Error(`${opfsPath} at ${at}: wrote ${r.wrote}/${r.want} ${r.err ?? ''}`);
    }

    const done = await page.evaluate(async (size: number) => {
      const g = window as unknown as {
        __call: (m: unknown) => Promise<Record<string, unknown>>;
        __worker?: Worker;
      };
      const r = await g.__call({ op: 'close', size });
      g.__worker?.terminate();
      return r;
    }, size);
    if (!done.ok || done.size !== size)
      throw new Error(`${opfsPath}: staged ${done.size} bytes, want ${size} ${done.err ?? ''}`);
    console.log(`staged ${opfsPath}: ${size} bytes (${(written / 1048576) | 0} MB non-zero)`);
  } finally {
    fs.closeSync(fd);
  }
}

// A numbered filmstrip of the canvas, so a failure anywhere still shows how far
// the boot got.
let frameNo = 0;
async function frame(page: Page, tag: string): Promise<void> {
  const name = `${String(frameNo++).padStart(2, '0')}-${tag}.png`;
  await page.locator('#screen').screenshot({ path: path.join(OUT, name) }).catch(() => {});
}

// How much of the canvas is lit, as a fraction. Setup's wizard covers the
// screen in blue; wall E28 is near-black with one grey button. This is the one
// measurement that separates them without a baseline image, and it is also how
// the boot is followed: a screen that stops changing has arrived somewhere.
//
// Measured on Playwright's screenshot of the element, not by drawImage from the
// canvas itself: #screen has had its control transferred to an OffscreenCanvas
// on the emulator's pthread (PROXY_TO_PTHREAD), and reading a transferred
// canvas from the main thread does not return the rendered pixels.  The first
// version of this function did exactly that, reported 99.5% lit on a blank
// display, and passed a run that had not booted anything (17 September).
async function litFraction(page: Page): Promise<number> {
  const png = await page.locator('#screen').screenshot();
  const dataUrl = `data:image/png;base64,${png.toString('base64')}`;
  return page.evaluate(async (src: string) => {
    const img = new Image();
    await new Promise<void>((ok, bad) => {
      img.onload = () => ok();
      img.onerror = () => bad(new Error('screenshot decode failed'));
      img.src = src;
    });
    const g = document.createElement('canvas');
    g.width = img.naturalWidth;
    g.height = img.naturalHeight;
    const ctx = g.getContext('2d')!;
    ctx.drawImage(img, 0, 0);
    const d = ctx.getImageData(0, 0, g.width, g.height).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 96) n++;
    return n / (d.length / 4);
  }, dataUrl);
}

// Wait until the canvas has held still for `stable` consecutive samples, or
// until `budgetMs` runs out. Returns the last lit fraction. Polling beats a
// fixed sleep here: the machine reaches the Open Firmware prompt in about 20
// seconds, and nothing is served by waiting minutes for it.
async function settle(page: Page, budgetMs: number, stable = 3): Promise<number> {
  const deadline = Date.now() + budgetMs;
  let last = -1;
  let same = 0;
  while (Date.now() < deadline) {
    await page.waitForTimeout(2000);
    const lit = await litFraction(page);
    same = Math.abs(lit - last) < 0.0005 ? same + 1 : 0;
    last = lit;
    if (same >= stable) break;
  }
  return last;
}

// One shell line into the Terminal panel's xterm.
async function sh(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(200);
}

// Type at the on-screen Open Firmware prompt, on the emulated ADB keyboard --
// the path the user uses, and the one the ADB queue's depth of 16 makes worth
// feeding in small pieces rather than all at once.
async function ofType(page: Page, text: string): Promise<void> {
  for (let i = 0; i < text.length; i += 4) {
    const piece = text.slice(i, i + 4).replace(/\\/g, '\\\\').replace(/"/g, '\\"');
    await sh(page, `machine.adb.keyboard.type("${piece}")`);
  }
  await sh(page, 'machine.adb.keyboard.type("\\n")');
}

// The two lines every powermac-nt-hal boot script is run with.
async function ofRun(page: Page, script: string): Promise<void> {
  await ofType(page, `load fd:,\\${script}`);
  await page.waitForTimeout(2000);
  await ofType(page, 'load-base loadsize eval');
}

// Pick the option whose label matches, failing with the list when none does --
// the dialog's media dropdowns are keyed by file name, so a staging mistake
// otherwise surfaces much later as a machine that just sits there.
async function pick(page: Page, selector: string, want: RegExp): Promise<void> {
  const sel = page.locator(selector);
  await expect(sel).toBeVisible({ timeout: 15_000 });
  const labels = await sel.locator('option').allTextContents();
  const hit = labels.find((l) => want.test(l));
  if (!hit) throw new Error(`${selector}: no option matching ${want} in ${JSON.stringify(labels)}`);
  await sel.selectOption({ label: hit });
}

// Opt-in: this cannot run on a machine that cannot hold the image (see the
// staging comment), and while the bug is live it is meant to fail -- neither
// belongs in a suite run.  REPRO_E28=1 turns it on.
test.skip(!process.env.REPRO_E28, 'set REPRO_E28=1 to run the wall-E28 reproduction');

test('ANS 500: GUI-mode Setup draws in the browser', async ({ page }) => {
  test.setTimeout(45 * 60_000);
  fs.mkdirSync(OUT, { recursive: true });
  for (const f of [ROM, FLOPPY, DISK]) {
    if (!fs.existsSync(f)) throw new Error(`fixture missing: ${f}`);
  }

  // Anything the emulator prints, kept whole: the xterm pane shows only its
  // viewport, so the terminal cannot be read back as a log.
  const printed: string[] = [];
  page.on('console', (m) => printed.push(m.text()));
  page.on('pageerror', (e) => printed.push(`PAGEERROR ${e.message}`));

  await gotoWeb2(page);

  // Playwright's context is incognito-shaped, and an incognito origin gets a
  // small OPFS quota however much disk there is: staging this 512 MB image
  // stopped dead at exactly 120 MiB with 7.6 GB free, and --unlimited-storage
  // does not reach OPFS.  Raise it for this origin through CDP instead.
  {
    const origin = new URL(page.url()).origin;
    const cdp = await page.context().newCDPSession(page);
    await cdp.send('Storage.overrideQuotaForOrigin', { origin, quotaSize: 4 * 1024 * 1024 * 1024 });
    console.log(`quota raised to 4 GB for ${origin}`);
  }

  await stage(page, '/opfs/images/rom/ans-2.26NT-ad405e01.rom', ROM);
  await stage(page, '/opfs/images/fd/boot-floppy.img', FLOPPY);
  await stage(page, '/opfs/images/hd/nt-disk.img', DISK);

  // The dialog enumerates storage when the app loads, so everything staged
  // above arrived after it had already decided there were no ROMs ("No ROMs in
  // storage", the first run of this spec).  Reload so it looks again.
  await page.reload();
  await expect(page.getByRole('button', { name: 'New Machine...' })).toBeVisible({
    timeout: 30_000,
  });

  // New Machine, configured the way the wiki's install guide says.  No CD:
  // wall E28 is the wizard's first screen, which needs nothing off it.  The
  // bay is left at its default on purpose -- making that default right for the
  // ANS 500 was part of this work, and overriding it would hide a regression.
  await page.getByRole('button', { name: 'New Machine...' }).click();
  await page.locator('#cfg-model').selectOption('ans500');
  // The ROM dropdown appears only when several ROMs match the model
  // (needsRomPicker); with one ANS ROM in storage it is chosen for us.
  if (await page.locator('#cfg-rom').count()) await pick(page, '#cfg-rom', /ans-2\.26NT/);
  // 64 MB: setup.of puts load-base at 3E00000, which is 62 MB, so a smaller
  // machine leaves `load` writing into nothing.
  await pick(page, '#cfg-ram', /^64 MB$/);
  await pick(page, '#cfg-fd0', /boot-floppy\.img/);
  await pick(page, '#cfg-hd', /nt-disk\.img/);
  await page.getByRole('button', { name: 'Start Machine' }).click();
  await expect(page.locator('.toast .msg').filter({ hasText: 'Machine started' })).toBeVisible({
    timeout: 30_000,
  });

  // Unthrottled, and the Terminal panel open for the ADB typing below.
  await page.getByRole('button', { name: 'fast-forward', exact: true }).click();
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });

  // The card narrates itself at level 1: the SR17 line says whether the
  // memory-mapped BLT register block is decoded at $B8000, and every "BLT
  // start" is one accelerated operation.  Headless, this boot logs
  // "SR17 = $04" and 1,782 BLT starts; a browser that logs neither never
  // reached the engine, and one that logs them but draws the wrong screen has
  // an engine computing the wrong answer.  page.on('console') keeps them all.
  await sh(page, 'log 54m30 level=1');

  // Open Firmware's banner and its `0 >` prompt: about 20 seconds.
  await settle(page, 90_000);
  await frame(page, 'of-prompt');

  // Once per machine: little-endian mode into NVRAM, then reset-all.
  await ofRun(page, 'setup.of');
  await settle(page, 90_000);
  await frame(page, 'after-setup-reset');

  // Every boot: the installed system.
  await ofRun(page, 'bootdisk.of');

  // Follow it into the GUI phase, a frame every 15 seconds, stopping as soon
  // as the wizard is up or the screen has clearly come to rest.
  let lit = 0;
  for (let i = 0; i < 32; i++) {
    await page.waitForTimeout(15_000);
    lit = await litFraction(page);
    await frame(page, `gui-${String(i).padStart(2, '0')}-lit${Math.round(lit * 100)}`);
    if (lit > 0.5) break;
  }

  // The ground truth, independent of WebGL: headless draws this wizard with
  // checksum 1594464709 at 640x480x8.  A different number here means the
  // emulation diverged under wasm; the same number means the canvas is lying.
  for (const probe of [
    'machine.screen.checksum()',
    'machine.screen.width',
    'machine.screen.height',
    'machine.screen.depth',
    'machine.cpu.pc',
    'machine.pci.slot[7].card.config.bar[0].base',
  ]) {
    await sh(page, probe);
    await page.waitForTimeout(500);
  }
  await page.waitForTimeout(2000);

  fs.writeFileSync(path.join(OUT, 'printed.txt'), printed.join('\n'));
  fs.writeFileSync(path.join(OUT, 'lit.json'), JSON.stringify({ lit }, null, 2));
  console.log('canvas lit fraction:', lit);

  expect(lit, 'GUI-mode Setup should have drawn its wizard').toBeGreaterThan(0.5);
});
