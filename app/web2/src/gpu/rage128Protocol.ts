// The Rage 128 WebGPU takeover's wire protocol — the TypeScript mirror of
// src/core/peripherals/pci/cards/rage128_gpu_protocol.h.  The translator on
// the emulator thread writes records into a byte ring inside the wasm heap;
// the GPU worker (rage128Gpu.worker.ts) consumes them.  Keep the two in
// step: PROTOCOL_VERSION is checked at attach.  The control block's layout
// is the Voodoo2's (voodoo2Protocol.ts), word for word.

export const PROTOCOL_VERSION = 1;
export const MAGIC = 0x52313247; // 'R12G'

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
export const R_SURFACE = 1; // {id, role, fmt, w, h}
export const R_SURFACE_FREE = 2; // {id}
export const R_UPLOAD = 3; // {id, y, w, h} + packed rows
export const R_TEX = 4; // {id, w, h}
export const R_TEX_UPLOAD = 5; // {id, y, w, h} + u32 texels
export const R_TEX_FREE = 6; // {id}
export const R_DRAW = 7; // header + uniform + vertices
export const R_FILL = 8; // {color_id, depth_id, x0, y0, x1, y1, value}
export const R_PRESENT = 9; // {id, w, h, rbits, gbits, bbits}
export const R_LUT = 10; // + 768 bytes
export const R_READBACK = 11; // {seq, id, y0, y1}
export const R_MODE = 12; // {engaged, w, h}
export const R_FENCE = 13; // {seq}
export const R_SHUTDOWN = 14; // {seq}

export const ROLE_COLOR = 0;
export const ROLE_DEPTH = 1;

// The DRAW header, in words after the {kind, len} pair.
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
export const PK_SUBTRACT = 1 << 1;
export const PK_SRC_SHIFT = 4;
export const PK_DST_SHIFT = 8;
export const PK_DEPTH = 1 << 12;
export const PK_DFUNC_SHIFT = 13;
export const PK_DEPTH_WRITE = 1 << 16;
export const PK_WMASK_SHIFT = 17;

// Blend factor codes (the register's 0..9, then the saturate).
export const BF_SRC_A_SAT = 10;

export const U_WORDS = 128;
export const U_BYTES = U_WORDS * 4;
export const VERTEX_FLOATS = 20;
export const VERTEX_BYTES = VERTEX_FLOATS * 4;
