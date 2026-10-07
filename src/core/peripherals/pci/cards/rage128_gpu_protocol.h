// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rage128_gpu_protocol.h
// The wire protocol between the Rage 128's WebGPU translator (the
// emulator thread, C — rage128_gpu.c) and the browser's GPU worker (JS —
// app/web2/src/gpu/rage128Gpu.worker.ts).  Both sides read shared wasm
// memory; nothing crosses as a message except the initial attach.
// MIRRORED in app/web2/src/gpu/rage128Protocol.ts — bump
// R128GPU_PROTOCOL_VERSION whenever a layout or a record changes, and the
// worker refuses a control block it does not understand.
//
// The transport is the Voodoo2's (voodoo2_gpu_protocol.h): one malloc'd
// region holding a control block, a byte ring of records (the
// mailbox_ring primitive: {uint32 kind, uint32 len} + payload, len a
// multiple of 8 counting the header, PAD records at the wrap) and a
// readback area.  HEAD/TAIL count bytes published/consumed; a record the
// translator waits on carries a `seq` the worker stores into ACK when
// done.  The page routes an attach to this worker by the control block's
// MAGIC word.

#ifndef RAGE128_GPU_PROTOCOL_H
#define RAGE128_GPU_PROTOCOL_H

#define R128GPU_PROTOCOL_VERSION 1u
#define R128GPU_MAGIC            0x52313247u // 'R12G'

// Control-block word indices (the Voodoo2's layout, word for word).
#define R128GPU_C_MAGIC          0
#define R128GPU_C_VERSION        1
#define R128GPU_C_RING_OFF       2 // byte offset of the op ring from the control base
#define R128GPU_C_RING_SIZE      3 // bytes (power of two)
#define R128GPU_C_RB_OFF         4 // byte offset of the readback area
#define R128GPU_C_RB_SIZE        5 // bytes
#define R128GPU_C_HEAD           6 // translator -> worker: bytes published
#define R128GPU_C_TAIL           7 // worker -> translator: bytes consumed
#define R128GPU_C_REQ            8 // translator: sequence of the last record wanting an ACK
#define R128GPU_C_ACK            9 // worker: sequence of the last such record completed
#define R128GPU_C_STATUS         10 // worker: R128GPU_STATUS_*
#define R128GPU_C_STAT_FRAMES    11 // worker: frames presented
#define R128GPU_C_STAT_DRAWS     12 // worker: draw calls encoded
#define R128GPU_C_STAT_FLUSHES   13 // worker: command buffers submitted
#define R128GPU_C_STAT_PIPELINES 14 // worker: render pipelines built
#define R128GPU_C_STAT_READBACKS 15 // worker: readback copies serviced
#define R128GPU_CTRL_WORDS       32

#define R128GPU_STATUS_DETACHED 0u
#define R128GPU_STATUS_ATTACHED 1u
#define R128GPU_STATUS_LOST     2u

#define R128GPU_RING_BYTES (16u << 20)
#define R128GPU_RB_BYTES   (512u << 10) // 64 rows of a 2048-pixel, 32 bpp surface
#define R128GPU_RB_ROWS    64u // rows per readback band

// Record kinds.  Surfaces and textures are named by ids the translator
// assigns (never 0).
#define R128GPU_R_PAD          0u
#define R128GPU_R_SURFACE      1u // {id, role, fmt, w, h}: create (role 0 colour rgba8, 1 depth16)
#define R128GPU_R_SURFACE_FREE 2u // {id}
#define R128GPU_R_UPLOAD       3u // {id, y, w, h} + rows of PACKED pixels (fmt's bytes), whole rows
#define R128GPU_R_TEX          4u // {id, w, h}: an rg32uint atlas, (ARGB8888, raw) per texel
#define R128GPU_R_TEX_UPLOAD   5u // {id, y, w, h} + u32 pairs
#define R128GPU_R_TEX_FREE     6u // {id}
#define R128GPU_R_DRAW         7u // r128gpu_draw_hdr_t + uniform block + vertices
#define R128GPU_R_FILL         8u // {color_id, depth_id, x0, y0, x1, y1, value}: a solid rectangle
#define R128GPU_R_PRESENT      9u // {id, w, h, rbits, gbits, bbits}: show through the LUT
#define R128GPU_R_LUT          10u // + 768 bytes: the composed 3 x 256 display table
#define R128GPU_R_READBACK     11u // {seq, id, y0, y1}: packed rows -> readback area; ACK
#define R128GPU_R_MODE         12u // {engaged, w, h}: overlay visibility for the page
#define R128GPU_R_FENCE        13u // {seq}: ACK once everything before it was submitted
#define R128GPU_R_SHUTDOWN     14u // {seq}: free everything, STATUS <- DETACHED, ACK

#define R128GPU_ROLE_COLOR 0u
#define R128GPU_ROLE_DEPTH 1u

// The DRAW record's fixed header, after the {kind, len} words.
typedef struct r128gpu_draw_hdr {
    uint32_t color_id; // colour surface
    uint32_t depth_id; // depth surface (0 = none)
    uint32_t tex_id[2]; // per-unit texture atlas (0 = unbound)
    uint32_t pipe_key; // R128GPU_PK_*: the state WebGPU bakes into a pipeline
    int32_t sx0, sy0, sx1, sy1; // scissor, surface pixels, half-open
    uint32_t n_verts; // vertices that follow the uniform block (triangle list)
    uint32_t reserved;
} r128gpu_draw_hdr_t;

// Pipeline-key bits.
#define R128GPU_PK_BLEND       (1u << 0)
#define R128GPU_PK_SUBTRACT    (1u << 1) // ALPHA_COMB_FCN 2: src - dst (clamped)
#define R128GPU_PK_SRC_SHIFT   4 // 4 bits: WebGPU blend factor code (R128GPU_BF_*)
#define R128GPU_PK_DST_SHIFT   8 // 4 bits
#define R128GPU_PK_DEPTH       (1u << 12) // a depth surface is bound
#define R128GPU_PK_DFUNC_SHIFT 13 // 3 bits: Z_TEST (0 never .. 7 always)
#define R128GPU_PK_DEPTH_WRITE (1u << 16)
#define R128GPU_PK_WMASK_SHIFT 17 // 4 bits: colour write mask (R, G, B, A)

// Blend factors as the worker maps them.
#define R128GPU_BF_ZERO            0u
#define R128GPU_BF_ONE             1u
#define R128GPU_BF_SRC             2u
#define R128GPU_BF_ONE_MINUS_SRC   3u
#define R128GPU_BF_SRC_A           4u
#define R128GPU_BF_ONE_MINUS_SRC_A 5u
#define R128GPU_BF_DST_A           6u
#define R128GPU_BF_ONE_MINUS_DST_A 7u
#define R128GPU_BF_DST             8u
#define R128GPU_BF_ONE_MINUS_DST   9u
#define R128GPU_BF_SRC_A_SAT       10u

// The per-draw uniform block: 128 words (512 bytes, the dynamic-offset
// stride).  Word indices; the WGSL struct mirrors them.  Raw registers
// where the shader decodes them as rage128_raster.c does.
#define R128GPU_U_WORDS     128
#define R128GPU_U_BYTES     (R128GPU_U_WORDS * 4)
#define R128GPU_U_TEX_CNTL  0
#define R128GPU_U_MISC      1
#define R128GPU_U_SCALE     2
#define R128GPU_U_ZS        3
#define R128GPU_U_CONSTANT  4
#define R128GPU_U_FOG_COLOR 5
#define R128GPU_U_KEY       6
#define R128GPU_U_KEY_MASK  7
#define R128GPU_U_DST_TYPE  8
#define R128GPU_U_FLAGS     9 // R128GPU_F_*
#define R128GPU_U_Z_BITS    10 // 16
#define R128GPU_U_AUX_CNTL  11
#define R128GPU_U_AUX       12 // 12 words: 3 x {left, right, top, bottom}, inclusive, i32
#define R128GPU_U_TEX0      24 // 16 words per unit
#define R128GPU_U_TEX1      40
#define R128GPU_U_FOGTABLE  56 // 64 words: 256 bytes
#define R128GPU_U_TARGET_W  120 // f32: the colour surface's size, for the vertex shader
#define R128GPU_U_TARGET_H  121
#define R128GPU_U_TEX_WORDS 16
// Per-unit words.
#define R128GPU_UT_ON      0
#define R128GPU_UT_CNTL    1
#define R128GPU_UT_COMB    2
#define R128GPU_UT_FMT     3
#define R128GPU_UT_LPITCH  4
#define R128GPU_UT_LHEIGHT 5
#define R128GPU_UT_TOP     6 // lsize - lmin: the deepest level index
#define R128GPU_UT_BORDER  7
#define R128GPU_UT_FLAGS   8 // R128GPU_TF_*

// Uniform flag bits.
#define R128GPU_F_BLENDING (1u << 0) // TC_ALPHA_EN: the pack does not dither under DITHER_TABLE|INIT
#define R128GPU_F_DEPTH    (1u << 1) // frag_depth is written (a depth surface is bound)

// Per-unit flag bits.
#define R128GPU_TF_PERSP (1u << 0) // divide by the interpolated W

// One vertex: 20 floats (80 bytes), all interpolated linearly in screen
// space (the walker's attribute planes): x, y (surface pixels, on the
// walker's 1/16 grid), z (0..1), r g b a, sr sg sb (0..255), fog (0..255),
// s0 t0 w0, s1 t1 w1 (S·W, T·W, W per unit), 2 pad.
#define R128GPU_VERTEX_FLOATS 20
#define R128GPU_VERTEX_BYTES  (R128GPU_VERTEX_FLOATS * 4)
#define R128GPU_MAX_VERTS     3072 // per DRAW record

#endif // RAGE128_GPU_PROTOCOL_H
