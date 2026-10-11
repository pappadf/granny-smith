// Startup graphics diagnosis: works out, before the emulator starts,
// whether this browser can render it, and if not, which of the known
// failure modes it is in, so the error page can say what to do.
//
// The emulator renders with WebGL 2 on an OffscreenCanvas transferred to
// its worker (src/platform/wasm/em_video.c).  Each step below checks one
// link of that chain, in order:
//
//   1. a canvas and WebGL at all            (no-canvas / no-webgl / no-webgl2)
//   2. a WebGL 2 context on the page that is alive and draws
//                                           (context-lost / gpu-broken)
//   3. OffscreenCanvas transfer to workers  (no-offscreencanvas)
//   4. WebGL 2 on an OffscreenCanvas in a worker, alive and drawing
//                                           (worker-no-webgl2 /
//                                            worker-context-lost /
//                                            worker-gpu-broken)
//
// A lost or non-drawing context almost always means the browser's GPU
// process is stuck, which only quitting and restarting the browser clears
// (seen in Safari: every page load failed until a restart).  WebGPU is not
// part of this: only the Voodoo2's GPU mode uses it, and its absence just
// withholds that option (gpu/voodoo2Gpu.svelte.ts reports why).

import { probeGlContext, releaseGlContext } from './gpuProbe';
import type { WorkerProbeAnswer } from './gpuProbe.worker';

// Every way the graphics check can fail, from the page probe or later from
// the emulator's renderer (the `state:renderer` event).
export type GraphicsFailure =
  | 'no-canvas'
  | 'no-webgl'
  | 'no-webgl2'
  | 'context-lost'
  | 'gpu-broken'
  | 'no-offscreencanvas'
  | 'worker-no-webgl2'
  | 'worker-context-lost'
  | 'worker-gpu-broken'
  | 'renderer-no-context'
  | 'renderer-context-dead'
  | 'renderer-context-lost'
  | 'renderer-shader-failed';

// The diagnosis: ok, or the failure with the browser's own words for it.
export type GraphicsDiagnosis =
  { ok: true; renderer: string } | { ok: false; failure: GraphicsFailure; detail: string };

// How long the worker probe may take before it is counted as inconclusive.
// A healthy browser answers in tens of milliseconds.
const WORKER_PROBE_TIMEOUT_MS = 5000;

// Steps 1 and 2: WebGL 2 on an off-DOM canvas on the page.
export function diagnosePageWebGL(): GraphicsDiagnosis {
  let canvas: HTMLCanvasElement;
  try {
    canvas = document.createElement('canvas');
    canvas.width = canvas.height = 16;
  } catch (e) {
    return { ok: false, failure: 'no-canvas', detail: String(e) };
  }

  let gl2: WebGL2RenderingContext | null;
  try {
    gl2 = canvas.getContext('webgl2');
  } catch (e) {
    return { ok: false, failure: 'no-webgl2', detail: String(e) };
  }
  if (!gl2) {
    // No WebGL 2: WebGL 1 tells "browser too old" from "GPU off or dead".
    let gl1: WebGLRenderingContext | null = null;
    try {
      gl1 = document.createElement('canvas').getContext('webgl');
    } catch {
      // Counted as no WebGL at all.
    }
    if (gl1) {
      releaseGlContext(gl1);
      return { ok: false, failure: 'no-webgl2', detail: 'WebGL 1 present, WebGL 2 missing' };
    }
    return { ok: false, failure: 'no-webgl', detail: 'getContext("webgl") returned null' };
  }

  try {
    const r = probeGlContext(gl2);
    if (r.ok) return { ok: true, renderer: r.renderer };
    return {
      ok: false,
      failure: r.reason === 'lost' ? 'context-lost' : 'gpu-broken',
      detail: withRenderer(r.detail, r.renderer),
    };
  } catch (e) {
    return { ok: false, failure: 'gpu-broken', detail: String(e) };
  } finally {
    releaseGlContext(gl2);
  }
}

// Steps 3 and 4: OffscreenCanvas transfer, and WebGL 2 inside a worker.
// Resolves ok when the probe cannot run or does not answer in time: the
// check must never keep a working browser from starting.
export async function diagnoseWorkerWebGL(): Promise<GraphicsDiagnosis> {
  if (
    typeof OffscreenCanvas === 'undefined' ||
    typeof HTMLCanvasElement.prototype.transferControlToOffscreen !== 'function'
  )
    return {
      ok: false,
      failure: 'no-offscreencanvas',
      detail: 'HTMLCanvasElement.transferControlToOffscreen is not available',
    };

  let worker: Worker;
  try {
    worker = new Worker(new URL('./gpuProbe.worker.ts', import.meta.url), {
      type: 'module',
      name: 'gpu-probe',
    });
  } catch (e) {
    console.warn('[graphics] the WebGL worker probe could not start; skipped:', e);
    return { ok: true, renderer: '' };
  }

  const answer = await new Promise<WorkerProbeAnswer | null>((resolve) => {
    const timer = setTimeout(() => {
      console.warn(
        `[graphics] the WebGL worker probe did not answer within ${WORKER_PROBE_TIMEOUT_MS} ms; skipped`,
      );
      resolve(null);
    }, WORKER_PROBE_TIMEOUT_MS);
    worker.onmessage = (ev: MessageEvent<WorkerProbeAnswer>) => {
      clearTimeout(timer);
      resolve(ev.data);
    };
    worker.onerror = (ev) => {
      clearTimeout(timer);
      console.warn('[graphics] the WebGL worker probe failed to run; skipped:', ev.message);
      resolve(null);
    };
  });
  worker.terminate();

  if (!answer || answer.ok) return { ok: true, renderer: answer?.renderer ?? '' };
  const failure: GraphicsFailure =
    answer.reason === 'no-offscreencanvas'
      ? 'no-offscreencanvas'
      : answer.reason === 'no-webgl2'
        ? 'worker-no-webgl2'
        : answer.reason === 'lost'
          ? 'worker-context-lost'
          : 'worker-gpu-broken';
  return { ok: false, failure, detail: withRenderer(answer.detail, answer.renderer) };
}

// The whole check, page first (cheap, and it names the commonest failures).
export async function diagnoseGraphics(): Promise<GraphicsDiagnosis> {
  const page = diagnosePageWebGL();
  const result = page.ok ? await diagnoseWorkerWebGL() : page;
  const final = result.ok && page.ok ? { ok: true as const, renderer: page.renderer } : result;
  // For bug reports and automation: what the check saw.
  (window as unknown as { __gsGraphics?: GraphicsDiagnosis }).__gsGraphics = final;
  if (!final.ok) console.error(`[graphics] ${final.failure}: ${final.detail}`);
  return final;
}

// The emulator renderer's own failure report (`state:renderer`, em_video.c)
// as a diagnosis; null for a status this page does not know.
export function rendererFailure(status: string, detail: string): GraphicsDiagnosis | null {
  const failure: GraphicsFailure | null =
    status === 'no_context'
      ? 'renderer-no-context'
      : status === 'context_dead'
        ? 'renderer-context-dead'
        : status === 'context_lost'
          ? 'renderer-context-lost'
          : status === 'shader_failed'
            ? 'renderer-shader-failed'
            : null;
  return failure ? { ok: false, failure, detail } : null;
}

// The detail line, with the GPU's name when the browser gave one.
function withRenderer(detail: string, renderer: string | undefined): string {
  return renderer ? `${detail} (renderer: ${renderer})` : detail;
}
