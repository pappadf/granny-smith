// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// voodoo2_gpu_protocol.h
// The wire protocol between the Voodoo2's WebGPU translator (the raster
// pthread, C — voodoo2_gpu.c) and the browser's GPU worker (JS —
// app/web2/src/gpu/voodoo2Gpu.worker.ts).  Both sides read shared wasm
// memory; nothing crosses as a message except the initial attach.
// MIRRORED in app/web2/src/gpu/voodoo2Protocol.ts — bump
// V2GPU_PROTOCOL_VERSION whenever a layout or a record changes, and
// the worker refuses a control block it does not understand.
//
// Layout of the region the translator allocates (one malloc):
//
//   control block   V2GPU_CTRL_WORDS uint32 words (indices below)
//   op ring         V2GPU_RING_BYTES: records, translator -> worker
//   readback area   V2GPU_RB_BYTES: pixel rows, worker -> translator
//
// The op ring is a byte ring of RECORDS: {uint32 kind, uint32 len}
// followed by a 4-byte-aligned payload; `len` counts the header.  A
// record never wraps: when one would not fit before the ring's end the
// translator writes a PAD record whose len reaches the end.  HEAD is
// the byte count the translator has published (monotonic, mod 2^32),
// TAIL the count the worker has consumed; both sides wake the other
// with Atomics.notify on those words (the C side through the
// gs_v2gpu_* seam).  A record whose completion the translator must
// wait for carries a `seq`; the worker stores it into ACK when done.

#ifndef VOODOO2_GPU_PROTOCOL_H
#define VOODOO2_GPU_PROTOCOL_H

#define V2GPU_PROTOCOL_VERSION 1u
#define V2GPU_MAGIC            0x56324750u // 'V2GP'

// Control-block word indices.
#define V2GPU_C_MAGIC          0 // V2GPU_MAGIC
#define V2GPU_C_VERSION        1 // V2GPU_PROTOCOL_VERSION
#define V2GPU_C_RING_OFF       2 // byte offset of the op ring from the control base
#define V2GPU_C_RING_SIZE      3 // bytes (power of two)
#define V2GPU_C_RB_OFF         4 // byte offset of the readback area
#define V2GPU_C_RB_SIZE        5 // bytes
#define V2GPU_C_HEAD           6 // translator -> worker: bytes published
#define V2GPU_C_TAIL           7 // worker -> translator: bytes consumed
#define V2GPU_C_REQ            8 // translator: sequence of the last record wanting an ACK
#define V2GPU_C_ACK            9 // worker: sequence of the last such record completed
#define V2GPU_C_STATUS         10 // worker: V2GPU_STATUS_*
#define V2GPU_C_STAT_FRAMES    11 // worker: frames presented
#define V2GPU_C_STAT_DRAWS     12 // worker: draw calls encoded
#define V2GPU_C_STAT_FLUSHES   13 // worker: command buffers submitted
#define V2GPU_C_STAT_PIPELINES 14 // worker: render pipelines built
#define V2GPU_C_STAT_READBACKS 15 // worker: readback copies serviced
#define V2GPU_CTRL_WORDS       32

#define V2GPU_STATUS_DETACHED 0u
#define V2GPU_STATUS_ATTACHED 1u
#define V2GPU_STATUS_LOST     2u // the GPUDevice was lost; the translator disengages

#define V2GPU_RING_BYTES (16u << 20)
#define V2GPU_RB_BYTES   (256u << 10) // 64 rows of the widest (1024-pixel) raster, 16 bpp
#define V2GPU_RB_ROWS    64u // rows per readback band

// Record kinds.
#define V2GPU_R_PAD         0u // skip to the ring start
#define V2GPU_R_TARGET      1u // {id, role, w, h}: create a render target (role 0 colour, 1 depth)
#define V2GPU_R_TARGET_FREE 2u // {id}
#define V2GPU_R_UPLOAD      3u // {id, x, y, w, h} + rows: rgba8 (colour) or u16 codes (depth)
#define V2GPU_R_TEX         4u // {id, w0, h0, levels}: create a mip-chain texture
#define V2GPU_R_TEX_UPLOAD  5u // {id, level, w, h} + rgba8 texels
#define V2GPU_R_TEX_FREE    6u // {id}
#define V2GPU_R_DRAW        7u // v2gpu_draw_hdr_t + uniform block + vertices
#define V2GPU_R_PRESENT     8u // {id}: show the colour target through the gamma LUT
#define V2GPU_R_GAMMA       9u // + 768 bytes: the 3 x 256 gamma ramp
#define V2GPU_R_READBACK    10u // {seq, id, y0, y1}: rows -> readback area as 16-bit pixels; ACK
#define V2GPU_R_MODE        11u // {engaged, w, h}: overlay visibility for the page
#define V2GPU_R_FENCE       12u // {seq}: ACK once everything before it was submitted
#define V2GPU_R_SHUTDOWN    13u // {seq}: free every resource, STATUS <- DETACHED, ACK

#define V2GPU_ROLE_COLOR 0u
#define V2GPU_ROLE_DEPTH 1u

// The DRAW record's fixed header, after the {kind, len} words.
typedef struct v2gpu_draw_hdr {
    uint32_t color_id; // colour target (0 = none: depth-only draw)
    uint32_t depth_id; // depth target (0 = none)
    uint32_t tex_id[2]; // per-TMU texture (0 = unbound)
    uint32_t pipe_key; // V2GPU_PK_* bits: the WebGPU pipeline state
    int32_t sx0, sy0, sx1, sy1; // scissor, target pixels, half-open
    uint32_t n_verts; // vertices that follow the uniform block
    uint32_t reserved;
} v2gpu_draw_hdr_t;

// Pipeline-key bits: exactly the state a GPURenderPipeline bakes in.
#define V2GPU_PK_BLEND       (1u << 0)
#define V2GPU_PK_SRC_SHIFT   4 // alphaMode[11:8] source factor code
#define V2GPU_PK_DST_SHIFT   8 // alphaMode[15:12] destination factor code
#define V2GPU_PK_DEPTH_TEST  (1u << 12)
#define V2GPU_PK_DFUNC_SHIFT 13 // fbzMode[7:5]
#define V2GPU_PK_DEPTH_WRITE (1u << 16)
#define V2GPU_PK_COLOR_WRITE (1u << 17)

// The per-draw uniform block: 128 words (512 bytes, the dynamic-offset
// stride).  Word indices; the WGSL struct mirrors them.
#define V2GPU_U_WORDS       128
#define V2GPU_U_BYTES       (V2GPU_U_WORDS * 4)
#define V2GPU_U_FBZ         0
#define V2GPU_U_FCP         1
#define V2GPU_U_AMODE       2
#define V2GPU_U_FOGMODE     3
#define V2GPU_U_FOGCOLOR    4
#define V2GPU_U_COLOR0      5
#define V2GPU_U_COLOR1      6
#define V2GPU_U_ZACOLOR     7
#define V2GPU_U_CHROMAKEY   8
#define V2GPU_U_CHROMARANGE 9
#define V2GPU_U_STIPPLE     10
#define V2GPU_U_FLAGS       11
#define V2GPU_U_SCREEN_H    12 // f32
#define V2GPU_U_TARGET_W    13 // f32
#define V2GPU_U_TARGET_H    14 // f32
#define V2GPU_U_FILL        15 // fill colour: rgb888 (dithered) or 565 (raw), per flags
#define V2GPU_U_FOGTABLE    16 // 32 words
#define V2GPU_U_TMU0        48 // 16 words per TMU
#define V2GPU_U_TMU1        64
#define V2GPU_U_TMU_WORDS   16
// Per-TMU words.
#define V2GPU_UT_MODE       0
#define V2GPU_UT_TLOD       1
#define V2GPU_UT_TREX1      2
#define V2GPU_UT_LODMIN     3 // i32, 4.2
#define V2GPU_UT_LODMAX     4
#define V2GPU_UT_LODBIAS    5
#define V2GPU_UT_FLAGS      6 // V2GPU_TF_*
#define V2GPU_UT_BASE_LEVEL 7 // the GPU texture's level 0 is this Voodoo LOD
#define V2GPU_UT_W0         8 // Voodoo LOD-0 width of the chain
#define V2GPU_UT_H0         9

// Uniform flag bits.
#define V2GPU_F_TEX_ON    (1u << 0) // the texture chain runs (fbzColorPath[27] && !texmap_dis)
#define V2GPU_F_USES_TEX  (1u << 1) // the pipeline reads tex_argb anywhere
#define V2GPU_F_SKIP_TMU1 (1u << 2) // TMU1's sample is dataflow-dead
#define V2GPU_F_Y_FLIP    (1u << 3) // fbzMode[17]: flip Y about screen_h
#define V2GPU_F_FILL      (1u << 4) // fill draw: colour from U_FILL, depth from zaColor
#define V2GPU_F_FILL_RAW  (1u << 5) // ...the fill colour is a raw 5-6-5 (SGRAM fill), not dithered
#define V2GPU_F_LFB_PIXEL (1u << 6) // a pipeline-processed LFB pixel: no texture, iterators from the vertex

// Per-TMU flag bits.
#define V2GPU_TF_BILIN_MIN  (1u << 0)
#define V2GPU_TF_BILIN_MAG  (1u << 1)
#define V2GPU_TF_CLAMP_S    (1u << 2)
#define V2GPU_TF_CLAMP_T    (1u << 3)
#define V2GPU_TF_PERSP      (1u << 4)
#define V2GPU_TF_TCLAMPW    (1u << 5)
#define V2GPU_TF_SEND_CFG   (1u << 6)
#define V2GPU_TF_TSPLIT     (1u << 7)
#define V2GPU_TF_LOD_ODD    (1u << 8)
#define V2GPU_TF_LOD_PINNED (1u << 9)
#define V2GPU_TF_BOUND      (1u << 10) // a texture is bound (else the chain reads black)

// One vertex: 16 floats (64 bytes), all linearly interpolated.
#define V2GPU_VERTEX_FLOATS 16
#define V2GPU_VERTEX_BYTES  (V2GPU_VERTEX_FLOATS * 4)
// x, y (walker pixel coordinates, before the half-pixel shift and the
// Y flip); z (20.12 -> 16-bit units), w (2.30 -> 1/W); r g b a (12.12
// -> 8-bit units); s0 t0 (14.18 -> texels) w0 (2.30); s1 t1 w1; 2 pad.
#define V2GPU_MAX_VERTS 3072 // per DRAW record

#endif // VOODOO2_GPU_PROTOCOL_H
