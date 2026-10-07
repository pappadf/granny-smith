// The page side of the Rage 128 WebGPU takeover: owns the card's overlay
// canvas, starts its GPU worker LAZILY — on the first attach, so a page
// that never boots the card never creates a second GPU device — and relays
// the core's attach/detach requests to it.  Whether a WebGPU device exists
// at all is answered once, by the Voodoo2's worker at page load
// (voodoo2Gpu.svelte.ts), and the core reads that answer for both cards.
//
// The core's transport seam is shared with the Voodoo2 (gs_v2gpu_* in
// system.h; Module.onVoodooGpuAttach in em_gpu.c): emulator.ts routes an
// attach here when the control block's MAGIC is the Rage 128's.
//
// The overlay: `#screen3d-r128` sits over `#screen` and is shown after the
// worker's first present following a MODE(on) record, and hidden by the
// display path once the canvas underneath holds a fresh frame (em_video.c
// → Module.onVoodooGpuOverlay(0), which hides every GPU overlay).

import { getModule } from '@/bus/emulator';
import { MAGIC } from './rage128Protocol';

// Reactive overlay state, read by ScreenView.
export const rage128Overlay = $state({ visible: false, width: 640, height: 480 });

let canvasEl: HTMLCanvasElement | null = null;
let worker: Worker | null = null;
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
    if (m.type === 'mode') {
      rage128Overlay.visible = !!m.engaged;
      if (m.w && m.h) {
        rage128Overlay.width = m.w;
        rage128Overlay.height = m.h;
      }
    } else if (m.type === 'lost') {
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
