// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The part of the WebGPU takeover wire protocol both cards share: the
// control block's words and status values, the surface roles and the DRAW
// header layout.  voodoo2Protocol.ts and rage128Protocol.ts re-export it
// beside their own magic, version, record kinds and pipeline-key bits; the
// C mirrors are voodoo2_gpu_protocol.h (V2GPU_*) and rage128_gpu_protocol.h
// (R128GPU_*), drift-tested by tests/unit/shmLayout.test.ts.

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

export const ROLE_COLOR = 0;
export const ROLE_DEPTH = 1;

// The DRAW header, in u32 words after the {kind, len} pair.
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
