// The page side of the Rage 128 WebGPU takeover: owns the card's overlay
// canvas, starts its GPU worker LAZILY — on the first attach, so a page
// that never boots the card never creates a second GPU device — and relays
// the core's attach/detach requests to it.  Whether a WebGPU device exists
// at all is answered once, by the Voodoo2's worker at page load
// (voodoo2Gpu.svelte.ts), and the core reads that answer for both cards.
//
// The core's transport seam is shared with the Voodoo2 (platform_v2gpu_* in
// platform_hooks.h; Module.onVoodooGpuAttach in em_gpu.c): emulator.ts routes an
// attach here when the control block's MAGIC is the Rage 128's.
//
// The overlay: `#screen3d-r128` sits over `#screen` and is shown after the
// worker's first present following a MODE(on) record, and hidden by the
// display path once the canvas underneath holds a fresh frame (em_video.c
// → Module.onVoodooGpuOverlay(0), which hides every GPU overlay).

import { getModule } from '@/bus/emulator';
import {
  MAGIC,
  C_HEAD,
  C_TAIL,
  C_REQ,
  C_ACK,
  C_STATUS,
  C_STAT_FRAMES,
  C_STAT_DRAWS,
  C_STAT_FLUSHES,
  C_STAT_READBACKS,
  C_PRESENTS_DONE,
  C_RING_SIZE,
} from './rage128Protocol';

// Reactive overlay state, read by ScreenView.
export const rage128Overlay = $state({ visible: false, width: 640, height: 480 });

let canvasEl: HTMLCanvasElement | null = null;
let worker: Worker | null = null;
// The worker's answers to 'stats' (the perf monitor, the diagnostics)
let statsWaiters: Array<(s: Record<string, unknown> | null) => void> = [];
let lostReason = '';
// Control blocks attached through this module (an array: not state).
const attachedCtrls: number[] = [];

// ScreenView hands the canvas over at mount; nothing starts until a card
// attaches.
export function registerRage128Canvas(canvas: HTMLCanvasElement): void {
  canvasEl = canvas;
}

// Is the control block at `ctrl` the Rage 128's?  (The transport seam is
// shared; the magic word tells the two cards apart.)
export function isRage128Ctrl(ctrl: number): boolean {
  const mod = getModule();
  if (!mod) return false;
  // The memory as it is now (the heap may have grown since HEAPU8 was made).
  const buffer =
    (mod as unknown as { wasmMemory?: WebAssembly.Memory }).wasmMemory?.buffer ?? mod.HEAPU8.buffer;
  return new Uint32Array(buffer)[ctrl >>> 2] >>> 0 === MAGIC;
}

function startWorker(): Worker | null {
  if (worker) return worker;
  if (!canvasEl || typeof canvasEl.transferControlToOffscreen !== 'function') return null;
  let offscreen: OffscreenCanvas;
  try {
    offscreen = canvasEl.transferControlToOffscreen();
  } catch (e) {
    console.warn('[rage128-gpu] the overlay canvas could not be transferred', e);
    return null;
  }
  try {
    worker = new Worker(new URL('./rage128Gpu.worker.ts', import.meta.url), {
      type: 'module',
      name: 'rage128-gpu',
    });
  } catch (e) {
    console.warn('[rage128-gpu] worker failed to start', e);
    return null;
  }
  worker.onmessage = (
    ev: MessageEvent<{ type: string; engaged?: boolean; w?: number; h?: number; reason?: string }>,
  ) => {
    const m = ev.data;
    if (m.type === 'stats') {
      const waiters = statsWaiters;
      statsWaiters = [];
      for (const w of waiters) w(m as unknown as Record<string, unknown>);
      return;
    }
    if (m.type === 'mode') {
      rage128Overlay.visible = !!m.engaged;
      if (m.w && m.h) {
        rage128Overlay.width = m.w;
        rage128Overlay.height = m.h;
      }
    } else if (m.type === 'lost') {
      lostReason = m.reason ?? 'lost';
      console.warn('[rage128-gpu] device lost:', m.reason);
      rage128Overlay.visible = false;
    } else if (m.type === 'unavailable') {
      console.warn('[rage128-gpu] no WebGPU device for the Rage 128 takeover', m.reason ?? '');
    }
  };
  worker.onerror = (e) => console.warn('[rage128-gpu] worker error', e.message);
  worker.postMessage({ type: 'init', canvas: offscreen }, [offscreen]);
  return worker;
}

// The core allocated a shared region at `ctrl`: start the worker if need
// be and hand it the wasm memory and the address.  The core waits (a few
// seconds) for the worker to mark the region attached, which covers the
// device's creation.
export function onRage128GpuAttach(ctrl: number): void {
  const mod = getModule();
  const w = startWorker();
  if (!w || !mod) return;
  const memory =
    (mod as unknown as { wasmMemory?: WebAssembly.Memory }).wasmMemory ?? mod.HEAPU8.buffer;
  if (!(memory instanceof WebAssembly.Memory) && !(memory instanceof SharedArrayBuffer)) return;
  if (!attachedCtrls.includes(ctrl)) attachedCtrls.push(ctrl);
  (window as unknown as { __r128gpu?: unknown }).__r128gpu = { memory, ctrl, worker: w };
  w.postMessage({ type: 'attach', memory, ctrl });
}

// Detach requests for a control block this module attached; false when
// the block is not ours (the caller tries the Voodoo2's).
export function onRage128GpuDetach(ctrl: number): boolean {
  const i = attachedCtrls.indexOf(ctrl);
  if (i < 0) return false;
  attachedCtrls.splice(i, 1);
  rage128Overlay.visible = false;
  worker?.postMessage({ type: 'detach', ctrl });
  return true;
}

export function hideRage128Overlay(): void {
  rage128Overlay.visible = false;
}

// The control block's words as they stand, read from this thread: where
// the op ring and the acknowledgements are, whatever the emulator thread
// or the worker is doing.  Null when no card is attached.
export function rage128GpuProbe(): Record<string, number | string> | null {
  const mod = getModule();
  const ctrl = attachedCtrls[0];
  if (!mod || ctrl === undefined) return null;
  const buffer =
    (mod as unknown as { wasmMemory?: WebAssembly.Memory }).wasmMemory?.buffer ?? mod.HEAPU8.buffer;
  const w = new Int32Array(buffer, ctrl, 32);
  const u = (i: number) => Atomics.load(w, i) >>> 0;
  return {
    head: u(C_HEAD),
    tail: u(C_TAIL),
    backlog: (u(C_HEAD) - u(C_TAIL)) >>> 0,
    ringSize: u(C_RING_SIZE),
    req: u(C_REQ),
    ack: u(C_ACK),
    status: u(C_STATUS),
    frames: u(C_STAT_FRAMES),
    draws: u(C_STAT_DRAWS),
    flushes: u(C_STAT_FLUSHES),
    readbacks: u(C_STAT_READBACKS),
    presentsDone: u(C_PRESENTS_DONE),
    workerStarted: worker ? 1 : 0,
    lost: lostReason,
  };
}

// The worker's own counters since the last call, or null when it does not
// answer within `timeoutMs` (it is busy in a drain, or wedged) or there is
// no worker.
export function rage128GpuWorkerStats(timeoutMs = 500): Promise<Record<string, unknown> | null> {
  if (!worker) return Promise.resolve(null);
  return new Promise((resolve) => {
    let done = false;
    const finish = (v: Record<string, unknown> | null) => {
      if (done) return;
      done = true;
      resolve(v);
    };
    statsWaiters.push(finish);
    setTimeout(() => finish(null), timeoutMs);
    worker!.postMessage({ type: 'stats' });
  });
}
