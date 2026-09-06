// The page side of the Voodoo2 WebGPU takeover (proposal-voodoo2-webgpu-
// takeover §5.8, §5.10): starts the GPU worker once with the overlay
// canvas, answers the core's availability question through the bridge,
// and relays the core's attach/detach requests (Module.onVoodooGpuAttach
// / onVoodooGpuDetach, fired from em_gpu.c) to the worker.  The worker
// then talks to the emulator through shared memory only; this module
// never sees a frame.
//
// The overlay: `#screen3d` sits over `#screen` and is shown exactly
// while the card drives the monitor in GPU mode (the worker posts the
// MODE records it consumes), so the pass-through switch is literally
// which canvas is on top.

import { getModule } from '@/bus/emulator';

// Reactive overlay state, read by ScreenView.
export const gpuOverlay = $state({ visible: false, width: 640, height: 480 });

let worker: Worker | null = null;
let ready: Promise<boolean> | null = null;
let deviceReady = false;

// Start the worker with the transferred overlay canvas; resolves to
// whether a WebGPU device exists.  Idempotent.
export function startVoodooGpu(canvas3d: HTMLCanvasElement): Promise<boolean> {
  if (ready) return ready;
  ready = new Promise<boolean>((resolve) => {
    if (!('gpu' in navigator) || typeof canvas3d.transferControlToOffscreen !== 'function') {
      resolve(false);
      return;
    }
    let offscreen: OffscreenCanvas;
    try {
      offscreen = canvas3d.transferControlToOffscreen();
    } catch {
      resolve(false);
      return;
    }
    try {
      // eslint-disable-next-line svelte/prefer-svelte-reactivity -- a module URL for Vite's worker bundling, not state
      worker = new Worker(new URL('./voodoo2Gpu.worker.ts', import.meta.url), {
        type: 'module',
        name: 'voodoo2-gpu',
      });
    } catch (e) {
      console.warn('[voodoo2-gpu] worker failed to start', e);
      resolve(false);
      return;
    }
    worker.onmessage = (
      ev: MessageEvent<{
        type: string;
        engaged?: boolean;
        w?: number;
        h?: number;
        reason?: string;
      }>,
    ) => {
      const m = ev.data;
      if (m.type === 'ready') {
        deviceReady = true;
        resolve(true);
      } else if (m.type === 'unavailable') {
        resolve(false);
      } else if (m.type === 'mode') {
        gpuOverlay.visible = !!m.engaged;
        if (m.w && m.h) {
          gpuOverlay.width = m.w;
          gpuOverlay.height = m.h;
        }
      } else if (m.type === 'lost') {
        console.warn('[voodoo2-gpu] device lost:', m.reason);
        gpuOverlay.visible = false;
        deviceReady = false;
      }
    };
    worker.onerror = (e) => {
      console.warn('[voodoo2-gpu] worker error', e.message);
      resolve(false);
    };
    worker.postMessage({ type: 'init', canvas: offscreen }, [offscreen]);
  });
  return ready;
}

export function voodooGpuAvailable(): boolean {
  return deviceReady;
}

// Resolves once the worker has answered (true: a device exists).  False
// immediately when the worker was never started.
export function whenVoodooGpuReady(): Promise<boolean> {
  return ready ?? Promise.resolve(false);
}

// Module.onVoodooGpuAttach: the core allocated a shared region at
// `ctrl`; hand the worker the wasm memory and the address.
export function onVoodooGpuAttach(ctrl: number): void {
  const mod = getModule();
  if (!worker || !deviceReady || !mod) return;
  const memory =
    (mod as unknown as { wasmMemory?: WebAssembly.Memory }).wasmMemory ?? mod.HEAPU8.buffer;
  if (!(memory instanceof WebAssembly.Memory) && !(memory instanceof SharedArrayBuffer)) return;
  // Diagnostics: the control block's address and the memory, for a page
  // script to read HEAD/TAIL/ACK/STATUS when something stalls.
  (window as unknown as { __v2gpu?: unknown }).__v2gpu = { memory, ctrl, worker };
  worker.postMessage({ type: 'attach', memory, ctrl });
}

// Module.onVoodooGpuOverlay(0): the display path drew a fresh frame on
// the canvas underneath — the overlay may go.
export function onVoodooGpuOverlay(visible: number): void {
  gpuOverlay.visible = visible !== 0;
}

export function onVoodooGpuDetach(ctrl: number): void {
  gpuOverlay.visible = false;
  worker?.postMessage({ type: 'detach', ctrl });
}
