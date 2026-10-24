// The Rage 128 WebGPU takeover's GPU worker.  Owns a GPUDevice and the
// card's overlay canvas; consumes the record stream the emulator thread's
// translator writes into the wasm heap (rage128Protocol.ts /
// rage128_gpu_protocol.h) and turns it into render passes, uploads, fills,
// presents and readbacks.  Built on the Voodoo2's worker
// (voodoo2Gpu.worker.ts): the same transport, the same consume loop, the
// same depth-restore trick; what differs is what a surface holds (any of
// the card's colour formats, expanded to rgba8) and how textures arrive
// (each texel converted and raw, an rg32uint atlas).
//
// Threading: the translator publishes bytes by advancing HEAD and
// notifying; this worker parks in Atomics.waitAsync on HEAD, consumes
// every complete record, and advances TAIL as it goes.  A READBACK, FENCE
// or SHUTDOWN record carries a sequence number the worker stores into ACK
// when done; the translator waits on ACK (the emulator thread blocks).

import {
  PROTOCOL_VERSION,
  MAGIC,
  C_VERSION,
  C_RING_OFF,
  C_RING_SIZE,
  C_RB_OFF,
  C_RB_SIZE,
  C_HEAD,
  C_TAIL,
  C_ACK,
  C_STATUS,
  C_STAT_FRAMES,
  C_STAT_DRAWS,
  C_STAT_FLUSHES,
  C_STAT_PIPELINES,
  C_STAT_READBACKS,
  STATUS_DETACHED,
  STATUS_ATTACHED,
  STATUS_LOST,
  R_PAD,
  R_SURFACE,
  R_SURFACE_FREE,
  R_UPLOAD,
  R_TEX,
  R_TEX_UPLOAD,
  R_TEX_FREE,
  R_DRAW,
  R_FILL,
  R_PRESENT,
  R_LUT,
  R_READBACK,
  R_MODE,
  R_FENCE,
  R_SHUTDOWN,
  ROLE_DEPTH,
  DH_COLOR_ID,
  DH_DEPTH_ID,
  DH_TEX0,
  DH_TEX1,
  DH_PIPE_KEY,
  DH_SX0,
  DH_SY0,
  DH_SX1,
  DH_SY1,
  DH_N_VERTS,
  DH_WORDS,
  PK_BLEND,
  PK_SUBTRACT,
  PK_SRC_SHIFT,
  PK_DST_SHIFT,
  PK_DEPTH,
  PK_DFUNC_SHIFT,
  PK_DEPTH_WRITE,
  PK_WMASK_SHIFT,
  U_BYTES,
  VERTEX_BYTES,
} from './rage128Protocol';
import { recordOk } from '@/bus/mailboxRing';
import { RAGE128_WGSL, PRESENT_WGSL, DEPTH_RESTORE_WGSL } from './rage128.wgsl';

// --- messages from the page -----------------------------------------------

interface InitMsg {
  type: 'init';
  canvas: OffscreenCanvas;
}
interface AttachMsg {
  type: 'attach';
  memory: WebAssembly.Memory | SharedArrayBuffer;
  ctrl: number;
}
interface DetachMsg {
  type: 'detach';
  ctrl: number;
}
interface DebugMsg {
  type: 'debug';
}
type InMsg = InitMsg | AttachMsg | DetachMsg | DebugMsg;

// --- device state -------------------------------------------------------

let device: GPUDevice | null = null;
let canvas: OffscreenCanvas | null = null;
let context: GPUCanvasContext | null = null;
let canvasFormat: GPUTextureFormat = 'bgra8unorm';
let mainModule: GPUShaderModule | null = null;
let mainLayout: GPUBindGroupLayout | null = null;
let mainPipelineLayout: GPUPipelineLayout | null = null;
let presentPipeline: GPURenderPipeline | null = null;
let presentLayout: GPUBindGroupLayout | null = null;
let presentUniform: GPUBuffer | null = null;
let restorePipeline: GPURenderPipeline | null = null;
let restoreLayout: GPUBindGroupLayout | null = null;
let lutTexture: GPUTexture | null = null;
let dummyAtlas: GPUTexture | null = null;
let uniformBuffer: GPUBuffer | null = null;
let vertexBuffer: GPUBuffer | null = null;
const UNIFORM_SLOTS = 8192;
const VERTEX_RING_BYTES = 32 << 20;
let uniformCursor = 0; // slot
let vertexCursor = 0; // bytes

// --- attached region -----------------------------------------------------

let ctrl: Int32Array | null = null; // the control block (Int32 for Atomics.wait)
let u32: Uint32Array | null = null; // the whole heap
let u8: Uint8Array | null = null;
let ctrlBase = 0;
let ringBase = 0;
let ringSize = 0;
let rbBase = 0;
let rbSize = 0;
let consumed = 0; // bytes consumed (mod 2^32)
let attached = false;
let lost = false;
let loopRunning = false;
let readbackPending = false;
let lastKind = -1;

// --- GPU-side resources --------------------------------------------------

interface Surface {
  color: GPUTexture | null; // rgba8unorm: the colour datatype's channels, widened
  depth: GPUTexture | null; // depth16unorm: the 16-bit Z codes
  codes: GPUTexture | null; // r16uint staging for depth uploads and fills
  fmt: number; // the colour datatype (rage128_raster.c's dst_type)
  w: number;
  h: number;
}
const surfaces = new Map<number, Surface>();
const atlases = new Map<number, GPUTexture>();
const pipelines = new Map<number, GPURenderPipeline>();
const bindGroups = new Map<string, GPUBindGroup>();

let encoder: GPUCommandEncoder | null = null;
let pass: GPURenderPassEncoder | null = null;
let passColor = 0;
let passDepth = 0;
let passPipelineKey = -1;
let passBindKey = '';
let passScissor = '';
// Resources the open encoder references: a queue write into one must
// wait for the encoder to be submitted (queue writes run before it).
const usedInEncoder = new Set<string>();

function heap(): GPUAllowSharedBufferSource {
  return u8 as unknown as GPUAllowSharedBufferSource;
}

function stat(word: number, delta: number): void {
  if (ctrl) Atomics.add(ctrl, word, delta);
}

// --- pixel formats -------------------------------------------------------
// The colour datatypes, packed little-endian as VRAM holds them, and the
// rgba8 the surface holds: each channel code widened as the walker's
// dst_unpack widens it (code * 255 / max), so a blend reads what the
// walker would; the inverse rounds, which recovers every widened code.

function bytesPer(fmt: number): number {
  switch (fmt) {
    case 6:
      return 4;
    case 7:
      return 1;
    default:
      return 2;
  }
}

const widen = (code: number, max: number): number => Math.floor((code * 255) / max);
const narrow = (v: number, max: number): number => Math.floor((v * max + 127) / 255);

function unpackPixel(fmt: number, v: number, out: Uint8Array, o: number): void {
  switch (fmt) {
    case 3:
      out[o] = widen((v >> 10) & 0x1f, 31);
      out[o + 1] = widen((v >> 5) & 0x1f, 31);
      out[o + 2] = widen(v & 0x1f, 31);
      out[o + 3] = v & 0x8000 ? 255 : 0;
      return;
    case 4:
      out[o] = widen((v >> 11) & 0x1f, 31);
      out[o + 1] = widen((v >> 5) & 0x3f, 63);
      out[o + 2] = widen(v & 0x1f, 31);
      out[o + 3] = 255;
      return;
    case 6:
      out[o] = (v >>> 16) & 0xff;
      out[o + 1] = (v >>> 8) & 0xff;
      out[o + 2] = v & 0xff;
      out[o + 3] = v >>> 24;
      return;
    case 7:
      out[o] = widen((v >> 5) & 7, 7);
      out[o + 1] = widen((v >> 2) & 7, 7);
      out[o + 2] = widen(v & 3, 3);
      out[o + 3] = 255;
      return;
    case 15:
      out[o] = ((v >> 8) & 0xf) * 17;
      out[o + 1] = ((v >> 4) & 0xf) * 17;
      out[o + 2] = (v & 0xf) * 17;
      out[o + 3] = ((v >> 12) & 0xf) * 17;
      return;
    default:
      out[o] = out[o + 1] = out[o + 2] = v & 0xff;
      out[o + 3] = 255;
  }
}

function packPixel(fmt: number, s: Uint8Array, i: number): number {
  const r = s[i];
  const g = s[i + 1];
  const b = s[i + 2];
  const a = s[i + 3];
  switch (fmt) {
    case 3:
      return (a >= 128 ? 0x8000 : 0) | (narrow(r, 31) << 10) | (narrow(g, 31) << 5) | narrow(b, 31);
    case 4:
      return (narrow(r, 31) << 11) | (narrow(g, 63) << 5) | narrow(b, 31);
    case 6:
      return ((a << 24) | (r << 16) | (g << 8) | b) >>> 0;
    case 7:
      return (narrow(r, 7) << 5) | (narrow(g, 7) << 2) | narrow(b, 3);
    case 15:
      return (narrow(a, 15) << 12) | (narrow(r, 15) << 8) | (narrow(g, 15) << 4) | narrow(b, 15);
    default:
      return g;
  }
}

function readPacked(fmt: number, src: Uint8Array, o: number): number {
  switch (bytesPer(fmt)) {
    case 4:
      return (src[o] | (src[o + 1] << 8) | (src[o + 2] << 16) | (src[o + 3] << 24)) >>> 0;
    case 1:
      return src[o];
    default:
      return src[o] | (src[o + 1] << 8);
  }
}

// --- device setup --------------------------------------------------------

async function initDevice(cv: OffscreenCanvas): Promise<boolean> {
  if (!('gpu' in navigator)) return false;
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) return false;
  const d = await adapter.requestDevice();
  device = d;
  canvas = cv;
  d.lost.then((info) => {
    lost = true;
    if (ctrl) {
      Atomics.store(ctrl, C_STATUS, STATUS_LOST);
      Atomics.notify(ctrl, C_STATUS);
      Atomics.notify(ctrl, C_TAIL);
      Atomics.notify(ctrl, C_ACK);
    }
    postMessage({ type: 'lost', reason: `${info.reason}: ${info.message}` });
  });
  d.addEventListener('uncapturederror', (e) => {
    console.error('[rage128-gpu] uncaptured error:', (e as GPUUncapturedErrorEvent).error.message);
  });
  context = cv.getContext('webgpu') as GPUCanvasContext | null;
  if (!context) return false;
  canvasFormat = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device: d, format: canvasFormat, alphaMode: 'opaque' });
  mainModule = d.createShaderModule({ label: 'rage128-pipe', code: RAGE128_WGSL });
  mainLayout = d.createBindGroupLayout({
    entries: [
      {
        binding: 0,
        visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
        buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: U_BYTES },
      },
      { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'uint' } },
      { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'uint' } },
    ],
  });
  mainPipelineLayout = d.createPipelineLayout({ bindGroupLayouts: [mainLayout] });
  const presentModule = d.createShaderModule({ label: 'rage128-present', code: PRESENT_WGSL });
  presentLayout = d.createBindGroupLayout({
    entries: [
      { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
      { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
      { binding: 2, visibility: GPUShaderStage.FRAGMENT, buffer: { type: 'uniform' } },
    ],
  });
  presentPipeline = d.createRenderPipeline({
    label: 'rage128-present',
    layout: d.createPipelineLayout({ bindGroupLayouts: [presentLayout] }),
    vertex: { module: presentModule, entryPoint: 'vs_present' },
    fragment: {
      module: presentModule,
      entryPoint: 'fs_present',
      targets: [{ format: canvasFormat }],
    },
    primitive: { topology: 'triangle-list' },
  });
  presentUniform = d.createBuffer({
    size: 16,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
  });
  const restoreModule = d.createShaderModule({
    label: 'rage128-depth-restore',
    code: DEPTH_RESTORE_WGSL,
  });
  restoreLayout = d.createBindGroupLayout({
    entries: [{ binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'uint' } }],
  });
  restorePipeline = d.createRenderPipeline({
    label: 'rage128-depth-restore',
    layout: d.createPipelineLayout({ bindGroupLayouts: [restoreLayout] }),
    vertex: { module: restoreModule, entryPoint: 'vs_restore' },
    fragment: { module: restoreModule, entryPoint: 'fs_restore', targets: [] },
    primitive: { topology: 'triangle-list' },
    depthStencil: { format: 'depth16unorm', depthWriteEnabled: true, depthCompare: 'always' },
  });
  lutTexture = d.createTexture({
    size: [256, 3],
    format: 'r8unorm',
    usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
  });
  const identity = new Uint8Array(3 * 256);
  for (let c = 0; c < 3; c++) for (let i = 0; i < 256; i++) identity[c * 256 + i] = i;
  d.queue.writeTexture({ texture: lutTexture }, identity, { bytesPerRow: 256 }, [256, 3]);
  dummyAtlas = d.createTexture({
    size: [1, 1],
    format: 'rg32uint',
    usage: GPUTextureUsage.TEXTURE_BINDING,
  });
  uniformBuffer = d.createBuffer({
    size: UNIFORM_SLOTS * U_BYTES,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
  });
  vertexBuffer = d.createBuffer({
    size: VERTEX_RING_BYTES,
    usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
  });
  return true;
}

// --- pipelines -----------------------------------------------------------

// The blend factor codes (rage128_gpu_protocol.h): the register's own
// 0..9, then the saturate.
const BLEND_FACTORS: GPUBlendFactor[] = [
  'zero',
  'one',
  'src',
  'one-minus-src',
  'src-alpha',
  'one-minus-src-alpha',
  'dst-alpha',
  'one-minus-dst-alpha',
  'dst',
  'one-minus-dst',
  'src-alpha-saturated',
];
// Z_TEST: never, less, less-equal, equal, greater-equal, greater,
// not-equal, always (cmp_op in rage128_raster.c: source OP stored).
const DEPTH_FUNCS: GPUCompareFunction[] = [
  'never',
  'less',
  'less-equal',
  'equal',
  'greater-equal',
  'greater',
  'not-equal',
  'always',
];

function pipelineFor(key: number): GPURenderPipeline {
  const cached = pipelines.get(key);
  if (cached) return cached;
  const d = device!;
  let blend: GPUBlendState | undefined;
  if (key & PK_BLEND) {
    const srcFactor = BLEND_FACTORS[(key >> PK_SRC_SHIFT) & 0xf] ?? 'zero';
    const dstFactor = BLEND_FACTORS[(key >> PK_DST_SHIFT) & 0xf] ?? 'zero';
    const operation: GPUBlendOperation = key & PK_SUBTRACT ? 'subtract' : 'add';
    blend = {
      color: { srcFactor, dstFactor, operation },
      alpha: { srcFactor, dstFactor, operation },
    };
  }
  const hasDepth = (key & PK_DEPTH) !== 0;
  const p = d.createRenderPipeline({
    label: `rage128-pipe-${key.toString(16)}`,
    layout: mainPipelineLayout!,
    vertex: {
      module: mainModule!,
      entryPoint: 'vs_main',
      buffers: [
        {
          arrayStride: VERTEX_BYTES,
          attributes: [
            { shaderLocation: 0, offset: 0, format: 'float32x2' },
            { shaderLocation: 1, offset: 8, format: 'float32' },
            { shaderLocation: 2, offset: 12, format: 'float32x4' },
            { shaderLocation: 3, offset: 28, format: 'float32x3' },
            { shaderLocation: 4, offset: 40, format: 'float32' },
            { shaderLocation: 5, offset: 44, format: 'float32x3' },
            { shaderLocation: 6, offset: 56, format: 'float32x3' },
          ],
        },
      ],
    },
    fragment: {
      module: mainModule!,
      entryPoint: hasDepth ? 'fs_main' : 'fs_nodepth',
      targets: [{ format: 'rgba8unorm', blend, writeMask: (key >> PK_WMASK_SHIFT) & 0xf }],
    },
    primitive: { topology: 'triangle-list', cullMode: 'none' },
    depthStencil: hasDepth
      ? {
          format: 'depth16unorm',
          depthWriteEnabled: (key & PK_DEPTH_WRITE) !== 0,
          depthCompare: DEPTH_FUNCS[(key >> PK_DFUNC_SHIFT) & 7],
        }
      : undefined,
  });
  pipelines.set(key, p);
  stat(C_STAT_PIPELINES, 1);
  return p;
}

function bindGroupFor(tex0: number, tex1: number): GPUBindGroup {
  const k = `${tex0}:${tex1}`;
  const cached = bindGroups.get(k);
  if (cached) return cached;
  const t0 = atlases.get(tex0) ?? dummyAtlas!;
  const t1 = atlases.get(tex1) ?? dummyAtlas!;
  const bg = device!.createBindGroup({
    layout: mainLayout!,
    entries: [
      { binding: 0, resource: { buffer: uniformBuffer!, size: U_BYTES } },
      { binding: 1, resource: t0.createView() },
      { binding: 2, resource: t1.createView() },
    ],
  });
  bindGroups.set(k, bg);
  return bg;
}

function dropBindGroupsFor(texId: number): void {
  for (const k of [...bindGroups.keys()]) {
    const [a, b] = k.split(':');
    if (Number(a) === texId || Number(b) === texId) bindGroups.delete(k);
  }
}

// --- passes and submission ----------------------------------------------

function endPass(): void {
  if (pass) {
    pass.end();
    pass = null;
  }
  passColor = 0;
  passDepth = 0;
  passPipelineKey = -1;
  passBindKey = '';
  passScissor = '';
}

function flush(): void {
  endPass();
  if (encoder) {
    device!.queue.submit([encoder.finish()]);
    encoder = null;
    usedInEncoder.clear();
    stat(C_STAT_FLUSHES, 1);
  }
}

function ensureEncoder(): GPUCommandEncoder {
  if (!encoder) encoder = device!.createCommandEncoder();
  return encoder;
}

function beginPass(colorId: number, depthId: number): boolean {
  if (pass && passColor === colorId && passDepth === depthId) return true;
  endPass();
  const ct = surfaces.get(colorId);
  const dt = depthId ? surfaces.get(depthId) : undefined;
  if (!ct?.color || (depthId && !dt?.depth)) return false;
  pass = ensureEncoder().beginRenderPass({
    colorAttachments: [{ view: ct.color.createView(), loadOp: 'load', storeOp: 'store' }],
    depthStencilAttachment: dt?.depth
      ? { view: dt.depth.createView(), depthLoadOp: 'load', depthStoreOp: 'store' }
      : undefined,
  });
  passColor = colorId;
  passDepth = depthId;
  usedInEncoder.add(`s${colorId}`);
  if (depthId) usedInEncoder.add(`s${depthId}`);
  return true;
}

// Write rows of 16-bit Z codes into a depth surface: stage them, then the
// restore pass writes them through frag_depth over [x, x+w) x [y, y+h).
function restoreDepth(id: number, s: Surface, x: number, y: number, w: number, h: number): void {
  endPass();
  const p = ensureEncoder().beginRenderPass({
    colorAttachments: [],
    depthStencilAttachment: {
      view: s.depth!.createView(),
      depthLoadOp: 'load',
      depthStoreOp: 'store',
    },
  });
  p.setPipeline(restorePipeline!);
  p.setBindGroup(
    0,
    device!.createBindGroup({
      layout: restoreLayout!,
      entries: [{ binding: 0, resource: s.codes!.createView() }],
    }),
  );
  p.setScissorRect(x, y, w, h);
  p.draw(3);
  p.end();
  usedInEncoder.add(`s${id}`);
}

// --- record handlers ------------------------------------------------------

function freeSurface(s: Surface): void {
  s.color?.destroy();
  s.depth?.destroy();
  s.codes?.destroy();
}

function onSurface(id: number, role: number, fmt: number, w: number, h: number): void {
  const d = device!;
  const old = surfaces.get(id);
  if (old) {
    if (usedInEncoder.has(`s${id}`)) flush();
    freeSurface(old);
  }
  const s: Surface = { color: null, depth: null, codes: null, fmt, w, h };
  if (role === ROLE_DEPTH) {
    s.depth = d.createTexture({
      label: `rage128-depth-${id}`,
      size: [w, h],
      format: 'depth16unorm',
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC,
    });
    s.codes = d.createTexture({
      label: `rage128-depth-codes-${id}`,
      size: [w, h],
      format: 'r16uint',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });
  } else {
    s.color = d.createTexture({
      label: `rage128-color-${id}`,
      size: [w, h],
      format: 'rgba8unorm',
      usage:
        GPUTextureUsage.RENDER_ATTACHMENT |
        GPUTextureUsage.COPY_SRC |
        GPUTextureUsage.COPY_DST |
        GPUTextureUsage.TEXTURE_BINDING,
    });
  }
  surfaces.set(id, s);
}

function onSurfaceFree(id: number): void {
  const s = surfaces.get(id);
  if (!s) return;
  if (usedInEncoder.has(`s${id}`)) flush();
  freeSurface(s);
  surfaces.delete(id);
}

// Whole rows of packed pixels from VRAM.
function onUpload(id: number, y: number, w: number, h: number, payload: number): void {
  const s = surfaces.get(id);
  if (!s) return;
  if (usedInEncoder.has(`s${id}`)) flush();
  if (s.depth) {
    device!.queue.writeTexture(
      { texture: s.codes!, origin: [0, y] },
      heap(),
      { offset: payload, bytesPerRow: w * 2 },
      [w, h],
    );
    restoreDepth(id, s, 0, y, w, h);
    return;
  }
  const bpp = bytesPer(s.fmt);
  const rgba = new Uint8Array(w * h * 4);
  const src = u8!;
  for (let i = 0, n = w * h; i < n; i++)
    unpackPixel(s.fmt, readPacked(s.fmt, src, payload + i * bpp), rgba, i * 4);
  device!.queue.writeTexture({ texture: s.color!, origin: [0, y] }, rgba, { bytesPerRow: w * 4 }, [
    w,
    h,
  ]);
}

function onFill(
  colorId: number,
  depthId: number,
  x0: number,
  y0: number,
  x1: number,
  y1: number,
  value: number,
): void {
  const id = colorId || depthId;
  const s = surfaces.get(id);
  const w = x1 - x0;
  const h = y1 - y0;
  if (!s || w <= 0 || h <= 0) return;
  if (usedInEncoder.has(`s${id}`)) flush();
  if (s.depth) {
    device!.queue.writeTexture(
      { texture: s.codes!, origin: [x0, y0] },
      new Uint16Array(w * h).fill(value & 0xffff),
      { bytesPerRow: w * 2 },
      [w, h],
    );
    restoreDepth(id, s, x0, y0, w, h);
    return;
  }
  const px = new Uint8Array(4);
  unpackPixel(s.fmt, value >>> 0, px, 0);
  const rgba = new Uint8Array(w * h * 4);
  for (let i = 0; i < w * h; i++) rgba.set(px, i * 4);
  device!.queue.writeTexture(
    { texture: s.color!, origin: [x0, y0] },
    rgba,
    { bytesPerRow: w * 4 },
    [w, h],
  );
}

function onTex(id: number, w: number, h: number): void {
  const old = atlases.get(id);
  if (old) {
    if (usedInEncoder.has(`x${id}`)) flush();
    old.destroy();
    dropBindGroupsFor(id);
  }
  atlases.set(
    id,
    device!.createTexture({
      label: `rage128-atlas-${id}`,
      size: [w, h],
      format: 'rg32uint',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    }),
  );
}

function onTexUpload(id: number, y: number, w: number, h: number, payload: number): void {
  const tex = atlases.get(id);
  if (!tex) return;
  if (usedInEncoder.has(`x${id}`)) flush();
  device!.queue.writeTexture(
    { texture: tex, origin: [0, y] },
    heap(),
    { offset: payload, bytesPerRow: w * 8 },
    [w, h],
  );
}

function onTexFree(id: number): void {
  const tex = atlases.get(id);
  if (!tex) return;
  if (usedInEncoder.has(`x${id}`)) flush();
  tex.destroy();
  atlases.delete(id);
  dropBindGroupsFor(id);
}

function onDraw(hdrOff: number): void {
  const hdr = hdrOff >> 2;
  const colorId = u32![hdr + DH_COLOR_ID];
  const depthId = u32![hdr + DH_DEPTH_ID];
  const tex0 = u32![hdr + DH_TEX0];
  const tex1 = u32![hdr + DH_TEX1];
  const key = u32![hdr + DH_PIPE_KEY];
  const sx0 = u32![hdr + DH_SX0] | 0;
  const sy0 = u32![hdr + DH_SY0] | 0;
  const sx1 = u32![hdr + DH_SX1] | 0;
  const sy1 = u32![hdr + DH_SY1] | 0;
  const nVerts = u32![hdr + DH_N_VERTS];
  if (!nVerts || sx1 <= sx0 || sy1 <= sy0) return;
  const uniformOff = hdrOff + DH_WORDS * 4;
  const vertexOff = uniformOff + U_BYTES;
  const vertexBytes = nVerts * VERTEX_BYTES;
  // The uniform and vertex rings: queue writes land before the open
  // encoder, so a wrap submits it first.
  if (uniformCursor >= UNIFORM_SLOTS) {
    flush();
    uniformCursor = 0;
  }
  if (vertexCursor + vertexBytes > VERTEX_RING_BYTES) {
    flush();
    vertexCursor = 0;
  }
  const d = device!;
  d.queue.writeBuffer(uniformBuffer!, uniformCursor * U_BYTES, heap(), uniformOff, U_BYTES);
  d.queue.writeBuffer(vertexBuffer!, vertexCursor, heap(), vertexOff, vertexBytes);
  if (!beginPass(colorId, depthId)) return;
  const p = pass!;
  if (key !== passPipelineKey) {
    p.setPipeline(pipelineFor(key));
    passPipelineKey = key;
  }
  const bindKey = `${tex0}:${tex1}`;
  if (bindKey !== passBindKey) {
    if (tex0) usedInEncoder.add(`x${tex0}`);
    if (tex1) usedInEncoder.add(`x${tex1}`);
    passBindKey = bindKey;
  }
  p.setBindGroup(0, bindGroupFor(tex0, tex1), [uniformCursor * U_BYTES]);
  const scissor = `${sx0},${sy0},${sx1},${sy1}`;
  if (scissor !== passScissor) {
    p.setScissorRect(sx0, sy0, sx1 - sx0, sy1 - sy0);
    passScissor = scissor;
  }
  p.setVertexBuffer(0, vertexBuffer!, vertexCursor, vertexBytes);
  p.draw(nVerts);
  uniformCursor++;
  vertexCursor += vertexBytes;
  stat(C_STAT_DRAWS, 1);
}

// The overlay is shown by the page only after the first present that
// follows a MODE(on) (so a new frame is what appears), and hidden by the
// display path underneath once its own canvas holds a fresh frame
// (em_video.c) — never from here.
let showAfterPresent = false;
let overlayW = 0;
let overlayH = 0;

function onMode(engaged: number, w: number, h: number): void {
  showAfterPresent = engaged !== 0;
  overlayW = w;
  overlayH = h;
}

function onPresent(id: number, w: number, h: number, rb: number, gb: number, bb: number): void {
  const s = surfaces.get(id);
  if (!s?.color || !canvas || !context) return;
  endPass();
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  device!.queue.writeBuffer(presentUniform!, 0, new Uint32Array([rb, gb, bb, 0]));
  const enc = ensureEncoder();
  const p = enc.beginRenderPass({
    colorAttachments: [
      {
        view: context.getCurrentTexture().createView(),
        loadOp: 'clear',
        clearValue: [0, 0, 0, 1],
        storeOp: 'store',
      },
    ],
  });
  p.setPipeline(presentPipeline!);
  p.setBindGroup(
    0,
    device!.createBindGroup({
      layout: presentLayout!,
      entries: [
        { binding: 0, resource: s.color.createView() },
        { binding: 1, resource: lutTexture!.createView() },
        { binding: 2, resource: { buffer: presentUniform! } },
      ],
    }),
  );
  p.draw(3);
  p.end();
  usedInEncoder.add(`s${id}`);
  flush();
  stat(C_STAT_FRAMES, 1);
  if (showAfterPresent) {
    showAfterPresent = false;
    postMessage({ type: 'mode', engaged: true, w: overlayW || w, h: overlayH || h });
  }
}

function onLut(payload: number): void {
  device!.queue.writeTexture(
    { texture: lutTexture! },
    heap(),
    { offset: payload, bytesPerRow: 256 },
    [256, 3],
  );
}

// Rows [y0, y1) of a surface into the readback area, packed as VRAM holds
// them (colour in its datatype, Z as 16-bit codes), then ACK.  A depth
// format copies only whole, so the depth path copies the whole texture.
async function onReadback(seq: number, id: number, y0: number, y1: number): Promise<void> {
  const s = surfaces.get(id);
  const d = device!;
  const bpp = s ? (s.depth ? 2 : bytesPer(s.fmt)) : 0;
  if (s && y1 > y0 && rbSize >= s.w * (y1 - y0) * bpp) {
    flush();
    const rows = y1 - y0;
    const texBpp = s.color ? 4 : 2;
    const bytesPerRow = (s.w * texBpp + 255) & ~255;
    const copyRows = s.color ? rows : s.h;
    const staging = d.createBuffer({
      size: bytesPerRow * copyRows,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    const enc = d.createCommandEncoder();
    if (s.color) {
      enc.copyTextureToBuffer(
        { texture: s.color, origin: [0, y0] },
        { buffer: staging, bytesPerRow },
        [s.w, rows],
      );
    } else {
      enc.copyTextureToBuffer(
        { texture: s.depth!, origin: [0, 0], aspect: 'depth-only' },
        { buffer: staging, bytesPerRow },
        [s.w, s.h],
      );
    }
    d.queue.submit([enc.finish()]);
    readbackPending = true;
    await staging.mapAsync(GPUMapMode.READ);
    readbackPending = false;
    const src = new Uint8Array(staging.getMappedRange());
    const out = u8!;
    if (s.color) {
      for (let y = 0; y < rows; y++) {
        const ro = y * bytesPerRow;
        const oo = rbBase + y * s.w * bpp;
        for (let x = 0; x < s.w; x++) {
          const v = packPixel(s.fmt, src, ro + x * 4);
          for (let b = 0; b < bpp; b++) out[oo + x * bpp + b] = (v >>> (8 * b)) & 0xff;
        }
      }
    } else {
      for (let y = 0; y < rows; y++) {
        const ro = (y + y0) * bytesPerRow;
        out.set(src.subarray(ro, ro + s.w * 2), rbBase + y * s.w * 2);
      }
    }
    staging.unmap();
    staging.destroy();
    stat(C_STAT_READBACKS, 1);
  }
  ack(seq);
}

function ack(seq: number): void {
  if (!ctrl) return;
  Atomics.store(ctrl, C_ACK, seq | 0);
  Atomics.notify(ctrl, C_ACK);
}

function freeEverything(): void {
  endPass();
  encoder = null;
  usedInEncoder.clear();
  for (const s of surfaces.values()) freeSurface(s);
  surfaces.clear();
  for (const t of atlases.values()) t.destroy();
  atlases.clear();
  bindGroups.clear();
}

// --- the consume loop --------------------------------------------------

function advanceTail(): void {
  Atomics.store(ctrl!, C_TAIL, consumed | 0);
  Atomics.notify(ctrl!, C_TAIL);
}

async function drain(head: number): Promise<boolean> {
  const mask = ringSize - 1;
  while (consumed !== head) {
    const off = consumed & mask;
    const at = ringBase + off;
    const kind = u32![at >> 2];
    const len = u32![(at >> 2) + 1];
    if (!recordOk(len, off, ringSize, (head - consumed) >>> 0)) {
      console.error('[rage128-gpu] corrupt record', kind, len);
      return false;
    }
    const p = (at >> 2) + 2;
    lastKind = kind;
    switch (kind) {
      case R_PAD:
        break;
      case R_SURFACE:
        onSurface(u32![p], u32![p + 1], u32![p + 2], u32![p + 3], u32![p + 4]);
        break;
      case R_SURFACE_FREE:
        onSurfaceFree(u32![p]);
        break;
      case R_UPLOAD:
        onUpload(u32![p], u32![p + 1], u32![p + 2], u32![p + 3], at + 24);
        break;
      case R_TEX:
        onTex(u32![p], u32![p + 1], u32![p + 2]);
        break;
      case R_TEX_UPLOAD:
        onTexUpload(u32![p], u32![p + 1], u32![p + 2], u32![p + 3], at + 24);
        break;
      case R_TEX_FREE:
        onTexFree(u32![p]);
        break;
      case R_DRAW:
        onDraw(at + 8);
        break;
      case R_FILL:
        onFill(
          u32![p],
          u32![p + 1],
          u32![p + 2],
          u32![p + 3],
          u32![p + 4],
          u32![p + 5],
          u32![p + 6],
        );
        break;
      case R_PRESENT:
        onPresent(u32![p], u32![p + 1], u32![p + 2], u32![p + 3], u32![p + 4], u32![p + 5]);
        break;
      case R_LUT:
        onLut(at + 8);
        break;
      case R_READBACK:
        await onReadback(u32![p], u32![p + 1], u32![p + 2], u32![p + 3]);
        break;
      case R_MODE:
        onMode(u32![p], u32![p + 1], u32![p + 2]);
        break;
      case R_FENCE:
        flush();
        ack(u32![p]);
        break;
      case R_SHUTDOWN: {
        flush();
        freeEverything();
        const seq = u32![p];
        consumed = (consumed + len) >>> 0;
        advanceTail();
        attached = false;
        Atomics.store(ctrl!, C_STATUS, STATUS_DETACHED);
        ack(seq);
        return false;
      }
      default:
        console.error('[rage128-gpu] unknown record kind', kind);
        return false;
    }
    consumed = (consumed + len) >>> 0;
    advanceTail();
  }
  return true;
}

async function loop(): Promise<void> {
  if (loopRunning) return;
  loopRunning = true;
  try {
    while (attached && !lost) {
      const head = Atomics.load(ctrl!, C_HEAD) >>> 0;
      if (head === consumed) {
        const r = Atomics.waitAsync(ctrl!, C_HEAD, head | 0, 1000);
        if (r.async) await r.value;
        continue;
      }
      if (!(await drain(head))) break;
    }
  } catch (e) {
    console.error('[rage128-gpu] worker loop failed:', e);
    if (ctrl) {
      Atomics.store(ctrl, C_STATUS, STATUS_LOST);
      Atomics.notify(ctrl, C_STATUS);
      Atomics.notify(ctrl, C_ACK);
      Atomics.notify(ctrl, C_TAIL);
    }
  } finally {
    loopRunning = false;
  }
}

function attach(memory: WebAssembly.Memory | SharedArrayBuffer, ctrlPtr: number): void {
  const buffer = memory instanceof SharedArrayBuffer ? memory : memory.buffer;
  u8 = new Uint8Array(buffer);
  u32 = new Uint32Array(buffer);
  ctrl = new Int32Array(buffer, ctrlPtr, 32);
  ctrlBase = ctrlPtr;
  if (u32[ctrlPtr >> 2] >>> 0 !== MAGIC || u32[(ctrlPtr >> 2) + C_VERSION] !== PROTOCOL_VERSION) {
    console.error(
      '[rage128-gpu] control block mismatch',
      u32[ctrlPtr >> 2].toString(16),
      u32[(ctrlPtr >> 2) + C_VERSION],
    );
    return;
  }
  ringBase = ctrlBase + u32[(ctrlPtr >> 2) + C_RING_OFF];
  ringSize = u32[(ctrlPtr >> 2) + C_RING_SIZE];
  rbBase = ctrlBase + u32[(ctrlPtr >> 2) + C_RB_OFF];
  rbSize = u32[(ctrlPtr >> 2) + C_RB_SIZE];
  consumed = Atomics.load(ctrl, C_HEAD) >>> 0;
  uniformCursor = 0;
  vertexCursor = 0;
  freeEverything();
  attached = true;
  Atomics.store(ctrl, C_STATUS, STATUS_ATTACHED);
  Atomics.notify(ctrl, C_STATUS);
  void loop();
}

function detach(): void {
  if (!attached) return;
  attached = false;
  flush();
  freeEverything();
  if (ctrl) {
    Atomics.store(ctrl, C_STATUS, STATUS_DETACHED);
    Atomics.notify(ctrl, C_STATUS);
  }
  ctrl = null;
}

// Attach requests that arrive while the device is still being created
// (the page starts this worker lazily, on the card's first attach).
let deviceReady: Promise<boolean> | null = null;

self.onmessage = (ev: MessageEvent<InMsg>) => {
  const msg = ev.data;
  if (msg.type === 'init') {
    deviceReady = initDevice(msg.canvas).catch(() => false);
    void deviceReady.then((ok) => postMessage({ type: ok ? 'ready' : 'unavailable' }));
  } else if (msg.type === 'attach') {
    void (deviceReady ?? Promise.resolve(false)).then((ok) => {
      if (ok && !lost) attach(msg.memory, msg.ctrl);
    });
  } else if (msg.type === 'detach') {
    if (ctrl && ctrlBase === msg.ctrl) detach();
  } else if (msg.type === 'debug') {
    postMessage({
      type: 'debug',
      attached,
      lost,
      loopRunning,
      consumed,
      head: ctrl ? Atomics.load(ctrl, C_HEAD) >>> 0 : -1,
      ack: ctrl ? Atomics.load(ctrl, C_ACK) : -1,
      status: ctrl ? Atomics.load(ctrl, C_STATUS) : -1,
      readbackPending,
      surfaces: surfaces.size,
      atlases: atlases.size,
      pipelines: pipelines.size,
      lastKind,
    });
  }
};
