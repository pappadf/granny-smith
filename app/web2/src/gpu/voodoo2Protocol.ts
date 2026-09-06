// The Voodoo2 WebGPU takeover's wire protocol — the TypeScript mirror of
// src/core/peripherals/pci/cards/voodoo2_gpu_protocol.h.  The translator
// on the emulator's raster pthread writes records into a byte ring inside
// the wasm heap; the GPU worker (voodoo2Gpu.worker.ts) consumes them.
// Keep the two in step: PROTOCOL_VERSION is checked at attach.

export const PROTOCOL_VERSION = 1;
export const MAGIC = 0x56324750; // 'V2GP'

// Control-block word indices (Uint32 view at the control base).
export const C_MAGIC = 0;
export const C_VERSION = 1;
export const C_RING_OFF = 2;
export const C_RING_SIZE = 3;
export const C_RB_OFF = 4;
export const C_RB_SIZE = 5;
export const C_HEAD = 6;
export const C_TAIL = 7;
export const C_REQ = 8;
export const C_ACK = 9;
export const C_STATUS = 10;
export const C_STAT_FRAMES = 11;
export const C_STAT_DRAWS = 12;
export const C_STAT_FLUSHES = 13;
export const C_STAT_PIPELINES = 14;
export const C_STAT_READBACKS = 15;

export const STATUS_DETACHED = 0;
export const STATUS_ATTACHED = 1;
export const STATUS_LOST = 2;

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

export const ROLE_COLOR = 0;
export const ROLE_DEPTH = 1;

// DRAW record: the fixed header after {kind, len}, in u32 words.
export const DH_COLOR_ID = 0;
export const DH_DEPTH_ID = 1;
export const DH_TEX0 = 2;
export const DH_TEX1 = 3;
export const DH_PIPE_KEY = 4;
export const DH_SX0 = 5;
export const DH_SY0 = 6;
export const DH_SX1 = 7;
export const DH_SY1 = 8;
export const DH_N_VERTS = 9;
export const DH_WORDS = 11;

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
