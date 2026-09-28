// File bytes between the page and the emulator's filesystem, without the page
// touching the filesystem itself.
//
// Under WasmFS a Module.FS call from the page runs ON the page's thread, which
// then busy-waits while WasmFS's OPFS thread does the I/O.  Chrome serves that
// thread's OPFS requests independently, so it only janked; WebKit serves a
// worker's OPFS request through the page's thread -- the one busy-waiting --
// so in Safari every upload deadlocked the page.  Here the page only copies
// bytes into (or out of) a fixed window in wasm memory, a plain memory access,
// and asks the core to move them to or from a file through the request bridge
// (storage.xfer_write / xfer_read), so every file access runs on the emulator
// thread like all the others.
//
// The window is one buffer, so its uses are serialised: a copy and the request
// that consumes it happen under one lock.

import { gsEval, gsErrorText, getModuleHeap, isGsError } from './emulator';

interface XferWindow {
  ptr: number;
  size: number;
}

let windowInfo: XferWindow | null = null;
let lock: Promise<unknown> = Promise.resolve();

// Run `fn` with the window to itself.
function withWindow<T>(fn: (w: XferWindow) => Promise<T>): Promise<T> {
  const run = lock.then(async () => fn(await xferWindow()));
  lock = run.catch(() => undefined);
  return run;
}

async function xferWindow(): Promise<XferWindow> {
  if (windowInfo) return windowInfo;
  const ptr = await gsEval('storage.xfer_buffer');
  const size = await gsEval('storage.xfer_size');
  if (typeof ptr !== 'number' || typeof size !== 'number' || !ptr || !size)
    throw new Error(`no file-transfer window: ${gsErrorText(isGsError(ptr) ? ptr : size)}`);
  windowInfo = { ptr, size };
  return windowInfo;
}

// The window's size: the most one write or read moves.
export async function xferChunkBytes(): Promise<number> {
  return (await xferWindow()).size;
}

// Write `bytes` (at most one window) to `path` at `offset`; offset 0 creates or
// truncates the file.
export function xferWrite(path: string, offset: number, bytes: Uint8Array): Promise<void> {
  return withWindow(async (w) => {
    if (bytes.length > w.size) throw new Error('xferWrite: chunk larger than the window');
    const heap = getModuleHeap();
    if (!heap) throw new Error('xferWrite: emulator not running');
    // Fetched per use: under memory growth the heap's buffer can be replaced.
    heap.u8.set(bytes, w.ptr);
    const r = await gsEval('storage.xfer_write', [path, offset, bytes.length]);
    if (r !== true) throw new Error(gsErrorText(r));
  });
}

// Read up to `len` bytes (at most one window) of `path` from `offset`.
export function xferRead(path: string, offset: number, len: number): Promise<Uint8Array> {
  return withWindow(async (w) => {
    const r = await gsEval('storage.xfer_read', [path, offset, Math.min(len, w.size)]);
    if (typeof r !== 'number') throw new Error(gsErrorText(r));
    const heap = getModuleHeap();
    if (!heap) throw new Error('xferRead: emulator not running');
    return heap.u8.slice(w.ptr, w.ptr + r);
  });
}

// The whole file, read a window at a time.
export async function xferReadAll(path: string): Promise<Uint8Array> {
  const size = await xferChunkBytes();
  const parts: Uint8Array[] = [];
  let total = 0;
  for (;;) {
    const part = await xferRead(path, total, size);
    if (!part.length) break;
    parts.push(part);
    total += part.length;
    if (part.length < size) break;
  }
  const out = new Uint8Array(total);
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}
