// The startup probe's worker: asks for WebGL 2 on an OffscreenCanvas inside
// a worker, as the emulator does, and checks that it draws (gpuProbe.ts).
// The page's own canvas can be healthy while this path is not: Safari
// before 17 has no WebGL in workers, and a stuck GPU process can fail one
// and not the other.  Answers once and is terminated by the page.

import { probeGlContext, releaseGlContext } from './gpuProbe';

// The answer posted back to the page (gpuDiagnostics.ts).
export type WorkerProbeAnswer =
  | { ok: true; renderer: string }
  | {
      ok: false;
      reason: 'no-offscreencanvas' | 'no-webgl2' | 'lost' | 'broken';
      detail: string;
      renderer?: string;
    };

// Run the probe on a small OffscreenCanvas.
function probe(): WorkerProbeAnswer {
  if (typeof OffscreenCanvas === 'undefined')
    return { ok: false, reason: 'no-offscreencanvas', detail: 'no OffscreenCanvas in workers' };
  let gl: WebGL2RenderingContext | null;
  try {
    gl = new OffscreenCanvas(16, 16).getContext('webgl2');
  } catch (e) {
    return { ok: false, reason: 'no-webgl2', detail: String(e) };
  }
  if (!gl) return { ok: false, reason: 'no-webgl2', detail: 'getContext("webgl2") returned null' };
  try {
    return probeGlContext(gl);
  } catch (e) {
    return { ok: false, reason: 'broken', detail: String(e) };
  } finally {
    releaseGlContext(gl);
  }
}

postMessage(probe());
