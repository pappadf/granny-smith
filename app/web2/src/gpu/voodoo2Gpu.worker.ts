// The Voodoo2 WebGPU takeover's GPU worker.  Owns the GPUDevice and the
// overlay canvas; consumes the record stream the emulator's raster
// pthread writes into the wasm heap (voodoo2Protocol.ts /
// voodoo2_gpu_protocol.h) and turns it into render passes, texture
// uploads, presents and readbacks.
//
// Why a worker of its own: WebGPU readback is asynchronous (mapAsync),
// the emulator pthread blocks in Atomics.wait at fences, and the raster
// pthread runs a C loop — neither can service a promise.  This worker's
// event loop does nothing else.  It touches emulator memory only through
// the regions the control block names; it never calls into the module.
//
// The transport -- the page's messages, attaching the region, the consume
// loop, ACK and TAIL -- is takeoverWorker.ts, shared with the Rage 128's
// worker; this file is the Voodoo2's device, pipelines and record handlers.

import {
  PROTOCOL_VERSION,
  MAGIC,
  C_STAT_FRAMES,
  C_STAT_DRAWS,
  C_STAT_FLUSHES,
  C_STAT_PIPELINES,
  C_STAT_READBACKS,
  R_TARGET,
  R_TARGET_FREE,
  R_UPLOAD,
  R_TEX,
  R_TEX_UPLOAD,
  R_TEX_FREE,
  R_DRAW,
  R_PRESENT,
  R_GAMMA,
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
  PK_SRC_SHIFT,
  PK_DST_SHIFT,
  PK_DEPTH_TEST,
  PK_DFUNC_SHIFT,
  PK_DEPTH_WRITE,
  PK_COLOR_WRITE,
  U_BYTES,
  VERTEX_BYTES,
} from './voodoo2Protocol';
import {
  ack,
  heap,
  markLost,
  rbBase,
  rbSize,
  runTakeoverWorker,
  stat,
  u32,
  u8,
} from './takeoverWorker';
import { VOODOO2_WGSL, PRESENT_WGSL, DEPTH_RESTORE_WGSL } from './voodoo2.wgsl';

// --- device state -------------------------------------------------------

let device: GPUDevice | null = null;
let canvas: OffscreenCanvas | null = null;
let context: GPUCanvasContext | null = null;
let canvasFormat: GPUTextureFormat = 'bgra8unorm';
let mainModule: GPUShaderModule | null = null;
let presentModule: GPUShaderModule | null = null;
let mainLayout: GPUBindGroupLayout | null = null;
let mainPipelineLayout: GPUPipelineLayout | null = null;
let presentPipeline: GPURenderPipeline | null = null;
let presentLayout: GPUBindGroupLayout | null = null;
let restorePipeline: GPURenderPipeline | null = null;
let restoreLayout: GPUBindGroupLayout | null = null;
let lutTexture: GPUTexture | null = null;
let dummyTexture: GPUTexture | null = null;
let uniformBuffer: GPUBuffer | null = null;
let vertexBuffer: GPUBuffer | null = null;
const UNIFORM_SLOTS = 8192;
const VERTEX_RING_BYTES = 32 << 20;
let uniformCursor = 0; // slot
let vertexCursor = 0; // bytes

// --- per-region state (the region itself: takeoverWorker.ts) -------------

let readbackPending = false; // a readback's mapAsync is in flight

// --- GPU-side resources --------------------------------------------------

interface Target {
  color: GPUTexture | null;
  depth: GPUTexture | null;
  // Depth uploads stage their rows here (r16uint takes partial copies;
  // depth16unorm does not) and a restore pass writes them through.
  codes: GPUTexture | null;
  w: number;
  h: number;
  role: number;
}
const targets = new Map<number, Target>();
const textures = new Map<number, GPUTexture>();
const pipelines = new Map<number, GPURenderPipeline>();
const bindGroups = new Map<string, GPUBindGroup>();

// The open command encoder and pass.
let encoder: GPUCommandEncoder | null = null;
let pass: GPURenderPassEncoder | null = null;
let passColor = 0;
let passDepth = 0;
let passPipelineKey = -1;
let passBindKey = '';
let passScissor = '';
// Textures/targets referenced by the open encoder: an upload into one
// of them must wait for the encoder to be submitted (queue writes run
// before submitted work) -- the pass boundary.
const usedInEncoder = new Set<string>();

// --- device setup --------------------------------------------------------

// Why initDevice answered false, for the page's log (voodoo2Gpu.svelte.ts).
let unavailableReason = '';

async function initDevice(cv: OffscreenCanvas): Promise<boolean> {
  if (!('gpu' in navigator)) {
    unavailableReason = 'no WebGPU in this browser (navigator.gpu is missing in workers)';
    return false;
  }
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) {
    // The API is there but no GPU answers: blocklisted, off, or a stuck
    // GPU process -- the last one clears only with a browser restart.
    unavailableReason = 'navigator.gpu.requestAdapter() returned null';
    return false;
  }
  const d = await adapter.requestDevice();
  device = d;
  canvas = cv;
  // The device is gone: tell the translator and the page.
  d.lost.then((info) => markLost(`${info.reason}: ${info.message}`));
  d.addEventListener('uncapturederror', (e) => {
    console.error('[voodoo2-gpu] uncaptured error:', (e as GPUUncapturedErrorEvent).error.message);
  });
  context = cv.getContext('webgpu') as GPUCanvasContext | null;
  if (!context) {
    unavailableReason = 'getContext("webgpu") returned null on the overlay canvas';
    return false;
  }
  canvasFormat = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device: d, format: canvasFormat, alphaMode: 'opaque' });
  mainModule = d.createShaderModule({ label: 'voodoo2-pipe', code: VOODOO2_WGSL });
  presentModule = d.createShaderModule({ label: 'voodoo2-present', code: PRESENT_WGSL });
  mainLayout = d.createBindGroupLayout({
    entries: [
      {
        binding: 0,
        visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
        buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: U_BYTES },
      },
      {
        binding: 1,
        visibility: GPUShaderStage.FRAGMENT,
        texture: { sampleType: 'float', viewDimension: '2d' },
      },
      {
        binding: 2,
        visibility: GPUShaderStage.FRAGMENT,
        texture: { sampleType: 'float', viewDimension: '2d' },
      },
    ],
  });
  mainPipelineLayout = d.createPipelineLayout({ bindGroupLayouts: [mainLayout] });
  presentLayout = d.createBindGroupLayout({
    entries: [
      {
        binding: 0,
        visibility: GPUShaderStage.FRAGMENT,
        texture: { sampleType: 'float', viewDimension: '2d' },
      },
      {
        binding: 1,
        visibility: GPUShaderStage.FRAGMENT,
        texture: { sampleType: 'float', viewDimension: '2d' },
      },
    ],
  });
  presentPipeline = d.createRenderPipeline({
    label: 'voodoo2-present',
    layout: d.createPipelineLayout({ bindGroupLayouts: [presentLayout] }),
    vertex: { module: presentModule, entryPoint: 'vs_present' },
    fragment: {
      module: presentModule,
      entryPoint: 'fs_present',
      targets: [{ format: canvasFormat }],
    },
    primitive: { topology: 'triangle-list' },
  });
  const restoreModule = d.createShaderModule({
    label: 'voodoo2-depth-restore',
    code: DEPTH_RESTORE_WGSL,
  });
  restoreLayout = d.createBindGroupLayout({
    entries: [
      {
        binding: 0,
        visibility: GPUShaderStage.FRAGMENT,
        texture: { sampleType: 'uint', viewDimension: '2d' },
      },
    ],
  });
  restorePipeline = d.createRenderPipeline({
    label: 'voodoo2-depth-restore',
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
  dummyTexture = d.createTexture({
    size: [1, 1],
    format: 'rgba8unorm',
    usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
  });
  d.queue.writeTexture(
    { texture: dummyTexture },
    new Uint8Array([0, 0, 0, 255]),
    { bytesPerRow: 4 },
    [1, 1],
  );
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

const BLEND_FACTORS: GPUBlendFactor[] = [
  'zero',
  'src-alpha',
  'dst',
  'dst-alpha',
  'one',
  'one-minus-src-alpha',
  'one-minus-dst',
  'one-minus-dst-alpha',
];
const BLEND_FACTORS_DST: GPUBlendFactor[] = [
  'zero',
  'src-alpha',
  'src',
  'dst-alpha',
  'one',
  'one-minus-src-alpha',
  'one-minus-src',
  'one-minus-dst-alpha',
];
const DEPTH_FUNCS: GPUCompareFunction[] = [
  'never',
  'less',
  'equal',
  'less-equal',
  'greater',
  'not-equal',
  'greater-equal',
  'always',
];

// Build (or fetch) the pipeline for a key: the blend state, the depth
// compare/write, the colour write mask — nothing else varies.
function pipelineFor(key: number, hasColor: boolean, hasDepth: boolean): GPURenderPipeline {
  const full = key | (hasColor ? 1 << 30 : 0) | (hasDepth ? 1 << 29 : 0);
  const cached = pipelines.get(full);
  if (cached) return cached;
  const d = device!;
  let blend: GPUBlendState | undefined;
  if (key & PK_BLEND) {
    const s = (key >> PK_SRC_SHIFT) & 0xf;
    const t = (key >> PK_DST_SHIFT) & 0xf;
    const srcFactor: GPUBlendFactor = s === 0xf ? 'src-alpha-saturated' : BLEND_FACTORS[s & 7];
    const dstFactor: GPUBlendFactor = BLEND_FACTORS_DST[t & 7];
    blend = {
      color: { srcFactor, dstFactor, operation: 'add' },
      // Destination alpha planes are not enabled: the attachment keeps 1.
      alpha: { srcFactor: 'zero', dstFactor: 'one', operation: 'add' },
    };
  }
  const targetsDesc: GPUColorTargetState[] = hasColor
    ? [{ format: 'rgba8unorm', blend, writeMask: key & PK_COLOR_WRITE ? GPUColorWrite.ALL : 0 }]
    : [];
  const depthStencil: GPUDepthStencilState | undefined = hasDepth
    ? {
        format: 'depth16unorm',
        depthWriteEnabled: (key & PK_DEPTH_WRITE) !== 0,
        depthCompare: key & PK_DEPTH_TEST ? DEPTH_FUNCS[(key >> PK_DFUNC_SHIFT) & 7] : 'always',
      }
    : undefined;
  const p = d.createRenderPipeline({
    label: `voodoo2-pipe-${full.toString(16)}`,
    layout: mainPipelineLayout!,
    vertex: {
      module: mainModule!,
      entryPoint: 'vs_main',
      buffers: [
        {
          arrayStride: VERTEX_BYTES,
          attributes: [
            { shaderLocation: 0, offset: 0, format: 'float32x2' },
            { shaderLocation: 1, offset: 8, format: 'float32x2' },
            { shaderLocation: 2, offset: 16, format: 'float32x4' },
            { shaderLocation: 3, offset: 32, format: 'float32x3' },
            { shaderLocation: 4, offset: 44, format: 'float32x3' },
          ],
        },
      ],
    },
    fragment: { module: mainModule!, entryPoint: 'fs_main', targets: targetsDesc },
    primitive: { topology: 'triangle-list', cullMode: 'none' },
    depthStencil,
  });
  pipelines.set(full, p);
  stat(C_STAT_PIPELINES, 1);
  return p;
}

function bindGroupFor(tex0: number, tex1: number): GPUBindGroup {
  const k = `${tex0}:${tex1}`;
  const cached = bindGroups.get(k);
  if (cached) return cached;
  const t0 = textures.get(tex0) ?? dummyTexture!;
  const t1 = textures.get(tex1) ?? dummyTexture!;
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

// Submit everything encoded so far.
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
  const ct = colorId ? targets.get(colorId) : undefined;
  const dt = depthId ? targets.get(depthId) : undefined;
  if (!ct?.color && !dt?.depth) return false;
  const enc = ensureEncoder();
  const desc: GPURenderPassDescriptor = {
    colorAttachments: ct?.color
      ? [{ view: ct.color.createView(), loadOp: 'load', storeOp: 'store' }]
      : [],
    depthStencilAttachment: dt?.depth
      ? { view: dt.depth.createView(), depthLoadOp: 'load', depthStoreOp: 'store' }
      : undefined,
  };
  pass = enc.beginRenderPass(desc);
  passColor = colorId;
  passDepth = depthId;
  passPipelineKey = -1;
  passBindKey = '';
  passScissor = '';
  if (colorId) usedInEncoder.add(`t${colorId}`);
  if (depthId) usedInEncoder.add(`t${depthId}`);
  return true;
}

// --- record handlers ------------------------------------------------------

function onTarget(id: number, role: number, w: number, h: number): void {
  const d = device!;
  const old = targets.get(id);
  if (old) {
    old.color?.destroy();
    old.depth?.destroy();
    old.codes?.destroy();
  }
  const t: Target = { color: null, depth: null, codes: null, w, h, role };
  if (role === ROLE_DEPTH) {
    t.depth = d.createTexture({
      label: `voodoo2-depth-${id}`,
      size: [w, h],
      format: 'depth16unorm',
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC,
    });
    t.codes = d.createTexture({
      label: `voodoo2-depth-codes-${id}`,
      size: [w, h],
      format: 'r16uint',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });
  } else {
    t.color = d.createTexture({
      label: `voodoo2-color-${id}`,
      size: [w, h],
      format: 'rgba8unorm',
      usage:
        GPUTextureUsage.RENDER_ATTACHMENT |
        GPUTextureUsage.COPY_SRC |
        GPUTextureUsage.COPY_DST |
        GPUTextureUsage.TEXTURE_BINDING,
    });
  }
  targets.set(id, t);
}
function onTargetFree(id: number): void {
  const t = targets.get(id);
  if (!t) return;
  if (usedInEncoder.has(`t${id}`)) flush();
  t.color?.destroy();
  t.depth?.destroy();
  t.codes?.destroy();
  targets.delete(id);
}
// Rows of a target from the ring: rgba8 for colour, u16 codes for depth.
function onUpload(id: number, x: number, y: number, w: number, h: number, payload: number): void {
  const t = targets.get(id);
  if (!t) return;
  if (usedInEncoder.has(`t${id}`)) flush();
  if (t.color) {
    device!.queue.writeTexture(
      { texture: t.color, origin: [x, y] },
      heap(),
      { offset: payload, bytesPerRow: w * 4 },
      [w, h],
    );
    return;
  }
  if (!t.depth || !t.codes) return;
  // Stage the codes, then write them into the depth attachment through
  // the restore pass (a depth format takes no partial buffer copy).
  device!.queue.writeTexture(
    { texture: t.codes, origin: [x, y] },
    heap(),
    { offset: payload, bytesPerRow: w * 2 },
    [w, h],
  );
  endPass();
  const enc = ensureEncoder();
  const p = enc.beginRenderPass({
    colorAttachments: [],
    depthStencilAttachment: {
      view: t.depth.createView(),
      depthLoadOp: 'load',
      depthStoreOp: 'store',
    },
  });
  p.setPipeline(restorePipeline!);
  p.setBindGroup(
    0,
    device!.createBindGroup({
      layout: restoreLayout!,
      entries: [{ binding: 0, resource: t.codes.createView() }],
    }),
  );
  p.setScissorRect(x, y, w, h);
  p.draw(3);
  p.end();
  usedInEncoder.add(`t${id}`);
}
function onTex(id: number, w0: number, h0: number, levels: number): void {
  const old = textures.get(id);
  if (old) {
    if (usedInEncoder.has(`x${id}`)) flush();
    old.destroy();
    dropBindGroupsFor(id);
  }
  const tex = device!.createTexture({
    label: `voodoo2-tex-${id}`,
    size: [w0, h0],
    mipLevelCount: levels,
    format: 'rgba8unorm',
    usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
  });
  textures.set(id, tex);
}

function onTexUpload(id: number, level: number, w: number, h: number, payload: number): void {
  const tex = textures.get(id);
  if (!tex) return;
  if (usedInEncoder.has(`x${id}`)) flush();
  device!.queue.writeTexture(
    { texture: tex, mipLevel: level },
    heap(),
    { offset: payload, bytesPerRow: w * 4 },
    [w, h],
  );
}

function onTexFree(id: number): void {
  const tex = textures.get(id);
  if (!tex) return;
  if (usedInEncoder.has(`x${id}`)) flush();
  tex.destroy();
  textures.delete(id);
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
  // The uniform and vertex rings: writes land before the submitted
  // encoder, so a wrap forces a submit first.
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
  const hasColor = !!(colorId && targets.get(colorId)?.color);
  const hasDepth = !!(depthId && targets.get(depthId)?.depth);
  const fullKey = key | (hasColor ? 1 << 30 : 0) | (hasDepth ? 1 << 29 : 0);
  if (fullKey !== passPipelineKey) {
    p.setPipeline(pipelineFor(key, hasColor, hasDepth));
    passPipelineKey = fullKey;
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

function onPresent(id: number): void {
  try {
    presentInner(id);
  } catch (e) {
    console.error('[voodoo2-gpu] present failed:', e);
    throw e;
  }
}

function presentInner(id: number): void {
  const t = targets.get(id);
  if (!t?.color || !canvas || !context) return;
  endPass();
  if (canvas.width !== t.w || canvas.height !== t.h) {
    canvas.width = t.w;
    canvas.height = t.h;
  }
  const enc = ensureEncoder();
  const view = context.getCurrentTexture().createView();
  const p = enc.beginRenderPass({
    colorAttachments: [{ view, loadOp: 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }],
  });
  p.setPipeline(presentPipeline!);
  p.setBindGroup(
    0,
    device!.createBindGroup({
      layout: presentLayout!,
      entries: [
        { binding: 0, resource: t.color.createView() },
        { binding: 1, resource: lutTexture!.createView() },
      ],
    }),
  );
  p.draw(3);
  p.end();
  usedInEncoder.add(`t${id}`);
  flush();
  stat(C_STAT_FRAMES, 1);
  if (showAfterPresent) {
    showAfterPresent = false;
    postMessage({ type: 'mode', engaged: true, w: overlayW || t.w, h: overlayH || t.h });
  }
}

function onGamma(payload: number): void {
  device!.queue.writeTexture(
    { texture: lutTexture! },
    heap(),
    { offset: payload, bytesPerRow: 256 },
    [256, 3],
  );
}

// A readback: rows [y0, y1) of the target into the readback area as
// 16-bit pixels (5-6-5 recovered from the expanded rgba8 — exact for
// what the shader stores — or the depth codes themselves), then ACK.
// A readback: rows [y0, y1) of the target into the readback area as
// 16-bit pixels (5-6-5 recovered from the expanded rgba8 — exact for
// what the shader stores — or the depth codes themselves), then ACK.
// A depth format must be copied whole (no partial subresource copy), so
// the depth path copies the entire texture and picks the rows out.
async function onReadback(seq: number, id: number, y0: number, y1: number): Promise<void> {
  const t = targets.get(id);
  const d = device!;
  if (t && y1 > y0 && rbSize >= t.w * (y1 - y0) * 2) {
    flush();
    const rows = y1 - y0;
    const bpp = t.color ? 4 : 2;
    const bytesPerRow = (t.w * bpp + 255) & ~255;
    const copyRows = t.color ? rows : t.h;
    const staging = d.createBuffer({
      size: bytesPerRow * copyRows,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    const enc = d.createCommandEncoder();
    if (t.color) {
      enc.copyTextureToBuffer(
        { texture: t.color, origin: [0, y0] },
        { buffer: staging, bytesPerRow },
        [t.w, rows],
      );
    } else {
      enc.copyTextureToBuffer(
        { texture: t.depth!, origin: [0, 0], aspect: 'depth-only' },
        { buffer: staging, bytesPerRow },
        [t.w, t.h],
      );
    }
    d.queue.submit([enc.finish()]);
    readbackPending = true;
    await staging.mapAsync(GPUMapMode.READ);
    readbackPending = false;
    const src = new Uint8Array(staging.getMappedRange());
    const out = new Uint16Array(u8!.buffer, rbBase, t.w * rows);
    if (t.color) {
      for (let y = 0; y < rows; y++) {
        const ro = y * bytesPerRow;
        const oo = y * t.w;
        for (let x = 0; x < t.w; x++) {
          const i = ro + x * 4;
          out[oo + x] = ((src[i] >> 3) << 11) | ((src[i + 1] >> 2) << 5) | (src[i + 2] >> 3);
        }
      }
    } else {
      const s16 = new Uint16Array(src.buffer, src.byteOffset, src.byteLength >> 1);
      for (let y = 0; y < rows; y++) {
        const ro = ((y + y0) * bytesPerRow) >> 1;
        out.set(s16.subarray(ro, ro + t.w), y * t.w);
      }
    }
    staging.unmap();
    staging.destroy();
    stat(C_STAT_READBACKS, 1);
  }
  ack(seq);
}

// The overlay is shown by the page only after the first present that
// follows an engagement (so a new frame is what appears, never the
// canvas's last one), and hidden by the display path underneath once
// its own canvas holds a fresh frame (em_video.c) — never from here.
let showAfterPresent = false;
let overlayW = 0;
let overlayH = 0;

function onMode(engaged: number, w: number, h: number): void {
  showAfterPresent = engaged !== 0;
  overlayW = w;
  overlayH = h;
}

function freeEverything(): void {
  endPass();
  encoder = null;
  usedInEncoder.clear();
  for (const t of targets.values()) {
    t.color?.destroy();
    t.depth?.destroy();
    t.codes?.destroy();
  }
  targets.clear();
  for (const t of textures.values()) t.destroy();
  textures.clear();
  bindGroups.clear();
}

// One record of the Voodoo2's own kinds (takeoverWorker.ts TakeoverCard.record;
// PAD, FENCE and SHUTDOWN are the transport's).  A readback's promise is
// what the drain waits out.
function record(kind: number, p: number, at: number): void | Promise<void> | false {
  switch (kind) {
    case R_TARGET:
      onTarget(u32![p], u32![p + 1], u32![p + 2], u32![p + 3]);
      return;
    case R_TARGET_FREE:
      onTargetFree(u32![p]);
      return;
    case R_UPLOAD:
      onUpload(u32![p], u32![p + 1], u32![p + 2], u32![p + 3], u32![p + 4], at + 28);
      return;
    case R_TEX:
      onTex(u32![p], u32![p + 1], u32![p + 2], u32![p + 3]);
      return;
    case R_TEX_UPLOAD:
      onTexUpload(u32![p], u32![p + 1], u32![p + 2], u32![p + 3], at + 24);
      return;
    case R_TEX_FREE:
      onTexFree(u32![p]);
      return;
    case R_DRAW:
      onDraw(at + 8);
      return;
    case R_PRESENT:
      onPresent(u32![p]);
      return;
    case R_GAMMA:
      onGamma(at + 8);
      return;
    case R_READBACK:
      return onReadback(u32![p], u32![p + 1], u32![p + 2], u32![p + 3]);
    case R_MODE:
      onMode(u32![p], u32![p + 1], u32![p + 2]);
      return;
    default:
      return false;
  }
}

runTakeoverWorker({
  name: 'voodoo2-gpu',
  magic: MAGIC,
  version: PROTOCOL_VERSION,
  fence: R_FENCE,
  shutdown: R_SHUTDOWN,
  initDevice,
  unavailableReason: () => unavailableReason,
  attached() {
    uniformCursor = 0;
    vertexCursor = 0;
    freeEverything();
  },
  record,
  flush,
  freeAll: freeEverything,
  debugInfo: () => ({
    readbackPending,
    targets: targets.size,
    textures: textures.size,
    pipelines: pipelines.size,
  }),
  statsInfo: () => ({
    readbackPending,
    targets: targets.size,
    textures: textures.size,
    pipelines: pipelines.size,
  }),
});
