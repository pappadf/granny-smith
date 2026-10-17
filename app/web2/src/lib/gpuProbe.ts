// One WebGL 2 health check, shared by the page (an off-DOM canvas) and the
// probe worker (an OffscreenCanvas, as the emulator renders).  Getting a
// context is not enough: a browser whose GPU process is stuck can hand out
// contexts that are lost from the start (getParameter answers null, which
// is what killed the emulator in Safari until it was restarted), or that
// accept every call and draw nothing.  So the check builds a program, draws
// a known colour and reads it back.

// The verdict on one context.
export type GlProbeResult =
  | { ok: true; renderer: string }
  | { ok: false; reason: 'lost' | 'broken'; detail: string; renderer: string };

// The colour the probe draws; 0.2/0.4/0.6 reads back as 51/102/153.
const PROBE_RGB = [51, 102, 153];

const VS = `#version 300 es
in vec2 a_position;
void main() { gl_Position = vec4(a_position, 0.0, 1.0); }`;

const FS = `#version 300 es
precision mediump float;
out vec4 o_color;
void main() { o_color = vec4(0.2, 0.4, 0.6, 1.0); }`;

// The GPU's name as WebGL reports it ("" when lost or hidden).
function rendererName(gl: WebGL2RenderingContext): string {
  try {
    const r: unknown = gl.getParameter(gl.RENDERER);
    return typeof r === 'string' ? r : '';
  } catch {
    return '';
  }
}

// Compile one shader stage; the driver's log on failure.
function compile(gl: WebGL2RenderingContext, type: number, src: string): WebGLShader | string {
  const s = gl.createShader(type);
  if (!s) return 'createShader returned null';
  gl.shaderSource(s, src);
  gl.compileShader(s);
  if (gl.getShaderParameter(s, gl.COMPILE_STATUS) === true) return s;
  const log = gl.getShaderInfoLog(s) ?? '';
  gl.deleteShader(s);
  return `shader compile failed${log ? `: ${log.trim()}` : ''}`;
}

// Probe a freshly created context: is it alive, and does it draw?
export function probeGlContext(gl: WebGL2RenderingContext): GlProbeResult {
  const renderer = rendererName(gl);
  const lost = (detail: string): GlProbeResult => ({ ok: false, reason: 'lost', detail, renderer });
  const broken = (detail: string): GlProbeResult =>
    gl.isContextLost() ? lost(detail) : { ok: false, reason: 'broken', detail, renderer };

  if (gl.isContextLost()) return lost('the context was lost as soon as it was created');
  // The query the emulator's canvas resize makes first; null only when lost.
  if (gl.getParameter(gl.VIEWPORT) === null) return lost('getParameter(VIEWPORT) returned null');

  const vs = compile(gl, gl.VERTEX_SHADER, VS);
  if (typeof vs === 'string') return broken(vs);
  const fs = compile(gl, gl.FRAGMENT_SHADER, FS);
  if (typeof fs === 'string') return broken(fs);
  const prog = gl.createProgram();
  if (!prog) return broken('createProgram returned null');
  gl.attachShader(prog, vs);
  gl.attachShader(prog, fs);
  gl.bindAttribLocation(prog, 0, 'a_position');
  gl.linkProgram(prog);
  if (gl.getProgramParameter(prog, gl.LINK_STATUS) !== true)
    return broken(`program link failed: ${(gl.getProgramInfoLog(prog) ?? '').trim()}`);

  // One triangle covering the whole viewport, in the probe colour.
  const buf = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, buf);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
  gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
  gl.clearColor(0, 0, 0, 1);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.useProgram(prog);
  gl.drawArrays(gl.TRIANGLES, 0, 3);

  // readPixels is a round trip to the GPU: a context the browser has given
  // up on is reported lost by the time it returns.
  const px = new Uint8Array(4);
  gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
  if (gl.isContextLost()) return lost('the context was lost while drawing a test frame');
  const err = gl.getError();
  if (err !== gl.NO_ERROR) return broken(`GL error 0x${err.toString(16)} drawing a test frame`);
  const off = PROBE_RGB.some((v, i) => Math.abs(px[i] - v) > 3);
  if (off)
    return broken(
      `a test frame read back as ${px[0]},${px[1]},${px[2]} instead of ${PROBE_RGB.join(',')}`,
    );
  return { ok: true, renderer };
}

// Hand the context back to the browser now rather than at garbage
// collection: browsers cap the number of live contexts.
export function releaseGlContext(gl: WebGL2RenderingContext | WebGLRenderingContext): void {
  try {
    gl.getExtension('WEBGL_lose_context')?.loseContext();
  } catch {
    // Nothing to release with.
  }
}
