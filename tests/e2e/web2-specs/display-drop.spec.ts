// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: Display drag-and-drop semantics — ROM auto-boot, floppy
// auto-mount, archive extraction, checkpoint restore, unknown-file
// rejection. Replaces the legacy tests/e2e/specs/drag-drop/ suite (retired
// with the legacy UI); DropOverlay.test.ts covers the overlay state machine
// at component level, but nothing else exercised the real
// processDataTransfer → probeAndPersist pipeline against the live worker.
//
// Only the drag GESTURE is synthetic (Playwright/CDP cannot drive native
// HTML5 drag-and-drop) — the DataTransfer carries a real File and the
// document-level DropOverlay handlers, bus/upload staging, C-side probes,
// persist and mount all run for real. Same concession as
// helpers/web2-fs.ts:treeDrag.

import { test, expect, type Page } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { gotoWeb2 } from '../helpers/web2-fs';

const DATA = path.resolve(__dirname, '../../data');
const PLUS_ROM = path.join(DATA, 'roms', 'plus-v3-4d1f8172.rom');
const SYSTEM_FD = path.join(DATA, 'systems', 'System_6_0_8.dsk');

// Dispatch dragenter/dragover/drop onto the Display area with a real File in
// the DataTransfer. Coordinates target the display's centre so the state
// machine routes Active → Display (isOverDisplay) rather than FsTree.
async function dropOnDisplay(page: Page, fileName: string, hostFile: string | Uint8Array) {
  const bytes = typeof hostFile === 'string' ? fs.readFileSync(hostFile) : hostFile;
  const b64 = Buffer.from(bytes).toString('base64');
  await page.evaluate(
    ({ name, data }: { name: string; data: string }) => {
      const el = document.querySelector('.gs-display-content, .screen-view');
      if (!el) throw new Error('display area not found');
      const r = el.getBoundingClientRect();
      const cx = r.x + r.width / 2;
      const cy = r.y + r.height / 2;
      const bin = atob(data);
      const buf = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);
      const file = new File([buf], name, { type: 'application/octet-stream' });
      const dt = new DataTransfer();
      dt.items.add(file);
      const fire = (type: string) =>
        el.dispatchEvent(
          new DragEvent(type, {
            bubbles: true,
            cancelable: true,
            dataTransfer: dt,
            clientX: cx,
            clientY: cy,
          }),
        );
      fire('dragenter');
      fire('dragover');
      fire('drop');
    },
    { name: fileName, data: b64 },
  );
}

function toast(page: Page, text: string | RegExp) {
  return page.locator('.toast .msg').filter({ hasText: text });
}

// Type one shell line into the Terminal panel's xterm.
async function terminalRun(page: Page, line: string): Promise<void> {
  const term = page.locator('.xterm');
  await term.click();
  await page.keyboard.type(line);
  await page.keyboard.press('Enter');
}

// Read machine.cpu.instr_count through the terminal with a unique key.
let probeSeq = 0;
async function readInstr(page: Page): Promise<number | null> {
  const key = `di${++probeSeq}`;
  await terminalRun(page, `echo "${key}=\${machine.cpu.instr_count}"`);
  await page.waitForTimeout(400);
  const text = await page.locator('.xterm-rows').innerText();
  const m = text.match(new RegExp(`${key}=(\\d+)`));
  return m ? Number(m[1]) : null;
}

test('drop workflow: ROM auto-boots, floppy auto-mounts, unknown file warns', async ({
  page,
}) => {
  test.setTimeout(240_000);
  await gotoWeb2(page);

  // 1. ROM onto the Welcome display → default machine boots straight away.
  await dropOnDisplay(page, 'plus-v3-4d1f8172.rom', PLUS_ROM);
  await expect(toast(page, 'Booted plus from uploaded ROM')).toBeVisible({ timeout: 60_000 });
  await expect(page.locator('.welcome-layer')).toHaveCount(0);
  await expect(page.locator('.gs-statusbar .sb-state .label')).toHaveText('Running');

  // 2. Floppy image → persisted and auto-inserted into the first empty drive.
  await dropOnDisplay(page, 'System_6_0_8.dsk', SYSTEM_FD);
  await expect(toast(page, 'Inserted into floppy drive 1')).toBeVisible({ timeout: 60_000 });

  // 3. A non-media file → rejected with the classifier warning, not mounted.
  //    (Archive drop-extraction is covered by the peeler integration test and
  //    the web2 filesystem-tab Unpack e2e; auto-mounting an extracted image
  //    into the 2nd drive races the still-booting first floppy, so it isn't
  //    re-tested here.)
  await dropOnDisplay(page, 'readme.txt', new Uint8Array(Buffer.from('just some notes\n')));
  await expect(toast(page, /doesn't look like a ROM, floppy, HD, CD, or archive/)).toBeVisible({
    timeout: 30_000,
  });
});

test('checkpoint drop restores the saved machine state', async ({ page }) => {
  test.setTimeout(240_000);
  await gotoWeb2(page);

  // Boot a machine via ROM drop, then pause it so the snapshot captures a
  // deterministic instruction count (a paused snapshot restores paused).
  await dropOnDisplay(page, 'plus-v3-4d1f8172.rom', PLUS_ROM);
  await expect(toast(page, 'Booted plus from uploaded ROM')).toBeVisible({ timeout: 60_000 });

  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator('.xterm')).toBeVisible({ timeout: 15_000 });
  await terminalRun(page, 'scheduler.stop');
  await page.waitForTimeout(500);
  const savedInstr = await readInstr(page);
  expect(savedInstr).not.toBeNull();

  // Save through the Checkpoints panel (checkpoint.snapshot completes
  // before its toast), then pull the state.checkpoint bytes back out of
  // OPFS — the shipped Download action is not implemented yet, and the
  // bytes are only a fixture for the drop below.
  await page.locator('button.ptab[data-tab="checkpoints"]').click();
  await page.getByRole('button', { name: 'Create Checkpoint' }).click();
  await expect(toast(page, /Checkpoint '.*' created/)).toBeVisible({ timeout: 30_000 });
  // Return the checkpoint as base64, not a number[] — a full-RAM snapshot
  // as a per-byte JS Array is ~8× bloat and OOMs the renderer in a
  // memory-constrained container.
  const ckptB64 = await page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    const dir = await root.getDirectoryHandle('checkpoints');
    for await (const [, machineDir] of dir as unknown as AsyncIterable<
      [string, FileSystemDirectoryHandle]
    >) {
      if (machineDir.kind !== 'directory') continue;
      try {
        const fh = await machineDir.getFileHandle('state.checkpoint');
        const f = await fh.getFile();
        const buf = new Uint8Array(await f.arrayBuffer());
        let s = '';
        for (let i = 0; i < buf.length; i++) s += String.fromCharCode(buf[i]);
        return btoa(s);
      } catch {
        /* not this dir */
      }
    }
    return null;
  });
  expect(ckptB64).not.toBeNull();
  const bytes = new Uint8Array(Buffer.from(ckptB64 as string, 'base64'));
  // The drop handler detects checkpoints by this signature.
  expect(String.fromCharCode(...bytes.slice(0, 7))).toBe('GSCHKPT');

  // Advance the machine past the saved state via the toolbar Run button
  // (resume free-run), let it run briefly, then stop and read — reading
  // instr_count through the terminal is only reliable against the stable
  // stopped prompt (the running disassembly prompt redraws the input line).
  await page.getByRole('button', { name: 'Run', exact: true }).click();
  await expect(page.locator('.gs-statusbar .sb-state .label')).toHaveText('Running', {
    timeout: 15_000,
  });
  await page.waitForTimeout(2_000);
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await terminalRun(page, 'scheduler.stop');
  await page.waitForTimeout(500);
  const advancedInstr = await readInstr(page);
  expect(advancedInstr, 'machine should have advanced past the saved state').toBeGreaterThan(
    savedInstr as number,
  );

  // Drop the checkpoint file — the signature short-circuit must restore, not
  // upload-classify.
  await dropOnDisplay(page, 'state.checkpoint', bytes);
  await expect(toast(page, /Checkpoint loaded/)).toBeVisible({ timeout: 60_000 });
  // Restored to the paused snapshot: the instruction counter is back to
  // exactly the saved value.
  await expect
    .poll(async () => await readInstr(page), { timeout: 30_000, intervals: [1_000] })
    .toBe(savedInstr);
});
