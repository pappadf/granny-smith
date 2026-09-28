// The page side of the emulated LaserWriter's interpreter (the pattern of
// gpu/voodoo2Gpu.svelte.ts): the core's printer bridge allocates a
// shared-memory ring on the first print job and fires
// Module.onPrinterAttach(ctrl, version) from em_main.c; this module starts
// the platen worker (printer/platen.worker.ts) lazily — the interpreter
// module, platen-<version>.js beside main.mjs, is fetched only now — and
// hands it the wasm memory and the control block's address.  The worker
// then talks to the bridge through shared memory only; what comes back
// here is each finished PDF, which is downloaded at once as
// <job id>-<title>.pdf (the blob + anchor path gs_download uses).

import { getModule } from '@/bus/emulator';
import { showNotification } from '@/state/toasts.svelte';
import type { DocumentMsg } from './platenRing';

let worker: Worker | null = null;
let ready = false;
let lost = false;
let pendingAttach: number | null = null; // a ctrl address that arrived before `ready`

// Module.onPrinterAttach: the core allocated its ring at `ctrl` and the
// interpreter library is `version` (names the module file to fetch).
export function onPrinterAttach(ctrl: number, version: string): void {
  if (lost) {
    console.warn(
      '[platen] attach requested after the worker was lost; the print job will time out',
    );
    return;
  }
  if (!worker) start(version);
  if (ready) attach(ctrl);
  else pendingAttach = ctrl;
}

// Starts the worker and has it fetch the module.  Both files are
// cache-busted the way main.mjs is, so a deploy never pairs a new page
// with a stale interpreter.
function start(version: string): void {
  const bust = Date.now();
  const moduleUrl = new URL(`platen-${version}.js?v=${bust}`, document.baseURI).href;
  const wasmUrl = new URL(`platen-${version}.wasm?v=${bust}`, document.baseURI).href;
  try {
    worker = new Worker(new URL('./platen.worker.ts', import.meta.url), {
      type: 'module',
      name: 'platen',
    });
  } catch (e) {
    console.warn('[platen] worker failed to start', e);
    lost = true;
    return;
  }
  worker.onmessage = (ev: MessageEvent<{ type: string; reason?: string } | DocumentMsg>) => {
    const m = ev.data;
    if (m.type === 'ready') {
      ready = true;
      if (pendingAttach !== null) {
        const ctrl = pendingAttach;
        pendingAttach = null;
        attach(ctrl);
      }
    } else if (m.type === 'document') {
      download(m as DocumentMsg);
    } else if (m.type === 'lost') {
      const reason = (m as { reason?: string }).reason ?? 'unknown';
      console.warn('[platen] interpreter worker lost:', reason);
      showNotification(`LaserWriter interpreter unavailable: ${reason}`, 'error');
      lost = true;
    }
  };
  worker.onerror = (e) => {
    console.warn('[platen] worker error', e.message);
  };
  worker.postMessage({ type: 'start', moduleUrl, wasmUrl });
}

// Hands the worker the shared memory and the control block's address.
function attach(ctrl: number): void {
  const mod = getModule();
  if (!worker || !mod) return;
  const memory =
    (mod as unknown as { wasmMemory?: WebAssembly.Memory }).wasmMemory ?? mod.HEAPU8.buffer;
  if (!(memory instanceof WebAssembly.Memory) && !(memory instanceof SharedArrayBuffer)) {
    console.warn('[platen] the emulator memory is not shared; cannot attach the interpreter');
    return;
  }
  // Diagnostics: the control block's address and the memory, for a page
  // script to read HEAD/TAIL/STATUS when something stalls.
  (window as unknown as { __platen?: unknown }).__platen = { memory, ctrl, worker };
  worker.postMessage({ type: 'attach', memory, ctrl });
}

// The automatic download of a finished document.
function download(doc: DocumentMsg): void {
  try {
    const blob = new Blob([doc.pdf as BlobPart], { type: 'application/pdf' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = doc.name;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(() => URL.revokeObjectURL(a.href), 0);
    showNotification(`LaserWriter: ${doc.name} (${doc.pages} page${doc.pages === 1 ? '' : 's'})`);
  } catch (e) {
    console.error('[platen] download failed:', e);
    showNotification(`LaserWriter: could not download ${doc.name}`, 'error');
  }
}
