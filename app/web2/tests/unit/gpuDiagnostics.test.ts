import { describe, it, expect } from 'vitest';
import { probeGlContext } from '@/lib/gpuProbe';
import { rendererFailure } from '@/lib/gpuDiagnostics';
import { renderWebGLErrorPage } from '@/lib/webglErrorPage';
import { dispatchCoreEvent, onRendererFailure } from '@/bus/emulator';
import { EVT_STATE } from '@/bus/mailbox';

// The startup graphics check (lib/gpuDiagnostics.ts) and the renderer's
// failure event.  A browser with a stuck GPU process hands out WebGL
// contexts that are lost from the start (getParameter answers null) or
// draw nothing; Safari did both until it was restarted, and the emulator
// died with "null is not an object (evaluating 'prevViewport[0]')".

// A fake WebGL 2 context: healthy unless told otherwise.
function fakeGl(opts: { lostAt?: 'start' | 'readback'; viewport?: null; pixel?: number[] } = {}) {
  let lost = opts.lostAt === 'start';
  const gl = {
    RENDERER: 1,
    VIEWPORT: 2,
    VERTEX_SHADER: 3,
    FRAGMENT_SHADER: 4,
    COMPILE_STATUS: 5,
    LINK_STATUS: 6,
    ARRAY_BUFFER: 7,
    STATIC_DRAW: 8,
    FLOAT: 9,
    COLOR_BUFFER_BIT: 10,
    TRIANGLES: 11,
    RGBA: 12,
    UNSIGNED_BYTE: 13,
    NO_ERROR: 0,
    drawingBufferWidth: 16,
    drawingBufferHeight: 16,
    isContextLost: () => lost,
    getParameter: (p: number) =>
      lost
        ? null
        : p === 1
          ? 'Fake GPU'
          : p === 2
            ? 'viewport' in opts
              ? opts.viewport
              : [0, 0, 16, 16]
            : null,
    createShader: () => ({}),
    shaderSource: () => {},
    compileShader: () => {},
    getShaderParameter: () => true,
    getShaderInfoLog: () => '',
    deleteShader: () => {},
    createProgram: () => ({}),
    attachShader: () => {},
    bindAttribLocation: () => {},
    linkProgram: () => {},
    getProgramParameter: () => true,
    getProgramInfoLog: () => '',
    createBuffer: () => ({}),
    bindBuffer: () => {},
    bufferData: () => {},
    enableVertexAttribArray: () => {},
    vertexAttribPointer: () => {},
    viewport: () => {},
    clearColor: () => {},
    clear: () => {},
    useProgram: () => {},
    drawArrays: () => {},
    readPixels: (
      _x: number,
      _y: number,
      _w: number,
      _h: number,
      _f: number,
      _t: number,
      px: Uint8Array,
    ) => {
      if (opts.lostAt === 'readback') lost = true;
      px.set(opts.pixel ?? [51, 102, 153, 255]);
    },
    getError: () => 0,
  };
  return gl as unknown as WebGL2RenderingContext;
}

describe('probeGlContext', () => {
  it('passes a context that draws the test colour', () => {
    expect(probeGlContext(fakeGl())).toEqual({ ok: true, renderer: 'Fake GPU' });
  });

  it('reports a context lost from the start', () => {
    const r = probeGlContext(fakeGl({ lostAt: 'start' }));
    expect(r.ok).toBe(false);
    expect(!r.ok && r.reason).toBe('lost');
  });

  it('reports a context whose getParameter(VIEWPORT) is null as lost', () => {
    const r = probeGlContext(fakeGl({ viewport: null }));
    expect(!r.ok && r.reason).toBe('lost');
  });

  it('reports a context lost while drawing', () => {
    const r = probeGlContext(fakeGl({ lostAt: 'readback' }));
    expect(!r.ok && r.reason).toBe('lost');
  });

  it('reports a context that draws the wrong colour as broken', () => {
    const r = probeGlContext(fakeGl({ pixel: [0, 0, 0, 255] }));
    expect(!r.ok && r.reason).toBe('broken');
    expect(!r.ok && r.detail).toMatch(/0,0,0/);
  });
});

describe('the renderer failure', () => {
  it('maps the core statuses, and ignores unknown ones', () => {
    expect(rendererFailure('context_lost', 'x')).toEqual({
      ok: false,
      failure: 'renderer-context-lost',
      detail: 'x',
    });
    expect(rendererFailure('no_context', '')?.ok).toBe(false);
    expect(rendererFailure('context_dead', '')?.ok).toBe(false);
    expect(rendererFailure('shader_failed', '')?.ok).toBe(false);
    expect(rendererFailure('something_else', '')).toBeNull();
  });

  it('reaches a listener registered after the event, once', () => {
    dispatchCoreEvent(EVT_STATE, '{"event":"renderer","status":"context_lost","detail":"gone"}');
    dispatchCoreEvent(EVT_STATE, '{"event":"renderer","status":"no_context","detail":"again"}');
    const heard: string[] = [];
    onRendererFailure((status, detail) => heard.push(`${status}:${detail}`));
    expect(heard).toEqual(['context_lost:gone']);
  });

  it('the error page recommends restarting the browser for a lost context', () => {
    const host = document.createElement('div');
    renderWebGLErrorPage(host, { ok: false, failure: 'worker-context-lost', detail: '<b>' });
    const card = host.querySelector('[role="alert"]');
    expect(card?.getAttribute('data-failure')).toBe('worker-context-lost');
    expect(card?.textContent).toMatch(/restart/i);
    // What the browser reported is escaped, not markup.
    expect(card?.querySelector('b')).toBeNull();
  });
});
