// Probes whether the browser can create a WebGL 2 context. The emulator's
// worker calls emscripten_webgl_create_context() with WebGL 2 attributes;
// if that fails the worker exits and the UI is left in a half-broken state
// (terminal works, Display never renders). Catching this at boot lets us
// show a clear error page instead.
//
// Common failure modes this catches:
//   - GPU process disabled (broken driver, missing libEGL, --disable-gpu)
//   - Hardware acceleration switched off in browser settings
//   - Headless Chromium without --use-gl=swiftshader
//   - Browser too old for WebGL 2 (Safari < 15, etc.)

export type WebGLCheckResult =
  | { ok: true }
  | { ok: false; reason: 'no-webgl2' | 'no-webgl' | 'no-canvas'; detail?: string };

export function checkWebGL2Available(): WebGLCheckResult {
  let canvas: HTMLCanvasElement;
  try {
    canvas = document.createElement('canvas');
  } catch (err) {
    return { ok: false, reason: 'no-canvas', detail: String(err) };
  }

  let gl2: WebGL2RenderingContext | null = null;
  try {
    gl2 = canvas.getContext('webgl2') as WebGL2RenderingContext | null;
  } catch (err) {
    return { ok: false, reason: 'no-webgl2', detail: String(err) };
  }

  if (gl2) {
    // Best effort: release the context immediately so we don't hold a GPU
    // slot. WEBGL_lose_context is widely supported.
    try {
      const ext = gl2.getExtension('WEBGL_lose_context');
      ext?.loseContext();
    } catch {
      /* ignore */
    }
    return { ok: true };
  }

  // No WebGL 2. Probe WebGL 1 so the error message can distinguish
  // "browser is too old" from "GPU process is dead".
  let gl1: WebGLRenderingContext | null = null;
  try {
    gl1 = canvas.getContext('webgl') as WebGLRenderingContext | null;
  } catch {
    /* fall through */
  }
  if (gl1) {
    return { ok: false, reason: 'no-webgl2', detail: 'WebGL 1 present, WebGL 2 missing' };
  }
  return { ok: false, reason: 'no-webgl' };
}
