// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// voodoo2_gpu.c
// The WebGPU takeover's translator (proposal-voodoo2-webgpu-takeover
// §4-§5): the raster pthread's GPU mode.  It consumes the SAME
// v2_cmd_t stream every backend consumes, and while ENGAGED turns it
// into records for the browser's GPU worker (voodoo2_gpu_protocol.h)
// instead of pixels: the setup unit's triangles become vertex-buffer
// entries under a cached pipeline plus a uniform block, fastfills and
// SGRAM fills become fill draws, texture memory becomes a cache of GPU
// mip chains converted from the shadow texture RAM by the normative
// v2_texel_expand, bypass LFB writes become texture uploads, and a
// swap's vblank becomes a present onto the overlay canvas.
//
// What stays on the CPU: the shadow.  The executor's target — texture
// RAM, palettes, NCC tables, counters, stipple — is kept coherent by
// running the non-pixel commands through the walker as usual; the
// FRAMEBUFFER shadow is patched only at fences, from readbacks of the
// rows a fence needs (§5.7).  Bring-up runs on the walker (§5.1):
// GPU mode engages when the card starts driving the monitor and
// disengages when it stops, on device loss, or on a readback storm.
//
// Anything the shader cannot express exactly enough (§5.5) — a
// rotate-mode stipple mask, the "colour before fog" destination blend
// factor, a zaColor depth compare — falls back: the touched rows are
// read back, the walker executes the command against the shadow, and
// the rectangle is uploaded again.  Correct by construction, slow only
// when it happens, counted by reason.
//
// Everything here runs on the raster pthread; the only other caller
// is v2_gpu_destroy (after the queue has been fenced).  Nothing reads
// live card state — the snapshot is the whole input, as for the walker.

#include "voodoo2_gpu.h"

#include "log.h"
#include "system.h"
#include "voodoo2_gpu_protocol.h"
#include "voodoo2_raster_priv.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("voodoo2");

#define V2GPU_MAX_TARGETS   8
#define V2GPU_MAX_TEX       1024
#define V2GPU_TEX_BYTES_CAP (64u << 20) // expanded texels held on the GPU
#define V2GPU_PAGE_SHIFT    12 // texture-RAM dirty tracking granularity (4 KB)
#define V2GPU_MAX_PAGES     (0x400000u >> V2GPU_PAGE_SHIFT) // 4 MB per TMU at most
#define V2GPU_STORM_BANDS   16 // readback bands per present interval that disengage
#define V2GPU_QUIET_FRAMES  8 // presents without a storm before re-engaging
#define V2GPU_ATTACH_MS     3000
#define V2GPU_ACK_MS        5000
#define V2GPU_MAX_ROWS      1024

// The fallback reasons, counted separately so a client that trips one
// constantly is visible in the stats line.
enum { V2GPU_FB_STIPPLE, V2GPU_FB_DSTFOG, V2GPU_FB_ZACMP, V2GPU_FB_COUNT };

// One render target: a physical framebuffer buffer the GPU holds as a
// colour or depth texture.  `rows_valid` marks the shadow rows that
// equal the GPU's (uploaded from the shadow, or read back) so a fence
// on a clean row costs no roundtrip.
typedef struct v2gpu_target {
    uint32_t id; // GPU-side id; 0 = unused slot
    uint32_t base; // physical byte base in the framebuffer
    uint32_t w, h; // pixels
    bool is_depth; // the aux buffer
    uint8_t rows_valid[V2GPU_MAX_ROWS / 8];
} v2gpu_target_t;

// One cached texture: a GPU mip chain built from the shadow texture RAM.
typedef struct v2gpu_tex {
    uint32_t id; // 0 = free
    int tmu;
    uint32_t hash;
    uint32_t fmt, ncc, pal_gen, mask;
    uint32_t lod_base[V2_RASTER_LODS];
    uint16_t lod_w0, lod_h0; // LOD-0 dims (the aspect/orientation)
    uint32_t base_level; // the GPU texture's level 0 is this Voodoo LOD
    uint32_t level_ok; // bit L set: LOD L is uploaded
    uint32_t level_gen[V2_RASTER_LODS]; // page generation the upload saw
    uint32_t bytes; // expanded bytes held on the GPU
    uint64_t last_use; // frame counter at last bind
} v2gpu_tex_t;

struct v2_gpu {
    v2_target_t *tgt;
    // The shared region and its views.
    uint8_t *region;
    uint32_t region_bytes;
    volatile uint32_t *ctrl;
    uint8_t *ring;
    uint32_t ring_size;
    uint8_t *rb;
    uint32_t rb_size;
    uint32_t head; // published byte count
    uint32_t wr; // write cursor (head..wr is written, unpublished)
    uint32_t last_wr0; // monotonic start of the last reserved record
    uint32_t seq; // last sync sequence requested
    bool attached, lost, warned_lost;
    // Mode.
    _Atomic int engaged;
    bool want; // the card drives the monitor
    uint32_t geom_stride, geom_h; // the engaged framebuffer geometry
    uint32_t next_id;
    v2gpu_target_t targets[V2GPU_MAX_TARGETS];
    // The open DRAW record: triangles of one state append to it.
    bool draw_open;
    uint32_t draw_off; // record start (ring offset, masked)
    uint32_t draw_wr0; // record start (the monotonic write cursor)
    v2gpu_draw_hdr_t draw_hdr;
    uint32_t draw_uniform[V2GPU_U_WORDS];
    // The open bypass-upload run: consecutive LFB pixels on one row.
    bool run_open;
    uint32_t run_off, run_wr0, run_tid, run_x, run_y, run_n;
    bool run_depth;
    // The texture cache and the texture-RAM dirty tracking.
    v2gpu_tex_t *tex;
    uint64_t tex_bytes;
    uint64_t frame; // presents so far (the LRU clock)
    uint32_t *page_gen[V2_RASTER_TMUS];
    uint32_t gen;
    uint32_t bound[V2_RASTER_TMUS]; // the last resolved texture per TMU (for the stats)
    // The gamma ramp, assembled from its chunks.
    uint8_t gamma[3 * 256];
    // Readback storm detection.
    uint32_t bands_this_frame, quiet_frames;
    // Statistics.
    uint64_t n_engage, n_disengage, n_storm, n_lost;
    uint64_t n_tris, n_draws, n_fills, n_lfb_runs, n_lfb_pipe, n_presents;
    uint64_t n_readback_bands, n_readback_rows;
    uint64_t n_tex_create, n_tex_upload, n_tex_upload_bytes, n_tex_evict;
    uint64_t n_fallback[V2GPU_FB_COUNT];
    uint64_t n_resync;
};

// ============================================================
// The ring writer
// ============================================================

static inline uint32_t v2gpu_load(v2_gpu_t *g, int word) {
    return __atomic_load_n(&g->ctrl[word], __ATOMIC_ACQUIRE);
}

static inline void v2gpu_store(v2_gpu_t *g, int word, uint32_t v) {
    __atomic_store_n(&g->ctrl[word], v, __ATOMIC_SEQ_CST);
}

// The worker reported the device gone (or stopped answering): drop to
// walker mode for good.  Logged once.
static void v2gpu_mark_lost(v2_gpu_t *g, const char *why) {
    if (!g->warned_lost) {
        g->warned_lost = true;
        LOG(0, "webgpu: %s — the walker draws from here on (the shadow may be stale)", why);
    }
    g->lost = true;
    g->n_lost++;
    atomic_store(&g->engaged, 0);
    for (int i = 0; i < V2GPU_MAX_TARGETS; i++)
        g->targets[i].id = 0;
    for (int i = 0; i < V2GPU_MAX_TEX; i++)
        g->tex[i].id = 0;
    g->tex_bytes = 0;
    g->draw_open = false;
    g->run_open = false;
}

static bool v2gpu_worker_lost(v2_gpu_t *g) {
    if (g->lost)
        return true;
    uint32_t st = v2gpu_load(g, V2GPU_C_STATUS);
    if (st != V2GPU_STATUS_ATTACHED) {
        v2gpu_mark_lost(g, st == V2GPU_STATUS_LOST ? "the GPU device was lost" : "the GPU worker detached");
        return true;
    }
    return false;
}

// Publish everything written so far.
static void v2gpu_publish(v2_gpu_t *g) {
    if (g->head == g->wr)
        return;
    g->head = g->wr;
    v2gpu_store(g, V2GPU_C_HEAD, g->head);
    gs_v2gpu_notify(&g->ctrl[V2GPU_C_HEAD]);
}

// Wait until `need` bytes are free in the ring (the worker consumes
// what was published; an open record between head and wr is never
// larger than the ring).
static bool v2gpu_wait_room(v2_gpu_t *g, uint32_t need) {
    uint32_t waited = 0;
    for (;;) {
        uint32_t tail = v2gpu_load(g, V2GPU_C_TAIL);
        uint32_t used = g->wr - tail;
        if (g->ring_size - used >= need)
            return true;
        if (v2gpu_worker_lost(g))
            return false;
        v2gpu_publish(g); // the worker can only free what it can see
        gs_v2gpu_wait(&g->ctrl[V2GPU_C_TAIL], tail, 20);
        waited += 20;
        if (waited > V2GPU_ACK_MS) {
            v2gpu_mark_lost(g, "the GPU worker stopped consuming the op ring");
            return false;
        }
    }
}

// Reserve a record of `len` bytes (header included, 4-byte multiple)
// that does not wrap; returns its ring offset or UINT32_MAX when lost.
static uint32_t v2gpu_reserve(v2_gpu_t *g, uint32_t kind, uint32_t len) {
    uint32_t mask = g->ring_size - 1u;
    uint32_t at = g->wr & mask;
    uint32_t pad = 0;
    if (at + len > g->ring_size)
        pad = g->ring_size - at; // a PAD record to the end first
    if (!v2gpu_wait_room(g, pad + len))
        return UINT32_MAX;
    if (pad) {
        uint32_t *p = (uint32_t *)(g->ring + at);
        p[0] = V2GPU_R_PAD;
        p[1] = pad;
        g->wr += pad;
        at = 0;
    }
    uint32_t *p = (uint32_t *)(g->ring + at);
    p[0] = kind;
    p[1] = len;
    g->last_wr0 = g->wr; // where this record starts, monotonic
    g->wr += len;
    return at;
}

// Emit a fixed-size record from `words`.
static bool v2gpu_emit(v2_gpu_t *g, uint32_t kind, const uint32_t *words, uint32_t n_words) {
    uint32_t at = v2gpu_reserve(g, kind, 8u + 4u * n_words);
    if (at == UINT32_MAX)
        return false;
    memcpy(g->ring + at + 8u, words, 4u * n_words);
    return true;
}

// Publish and wait until the worker has acknowledged `seq`.
static bool v2gpu_wait_ack(v2_gpu_t *g, uint32_t seq) {
    v2gpu_publish(g);
    uint32_t waited = 0;
    for (;;) {
        uint32_t ack = v2gpu_load(g, V2GPU_C_ACK);
        if ((int32_t)(ack - seq) >= 0)
            return true;
        if (v2gpu_worker_lost(g))
            return false;
        gs_v2gpu_wait(&g->ctrl[V2GPU_C_ACK], ack, 20);
        waited += 20;
        if (waited > V2GPU_ACK_MS) {
            v2gpu_mark_lost(g, "the GPU worker stopped answering");
            return false;
        }
    }
}

// ============================================================
// The open records: the draw range and the bypass run
// ============================================================

// Close the open DRAW record: patch its length and vertex count.
static void v2gpu_close_draw(v2_gpu_t *g) {
    if (!g->draw_open)
        return;
    g->draw_open = false;
    uint32_t *p = (uint32_t *)(g->ring + g->draw_off);
    uint32_t len = 8u + (uint32_t)sizeof(v2gpu_draw_hdr_t) + V2GPU_U_BYTES + g->draw_hdr.n_verts * V2GPU_VERTEX_BYTES;
    p[1] = len;
    memcpy(g->ring + g->draw_off + 8u, &g->draw_hdr, sizeof(g->draw_hdr));
    g->wr = g->draw_wr0 + len; // give the unused reservation back
    g->n_draws++;
}

// Close the open bypass-upload run.
static void v2gpu_close_run(v2_gpu_t *g) {
    if (!g->run_open)
        return;
    g->run_open = false;
    uint32_t *p = (uint32_t *)(g->ring + g->run_off);
    uint32_t bpp = g->run_depth ? 2u : 4u;
    uint32_t len = 8u + 20u + ((g->run_n * bpp + 3u) & ~3u);
    p[1] = len;
    p[2] = g->run_tid;
    p[3] = g->run_x;
    p[4] = g->run_y;
    p[5] = g->run_n;
    p[6] = 1;
    g->wr = g->run_wr0 + len;
    g->n_lfb_runs++;
}

static void v2gpu_close_all(v2_gpu_t *g) {
    v2gpu_close_draw(g);
    v2gpu_close_run(g);
}

// ============================================================
// Targets
// ============================================================

static inline void v2gpu_rows_set(v2gpu_target_t *t, uint32_t y0, uint32_t y1, bool valid) {
    for (uint32_t y = y0; y < y1 && y < V2GPU_MAX_ROWS; y++) {
        if (valid)
            t->rows_valid[y >> 3] |= (uint8_t)(1u << (y & 7));
        else
            t->rows_valid[y >> 3] &= (uint8_t) ~(1u << (y & 7));
    }
}

static inline bool v2gpu_row_valid(const v2gpu_target_t *t, uint32_t y) {
    return (t->rows_valid[y >> 3] >> (y & 7)) & 1u;
}

// Expand one 5-6-5 pixel to the rgba8 the colour attachment holds.
static inline void v2gpu_expand565(uint16_t px, uint8_t *out) {
    uint32_t r5 = (px >> 11) & 0x1Fu, g6 = (px >> 5) & 0x3Fu, b5 = px & 0x1Fu;
    out[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
    out[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
    out[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    out[3] = 0xFF;
}

// Upload a rectangle of the target from the shadow (rows y0..y1, all
// columns x0..x1), in bands so no record exceeds a fraction of the ring.
static bool v2gpu_upload_rect(v2_gpu_t *g, v2gpu_target_t *t, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1) {
    v2gpu_close_all(g);
    if (x1 > t->w)
        x1 = t->w;
    if (y1 > t->h)
        y1 = t->h;
    if (x0 >= x1 || y0 >= y1)
        return true;
    uint32_t bpp = t->is_depth ? 2u : 4u;
    uint32_t w = x1 - x0;
    uint32_t band = V2GPU_RB_ROWS;
    for (uint32_t y = y0; y < y1; y += band) {
        uint32_t n = y1 - y < band ? y1 - y : band;
        uint32_t bytes = w * n * bpp;
        uint32_t at = v2gpu_reserve(g, V2GPU_R_UPLOAD, 8u + 20u + ((bytes + 3u) & ~3u));
        if (at == UINT32_MAX)
            return false;
        uint32_t *p = (uint32_t *)(g->ring + at + 8u);
        p[0] = t->id;
        p[1] = x0;
        p[2] = y;
        p[3] = w;
        p[4] = n;
        uint8_t *dst = g->ring + at + 28u;
        for (uint32_t row = 0; row < n; row++) {
            uint32_t src = (t->base + (y + row) * g->geom_stride + x0 * 2u) & V2_FB_MASK;
            for (uint32_t x = 0; x < w; x++) {
                uint32_t a = (src + 2u * x) & V2_FB_MASK;
                uint16_t px = (uint16_t)(g->tgt->fb[a] | ((uint16_t)g->tgt->fb[(a + 1u) & V2_FB_MASK] << 8));
                if (t->is_depth) {
                    dst[0] = (uint8_t)px;
                    dst[1] = (uint8_t)(px >> 8);
                    dst += 2;
                } else {
                    v2gpu_expand565(px, dst);
                    dst += 4;
                }
            }
        }
    }
    return true;
}

// Read rows [y0, y1) of the target back into the shadow, band by band,
// skipping bands whose rows are already valid.  Each band is one
// roundtrip through the worker (a fence in the proposal's sense).
static bool v2gpu_readback_rows(v2_gpu_t *g, v2gpu_target_t *t, uint32_t y0, uint32_t y1) {
    v2gpu_close_all(g);
    if (y1 > t->h)
        y1 = t->h;
    uint32_t band = V2GPU_RB_ROWS;
    for (uint32_t b = y0 - (y0 % band); b < y1; b += band) {
        uint32_t b1 = b + band < t->h ? b + band : t->h;
        bool dirty = false;
        for (uint32_t y = b; y < b1; y++) {
            if (!v2gpu_row_valid(t, y)) {
                dirty = true;
                break;
            }
        }
        if (!dirty)
            continue;
        uint32_t words[4] = {++g->seq, t->id, b, b1};
        v2gpu_store(g, V2GPU_C_REQ, g->seq);
        if (!v2gpu_emit(g, V2GPU_R_READBACK, words, 4))
            return false;
        if (!v2gpu_wait_ack(g, g->seq))
            return false;
        // The worker left 16-bit pixels, w per row, packed; copy them
        // into the shadow at the buffer's own addresses.
        const uint8_t *src = g->rb;
        for (uint32_t y = b; y < b1; y++) {
            uint32_t dst = (t->base + y * g->geom_stride) & V2_FB_MASK;
            if (dst + t->w * 2u <= V2_RASTER_FB_SIZE)
                memcpy(g->tgt->fb + dst, src, t->w * 2u);
            src += t->w * 2u;
        }
        v2gpu_rows_set(t, b, b1, true);
        g->n_readback_bands++;
        g->n_readback_rows += b1 - b;
        g->bands_this_frame++;
    }
    return true;
}

// Find or create the target for a software buffer select (0 front, 1
// back, 3 aux) under the snapshot's geometry.
static v2gpu_target_t *v2gpu_target_for_buffer(v2_gpu_t *g, const v2_draw_state_t *st, uint32_t buffer) {
    uint32_t base = st->buf_base[buffer & 3u];
    bool is_depth = (buffer & 3u) == 3u;
    v2gpu_target_t *free_slot = NULL;
    for (int i = 0; i < V2GPU_MAX_TARGETS; i++) {
        v2gpu_target_t *t = &g->targets[i];
        if (t->id && t->base == base)
            return t;
        if (!t->id && !free_slot)
            free_slot = t;
    }
    if (!free_slot)
        return NULL;
    v2gpu_target_t *t = free_slot;
    memset(t, 0, sizeof(*t));
    t->id = ++g->next_id;
    t->base = base;
    t->w = st->stride / 2u;
    if (t->w > 1024u)
        t->w = 1024u;
    t->h = st->screen_h > V2GPU_MAX_ROWS ? V2GPU_MAX_ROWS : st->screen_h;
    t->is_depth = is_depth;
    uint32_t words[4] = {t->id, is_depth ? V2GPU_ROLE_DEPTH : V2GPU_ROLE_COLOR, t->w, t->h};
    v2gpu_close_all(g);
    if (!v2gpu_emit(g, V2GPU_R_TARGET, words, 4))
        return NULL;
    // A new target starts as the shadow's contents: nothing has drawn
    // into it on the GPU yet, so the shadow is exact.
    if (!v2gpu_upload_rect(g, t, 0, t->w, 0, t->h))
        return NULL;
    v2gpu_rows_set(t, 0, t->h, true);
    return t;
}

// The target whose rows cover framebuffer byte address `addr`, if any.
static v2gpu_target_t *v2gpu_target_for_addr(v2_gpu_t *g, uint32_t addr) {
    for (int i = 0; i < V2GPU_MAX_TARGETS; i++) {
        v2gpu_target_t *t = &g->targets[i];
        if (t->id && addr >= t->base && addr < t->base + t->h * g->geom_stride)
            return t;
    }
    return NULL;
}

// ============================================================
// Engagement
// ============================================================

static void v2gpu_emit_mode(v2_gpu_t *g, bool on, uint32_t w, uint32_t h) {
    uint32_t words[3] = {on ? 1u : 0u, w, h};
    v2gpu_emit(g, V2GPU_R_MODE, words, 3);
}

static void v2gpu_free_textures(v2_gpu_t *g) {
    for (int i = 0; i < V2GPU_MAX_TEX; i++) {
        if (g->tex[i].id) {
            uint32_t w = g->tex[i].id;
            v2gpu_emit(g, V2GPU_R_TEX_FREE, &w, 1);
            g->tex[i].id = 0;
        }
    }
    g->tex_bytes = 0;
    g->bound[0] = g->bound[1] = 0;
}

// Engage GPU mode under the snapshot's geometry: the colour and aux
// buffers go up from the shadow (exact at this instant — the fence
// audit guarantees it), the page tracking resets.
static void v2gpu_engage(v2_gpu_t *g, const v2_draw_state_t *st) {
    if (g->lost || atomic_load(&g->engaged))
        return;
    g->geom_stride = st->stride;
    g->geom_h = st->screen_h;
    for (int i = 0; i < V2GPU_MAX_TARGETS; i++)
        g->targets[i].id = 0;
    atomic_store(&g->engaged, 1); // the target helpers require it
    v2gpu_target_t *front = v2gpu_target_for_buffer(g, st, 0);
    v2gpu_target_for_buffer(g, st, 1);
    v2gpu_target_for_buffer(g, st, 3);
    if (!front || g->lost) {
        atomic_store(&g->engaged, 0);
        return;
    }
    for (int t = 0; t < V2_RASTER_TMUS; t++)
        memset(g->page_gen[t], 0, V2GPU_MAX_PAGES * sizeof(uint32_t));
    g->gen = 1;
    g->bands_this_frame = 0;
    v2gpu_emit_mode(g, true, front->w, front->h);
    v2gpu_publish(g);
    g->n_engage++;
    LOG(1, "webgpu: engaged (%ux%u, stride %u)", front->w, front->h, st->stride);
}

// Leave GPU mode: read every target back unless `discard`, then drop
// every GPU resource.  The walker continues from the shadow.
static void v2gpu_disengage(v2_gpu_t *g, bool discard) {
    if (!atomic_load(&g->engaged))
        return;
    v2gpu_close_all(g);
    if (!discard && !g->lost) {
        for (int i = 0; i < V2GPU_MAX_TARGETS; i++) {
            if (g->targets[i].id)
                v2gpu_readback_rows(g, &g->targets[i], 0, g->targets[i].h);
        }
    }
    if (!g->lost) {
        for (int i = 0; i < V2GPU_MAX_TARGETS; i++) {
            if (g->targets[i].id) {
                uint32_t w = g->targets[i].id;
                v2gpu_emit(g, V2GPU_R_TARGET_FREE, &w, 1);
            }
        }
        v2gpu_free_textures(g);
        v2gpu_emit_mode(g, false, 0, 0);
        v2gpu_publish(g);
    }
    for (int i = 0; i < V2GPU_MAX_TARGETS; i++)
        g->targets[i].id = 0;
    atomic_store(&g->engaged, 0);
    g->n_disengage++;
    LOG(1, "webgpu: disengaged (%s)", discard ? "discarding the GPU's pixels" : "read back into the shadow");
}

// The framebuffer geometry moved under an engaged GPU: read the old
// layout back, then come back up under the new one.
static bool v2gpu_check_geometry(v2_gpu_t *g, const v2_draw_state_t *st) {
    if (!atomic_load(&g->engaged))
        return false;
    if (st->stride == g->geom_stride && st->screen_h == g->geom_h)
        return true;
    g->n_resync++;
    v2gpu_disengage(g, false);
    v2gpu_engage(g, st);
    return atomic_load(&g->engaged) != 0;
}

// ============================================================
// The texture cache
// ============================================================

static inline uint32_t v2gpu_hash32(uint32_t h, uint32_t v) {
    h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
    return h;
}

// Mark the texture-RAM pages a download touched (the executor already
// wrote the bytes; this records that any cached level over them is
// stale).
static void v2gpu_mark_tex_write(v2_gpu_t *g, const v2_draw_state_t *st, const v2_cmd_t *cmd) {
    g->gen++;
    for (uint32_t i = 0; i < cmd->u.tex.n; i++) {
        uint32_t off = cmd->u.tex.off + 4u * i;
        int tmu = (off >> 21) & 3u;
        if (tmu >= V2_RASTER_TMUS)
            tmu &= 1;
        const v2_tmu_state_t *tm = &st->tmu[tmu];
        uint32_t lod = (off >> 17) & 0xFu;
        if (lod >= V2_RASTER_LODS)
            lod = V2_RASTER_LODS - 1u;
        uint32_t t = (off >> 9) & 0xFFu;
        uint32_t s;
        if (!tm->is8)
            s = ((off >> 2) & 0x7Fu) << 1;
        else if (tm->mode & (1u << 31))
            s = ((off >> 2) & 0x7Fu) << 2;
        else
            s = ((off >> 3) & 0x3Fu) << 2;
        uint32_t a0 = v2_texel_addr(tm, (int)lod, s, t);
        uint32_t a1 = (a0 + 8u) & tm->mask; // one word spans at most 8 bytes
        g->page_gen[tmu][a0 >> V2GPU_PAGE_SHIFT] = g->gen;
        g->page_gen[tmu][a1 >> V2GPU_PAGE_SHIFT] = g->gen;
    }
}

// Newest page generation over the byte range [lo, lo + len) of a TMU's
// RAM, wrapping at its addressable size.
static uint32_t v2gpu_range_gen(v2_gpu_t *g, int tmu, uint32_t mask, uint32_t lo, uint32_t len) {
    uint32_t newest = 0;
    uint32_t size = mask + 1u;
    if (len > size)
        len = size;
    uint32_t p0 = (lo & mask) >> V2GPU_PAGE_SHIFT;
    uint32_t p1 = ((lo + len - 1u) & mask) >> V2GPU_PAGE_SHIFT;
    uint32_t last = mask >> V2GPU_PAGE_SHIFT;
    if (p1 >= p0) {
        for (uint32_t p = p0; p <= p1; p++)
            if (g->page_gen[tmu][p] > newest)
                newest = g->page_gen[tmu][p];
    } else { // wrapped
        for (uint32_t p = p0; p <= last; p++)
            if (g->page_gen[tmu][p] > newest)
                newest = g->page_gen[tmu][p];
        for (uint32_t p = 0; p <= p1; p++)
            if (g->page_gen[tmu][p] > newest)
                newest = g->page_gen[tmu][p];
    }
    return newest;
}

static bool v2gpu_fmt_uses_tables(uint32_t fmt) {
    return fmt == 1u || fmt == 5u || fmt == 6u || fmt == 9u || fmt == 14u;
}

static void v2gpu_tex_free(v2_gpu_t *g, v2gpu_tex_t *e) {
    uint32_t w = e->id;
    v2gpu_emit(g, V2GPU_R_TEX_FREE, &w, 1);
    g->tex_bytes -= e->bytes;
    if (g->bound[e->tmu] == e->id)
        g->bound[e->tmu] = 0;
    e->id = 0;
}

// Evict least-recently-used entries (never one bound this frame) until
// the GPU holds less than the cap.
static void v2gpu_tex_trim(v2_gpu_t *g) {
    while (g->tex_bytes > V2GPU_TEX_BYTES_CAP) {
        v2gpu_tex_t *victim = NULL;
        for (int i = 0; i < V2GPU_MAX_TEX; i++) {
            v2gpu_tex_t *e = &g->tex[i];
            if (!e->id || e->last_use == g->frame)
                continue;
            if (!victim || e->last_use < victim->last_use)
                victim = e;
        }
        if (!victim)
            return;
        v2gpu_tex_free(g, victim);
        g->n_tex_evict++;
    }
}

// Convert and upload one LOD level from the shadow texture RAM through
// the normative expansion.
static bool v2gpu_tex_upload_level(v2_gpu_t *g, const v2_tmu_state_t *tm, int tmu, v2gpu_tex_t *e, uint32_t lod) {
    uint32_t w = tm->lod_w[lod], h = tm->lod_h[lod];
    if (!w || !h)
        return true;
    uint32_t bytes = w * h * 4u;
    uint32_t at = v2gpu_reserve(g, V2GPU_R_TEX_UPLOAD, 8u + 16u + bytes);
    if (at == UINT32_MAX)
        return false;
    uint32_t *p = (uint32_t *)(g->ring + at + 8u);
    p[0] = e->id;
    p[1] = lod - e->base_level;
    p[2] = w;
    p[3] = h;
    uint8_t *dst = g->ring + at + 24u;
    const uint32_t *lut = tm->is8 ? v2_expand_lut(tm, g->tgt, tmu) : NULL;
    for (uint32_t t = 0; t < h; t++) {
        for (uint32_t s = 0; s < w; s++) {
            uint32_t raw = v2_texel_raw(tm, g->tgt, tmu, (int)lod, s, t);
            uint32_t argb = lut ? lut[raw] : v2_texel_expand(tm, g->tgt, tmu, raw);
            dst[0] = (uint8_t)(argb >> 16);
            dst[1] = (uint8_t)(argb >> 8);
            dst[2] = (uint8_t)argb;
            dst[3] = (uint8_t)(argb >> 24);
            dst += 4;
        }
    }
    e->level_ok |= 1u << lod;
    e->level_gen[lod] = g->gen;
    g->n_tex_upload++;
    g->n_tex_upload_bytes += bytes;
    return true;
}

// Resolve the GPU texture for one TMU under the snapshot: find the
// cache entry by identity, (re)create it when its base level must
// drop, and bring every level the draw can sample up to date.
// Returns the GPU id, or 0 when the GPU is gone.
static uint32_t v2gpu_tex_resolve(v2_gpu_t *g, const v2_draw_state_t *st, int tmu, uint32_t *base_level_out) {
    const v2_tmu_state_t *tm = &st->tmu[tmu];
    uint32_t pal_gen = v2gpu_fmt_uses_tables(tm->fmt) ? g->tgt->pal_gen[tmu] : 0u;
    uint32_t hash = v2gpu_hash32(0x811C9DC5u, (uint32_t)tmu);
    hash = v2gpu_hash32(hash, tm->fmt | (tm->ncc_table << 4) | (tm->mask << 8));
    hash = v2gpu_hash32(hash, pal_gen);
    for (int l = 0; l < V2_RASTER_LODS; l++)
        hash = v2gpu_hash32(hash, tm->lod_base[l]);
    hash = v2gpu_hash32(hash, tm->lod_w[0] | ((uint32_t)tm->lod_h[0] << 16));
    // The levels the draw can reach: lodmin..lodmax in whole levels,
    // one more for the split-texture parity snap, never past 8.
    uint32_t lo = (uint32_t)(tm->lodmin < 0 ? 0 : tm->lodmin) >> 2;
    uint32_t hi = (uint32_t)(tm->lodmax < 0 ? 0 : tm->lodmax) >> 2;
    if (tm->tsplit && hi < 8u)
        hi++;
    if (lo > 8u)
        lo = 8u;
    if (hi > 8u)
        hi = 8u;
    if (hi < lo)
        hi = lo;
    v2gpu_tex_t *e = NULL;
    v2gpu_tex_t *free_slot = NULL;
    v2gpu_tex_t *oldest = NULL;
    for (int i = 0; i < V2GPU_MAX_TEX; i++) {
        v2gpu_tex_t *c = &g->tex[i];
        if (!c->id) {
            if (!free_slot)
                free_slot = c;
            continue;
        }
        if (!oldest || c->last_use < oldest->last_use)
            oldest = c;
        if (c->hash != hash || c->tmu != tmu || c->fmt != tm->fmt || c->ncc != tm->ncc_table || c->pal_gen != pal_gen ||
            c->mask != tm->mask || c->lod_w0 != tm->lod_w[0] || c->lod_h0 != tm->lod_h[0] ||
            memcmp(c->lod_base, tm->lod_base, sizeof(c->lod_base)) != 0)
            continue;
        e = c;
        break;
    }
    if (e && lo < e->base_level) {
        // The chain must start higher than it was built: rebuild.
        v2gpu_tex_free(g, e);
        e = NULL;
    }
    if (!e) {
        if (!free_slot) {
            if (!oldest)
                return 0;
            v2gpu_tex_free(g, oldest);
            g->n_tex_evict++;
            free_slot = oldest;
        }
        e = free_slot;
        memset(e, 0, sizeof(*e));
        e->id = ++g->next_id;
        e->tmu = tmu;
        e->hash = hash;
        e->fmt = tm->fmt;
        e->ncc = tm->ncc_table;
        e->pal_gen = pal_gen;
        e->mask = tm->mask;
        e->lod_w0 = tm->lod_w[0];
        e->lod_h0 = tm->lod_h[0];
        memcpy(e->lod_base, tm->lod_base, sizeof(e->lod_base));
        e->base_level = lo;
        uint32_t levels = 9u - lo;
        uint32_t words[4] = {e->id, tm->lod_w[lo], tm->lod_h[lo], levels};
        v2gpu_close_all(g);
        if (!v2gpu_emit(g, V2GPU_R_TEX, words, 4))
            return 0;
        for (uint32_t l = lo; l <= 8u; l++)
            e->bytes += (uint32_t)tm->lod_w[l] * tm->lod_h[l] * 4u;
        g->tex_bytes += e->bytes;
        g->n_tex_create++;
        v2gpu_tex_trim(g);
    }
    e->last_use = g->frame;
    // Bring the reachable levels up to date against the page tracking.
    for (uint32_t l = lo; l <= hi; l++) {
        uint32_t len = (uint32_t)tm->lod_w[l] * tm->lod_h[l] * (tm->is8 ? 1u : 2u);
        bool stale =
            !(e->level_ok & (1u << l)) || v2gpu_range_gen(g, tmu, tm->mask, tm->lod_base[l], len) > e->level_gen[l];
        if (stale) {
            v2gpu_close_all(g);
            if (!v2gpu_tex_upload_level(g, tm, tmu, e, l))
                return 0;
        }
    }
    *base_level_out = e->base_level;
    g->bound[tmu] = e->id;
    return e->id;
}

// ============================================================
// Draw records
// ============================================================

static inline float v2gpu_f(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}
static inline uint32_t v2gpu_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

// The pipeline key: the state WebGPU bakes into a render pipeline.
static uint32_t v2gpu_pipe_key(const v2_draw_state_t *st, bool fill, bool write_color, bool write_depth) {
    uint32_t key = 0;
    if (fill) {
        if (write_depth)
            key |= V2GPU_PK_DEPTH_WRITE;
        if (write_color)
            key |= V2GPU_PK_COLOR_WRITE;
        return key;
    }
    uint32_t fbz = st->fbz, amode = st->amode;
    if (amode & 0x10u) {
        key |= V2GPU_PK_BLEND;
        key |= ((amode >> 8) & 0xFu) << V2GPU_PK_SRC_SHIFT;
        key |= ((amode >> 12) & 0xFu) << V2GPU_PK_DST_SHIFT;
    }
    if (fbz & 0x10u) {
        key |= V2GPU_PK_DEPTH_TEST;
        key |= ((fbz >> 5) & 7u) << V2GPU_PK_DFUNC_SHIFT;
    }
    if (fbz & 0x400u)
        key |= V2GPU_PK_DEPTH_WRITE;
    if (fbz & 0x200u)
        key |= V2GPU_PK_COLOR_WRITE;
    return key;
}

// Fill the uniform block from the snapshot.
static void v2gpu_build_uniform(uint32_t *u, const v2_draw_state_t *st, uint32_t flags, uint32_t fill, float tw,
                                float th, const uint32_t base_level[2], const bool bound[2]) {
    memset(u, 0, V2GPU_U_BYTES);
    u[V2GPU_U_FBZ] = st->fbz;
    u[V2GPU_U_FCP] = st->fcp;
    u[V2GPU_U_AMODE] = st->amode;
    u[V2GPU_U_FOGMODE] = st->fogmode;
    u[V2GPU_U_FOGCOLOR] = st->fogcolor;
    u[V2GPU_U_COLOR0] = st->color0;
    u[V2GPU_U_COLOR1] = st->color1;
    u[V2GPU_U_ZACOLOR] = st->zacolor;
    u[V2GPU_U_CHROMAKEY] = st->chromakey;
    u[V2GPU_U_CHROMARANGE] = st->chromarange;
    u[V2GPU_U_STIPPLE] = 0; // filled by the caller from the target
    u[V2GPU_U_FLAGS] = flags;
    u[V2GPU_U_SCREEN_H] = v2gpu_bits((float)st->screen_h);
    u[V2GPU_U_TARGET_W] = v2gpu_bits(tw);
    u[V2GPU_U_TARGET_H] = v2gpu_bits(th);
    u[V2GPU_U_FILL] = fill;
    memcpy(&u[V2GPU_U_FOGTABLE], st->fogtable, sizeof(st->fogtable));
    for (int t = 0; t < V2_RASTER_TMUS; t++) {
        const v2_tmu_state_t *tm = &st->tmu[t];
        uint32_t *ut = &u[t == 0 ? V2GPU_U_TMU0 : V2GPU_U_TMU1];
        ut[V2GPU_UT_MODE] = tm->mode;
        ut[V2GPU_UT_TLOD] = tm->tlod;
        ut[V2GPU_UT_TREX1] = tm->trex1;
        ut[V2GPU_UT_LODMIN] = (uint32_t)tm->lodmin;
        ut[V2GPU_UT_LODMAX] = (uint32_t)tm->lodmax;
        ut[V2GPU_UT_LODBIAS] = (uint32_t)tm->lodbias;
        uint32_t tf = 0;
        if (tm->bilin_min)
            tf |= V2GPU_TF_BILIN_MIN;
        if (tm->bilin_mag)
            tf |= V2GPU_TF_BILIN_MAG;
        if (tm->clamp_s)
            tf |= V2GPU_TF_CLAMP_S;
        if (tm->clamp_t)
            tf |= V2GPU_TF_CLAMP_T;
        if (tm->persp)
            tf |= V2GPU_TF_PERSP;
        if (tm->tclampw)
            tf |= V2GPU_TF_TCLAMPW;
        if (tm->send_config)
            tf |= V2GPU_TF_SEND_CFG;
        if (tm->tsplit)
            tf |= V2GPU_TF_TSPLIT;
        if (tm->lod_odd)
            tf |= V2GPU_TF_LOD_ODD;
        if (tm->lod_pinned)
            tf |= V2GPU_TF_LOD_PINNED;
        if (bound[t])
            tf |= V2GPU_TF_BOUND;
        ut[V2GPU_UT_FLAGS] = tf;
        ut[V2GPU_UT_BASE_LEVEL] = base_level[t];
        ut[V2GPU_UT_W0] = tm->lod_w[0];
        ut[V2GPU_UT_H0] = tm->lod_h[0];
    }
}

// Open a DRAW record (closing the previous one) with the given header
// and uniform block; vertices are appended by the caller.
static bool v2gpu_open_draw(v2_gpu_t *g, const v2gpu_draw_hdr_t *hdr, const uint32_t *uniform) {
    v2gpu_close_all(g);
    uint32_t max_len = 8u + (uint32_t)sizeof(v2gpu_draw_hdr_t) + V2GPU_U_BYTES + V2GPU_MAX_VERTS * V2GPU_VERTEX_BYTES;
    uint32_t at = v2gpu_reserve(g, V2GPU_R_DRAW, max_len);
    if (at == UINT32_MAX)
        return false;
    g->draw_open = true;
    g->draw_off = at;
    g->draw_wr0 = g->last_wr0;
    g->draw_hdr = *hdr;
    g->draw_hdr.n_verts = 0;
    memcpy(g->draw_uniform, uniform, V2GPU_U_BYTES);
    memcpy(g->ring + at + 8u + sizeof(v2gpu_draw_hdr_t), uniform, V2GPU_U_BYTES);
    return true;
}

// Can `n` more vertices under this header/uniform join the open record?
static bool v2gpu_draw_matches(const v2_gpu_t *g, const v2gpu_draw_hdr_t *hdr, const uint32_t *uniform, uint32_t n) {
    if (!g->draw_open || g->draw_hdr.n_verts + n > V2GPU_MAX_VERTS)
        return false;
    const v2gpu_draw_hdr_t *o = &g->draw_hdr;
    if (o->color_id != hdr->color_id || o->depth_id != hdr->depth_id || o->tex_id[0] != hdr->tex_id[0] ||
        o->tex_id[1] != hdr->tex_id[1] || o->pipe_key != hdr->pipe_key || o->sx0 != hdr->sx0 || o->sy0 != hdr->sy0 ||
        o->sx1 != hdr->sx1 || o->sy1 != hdr->sy1)
        return false;
    return memcmp(g->draw_uniform, uniform, V2GPU_U_BYTES) == 0;
}

static float *v2gpu_vertex_ptr(v2_gpu_t *g) {
    uint32_t off = g->draw_off + 8u + (uint32_t)sizeof(v2gpu_draw_hdr_t) + V2GPU_U_BYTES +
                   g->draw_hdr.n_verts * V2GPU_VERTEX_BYTES;
    g->draw_hdr.n_verts++;
    return (float *)(g->ring + off);
}

// One vertex of a triangle: the walker's iterators, evaluated in closed
// form at the vertex's real 12.4 position, relative to vertex A's
// truncated position — the plane the walker iterates, so the GPU's
// linear interpolation reproduces it at every pixel centre (§5.3).
static void v2gpu_vertex(float *o, const voodoo2_tri_t *T, int32_t vx, int32_t vy) {
    double x = vx / 16.0, y = vy / 16.0;
    double dx = x - (double)(T->ax >> 4), dy = y - (double)(T->ay >> 4);
    o[0] = (float)x;
    o[1] = (float)y;
    o[2] = (float)(((double)T->z + (double)T->dzdx * dx + (double)T->dzdy * dy) / 4096.0);
    o[3] = (float)(((double)T->w + (double)T->dwdx * dx + (double)T->dwdy * dy) / 1073741824.0);
    o[4] = (float)(((double)T->r + (double)T->drdx * dx + (double)T->drdy * dy) / 4096.0);
    o[5] = (float)(((double)T->g + (double)T->dgdx * dx + (double)T->dgdy * dy) / 4096.0);
    o[6] = (float)(((double)T->b + (double)T->dbdx * dx + (double)T->dbdy * dy) / 4096.0);
    o[7] = (float)(((double)T->a + (double)T->dadx * dx + (double)T->dady * dy) / 4096.0);
    for (int t = 0; t < V2_RASTER_TMUS; t++) {
        o[8 + 3 * t] = (float)(((double)T->s[t] + (double)T->dsdx[t] * dx + (double)T->dsdy[t] * dy) / 262144.0);
        o[9 + 3 * t] = (float)(((double)T->t[t] + (double)T->dtdx[t] * dx + (double)T->dtdy[t] * dy) / 262144.0);
        o[10 + 3 * t] =
            (float)(((double)T->tw[t] + (double)T->dtwdx[t] * dx + (double)T->dtwdy[t] * dy) / 1073741824.0);
    }
    o[14] = 0.0f;
    o[15] = 0.0f;
}

// A vertex carrying literal iterator values (fills, LFB pixels).
static void v2gpu_vertex_flat(float *o, float x, float y, float z, float w, float r, float gg, float b, float a) {
    memset(o, 0, V2GPU_VERTEX_BYTES);
    o[0] = x;
    o[1] = y;
    o[2] = z;
    o[3] = w;
    o[4] = r;
    o[5] = gg;
    o[6] = b;
    o[7] = a;
}

// The scissor rectangle in target rows for a walker-space clip rect.
static void v2gpu_scissor(const v2_draw_state_t *st, const v2gpu_target_t *t, int32_t x0, int32_t x1, int32_t y0,
                          int32_t y1, bool flip, v2gpu_draw_hdr_t *hdr) {
    if (flip) {
        int32_t h = (int32_t)st->screen_h;
        int32_t fy0 = h - y1, fy1 = h - y0;
        y0 = fy0;
        y1 = fy1;
    }
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > (int32_t)t->w)
        x1 = (int32_t)t->w;
    if (y1 > (int32_t)t->h)
        y1 = (int32_t)t->h;
    hdr->sx0 = x0;
    hdr->sy0 = y0;
    hdr->sx1 = x1 < x0 ? x0 : x1;
    hdr->sy1 = y1 < y0 ? y0 : y1;
}

// Emit a fill draw over [x0,x1) x [y0,y1) (walker coordinates when
// `flip` follows fbzMode[17], target rows otherwise).
static bool v2gpu_fill(v2_gpu_t *g, const v2_draw_state_t *st, v2gpu_target_t *ct, v2gpu_target_t *dt, int32_t x0,
                       int32_t x1, int32_t y0, int32_t y1, bool flip, uint32_t fill, bool raw565, uint32_t depth16,
                       bool write_color, bool write_depth) {
    if (x0 >= x1 || y0 >= y1)
        return true;
    v2gpu_target_t *dims = ct ? ct : dt;
    if (!dims)
        return true;
    v2gpu_draw_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.color_id = ct ? ct->id : 0;
    hdr.depth_id = dt ? dt->id : 0;
    hdr.pipe_key = v2gpu_pipe_key(st, true, write_color && ct, write_depth && dt);
    hdr.sx0 = 0;
    hdr.sy0 = 0;
    hdr.sx1 = (int32_t)dims->w;
    hdr.sy1 = (int32_t)dims->h;
    uint32_t flags = V2GPU_F_FILL | (raw565 ? V2GPU_F_FILL_RAW : 0u) | (flip ? V2GPU_F_Y_FLIP : 0u);
    uint32_t uniform[V2GPU_U_WORDS];
    uint32_t base_level[2] = {0, 0};
    bool bound[2] = {false, false};
    v2gpu_build_uniform(uniform, st, flags, fill, (float)dims->w, (float)dims->h, base_level, bound);
    uniform[V2GPU_U_ZACOLOR] = (st->zacolor & 0xFFFF0000u) | (depth16 & 0xFFFFu);
    if (!v2gpu_open_draw(g, &hdr, uniform))
        return false;
    float fx0 = (float)x0 - 0.5f, fx1 = (float)x1 - 0.5f, fy0 = (float)y0 - 0.5f, fy1 = (float)y1 - 0.5f;
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), fx0, fy0, 0, 0, 0, 0, 0, 0);
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), fx1, fy0, 0, 0, 0, 0, 0, 0);
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), fx0, fy1, 0, 0, 0, 0, 0, 0);
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), fx1, fy0, 0, 0, 0, 0, 0, 0);
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), fx1, fy1, 0, 0, 0, 0, 0, 0);
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), fx0, fy1, 0, 0, 0, 0, 0, 0);
    v2gpu_close_draw(g);
    // The rows the fill touched are the GPU's now.
    int32_t ry0 = y0, ry1 = y1;
    if (flip) {
        ry0 = (int32_t)st->screen_h - y1;
        ry1 = (int32_t)st->screen_h - y0;
    }
    if (ry0 < 0)
        ry0 = 0;
    if (write_color && ct)
        v2gpu_rows_set(ct, (uint32_t)ry0, (uint32_t)ry1, false);
    if (write_depth && dt)
        v2gpu_rows_set(dt, (uint32_t)ry0, (uint32_t)ry1, false);
    g->n_fills++;
    return true;
}

// ============================================================
// Fallback: the walker draws, bracketed by a readback and an upload
// ============================================================

static void v2gpu_fallback(v2_gpu_t *g, const v2_draw_state_t *st, v2_target_t *tgt, const v2_cmd_t *cmd, int reason,
                           int32_t x0, int32_t x1, int32_t y0, int32_t y1) {
    g->n_fallback[reason]++;
    bool flip = (st->fbz >> 17) & 1u;
    uint32_t draw_buf = (st->fbz >> 14) & 3u;
    if (draw_buf > 1u)
        draw_buf = 0;
    if (x0 < st->clip_x0)
        x0 = st->clip_x0;
    if (x1 > st->clip_x1)
        x1 = st->clip_x1;
    if (y0 < st->clip_y0)
        y0 = st->clip_y0;
    if (y1 > st->clip_y1)
        y1 = st->clip_y1;
    if (x0 >= x1 || y0 >= y1)
        return;
    int32_t ry0 = flip ? (int32_t)st->screen_h - y1 : y0;
    int32_t ry1 = flip ? (int32_t)st->screen_h - y0 : y1;
    if (ry0 < 0)
        ry0 = 0;
    v2gpu_target_t *ct = v2gpu_target_for_buffer(g, st, draw_buf);
    v2gpu_target_t *dt = v2gpu_target_for_buffer(g, st, 3);
    if (!ct || !dt)
        return;
    v2gpu_readback_rows(g, ct, (uint32_t)ry0, (uint32_t)ry1);
    v2gpu_readback_rows(g, dt, (uint32_t)ry0, (uint32_t)ry1);
    v2_raster_execute(st, tgt, cmd);
    if (st->fbz & 0x200u)
        v2gpu_upload_rect(g, ct, (uint32_t)x0, (uint32_t)x1, (uint32_t)ry0, (uint32_t)ry1);
    if (st->fbz & 0x400u)
        v2gpu_upload_rect(g, dt, (uint32_t)x0, (uint32_t)x1, (uint32_t)ry0, (uint32_t)ry1);
}

// ============================================================
// Command translation
// ============================================================

static void v2gpu_triangle(v2_gpu_t *g, const v2_draw_state_t *st, v2_target_t *tgt, const v2_cmd_t *cmd) {
    const voodoo2_tri_t *T = &cmd->u.tri;
    uint32_t fbz = st->fbz, amode = st->amode;
    int32_t minx = T->ax, maxx = T->ax, miny = T->ay, maxy = T->ay;
    if (T->bx < minx)
        minx = T->bx;
    if (T->cx < minx)
        minx = T->cx;
    if (T->bx > maxx)
        maxx = T->bx;
    if (T->cx > maxx)
        maxx = T->cx;
    if (T->by < miny)
        miny = T->by;
    if (T->cy < miny)
        miny = T->cy;
    if (T->by > maxy)
        maxy = T->by;
    if (T->cy > maxy)
        maxy = T->cy;
    int32_t bx0 = minx >> 4, bx1 = (maxx + 15) >> 4, by0 = miny >> 4, by1 = (maxy + 15) >> 4;
    // The fallbacks (§5.5): a rotate-mode stipple MASK (per-pixel
    // register order), the "colour before fog" destination factor, and
    // the zaColor depth compare — none expressible on the GPU.
    int reason = -1;
    if ((fbz & 4u) && !(fbz & 0x1000u))
        reason = V2GPU_FB_STIPPLE;
    else if ((amode & 0x10u) && ((amode >> 12) & 0xFu) == 0xFu)
        reason = V2GPU_FB_DSTFOG;
    else if ((fbz & 0x10u) && (fbz & 0x100000u))
        reason = V2GPU_FB_ZACMP;
    if (reason >= 0) {
        v2gpu_fallback(g, st, tgt, cmd, reason, bx0, bx1, by0, by1);
        return;
    }
    // Orientation: a command sign that disagrees with the geometry draws
    // nothing on the walker; the GPU would draw it, so drop it here.
    int64_t area = (int64_t)(T->bx - T->ax) * (T->cy - T->ay) - (int64_t)(T->by - T->ay) * (T->cx - T->ax);
    if (area == 0 || (area < 0) != T->area_sign)
        return;
    bool flip = (fbz >> 17) & 1u;
    uint32_t draw_buf = (fbz >> 14) & 3u;
    if (draw_buf > 1u)
        draw_buf = 0;
    v2gpu_target_t *ct = v2gpu_target_for_buffer(g, st, draw_buf);
    v2gpu_target_t *dt = v2gpu_target_for_buffer(g, st, 3);
    if (!ct || !dt)
        return;
    // Textures, when the pipeline reads the chain at all.
    uint32_t tex_id[2] = {0, 0};
    uint32_t base_level[2] = {0, 0};
    bool bound[2] = {false, false};
    uint32_t flags = 0;
    if (flip)
        flags |= V2GPU_F_Y_FLIP;
    if (st->tex_on)
        flags |= V2GPU_F_TEX_ON;
    if (st->uses_tex)
        flags |= V2GPU_F_USES_TEX;
    if (st->skip_tmu1)
        flags |= V2GPU_F_SKIP_TMU1;
    if (st->tex_on && st->uses_tex) {
        for (int t = 0; t < V2_RASTER_TMUS; t++) {
            if (t == 1 && st->skip_tmu1)
                continue;
            if (st->tmu[t].send_config)
                continue; // the chain echoes a configuration word, no texels
            tex_id[t] = v2gpu_tex_resolve(g, st, t, &base_level[t]);
            if (!tex_id[t] && g->lost)
                return;
            bound[t] = tex_id[t] != 0;
        }
    }
    v2gpu_draw_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.color_id = ct->id;
    hdr.depth_id = dt->id;
    hdr.tex_id[0] = tex_id[0];
    hdr.tex_id[1] = tex_id[1];
    hdr.pipe_key = v2gpu_pipe_key(st, false, false, false);
    v2gpu_scissor(st, ct, st->clip_x0, st->clip_x1, st->clip_y0, st->clip_y1, flip, &hdr);
    if (hdr.sx0 >= hdr.sx1 || hdr.sy0 >= hdr.sy1)
        return;
    uint32_t uniform[V2GPU_U_WORDS];
    v2gpu_build_uniform(uniform, st, flags, 0, (float)ct->w, (float)ct->h, base_level, bound);
    uniform[V2GPU_U_STIPPLE] = tgt->stipple;
    if (!v2gpu_draw_matches(g, &hdr, uniform, 3)) {
        if (!v2gpu_open_draw(g, &hdr, uniform))
            return;
    }
    v2gpu_vertex(v2gpu_vertex_ptr(g), T, T->ax, T->ay);
    v2gpu_vertex(v2gpu_vertex_ptr(g), T, T->bx, T->by);
    v2gpu_vertex(v2gpu_vertex_ptr(g), T, T->cx, T->cy);
    g->n_tris++;
    // The rows the triangle can touch belong to the GPU now.
    int32_t y0 = by0 < st->clip_y0 ? st->clip_y0 : by0, y1 = by1 > st->clip_y1 ? st->clip_y1 : by1;
    if (y0 < y1) {
        int32_t ry0 = flip ? (int32_t)st->screen_h - y1 : y0, ry1 = flip ? (int32_t)st->screen_h - y0 : y1;
        if (ry0 < 0)
            ry0 = 0;
        if (fbz & 0x200u)
            v2gpu_rows_set(ct, (uint32_t)ry0, (uint32_t)ry1, false);
        if (fbz & 0x400u)
            v2gpu_rows_set(dt, (uint32_t)ry0, (uint32_t)ry1, false);
    }
    // The statistics counters are APPROXIMATE in GPU mode (§5.4): the
    // analytic covered area, clipped by the bounding box's visible
    // fraction; the failure counters do not move.
    double pixels = (double)(area < 0 ? -area : area) / 512.0;
    double bw = (double)(bx1 - bx0), bh = (double)(by1 - by0);
    int32_t cx0 = bx0 < st->clip_x0 ? st->clip_x0 : bx0, cx1 = bx1 > st->clip_x1 ? st->clip_x1 : bx1;
    if (bw > 0 && bh > 0 && cx1 > cx0 && y1 > y0)
        pixels *= ((double)(cx1 - cx0) * (double)(y1 - y0)) / (bw * bh);
    else
        pixels = 0;
    uint32_t n = (uint32_t)(pixels + 0.5);
    tgt->pixels_in = (tgt->pixels_in + n) & 0xFFFFFFu;
    tgt->pixels_out = (tgt->pixels_out + n) & 0xFFFFFFu;
}

static void v2gpu_fastfill(v2_gpu_t *g, const v2_draw_state_t *st) {
    uint32_t fbz = st->fbz;
    uint32_t draw_buf = (fbz >> 14) & 3u;
    if (draw_buf > 1u)
        draw_buf = 0;
    bool wc = fbz & 0x200u, wd = fbz & 0x400u;
    if (!wc && !wd)
        return;
    v2gpu_target_t *ct = wc ? v2gpu_target_for_buffer(g, st, draw_buf) : NULL;
    v2gpu_target_t *dt = wd ? v2gpu_target_for_buffer(g, st, 3) : NULL;
    v2gpu_fill(g, st, ct, dt, st->fill_x0, st->fill_x1, st->fill_y0, st->fill_y1, (fbz >> 17) & 1u,
               st->color1 & 0xFFFFFFu, false, st->zacolor & 0xFFFFu, wc, wd);
}

// One contiguous framebuffer byte range filled with a 16-bit value:
// rows of whichever target(s) it crosses, as at most three rectangles
// per target (a partial first row, whole rows, a partial last row).
static void v2gpu_fill_range(v2_gpu_t *g, const v2_draw_state_t *st, uint32_t a0, uint32_t a1, uint16_t color) {
    for (int i = 0; i < V2GPU_MAX_TARGETS; i++) {
        v2gpu_target_t *t = &g->targets[i];
        if (!t->id)
            continue;
        uint32_t tb = t->base, te = t->base + t->h * g->geom_stride;
        uint32_t lo = a0 > tb ? a0 : tb, hi = a1 < te ? a1 : te;
        if (lo >= hi)
            continue;
        uint32_t off0 = lo - tb, off1 = hi - tb;
        uint32_t row0 = off0 / g->geom_stride, x0 = (off0 % g->geom_stride) / 2u;
        uint32_t row1 = (off1 - 1u) / g->geom_stride, x1 = ((off1 - 1u) % g->geom_stride) / 2u + 1u;
        uint32_t fill = t->is_depth ? 0u : color;
        uint32_t depth = color;
        v2gpu_target_t *ct = t->is_depth ? NULL : t;
        v2gpu_target_t *dt = t->is_depth ? t : NULL;
        if (row0 == row1) {
            v2gpu_fill(g, st, ct, dt, (int32_t)x0, (int32_t)x1, (int32_t)row0, (int32_t)row0 + 1, false, fill, true,
                       depth, !t->is_depth, t->is_depth);
            continue;
        }
        v2gpu_fill(g, st, ct, dt, (int32_t)x0, (int32_t)t->w, (int32_t)row0, (int32_t)row0 + 1, false, fill, true,
                   depth, !t->is_depth, t->is_depth);
        if (row1 > row0 + 1u)
            v2gpu_fill(g, st, ct, dt, 0, (int32_t)t->w, (int32_t)row0 + 1, (int32_t)row1, false, fill, true, depth,
                       !t->is_depth, t->is_depth);
        v2gpu_fill(g, st, ct, dt, 0, (int32_t)x1, (int32_t)row1, (int32_t)row1 + 1, false, fill, true, depth,
                   !t->is_depth, t->is_depth);
    }
}

// The SGRAM page-space fill: coalesce its page rows into contiguous
// byte ranges, then fill the targets they cross.
static void v2gpu_blt_fill(v2_gpu_t *g, const v2_draw_state_t *st, v2_target_t *tgt, const v2_cmd_t *cmd) {
    (void)tgt;
    uint32_t base = cmd->u.blt.base, x0 = cmd->u.blt.x0, y0 = cmd->u.blt.y0;
    uint32_t span = cmd->u.blt.units * 8u;
    uint32_t run0 = 0, run1 = 0;
    bool open = false;
    for (uint32_t r = 0; r < cmd->u.blt.rows; r++) {
        uint32_t a = (base + (y0 + r) * 4096u + x0 * 8u) & V2_FB_MASK;
        uint32_t b = a + span;
        if (open && a == run1) {
            run1 = b;
            continue;
        }
        if (open)
            v2gpu_fill_range(g, st, run0, run1, cmd->u.blt.color);
        run0 = a;
        run1 = b;
        open = true;
    }
    if (open)
        v2gpu_fill_range(g, st, run0, run1, cmd->u.blt.color);
}

// Append one bypass pixel (already stored in the shadow) to the open
// upload run, or start a new run.
static void v2gpu_run_pixel(v2_gpu_t *g, v2gpu_target_t *t, uint32_t x, uint32_t y, uint16_t px) {
    if (x >= t->w || y >= t->h)
        return;
    bool extend = g->run_open && g->run_tid == t->id && g->run_y == y && g->run_x + g->run_n == x &&
                  g->run_depth == t->is_depth && g->run_n < t->w;
    if (!extend) {
        v2gpu_close_all(g);
        uint32_t bpp = t->is_depth ? 2u : 4u;
        uint32_t at = v2gpu_reserve(g, V2GPU_R_UPLOAD, 8u + 20u + t->w * bpp);
        if (at == UINT32_MAX)
            return;
        g->run_open = true;
        g->run_off = at;
        g->run_wr0 = g->last_wr0;
        g->run_tid = t->id;
        g->run_x = x;
        g->run_y = y;
        g->run_n = 0;
        g->run_depth = t->is_depth;
    }
    uint8_t *dst = g->ring + g->run_off + 28u + g->run_n * (t->is_depth ? 2u : 4u);
    if (t->is_depth) {
        dst[0] = (uint8_t)px;
        dst[1] = (uint8_t)(px >> 8);
    } else {
        v2gpu_expand565(px, dst);
    }
    g->run_n++;
}

// A bypass store the walker made into the shadow at (buffer, x, y):
// mirror it onto the GPU.
static void v2gpu_mirror_store(v2_gpu_t *g, const v2_draw_state_t *st, uint32_t buffer, uint32_t x, uint32_t y) {
    v2gpu_target_t *t = v2gpu_target_for_buffer(g, st, buffer);
    if (!t)
        return;
    uint32_t at = v2_fb_addr(st, buffer, x, y);
    uint16_t px = (uint16_t)(g->tgt->fb[at] | ((uint16_t)g->tgt->fb[(at + 1u) & V2_FB_MASK] << 8));
    v2gpu_run_pixel(g, t, x, y, px);
}

static void v2gpu_lfb_pixel(v2_gpu_t *g, const v2_draw_state_t *st, v2_target_t *tgt, const v2_cmd_t *cmd) {
    if (!((st->lfbmode >> 8) & 1u)) {
        // The bypass: the walker stores the exact 5-6-5 (dithered) into
        // the shadow; the same pixel goes up as an upload run.
        v2_raster_execute(st, tgt, cmd);
        if (cmd->u.lfb.write_color)
            v2gpu_mirror_store(g, st, cmd->u.lfb.buffer, cmd->u.lfb.x, cmd->u.lfb.y);
        if (cmd->u.lfb.write_z)
            v2gpu_mirror_store(g, st, 3u, cmd->u.lfb.x, cmd->u.lfb.y);
        return;
    }
    // Pipeline-processed: one pixel through the shader, as a 1x1 quad
    // whose vertices carry the pixel's iterator values (V2 p.51-52).
    uint32_t fbz = st->fbz;
    uint32_t draw_buf = (fbz >> 14) & 3u;
    if (draw_buf > 1u)
        draw_buf = 0;
    bool flip = (fbz >> 17) & 1u;
    v2gpu_target_t *ct = v2gpu_target_for_buffer(g, st, draw_buf);
    v2gpu_target_t *dt = v2gpu_target_for_buffer(g, st, 3);
    if (!ct || !dt)
        return;
    v2gpu_draw_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.color_id = ct->id;
    hdr.depth_id = dt->id;
    hdr.pipe_key = v2gpu_pipe_key(st, false, false, false);
    v2gpu_scissor(st, ct, st->clip_x0, st->clip_x1, st->clip_y0, st->clip_y1, flip, &hdr);
    if (hdr.sx0 >= hdr.sx1 || hdr.sy0 >= hdr.sy1)
        return;
    uint32_t flags = V2GPU_F_LFB_PIXEL | (flip ? V2GPU_F_Y_FLIP : 0u);
    uint32_t uniform[V2GPU_U_WORDS];
    uint32_t base_level[2] = {0, 0};
    bool bound[2] = {false, false};
    v2gpu_build_uniform(uniform, st, flags, 0, (float)ct->w, (float)ct->h, base_level, bound);
    uniform[V2GPU_U_STIPPLE] = tgt->stipple;
    uint16_t z16 = cmd->u.lfb.has_z ? cmd->u.lfb.z : (uint16_t)(st->zacolor & 0xFFFFu);
    uint16_t wsrc = (st->lfbmode & 0x4000u) ? (uint16_t)(st->zacolor & 0xFFFFu) : z16;
    float z = (float)z16, w = (float)((int64_t)wsrc << 14) / 1073741824.0f;
    float x = (float)cmd->u.lfb.x, y = (float)cmd->u.lfb.y;
    float r = (float)cmd->u.lfb.r, gg = (float)cmd->u.lfb.g, b = (float)cmd->u.lfb.b, a = (float)cmd->u.lfb.a;
    if (!v2gpu_draw_matches(g, &hdr, uniform, 6)) {
        if (!v2gpu_open_draw(g, &hdr, uniform))
            return;
    }
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), x - 0.5f, y - 0.5f, z, w, r, gg, b, a);
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), x + 0.5f, y - 0.5f, z, w, r, gg, b, a);
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), x - 0.5f, y + 0.5f, z, w, r, gg, b, a);
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), x + 0.5f, y - 0.5f, z, w, r, gg, b, a);
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), x + 0.5f, y + 0.5f, z, w, r, gg, b, a);
    v2gpu_vertex_flat(v2gpu_vertex_ptr(g), x - 0.5f, y + 0.5f, z, w, r, gg, b, a);
    int32_t py = flip ? (int32_t)st->screen_h - 1 - (int32_t)cmd->u.lfb.y : (int32_t)cmd->u.lfb.y;
    if (py >= 0) {
        if (fbz & 0x200u)
            v2gpu_rows_set(ct, (uint32_t)py, (uint32_t)py + 1u, false);
        if (fbz & 0x400u)
            v2gpu_rows_set(dt, (uint32_t)py, (uint32_t)py + 1u, false);
    }
    g->n_lfb_pipe++;
    tgt->pixels_in = (tgt->pixels_in + 1u) & 0xFFFFFFu;
    tgt->pixels_out = (tgt->pixels_out + 1u) & 0xFFFFFFu;
}

// A vblank while driving: present the displayed buffer, tick the
// frame clock, and re-engage after a quiet spell if a storm dropped
// the GPU.
static void v2gpu_present(v2_gpu_t *g, const v2_draw_state_t *st, uint32_t fb_base) {
    g->frame++;
    g->bands_this_frame = 0;
    if (!atomic_load(&g->engaged)) {
        if (g->want && !g->lost && ++g->quiet_frames >= V2GPU_QUIET_FRAMES) {
            g->quiet_frames = 0;
            v2gpu_engage(g, st);
        }
        return;
    }
    v2gpu_close_all(g);
    v2gpu_target_t *t = v2gpu_target_for_addr(g, fb_base);
    if (!t) {
        // Never drawn on the GPU: bring it up from the shadow (front is
        // buffer 0 after the producer's swap bookkeeping).
        for (uint32_t b = 0; b < 2u && !t; b++) {
            if (st->buf_base[b] == fb_base)
                t = v2gpu_target_for_buffer(g, st, b);
        }
        if (!t)
            return;
    }
    v2gpu_emit(g, V2GPU_R_PRESENT, &t->id, 1);
    v2gpu_publish(g);
    g->n_presents++;
}

static void v2gpu_gamma_chunk(v2_gpu_t *g, const v2_cmd_t *cmd) {
    uint32_t off = cmd->u.tex.off * 192u;
    if (off + 192u > sizeof(g->gamma))
        return;
    memcpy(g->gamma + off, cmd->u.tex.words, 192u);
    if (cmd->u.tex.off == 3u && atomic_load(&g->engaged)) {
        v2gpu_close_all(g);
        uint32_t at = v2gpu_reserve(g, V2GPU_R_GAMMA, 8u + sizeof(g->gamma));
        if (at != UINT32_MAX)
            memcpy(g->ring + at + 8u, g->gamma, sizeof(g->gamma));
    }
}

// A fence wants the shadow bytes [addr, addr+len): read back the rows
// of whichever targets cover them, and watch for a storm.
static void v2gpu_readback_range(v2_gpu_t *g, uint32_t addr, uint32_t len) {
    if (!atomic_load(&g->engaged))
        return;
    v2gpu_close_all(g);
    uint32_t end = addr + len;
    for (int i = 0; i < V2GPU_MAX_TARGETS; i++) {
        v2gpu_target_t *t = &g->targets[i];
        if (!t->id)
            continue;
        uint32_t tb = t->base, te = t->base + t->h * g->geom_stride;
        uint32_t lo = addr > tb ? addr : tb, hi = end < te ? end : te;
        if (lo >= hi)
            continue;
        uint32_t y0 = (lo - tb) / g->geom_stride, y1 = (hi - 1u - tb) / g->geom_stride + 1u;
        v2gpu_readback_rows(g, t, y0, y1);
    }
    if (g->bands_this_frame > V2GPU_STORM_BANDS) {
        // A client reading the framebuffer wholesale every frame: the
        // walker serves it better.  Come back after a quiet spell.
        g->n_storm++;
        g->quiet_frames = 0;
        LOG(1, "webgpu: readback storm (%u bands this frame) — disengaging", g->bands_this_frame);
        v2gpu_disengage(g, false);
    }
}

// ============================================================
// The public face
// ============================================================

void v2_gpu_execute(v2_gpu_t *g, const v2_draw_state_t *st, v2_target_t *tgt, const v2_cmd_t *cmd) {
    bool on = atomic_load(&g->engaged) != 0;
    switch ((v2_cmd_kind_t)cmd->kind) {
    case V2_CMD_TRIANGLE:
        if (on && v2gpu_check_geometry(g, st))
            v2gpu_triangle(g, st, tgt, cmd);
        else
            v2_raster_execute(st, tgt, cmd);
        return;
    case V2_CMD_FASTFILL:
        if (on && v2gpu_check_geometry(g, st))
            v2gpu_fastfill(g, st);
        else
            v2_raster_execute(st, tgt, cmd);
        return;
    case V2_CMD_LFB_PIXEL:
        if (on && v2gpu_check_geometry(g, st))
            v2gpu_lfb_pixel(g, st, tgt, cmd);
        else
            v2_raster_execute(st, tgt, cmd);
        return;
    case V2_CMD_FB_STORE16:
        v2_raster_execute(st, tgt, cmd);
        if (on && v2gpu_check_geometry(g, st))
            v2gpu_mirror_store(g, st, cmd->u.store.buffer, cmd->u.store.x, cmd->u.store.y);
        return;
    case V2_CMD_BLT_FILL:
        if (on && v2gpu_check_geometry(g, st))
            v2gpu_blt_fill(g, st, tgt, cmd);
        else
            v2_raster_execute(st, tgt, cmd);
        return;
    case V2_CMD_TEX_WRITE:
        v2_raster_execute(st, tgt, cmd);
        if (on)
            v2gpu_mark_tex_write(g, st, cmd);
        return;
    case V2_CMD_PALETTE:
    case V2_CMD_NCC:
    case V2_CMD_STAT_CLEAR:
    case V2_CMD_STIPPLE:
        v2_raster_execute(st, tgt, cmd);
        return;
    case V2_CMD_GPU_ENGAGE:
        g->want = cmd->u.gpu.flags & 1u;
        if (g->want && !on && !g->lost)
            v2gpu_engage(g, st);
        else if (!g->want && on)
            v2gpu_disengage(g, !(cmd->u.gpu.flags & 2u));
        return;
    case V2_CMD_GPU_PRESENT:
        v2gpu_present(g, st, cmd->u.gpu.addr);
        return;
    case V2_CMD_GPU_GAMMA:
        v2gpu_gamma_chunk(g, cmd);
        return;
    case V2_CMD_GPU_READBACK:
        v2gpu_readback_range(g, cmd->u.gpu.addr, cmd->u.gpu.len);
        return;
    }
}

void v2_gpu_idle(v2_gpu_t *g) {
    if (!atomic_load(&g->engaged))
        return;
    v2gpu_close_all(g);
    v2gpu_publish(g);
}

bool v2_gpu_engaged(const v2_gpu_t *g) {
    return g && atomic_load(&g->engaged) != 0;
}

const char *v2_gpu_stats(v2_gpu_t *g, char *buf, size_t n) {
    if (!g) {
        if (n)
            buf[0] = 0;
        return buf;
    }
    snprintf(buf, n,
             "engaged=%d lost=%d engages=%llu disengages=%llu storms=%llu resyncs=%llu tris=%llu draws=%llu "
             "fills=%llu lfb_runs=%llu lfb_pipe=%llu presents=%llu readback_bands=%llu readback_rows=%llu "
             "tex_create=%llu tex_upload=%llu tex_upload_bytes=%llu tex_evict=%llu tex_bytes=%llu "
             "fallback_stipple=%llu fallback_dstfog=%llu fallback_zacmp=%llu gpu_frames=%u gpu_draws=%u "
             "gpu_flushes=%u gpu_pipelines=%u gpu_readbacks=%u",
             atomic_load(&g->engaged), g->lost ? 1 : 0, (unsigned long long)g->n_engage,
             (unsigned long long)g->n_disengage, (unsigned long long)g->n_storm, (unsigned long long)g->n_resync,
             (unsigned long long)g->n_tris, (unsigned long long)g->n_draws, (unsigned long long)g->n_fills,
             (unsigned long long)g->n_lfb_runs, (unsigned long long)g->n_lfb_pipe, (unsigned long long)g->n_presents,
             (unsigned long long)g->n_readback_bands, (unsigned long long)g->n_readback_rows,
             (unsigned long long)g->n_tex_create, (unsigned long long)g->n_tex_upload,
             (unsigned long long)g->n_tex_upload_bytes, (unsigned long long)g->n_tex_evict,
             (unsigned long long)g->tex_bytes, (unsigned long long)g->n_fallback[V2GPU_FB_STIPPLE],
             (unsigned long long)g->n_fallback[V2GPU_FB_DSTFOG], (unsigned long long)g->n_fallback[V2GPU_FB_ZACMP],
             g->ctrl ? v2gpu_load(g, V2GPU_C_STAT_FRAMES) : 0u, g->ctrl ? v2gpu_load(g, V2GPU_C_STAT_DRAWS) : 0u,
             g->ctrl ? v2gpu_load(g, V2GPU_C_STAT_FLUSHES) : 0u, g->ctrl ? v2gpu_load(g, V2GPU_C_STAT_PIPELINES) : 0u,
             g->ctrl ? v2gpu_load(g, V2GPU_C_STAT_READBACKS) : 0u);
    return buf;
}

// ============================================================
// Lifetime
// ============================================================

v2_gpu_t *v2_gpu_create(v2_target_t *tgt) {
    if (!gs_v2gpu_available()) {
        LOG(1, "raster=webgpu: no WebGPU transport on this host — using the thread backend");
        return NULL;
    }
    v2_gpu_t *g = (v2_gpu_t *)calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->tgt = tgt;
    g->tex = (v2gpu_tex_t *)calloc(V2GPU_MAX_TEX, sizeof(v2gpu_tex_t));
    for (int t = 0; t < V2_RASTER_TMUS; t++)
        g->page_gen[t] = (uint32_t *)calloc(V2GPU_MAX_PAGES, sizeof(uint32_t));
    uint32_t ctrl_bytes = V2GPU_CTRL_WORDS * 4u;
    g->ring_size = V2GPU_RING_BYTES;
    g->rb_size = V2GPU_RB_BYTES;
    g->region_bytes = 64u + ctrl_bytes + g->ring_size + g->rb_size;
    g->region = (uint8_t *)calloc(1, g->region_bytes);
    if (!g->tex || !g->page_gen[0] || !g->page_gen[1] || !g->region) {
        v2_gpu_destroy(g);
        return NULL;
    }
    // 64-byte align the control block inside the allocation.
    uintptr_t base = ((uintptr_t)g->region + 63u) & ~(uintptr_t)63u;
    g->ctrl = (volatile uint32_t *)base;
    g->ring = (uint8_t *)(base + ctrl_bytes);
    g->rb = g->ring + g->ring_size;
    for (int i = 0; i < V2GPU_CTRL_WORDS; i++)
        g->ctrl[i] = 0;
    g->ctrl[V2GPU_C_MAGIC] = V2GPU_MAGIC;
    g->ctrl[V2GPU_C_VERSION] = V2GPU_PROTOCOL_VERSION;
    g->ctrl[V2GPU_C_RING_OFF] = ctrl_bytes;
    g->ctrl[V2GPU_C_RING_SIZE] = g->ring_size;
    g->ctrl[V2GPU_C_RB_OFF] = ctrl_bytes + g->ring_size;
    g->ctrl[V2GPU_C_RB_SIZE] = g->rb_size;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!gs_v2gpu_attach((void *)g->ctrl, ctrl_bytes + g->ring_size + g->rb_size)) {
        LOG(0, "raster=webgpu: the host refused to attach a GPU worker — using the thread backend");
        v2_gpu_destroy(g);
        return NULL;
    }
    uint32_t waited = 0;
    while (v2gpu_load(g, V2GPU_C_STATUS) != V2GPU_STATUS_ATTACHED) {
        if (waited >= V2GPU_ATTACH_MS) {
            LOG(0, "raster=webgpu: the GPU worker did not attach within %u ms — using the thread backend",
                V2GPU_ATTACH_MS);
            gs_v2gpu_detach((void *)g->ctrl);
            v2_gpu_destroy(g);
            return NULL;
        }
        gs_v2gpu_wait(&g->ctrl[V2GPU_C_STATUS], V2GPU_STATUS_DETACHED, 20);
        waited += 20;
    }
    g->attached = true;
    LOG(1, "raster=webgpu: GPU worker attached (op ring %u KB)", g->ring_size >> 10);
    return g;
}

void v2_gpu_destroy(v2_gpu_t *g) {
    if (!g)
        return;
    if (g->attached && !g->lost) {
        v2gpu_close_all(g);
        uint32_t seq = ++g->seq;
        v2gpu_store(g, V2GPU_C_REQ, seq);
        v2gpu_emit(g, V2GPU_R_SHUTDOWN, &seq, 1);
        v2gpu_wait_ack(g, seq);
    }
    if (g->attached)
        gs_v2gpu_detach((void *)g->ctrl);
    free(g->region);
    free(g->tex);
    for (int t = 0; t < V2_RASTER_TMUS; t++)
        free(g->page_gen[t]);
    free(g);
}
