// The Voodoo2 WebGPU takeover's wire protocol — the TypeScript mirror of
// src/core/peripherals/pci/cards/voodoo2_gpu_protocol.h.  The translator
// on the emulator's raster pthread writes records into a byte ring inside
// the wasm heap; the GPU worker (voodoo2Gpu.worker.ts) consumes them.
// Keep the two in step: PROTOCOL_VERSION is checked at attach.  The
// control block, roles and DRAW header are shared with the Rage 128
// (takeoverProtocol.ts).

export * from './takeoverProtocol';

export const PROTOCOL_VERSION = 2; // 2: records are multiples of 8 (mailbox_ring)
export const MAGIC = 0x56324750; // 'V2GP'

// Record kinds.
export const R_PAD = 0;
export const R_TARGET = 1;
export const R_TARGET_FREE = 2;
export const R_UPLOAD = 3;
export const R_TEX = 4;
export const R_TEX_UPLOAD = 5;
export const R_TEX_FREE = 6;
export const R_DRAW = 7;
export const R_PRESENT = 8;
export const R_GAMMA = 9;
export const R_READBACK = 10;
export const R_MODE = 11;
export const R_FENCE = 12;
export const R_SHUTDOWN = 13;

// Pipeline-key bits.
export const PK_BLEND = 1 << 0;
export const PK_SRC_SHIFT = 4;
export const PK_DST_SHIFT = 8;
export const PK_DEPTH_TEST = 1 << 12;
export const PK_DFUNC_SHIFT = 13;
export const PK_DEPTH_WRITE = 1 << 16;
export const PK_COLOR_WRITE = 1 << 17;

export const U_BYTES = 512; // the uniform block (and the dynamic-offset stride)
export const VERTEX_BYTES = 64;
export const MAX_VERTS = 3072;
