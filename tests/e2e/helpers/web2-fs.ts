// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Helpers for the web2 Filesystem-tab e2e. Everything here drives the real
// UI; the one concession is treeDrag(), which synthesises the drag *gesture*
// (Playwright/CDP cannot drive native HTML5 drag-and-drop) while letting the
// component's real handlers, bus/fsOps, the worker and OPFS all run for real.

import { type Page, type Locator } from '@playwright/test';
import * as fs from 'node:fs';

// Load the web2 app and wait for the WASM module / worker bridge to come up.
// No ROM, no machine: the Filesystem tab, object-model shell and OPFS are all
// live at module-ready, independent of any emulated machine.
export async function gotoWeb2(page: Page): Promise<void> {
  await page.goto('/index.html');
  await page.waitForFunction(() => (window as { __gsReady?: boolean }).__gsReady === true, undefined, {
    timeout: 60_000,
  });
  // First visit shows a non-dismissible "preview build" modal whose backdrop
  // intercepts clicks. Dismiss it via its Continue button before doing anything.
  const cont = page.getByRole('button', { name: 'Continue' });
  if (await cont.isVisible().catch(() => false)) await cont.click();
}

// Stage a real host file into OPFS as a test *precondition* (the fixture
// input). This is exactly what the shipped upload path does — writeToOPFS()
// writes through the page's navigator.storage — so the worker's vfs.list
// reads it back fine. No machine / boot involved.
export async function stageOpfsFile(page: Page, opfsPath: string, hostFile: string): Promise<void> {
  const data = fs.readFileSync(hostFile).toString('base64');
  await page.evaluate(
    async ({ path, data }: { path: string; data: string }) => {
      const bin = atob(data);
      const bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
      // /opfs is the navigator.storage root in the app; strip it and walk.
      const rel = path.replace(/^\/opfs\/?/, '').split('/').filter(Boolean);
      const fileName = rel.pop() as string;
      let dir = await navigator.storage.getDirectory();
      for (const part of rel) dir = await dir.getDirectoryHandle(part, { create: true });
      const fh = await dir.getFileHandle(fileName, { create: true });
      const w = await fh.createWritable();
      await w.write(bytes);
      await w.close();
    },
    { path: opfsPath, data },
  );
}

// Stage inline bytes as an OPFS file (for plain-file tests that need no real
// host fixture). Same navigator.storage write the upload path uses.
export async function stageOpfsText(page: Page, opfsPath: string, text: string): Promise<void> {
  await page.evaluate(
    async ({ path, text }: { path: string; text: string }) => {
      const rel = path.replace(/^\/opfs\/?/, '').split('/').filter(Boolean);
      const fileName = rel.pop() as string;
      let dir = await navigator.storage.getDirectory();
      for (const part of rel) dir = await dir.getDirectoryHandle(part, { create: true });
      const fh = await dir.getFileHandle(fileName, { create: true });
      const w = await fh.createWritable();
      await w.write(new TextEncoder().encode(text));
      await w.close();
    },
    { path: opfsPath, text },
  );
}

// Create an empty OPFS directory (e.g. a move destination).
export async function mkdirOpfs(page: Page, opfsPath: string): Promise<void> {
  await page.evaluate(async (path: string) => {
    const rel = path.replace(/^\/opfs\/?/, '').split('/').filter(Boolean);
    let dir = await navigator.storage.getDirectory();
    for (const part of rel) dir = await dir.getDirectoryHandle(part, { create: true });
  }, opfsPath);
}

// Open the Filesystem panel tab and wait for the /opfs root row.
export async function openFilesystemTab(page: Page): Promise<void> {
  await page.locator('button.ptab[data-tab="filesystem"]').click();
  await row(page, '/opfs').first().waitFor({ state: 'visible' });
}

// Anchor a label to a full, exact match so "Installer" can't also match
// "Installer Script".
function exact(text: string): RegExp {
  return new RegExp(`^${text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`);
}

// A tree row located by its exact visible label.
export function row(scope: Page | Locator, label: string): Locator {
  return scope.locator('.tree-row').filter({ has: scope.locator('.label', { hasText: exact(label) }) });
}

// Expand a branch row (folder / disk image) by clicking its twistie, then
// wait for one of its children to render. Lazy children (image descent) are a
// worker round-trip, so this awaits the child rather than the click alone.
export async function expand(page: Page, label: string, childLabel: string): Promise<void> {
  await row(page, label).first().locator('.twistie').click();
  await row(page, childLabel).first().waitFor({ state: 'visible' });
}

// Collapse a branch row.
export async function collapse(page: Page, label: string): Promise<void> {
  await row(page, label).first().locator('.twistie').click();
}

// Synthesise an internal tree drag: dispatch dragstart on the real source row
// and dragenter/dragover/drop on the real target row, sharing ONE real
// DataTransfer so the component's real handleDragStart populates it and
// handleDrop reads it. Only the gesture is synthetic — the move/copy that
// follows (bus/fsOps → worker → OPFS) is entirely real.
//
// A real browser fires `drop` only if a `dragover` handler accepted the
// target with preventDefault — so this helper enforces the same contract:
// if the app's handleDragOver rejects the target, it throws instead of
// dropping anyway, keeping the e2e honest about the drop-acceptance gate.
export async function treeDrag(page: Page, source: Locator, target: Locator): Promise<void> {
  const src = await source.elementHandle();
  const tgt = await target.elementHandle();
  if (!src || !tgt) throw new Error('treeDrag: source or target row not found');
  const accepted = await page.evaluate(
    ({ s, t }) => {
      const dt = new DataTransfer();
      const fire = (el: Element, type: string) =>
        el.dispatchEvent(new DragEvent(type, { bubbles: true, cancelable: true, dataTransfer: dt }));
      fire(s, 'dragstart'); // real handleDragStart writes the payload into dt
      fire(t, 'dragenter');
      // dispatchEvent returns false iff a handler called preventDefault —
      // i.e. handleDragOver accepted this drop target.
      const ok = !t.dispatchEvent(
        new DragEvent('dragover', { bubbles: true, cancelable: true, dataTransfer: dt }),
      );
      if (ok) fire(t, 'drop'); // real handleDrop runs the copy/move
      fire(s, 'dragend');
      return ok;
    },
    { s: src, t: tgt },
  );
  if (!accepted) throw new Error('treeDrag: target rejected the drop (dragover not accepted)');
}

// Synthesise an external file drop onto a real folder row: a DataTransfer
// carrying the File, dispatched as dragenter/dragover/drop. Exercises the real
// acceptFilesRaw upload path (writeToOPFS) — only the gesture is synthetic.
export async function dropFileOnRow(
  page: Page,
  target: Locator,
  fileName: string,
  text: string,
): Promise<void> {
  const el = await target.elementHandle();
  if (!el) throw new Error('dropFileOnRow: target row not found');
  await page.evaluate(
    ({ el, name, text }) => {
      const file = new File([new TextEncoder().encode(text)], name, {
        type: 'application/octet-stream',
      });
      const dt = new DataTransfer();
      dt.items.add(file);
      const fire = (type: string) =>
        el.dispatchEvent(new DragEvent(type, { bubbles: true, cancelable: true, dataTransfer: dt }));
      fire('dragenter');
      fire('dragover');
      fire('drop');
    },
    { el, name: fileName, text },
  );
}
