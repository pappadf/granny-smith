// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// voodoo2_raster.h
// The raster-backend seam (proposal-pci-3dfx-voodoo2 §3.6, extended by
// proposal-voodoo2-raster-thread §5 and proposal-voodoo2-walker-
// optimization §3.1).
//
// The two triangle routes — host-setup and on-chip setup — converge on
// voodoo2_tri_t; everything downstream of it sits behind this seam.
// The software walker in voodoo2_raster.c is the DEFAULT and NORMATIVE
// backend: it defines the semantics, produces every golden, and runs
// every gate.  Two invariants keep any backend guest-invisible:
//
//   1. TIMING IS ANALYTIC.  Work completes synchronously at issue and
//      busy/idle is computed from bookkeeping, never from the cost of
//      rasterising — so the guest's instruction stream is
//      backend-independent by construction.  A triangle consumes zero
//      scheduled time whichever backend draws it.
//   2. THE CPU SHADOW IS AUTHORITATIVE FOR OBSERVATION.  A backend that
//      renders elsewhere (or later) must sync() — retire every queued
//      command — before an LFB read, a counter read, scanout, or a
//      checkpoint.  The synchronous walker's sync() is a fold of the
//      statistics counters and nothing else.
//
// THE SHAPE OF THE SEAM.  The card (voodoo2.c) is the PRODUCER: it owns
// the register file and translates guest traffic into commands
// (v2_cmd_t) that carry a value snapshot of everything the pipeline
// reads (v2_draw_state_t).  The EXECUTOR (v2_raster_execute) turns a
// command into pixels against a v2_target_t — the framebuffer, the
// texture RAMs, the palettes/NCC tables and the statistics counters,
// which the executor OWNS: the producer touches them only through
// commands, or after a sync().  The executor never sees the card
// struct at all — this translation unit does not include its
// definition — which is the strongest available form of the rule that
// a worker thread must never read live card state (thread proposal
// §4).  A backend is only WHERE the executor runs:
//
//   sw      the executor runs inline at submit — the NORMATIVE backend:
//           it defines the semantics; the equivalence rows replay on it
//   null    as sw, but TRIANGLE commands are dropped — pins invariant 1
//   thread  commands go to a bounded SPSC queue drained by one worker
//           pthread; sync() is a fence.  The DEFAULT on every build
//           (native pthreads; in the browser a second Web Worker,
//           preallocated by the link — thread proposal §5.7).  A build
//           can default to sw with -DGS_V2_RASTER_DEFAULT='"sw"', or
//           leave the backend out with -DGS_V2_THREAD_BACKEND=0; any
//           boot can pick one with pci_option="raster=...".
//   webgpu  the thread backend whose worker TRANSLATES for the
//           browser's GPU while the card drives the monitor
//           (voodoo2_gpu.c; proposal-voodoo2-webgpu-takeover): an
//           ALTERNATIVE the user picks, approximate by design (the
//           browser frame is a rendering of the scene, the model's
//           frame is the walker's), falling back to `thread` wherever
//           no GPU worker attaches (native builds, no WebGPU).
//
// Because queue order is submission order and every observation point
// fences first, the thread backend's output is byte-identical to the
// walker's — the whole test suite is its equivalence oracle.
//
// The FILL CONVENTION the walker implements is CHOSEN, NOT KNOWN: the
// Voodoo2 spec's own §7.2 defers the TRIANGLE walk to an SST-1
// Programming Guide nobody holds.  The convention (documented in
// docs/core/peripherals/pci/cards/voodoo2.md): vertices in 12.4, the
// sample point at the pixel's integer coordinate, half-open top-left
// edge inclusion with the winding taken from the command's area sign,
// and parameter iteration from vertex A's truncated position.  Goldens
// record what THIS rasteriser draws — regression anchors, not hardware
// conformance.

#ifndef VOODOO2_RASTER_H
#define VOODOO2_RASTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define V2_RASTER_TMUS    2
#define V2_RASTER_FB_SIZE 0x400000u // the 4 MB framebuffer, every SKU
#define V2_RASTER_LODS    10 // LOD 0..8, plus the split-texture parity snap past 8

// One triangle, in the register file's own fixed-point formats, after
// both submission routes have converged (host setup latches these
// directly; on-chip setup computes the gradients from its vertices).
typedef struct voodoo2_tri {
    int32_t ax, ay, bx, by, cx, cy; // vertices, 12.4 two's complement
    int32_t r, g, b, a; // colour start values, 12.12 (sign-extended)
    int32_t drdx, dgdx, dbdx, dadx; // 12.12
    int32_t drdy, dgdy, dbdy, dady;
    int32_t z, dzdx, dzdy; // 20.12
    int64_t w, dwdx, dwdy; // FBI 1/W, 2.30 (sign-extended to 64)
    // Per-TMU texture parameters: S/W and T/W in 14.18, per-TMU 1/W in
    // 2.30 (sWtmu*).
    int64_t s[2], dsdx[2], dsdy[2];
    int64_t t[2], dtdx[2], dtdy[2];
    int64_t tw[2], dtwdx[2], dtwdy[2];
    bool area_sign; // triangleCMD bit 31: 1 = clockwise / negative area
} voodoo2_tri_t;

// Everything one TMU's sampler reads, snapshotted per draw (walker
// proposal §3.1: the raw registers plus their derived, per-draw
// constant decode — each derived field names the bits it caches).
typedef struct v2_tmu_state {
    uint32_t mode; // textureMode
    uint32_t tlod; // tLOD
    uint32_t trex1; // trexInit1
    uint32_t texbase; // texBaseAddr (the watch instrument prints it)
    uint32_t mask; // addressable texture size - 1 (trexInit0[14] gate)
    uint32_t lod_base[V2_RASTER_LODS]; // DRAM base of each LOD level
    uint16_t lod_w[V2_RASTER_LODS]; // level width in texels
    uint16_t lod_h[V2_RASTER_LODS]; // level height in texels
    uint8_t fmt; // textureMode[11:8]
    uint8_t ncc_table; // textureMode[5] tnccselect
    bool is8; // fmt < 8: one byte per texel
    bool clamp_s, clamp_t; // textureMode[6], [7]
    bool persp; // textureMode[0] perspective correct
    bool tclampw; // textureMode[3]
    bool bilin_min, bilin_mag; // textureMode[1], [2]
    bool send_config; // trexInit1[18]
    bool tsplit; // tLOD[19]
    uint32_t lod_odd; // tLOD[18]
    int32_t lodmin, lodmax, lodbias; // tLOD[5:0], [11:6] (clamped 32), [17:12] signed 4.2
    bool lod_pinned; // lodmin == lodmax and both filters agree: the
                     // per-pixel LOD estimate cannot change the output
    uint32_t tc_ctl, tca_ctl; // combine-unit control nibbles
    uint32_t tc_msel, tca_msel; // textureMode[16:14], [25:23]
    uint8_t tc_add; // 0 none, 1 c_local, 2 a_local (textureMode[18], [19])
    bool tca_add; // textureMode[27] | [28]
    bool ignores_other; // the combine consumes nothing from c_other/a_other
} v2_tmu_state_t;

// The draw-state snapshot: every register the pixel pipeline, the
// walker, FASTFILL, the LFB path and texture downloads read.  Copied by
// the producer when a state register changes (not per command), and
// the ONLY card state the executor ever sees.
typedef struct v2_draw_state {
    uint32_t fbz, fcp, amode, fogmode, fogcolor;
    uint32_t color0, color1, zacolor, chromakey, chromarange, lfbmode;
    uint32_t fogtable[32];
    int32_t clip_x0, clip_x1, clip_y0, clip_y1; // effective clip (fbzMode[0] applied)
    int32_t fill_x0, fill_x1, fill_y0, fill_y1; // raw clipLeftRight/TopBottom (FASTFILL)
    uint32_t screen_h; // videoDimensions height, for the Y-origin flips
    uint32_t buf_base[4]; // physical byte base per software buffer select
    uint32_t stride; // bytes per framebuffer row (tilesInX x 32 x 2)
    bool tex_on; // fbzColorPath[27] && !fbiInit3[6]
    bool uses_tex; // the pipeline reads tex_argb anywhere
    bool skip_tmu1; // TMU0 ignores its chain input: the TMU1 sample is dead
    v2_tmu_state_t tmu[V2_RASTER_TMUS];
} v2_draw_state_t;

// The executor's target: the memories it renders into and the
// guest-visible state it MUTATES per pixel.  Owned by the executor;
// the producer reads it only after a sync() and writes it only through
// commands (or after a sync(), for reset/restore).
typedef struct v2_target {
    uint8_t *fb; // the 4 MB framebuffer
    uint8_t *tex[V2_RASTER_TMUS]; // texture RAM per TMU
    uint32_t palette[V2_RASTER_TMUS][256]; // 24-bit RGB entries
    uint32_t ncc[V2_RASTER_TMUS][2][12]; // two NCC tables per TMU, raw words
    uint32_t pixels_in, chroma_fail, zfunc_fail, afunc_fail, pixels_out; // 24-bit
    uint32_t stipple; // the live stipple register (rotates per pixel)
    // Texel-expansion cache for the 8-bit formats: rebuilt when the
    // (format, table, palette/NCC generation) key changes.
    uint32_t pal_gen[V2_RASTER_TMUS];
    uint32_t lut_key[V2_RASTER_TMUS];
    uint32_t lut[V2_RASTER_TMUS][256];
} v2_target_t;

// The command stream.  Order is submission order, always.
typedef enum v2_cmd_kind {
    V2_CMD_TRIANGLE, // rasterise one triangle through the pipeline
    V2_CMD_FASTFILL, // clear the clip rectangle (fastfillCMD)
    V2_CMD_LFB_PIXEL, // one LFB-face pixel: bypass or pipeline per lfbMode[8]
    V2_CMD_FB_STORE16, // one raw 16-bit store into a buffer (depth-format LFB writes)
    V2_CMD_TEX_WRITE, // texture-aperture download words (packet-5 rows batch)
    V2_CMD_PALETTE, // one texture-palette entry
    V2_CMD_NCC, // one NCC table word
    V2_CMD_BLT_FILL, // the SGRAM page-space rectangle fill (FRECTFILL)
    V2_CMD_STAT_CLEAR, // nopCMD[0]: zero the five statistics counters
    V2_CMD_STIPPLE, // a guest write to the stipple register
    // The WebGPU takeover's own commands (voodoo2_gpu.c).  Ordered
    // through the same queue as the draws they bracket; the executor
    // ignores them, so on every other backend they are no-ops.
    V2_CMD_GPU_ENGAGE, // u.gpu.flags: bit 0 = want GPU mode, bit 1 = read the GPU back first
    V2_CMD_GPU_PRESENT, // u.gpu.addr: the displayed colour buffer's base; one per vblank
    V2_CMD_GPU_GAMMA, // u.tex: one 192-byte chunk (off = chunk index) of the 3 x 256 gamma ramp
    V2_CMD_GPU_READBACK, // u.gpu.addr/len: make the shadow authoritative over that byte range
} v2_cmd_kind_t;

#define V2_TEX_WRITE_MAX_WORDS 48

typedef struct v2_cmd {
    uint8_t kind; // v2_cmd_kind_t
    uint16_t state; // draw-state ring slot this command reads
    union {
        voodoo2_tri_t tri;
        struct {
            uint32_t buffer, x, y, r, g, b, a;
            uint16_t z;
            bool has_z, write_color, write_z;
        } lfb;
        struct {
            uint32_t buffer, x, y;
            uint16_t px;
        } store;
        struct {
            uint32_t off; // aperture field encoding of the first word
            uint32_t n; // words that follow, consecutive at off + 4i
            uint32_t words[V2_TEX_WRITE_MAX_WORDS]; // little-endian card domain
        } tex;
        struct {
            uint8_t tmu;
            uint16_t index;
            uint32_t rgb;
        } pal;
        struct {
            uint8_t tmu, table, off;
            uint32_t value;
        } ncc;
        struct {
            uint32_t rows, units, y0, x0, base;
            uint16_t color;
        } blt;
        struct {
            uint32_t value;
        } stipple;
        struct {
            uint32_t flags, addr, len;
        } gpu;
    } u;
} v2_cmd_t;

// --- the executor (the normative walker and the pipeline) ------------------

// Execute one command against the target with the given snapshot.
void v2_raster_execute(const v2_draw_state_t *st, v2_target_t *tgt, const v2_cmd_t *cmd);

// Is the GS_V2_WATCH pixel-provenance instrument armed?  (Read once
// from the environment.)  A threaded backend refuses to start while it
// is: diagnosis uses the synchronous walker.
bool v2_raster_watch_armed(void);

// --- the backend context ---------------------------------------------------

struct v2_raster;
typedef struct v2_raster v2_raster_t;

// Producer callback: fill a draw-state slot from the live registers.
typedef void (*v2_state_build_fn)(void *ctx, v2_draw_state_t *st);

// Create a backend: `kind` is "sw", "null", "thread" or "webgpu"
// (unknown kinds and an unavailable thread backend fall back to "sw",
// an unavailable webgpu transport to "thread", each with a log line).
// `build` is called, with `ctx`, whenever a command needs a fresher
// snapshot than the ring holds.
v2_raster_t *v2_raster_create(const char *kind, v2_target_t *tgt, v2_state_build_fn build, void *ctx);
// Fence, stop the worker if any, free.
void v2_raster_destroy(v2_raster_t *r);
const char *v2_raster_name(const v2_raster_t *r);
// The producer changed a state register: the next command snapshots.
void v2_raster_state_dirty(v2_raster_t *r);
// Submit one command (copied).  cmd->state is filled in here.
void v2_raster_submit(v2_raster_t *r, v2_cmd_t *cmd);
// Retire every submitted command; on return the target is
// authoritative — its counters, stipple, palettes/NCC tables and
// texture RAM — and the producer may touch it directly.  Under the
// WebGPU takeover the FRAMEBUFFER is the exception: pixels the GPU
// drew reach the shadow only through v2_raster_sync_fb().
void v2_raster_sync(v2_raster_t *r);
// As v2_raster_sync, and additionally make the framebuffer bytes
// [addr, addr + len) authoritative (a readback under the takeover; a
// plain sync elsewhere).  LFB reads, screenshots and checkpoints use it;
// `guest` marks the guest's own reads (LFB), the only ones the takeover
// counts toward a readback storm — a checkpoint or a screenshot reads
// everything back once and must never cost the GPU its engagement.
void v2_raster_sync_fb(v2_raster_t *r, uint32_t addr, uint32_t len, bool guest);

// --- the WebGPU takeover (proposal-voodoo2-webgpu-takeover) ---------------
// No-ops on every backend but "webgpu".  The card calls them from the
// producer side; each travels through the queue so it is ordered with
// the draws around it.
// The card started/stopped driving the monitor: GPU mode engages on
// the rising edge and disengages (reading the GPU's pixels back into
// the shadow unless `discard`) on the falling one.
void v2_raster_engage(v2_raster_t *r, bool drives, bool discard);
// One vblank while driving: present the displayed colour buffer at
// `fb_base` (its physical byte address) through the gamma ramp.
void v2_raster_present(v2_raster_t *r, uint32_t fb_base);
// The gamma ramp changed: 3 x 256 bytes, [0]=R, [1]=G, [2]=B.
void v2_raster_gamma(v2_raster_t *r, const uint8_t lut[3][256]);
// True while the GPU is presenting the card's frames itself, i.e. the
// scanout conversion and the display upload can be skipped.
bool v2_raster_presents(const v2_raster_t *r);
// A one-line status/statistics report of the takeover ("" elsewhere).
const char *v2_raster_gpu_stats(v2_raster_t *r, char *buf, size_t n);

#endif // VOODOO2_RASTER_H
