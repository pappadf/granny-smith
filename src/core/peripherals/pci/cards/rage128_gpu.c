// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// rage128_gpu.c
// The Rage 128's WebGPU takeover: the translator
// (docs/internals/core/peripherals/pci/cards/rage128.md, "The WebGPU
// takeover").  While ENGAGED, the 3D engine's triangles — set up, snapped
// and culled by the walker's own code in rage128_raster.c — become
// vertex-buffer entries under a pipeline key and a uniform block of the
// snapshot's registers; the colour and Z buffers they render into live
// on the GPU as SURFACES; textures become r32uint atlases of raw texels
// converted from VRAM, cached by VRAM page generation; the 2D engine's
// solid fills of a surface become GPU fills; and the vertical blank
// presents the scanned-out surface through the display table onto an
// overlay canvas.
//
// What stays in VRAM: everything.  A surface row is current in VRAM, or
// newer on the GPU (drawn there), or newer in VRAM (written by the CPU,
// the 2D engine, the CCE or the walker, not uploaded yet).  Every VRAM
// access that is not the GPU's passes r128_vram_access (rage128_priv.h):
// a GPU-newer row is read back before it is seen, and a write marks its
// rows VRAM-newer, uploaded before the next GPU use.  The walker draws
// whatever the GPU cannot express (points, lines, ROP3s, stipples,
// stencil, 24/32-bit Z, the wrapping blend functions) against VRAM, its
// accesses fenced like anyone's.
//
// Engagement.  Under the default policy a 3D draw into a SCREEN-SHAPED
// surface (the scanout's stride and pixel format) engages GPU mode; it
// ends after a second or two without 3D, on a readback storm (more than
// R128GPU_STORM_BANDS bands in each of R128GPU_STORM_FRAMES vblanks, with
// a cool-down before the next engagement), on a reset or checkpoint
// restore (discarding the GPU's pixels: VRAM is the restored truth), or
// when the device is lost (the walker draws from there on).

#include "rage128_gpu.h"

#include "display.h"
#include "log.h"
#include "rage128_gpu_protocol.h"
#include "system.h"
#include "mailbox/mailbox_ring.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_USE_CATEGORY_NAME("video");

#define R128GPU_MAX_SURF      8
#define R128GPU_MAX_ROWS      2048
#define R128GPU_MAX_TEX       1024
#define R128GPU_TEX_BYTES_CAP (96u << 20) // atlas bytes held on the GPU
#define R128GPU_PAGE_SHIFT    12 // VRAM write tracking granularity (4 KB)
#define R128GPU_IDLE_VBLS     120 // vblanks without 3D before disengaging
#define R128GPU_STORM_BANDS   32 // readback bands in one vblank interval...
#define R128GPU_STORM_FRAMES  3 // ...in this many consecutive ones: a storm
#define R128GPU_COOLDOWN_VBLS 300 // no re-engagement this long after a storm
#define R128GPU_ATTACH_MS     10000 // the worker starts lazily: a device is created first
// How long a wait for the worker may last before the device counts as
// lost.  Generous: a software adapter compiles a pipeline on its first use
// and can take seconds; the waits keep the page's stall watch fed
// (gs_v2gpu_keepalive) meanwhile.
#ifndef R128GPU_ACK_MS
#define R128GPU_ACK_MS 20000
#endif

// The reasons a batch is left to the walker while engaged.
enum {
    FB_PRIM, // points, lines, or a face drawn as points/lines
    FB_ROP, // a ROP3 other than SRCCOPY
    FB_STIPPLE, // a patterned brush gates pixels
    FB_STENCIL,
    FB_ZDEPTH, // a Z buffer other than 16-bit
    FB_BLEND, // a wrapping blend function, or a factor WebGPU lacks
    FB_WMASK, // a write mask that is not whole channels
    FB_FORMAT, // a colour buffer the GPU path does not hold (8-bit)
    FB_SIZE, // colour and Z surfaces of different widths, or too large
    FB_TEX, // a texture could not be resolved
    FB_COUNT
};
static const char *const k_fb_names[FB_COUNT] = {"prim",  "rop",   "stipple", "stencil", "zdepth",
                                                 "blend", "wmask", "format",  "size",    "tex"};

typedef struct surf {
    uint32_t id; // 0 = unused
    uint32_t base, stride, w, h, bpp, fmt; // fmt: the colour datatype, or 0 for depth
    bool depth;
    uint32_t n_gpu, n_cpu; // rows in each "newer" state
    uint8_t gpu_newer[R128GPU_MAX_ROWS / 8];
    uint8_t cpu_newer[R128GPU_MAX_ROWS / 8];
} surf_t;

typedef struct gtex {
    uint32_t id; // 0 = free
    uint32_t fmt, lpitch, lheight, top;
    uint32_t off[11];
    bool agp; // a level lives in AGP space: content-hashed, once a frame
    uint32_t gen_seen; // VRAM page generation the upload saw
    uint64_t hash;
    uint64_t checked_frame;
    uint32_t bytes;
    uint64_t last_use;
} gtex_t;

struct r128_gpu {
    rage128_t *r;
    int policy;
    // The shared region.
    uint8_t *region;
    volatile uint32_t *ctrl;
    mbx_ring_t ring;
    uint8_t *rb;
    uint32_t rb_size;
    uint32_t seq;
    bool attached, lost, warned_lost;
    // Mode.
    bool engaged;
    bool presenting; // the last vblank presented from the GPU
    uint32_t next_id;
    surf_t surf[R128GPU_MAX_SURF];
    uint32_t *page_gen; // per VRAM page; r->gpu_page_gen while engaged
    // The batch in flight.
    bool batch_empty;
    surf_t *bc, *bd;
    int32_t sc_x0, sc_y0, sc_x1, sc_y1; // surface pixels, half-open
    int32_t by0, by1; // rows the batch's triangles reach
    bool write_color, write_depth;
    r128gpu_draw_hdr_t hdr;
    uint32_t uni[R128GPU_U_WORDS];
    // The open DRAW record.
    bool draw_open;
    uint32_t draw_off;
    r128gpu_draw_hdr_t draw_hdr;
    uint32_t draw_uni[R128GPU_U_WORDS];
    // Textures.
    gtex_t *tex;
    uint64_t tex_bytes;
    uint64_t frame;
    // The display table last sent.
    uint8_t lut[3][256];
    bool lut_sent;
    // Rules.
    uint32_t idle_vbls, bands_frame, storm_frames, cooldown;
    // Statistics.
    uint64_t n_engage, n_disengage, n_storm, n_lost, n_batches, n_tris, n_draws, n_fills, n_presents;
    uint64_t n_readback_bands, n_readback_rows, n_upload_rows;
    uint64_t n_tex_create, n_tex_upload_bytes, n_tex_evict;
    uint64_t n_fallback[FB_COUNT];
};

// ============================================================
// The ring writer (the Voodoo2's, voodoo2_gpu.c)
// ============================================================

static inline uint32_t gload(r128_gpu_t *g, int word) {
    return mbx_load(g->ctrl, word);
}

static inline void gstore(r128_gpu_t *g, int word, uint32_t v) {
    mbx_store(g->ctrl, word, v);
}

// Real elapsed milliseconds: the waits bound time, not wakeups.
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void recompute_bounds(r128_gpu_t *g);

// The worker reported the device gone (or stopped answering): the walker
// draws from here on.  VRAM keeps whatever was read back last.
static void mark_lost(r128_gpu_t *g, const char *why) {
    if (!g->warned_lost) {
        g->warned_lost = true;
        LOG(0, "Rage 128 webgpu: %s — the software rasteriser draws from here on (VRAM may be stale)", why);
    }
    g->lost = true;
    g->n_lost++;
    g->engaged = false;
    g->presenting = false;
    for (int i = 0; i < R128GPU_MAX_SURF; i++)
        g->surf[i].id = 0;
    for (int i = 0; i < R128GPU_MAX_TEX; i++)
        g->tex[i].id = 0;
    g->tex_bytes = 0;
    g->draw_open = false;
    g->r->gpu_page_gen = NULL;
    recompute_bounds(g);
}

static bool worker_lost(r128_gpu_t *g) {
    if (g->lost)
        return true;
    uint32_t st = gload(g, R128GPU_C_STATUS);
    if (st != R128GPU_STATUS_ATTACHED) {
        mark_lost(g, st == R128GPU_STATUS_LOST ? "the GPU device was lost" : "the GPU worker detached");
        return true;
    }
    return false;
}

static void publish(r128_gpu_t *g) {
    if (!mbx_unpublished(&g->ring))
        return;
    mbx_publish(&g->ring);
    gs_v2gpu_notify(&g->ctrl[R128GPU_C_HEAD]);
}

// Reserve a record (header included) that does not wrap, waiting for
// room; UINT32_MAX when the worker is gone.
static uint32_t reserve(r128_gpu_t *g, uint32_t kind, uint32_t len) {
    double t0 = now_ms();
    for (;;) {
        uint32_t tail = gload(g, R128GPU_C_TAIL);
        uint32_t at = mbx_reserve(&g->ring, kind, len);
        if (at != UINT32_MAX)
            return at;
        if (worker_lost(g))
            return UINT32_MAX;
        publish(g);
        gs_v2gpu_wait(&g->ctrl[R128GPU_C_TAIL], tail, 20);
        gs_v2gpu_keepalive();
        if (now_ms() - t0 > (double)R128GPU_ACK_MS) {
            mark_lost(g, "the GPU worker stopped consuming the op ring");
            return UINT32_MAX;
        }
    }
}

static inline uint8_t *payload(r128_gpu_t *g, uint32_t at) {
    return mbx_payload(&g->ring, at);
}

static void close_draw(r128_gpu_t *g);

static bool emit(r128_gpu_t *g, uint32_t kind, const uint32_t *words, uint32_t n_words) {
    close_draw(g);
    uint32_t at = reserve(g, kind, 8u + 4u * n_words);
    if (at == UINT32_MAX)
        return false;
    memcpy(payload(g, at), words, 4u * n_words);
    return true;
}

static bool wait_ack(r128_gpu_t *g, uint32_t seq) {
    publish(g);
    double t0 = now_ms();
    for (;;) {
        uint32_t ack = gload(g, R128GPU_C_ACK);
        if ((int32_t)(ack - seq) >= 0) {
            double dt = now_ms() - t0;
            if (dt > 250.0)
                LOG(1, "Rage 128 webgpu: the GPU worker took %.0f ms to acknowledge (seq %u)", dt, seq);
            return true;
        }
        if (worker_lost(g))
            return false;
        gs_v2gpu_wait(&g->ctrl[R128GPU_C_ACK], ack, 20);
        gs_v2gpu_keepalive();
        if (now_ms() - t0 > (double)R128GPU_ACK_MS) {
            mark_lost(g, "the GPU worker stopped answering");
            return false;
        }
    }
}

// Close the open DRAW record: patch its header and give back the unused
// reservation (it is always the last record reserved).
static void close_draw(r128_gpu_t *g) {
    if (!g->draw_open)
        return;
    g->draw_open = false;
    uint32_t len =
        8u + (uint32_t)sizeof(r128gpu_draw_hdr_t) + R128GPU_U_BYTES + g->draw_hdr.n_verts * R128GPU_VERTEX_BYTES;
    memcpy(payload(g, g->draw_off), &g->draw_hdr, sizeof(g->draw_hdr));
    mbx_shrink_last(&g->ring, len);
    g->n_draws++;
}

// ============================================================
// Surfaces
// ============================================================

static inline bool bit_get(const uint8_t *b, uint32_t y) {
    return (b[y >> 3] >> (y & 7)) & 1u;
}

static inline void bit_set(uint8_t *b, uint32_t y) {
    b[y >> 3] |= (uint8_t)(1u << (y & 7));
}

static inline void bit_clr(uint8_t *b, uint32_t y) {
    b[y >> 3] &= (uint8_t) ~(1u << (y & 7));
}

static void mark_gpu_newer(surf_t *s, uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1 && y < s->h; y++) {
        if (bit_get(s->cpu_newer, y))
            bit_clr(s->cpu_newer, y), s->n_cpu--;
        if (!bit_get(s->gpu_newer, y))
            bit_set(s->gpu_newer, y), s->n_gpu++;
    }
}

static void mark_cpu_newer(surf_t *s, uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1 && y < s->h; y++) {
        if (bit_get(s->gpu_newer, y))
            bit_clr(s->gpu_newer, y), s->n_gpu--;
        if (!bit_get(s->cpu_newer, y))
            bit_set(s->cpu_newer, y), s->n_cpu++;
    }
}

static inline uint32_t surf_end(const surf_t *s) {
    return s->base + s->h * s->stride;
}

// The union of the surfaces' byte ranges, for r128_vram_access's test.
static void recompute_bounds(r128_gpu_t *g) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (int i = 0; i < R128GPU_MAX_SURF; i++) {
        const surf_t *s = &g->surf[i];
        if (!s->id)
            continue;
        if (s->base < lo)
            lo = s->base;
        if (surf_end(s) > hi)
            hi = surf_end(s);
    }
    g->r->gpu_lo = lo == UINT32_MAX ? 0 : lo;
    g->r->gpu_hi = hi;
}

// Bump the page generation over VRAM bytes [lo, hi): cached textures
// there are stale.
static void bump_pages(r128_gpu_t *g, uint32_t lo, uint32_t hi) {
    rage128_t *r = g->r;
    if (!r->gpu_page_gen || hi <= lo)
        return;
    uint32_t last = (r->vram_size - 1u) >> R128GPU_PAGE_SHIFT;
    for (uint32_t p = lo >> R128GPU_PAGE_SHIFT; p <= ((hi - 1u) >> R128GPU_PAGE_SHIFT) && p <= last; p++)
        r->gpu_page_gen[p] = ++r->gpu_gen;
}

// Read rows [y0, y1) back into VRAM, band by band, copying only the rows
// newer on the GPU (a VRAM-newer row in the same band must not be
// overwritten).  Each band is one roundtrip.
static bool readback_rows(r128_gpu_t *g, surf_t *s, uint32_t y0, uint32_t y1) {
    if (!s->n_gpu)
        return true;
    if (y1 > s->h)
        y1 = s->h;
    uint32_t band = R128GPU_RB_ROWS;
    uint32_t row_bytes = s->w * s->bpp;
    if (row_bytes * band > g->rb_size)
        band = g->rb_size / row_bytes;
    for (uint32_t b = y0 - (y0 % band); b < y1; b += band) {
        uint32_t b1 = b + band < s->h ? b + band : s->h;
        bool any = false;
        for (uint32_t y = b; y < b1 && !any; y++)
            any = bit_get(s->gpu_newer, y);
        if (!any)
            continue;
        uint32_t words[4] = {++g->seq, s->id, b, b1};
        gstore(g, R128GPU_C_REQ, g->seq);
        if (!emit(g, R128GPU_R_READBACK, words, 4) || !wait_ack(g, g->seq))
            return false;
        rage128_t *r = g->r;
        const uint8_t *src = g->rb;
        for (uint32_t y = b; y < b1; y++, src += row_bytes) {
            if (!bit_get(s->gpu_newer, y))
                continue;
            uint32_t dst = s->base + y * s->stride;
            if (dst + row_bytes <= r->vram_size)
                memcpy(r->vram + dst, src, row_bytes);
            bit_clr(s->gpu_newer, y);
            s->n_gpu--;
            g->n_readback_rows++;
        }
        g->n_readback_bands++;
        g->bands_frame++;
    }
    return true;
}

// Upload every VRAM-newer row, in runs of whole rows.
static bool upload_rows(r128_gpu_t *g, surf_t *s) {
    if (!s->n_cpu)
        return true;
    rage128_t *r = g->r;
    uint32_t row_bytes = s->w * s->bpp;
    uint32_t max_rows = (256u << 10) / row_bytes;
    if (!max_rows)
        max_rows = 1;
    uint32_t y = 0;
    while (y < s->h && s->n_cpu) {
        if (!bit_get(s->cpu_newer, y)) {
            y++;
            continue;
        }
        uint32_t y1 = y;
        while (y1 < s->h && bit_get(s->cpu_newer, y1) && y1 - y < max_rows)
            y1++;
        uint32_t n = y1 - y, bytes = n * row_bytes;
        close_draw(g);
        uint32_t at = reserve(g, R128GPU_R_UPLOAD, 8u + 16u + ((bytes + 3u) & ~3u));
        if (at == UINT32_MAX)
            return false;
        uint32_t *p = (uint32_t *)payload(g, at);
        p[0] = s->id;
        p[1] = y;
        p[2] = s->w;
        p[3] = n;
        uint8_t *dst = payload(g, at) + 16u;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t a = s->base + (y + k) * s->stride;
            if (a + row_bytes <= r->vram_size)
                memcpy(dst + k * row_bytes, r->vram + a, row_bytes);
            else
                memset(dst + k * row_bytes, 0, row_bytes);
            bit_clr(s->cpu_newer, y + k);
            s->n_cpu--;
        }
        g->n_upload_rows += n;
        y = y1;
    }
    return true;
}

static void surf_free(r128_gpu_t *g, surf_t *s, bool readback) {
    if (!s->id)
        return;
    if (readback && !g->lost)
        readback_rows(g, s, 0, s->h);
    if (!g->lost) {
        uint32_t w = s->id;
        emit(g, R128GPU_R_SURFACE_FREE, &w, 1);
    }
    s->id = 0;
    recompute_bounds(g);
}

// Find, create or grow the surface at `base` with this geometry, `h`
// rows at least.  A surface overlapping it with another geometry is
// read back and dropped first.
static surf_t *surf_get(r128_gpu_t *g, uint32_t base, uint32_t stride, uint32_t bpp, uint32_t fmt, bool depth,
                        uint32_t h) {
    rage128_t *r = g->r;
    if (!stride || stride % bpp)
        return NULL;
    uint32_t w = stride / bpp;
    if (w > 4096u)
        return NULL;
    uint32_t vram_rows = base < r->vram_size ? (r->vram_size - base) / stride : 0;
    if (h > vram_rows)
        h = vram_rows;
    if (h > R128GPU_MAX_ROWS)
        h = R128GPU_MAX_ROWS;
    if (!h)
        return NULL;
    surf_t *hit = NULL, *free_slot = NULL;
    for (int i = 0; i < R128GPU_MAX_SURF; i++) {
        surf_t *s = &g->surf[i];
        if (!s->id) {
            if (!free_slot)
                free_slot = s;
            continue;
        }
        if (s->base == base && s->stride == stride && s->bpp == bpp && s->fmt == fmt && s->depth == depth) {
            hit = s;
            continue;
        }
        uint32_t end = base + h * stride;
        if (s->base < end && surf_end(s) > base) {
            surf_free(g, s, true); // another geometry over the same bytes
            if (!free_slot)
                free_slot = s;
        }
    }
    if (hit && hit->h >= h)
        return hit;
    if (hit) {
        // Grow: everything to VRAM, then recreate taller.
        surf_free(g, hit, true);
        free_slot = hit;
    }
    if (!free_slot)
        return NULL;
    surf_t *s = free_slot;
    memset(s, 0, sizeof(*s));
    s->id = ++g->next_id;
    s->base = base;
    s->stride = stride;
    s->w = w;
    s->h = h;
    s->bpp = bpp;
    s->fmt = fmt;
    s->depth = depth;
    uint32_t words[5] = {s->id, depth ? R128GPU_ROLE_DEPTH : R128GPU_ROLE_COLOR, fmt, w, h};
    if (!emit(g, R128GPU_R_SURFACE, words, 5))
        return NULL;
    // A new surface starts as VRAM's contents.
    mark_cpu_newer(s, 0, h);
    recompute_bounds(g);
    return s;
}

void r128_gpu_fence(r128_gpu_t *g, uint32_t at, uint32_t len, bool write) {
    if (!g || !g->engaged || !len)
        return;
    for (int i = 0; i < R128GPU_MAX_SURF; i++) {
        surf_t *s = &g->surf[i];
        if (!s->id)
            continue;
        uint32_t end = surf_end(s);
        if (at >= end || at + len <= s->base)
            continue;
        uint32_t lo = at > s->base ? at : s->base, hi = at + len < end ? at + len : end;
        uint32_t y0 = (lo - s->base) / s->stride, y1 = (hi - 1u - s->base) / s->stride + 1u;
        if (s->n_gpu)
            readback_rows(g, s, y0, y1);
        if (g->lost)
            return;
        if (write)
            mark_cpu_newer(s, y0, y1);
    }
}

// ============================================================
// Engagement
// ============================================================

static void free_textures(r128_gpu_t *g) {
    for (int i = 0; i < R128GPU_MAX_TEX; i++) {
        if (g->tex[i].id) {
            if (!g->lost) {
                uint32_t w = g->tex[i].id;
                emit(g, R128GPU_R_TEX_FREE, &w, 1);
            }
            g->tex[i].id = 0;
        }
    }
    g->tex_bytes = 0;
}

static void emit_mode(r128_gpu_t *g, bool on) {
    const display_t *d = &g->r->display;
    uint32_t words[3] = {on ? 1u : 0u, on ? d->width : 0u, on ? d->height : 0u};
    emit(g, R128GPU_R_MODE, words, 3);
}

static void engage(r128_gpu_t *g) {
    if (g->lost || g->engaged)
        return;
    rage128_t *r = g->r;
    memset(g->page_gen, 0, ((r->vram_size >> R128GPU_PAGE_SHIFT) + 1u) * sizeof(uint32_t));
    r->gpu_gen = 1;
    r->gpu_page_gen = g->page_gen;
    g->engaged = true;
    g->presenting = false;
    g->idle_vbls = g->bands_frame = g->storm_frames = 0;
    g->n_engage++;
    LOG(1, "Rage 128 webgpu: engaged");
}

void r128_gpu_disengage(r128_gpu_t *g, bool discard) {
    if (!g || !g->engaged)
        return;
    close_draw(g);
    for (int i = 0; i < R128GPU_MAX_SURF; i++)
        surf_free(g, &g->surf[i], !discard);
    free_textures(g);
    if (!g->lost) {
        // The overlay is hidden by the display path once the canvas
        // underneath holds a fresh frame (em_video.c), never here.
        if (g->presenting)
            emit_mode(g, false);
        publish(g);
    }
    g->presenting = false;
    g->engaged = false;
    g->r->gpu_page_gen = NULL;
    recompute_bounds(g);
    g->n_disengage++;
    LOG(1, "Rage 128 webgpu: disengaged (%s)", discard ? "discarding the GPU's pixels" : "read back into VRAM");
}

void r128_gpu_sync_all(r128_gpu_t *g) {
    if (!g || !g->engaged)
        return;
    for (int i = 0; i < R128GPU_MAX_SURF; i++)
        if (g->surf[i].id)
            readback_rows(g, &g->surf[i], 0, g->surf[i].h);
}

// The scanout's colour datatype (the 3D engine's code for it), or 0 when
// the CRTC shows no direct-colour frame.
static uint32_t scan_dst_type(const rage128_t *r) {
    if (r->scan_blanked || !r->display.bits)
        return 0;
    switch (r->display.format) {
    case PIXEL_16BPP_555:
        return 3;
    case PIXEL_16BPP_565:
        return 4;
    case PIXEL_32BPP_XRGB:
        return 6;
    default:
        return 0;
    }
}

static bool wants_engage(r128_gpu_t *g, const st_t *s) {
    if (g->policy == R128GPU_ENGAGE_ALWAYS)
        return true;
    const rage128_t *r = g->r;
    return scan_dst_type(r) == s->dst_type && s->dst_stride == r->display.stride;
}

// ============================================================
// Textures
// ============================================================

static uint32_t tex_bits(uint32_t fmt) {
    return r128_3d_texel_bits(fmt);
}

// Level `lev` of unit `u`: its VRAM/AGP byte base, size and dimensions.
static void tex_level(const tex_unit_t *u, uint32_t lev, uint32_t *base, uint32_t *bytes, uint32_t *w, uint32_t *h) {
    uint32_t lw = u->lpitch > lev ? u->lpitch - lev : 0, lh = u->lheight > lev ? u->lheight - lev : 0;
    int32_t idx = (int32_t)(u->lsize - u->lmin) - (int32_t)lev;
    if (idx < 0)
        idx = 0;
    if (idx > 10)
        idx = 10;
    *base = u->off[idx] & 0x3FFFFFFu;
    *w = 1u << lw;
    *h = 1u << lh;
    *bytes = ((*w) * (*h) * tex_bits(u->fmt) + 7u) / 8u;
}

static uint32_t tex_mem(rage128_t *r, uint32_t addr, uint32_t bytes) {
    if (!(addr & 0x2000000u)) {
        if (addr + bytes > r->vram_size)
            return 0;
        uint32_t v = 0;
        for (uint32_t i = 0; i < bytes; i++)
            v |= (uint32_t)r->vram[addr + i] << (8u * i);
        return v;
    }
    uint32_t w;
    if (!r128_card_read(r, addr & ~3u, &w, 1))
        return 0;
    return (w >> (8u * (addr & 3u))) & (bytes >= 4 ? 0xFFFFFFFFu : ((1u << (8u * bytes)) - 1u));
}

// The raw texel at (tx, ty) of a level, as texel() in rage128_raster.c
// reads it.
static uint32_t tex_raw(rage128_t *r, uint32_t fmt, uint32_t base, uint32_t lw, uint32_t tx, uint32_t ty) {
    uint32_t bits = tex_bits(fmt);
    uint32_t bit = (ty * (1u << lw) + tx) * bits;
    uint32_t v = bits ? tex_mem(r, base + bit / 8u, bits >= 8 ? bits / 8u : 1u) : 0;
    if (bits == 4)
        v = (v >> ((bit & 4u) ? 4 : 0)) & 0xFu;
    return v;
}

static uint64_t tex_hash(rage128_t *r, const tex_unit_t *u, uint32_t top) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t lev = 0; lev <= top; lev++) {
        uint32_t base, bytes, w, hh;
        tex_level(u, lev, &base, &bytes, &w, &hh);
        for (uint32_t a = 0; a < bytes; a += 4) {
            h ^= tex_mem(r, base + a, bytes - a >= 4 ? 4 : bytes - a);
            h *= 1099511628211ull;
        }
    }
    return h;
}

static uint32_t tex_newest_gen(r128_gpu_t *g, const tex_unit_t *u, uint32_t top) {
    rage128_t *r = g->r;
    uint32_t gen = 0, last = (r->vram_size - 1u) >> R128GPU_PAGE_SHIFT;
    for (uint32_t lev = 0; lev <= top; lev++) {
        uint32_t base, bytes, w, h;
        tex_level(u, lev, &base, &bytes, &w, &h);
        if (base & 0x2000000u || !bytes)
            continue;
        for (uint32_t p = base >> R128GPU_PAGE_SHIFT; p <= ((base + bytes - 1u) >> R128GPU_PAGE_SHIFT) && p <= last;
             p++)
            if (r->gpu_page_gen[p] > gen)
                gen = r->gpu_page_gen[p];
    }
    return gen;
}

static void tex_free(r128_gpu_t *g, gtex_t *e) {
    uint32_t w = e->id;
    emit(g, R128GPU_R_TEX_FREE, &w, 1);
    g->tex_bytes -= e->bytes;
    e->id = 0;
    g->n_tex_evict++;
}

// Evict least-recently-used atlases (never one used this frame) until
// `need` more bytes fit.
static void tex_trim(r128_gpu_t *g, uint32_t need) {
    while (g->tex_bytes + need > R128GPU_TEX_BYTES_CAP) {
        gtex_t *old = NULL;
        for (int i = 0; i < R128GPU_MAX_TEX; i++) {
            gtex_t *e = &g->tex[i];
            if (e->id && e->last_use < g->frame && (!old || e->last_use < old->last_use))
                old = e;
        }
        if (!old)
            return;
        tex_free(g, old);
    }
}

// Convert unit `u`'s levels 0..top into an atlas (level L below level
// L-1, each at its own width) and upload it under `e->id`.
static bool tex_upload(r128_gpu_t *g, const tex_unit_t *u, uint32_t top, gtex_t *e) {
    rage128_t *r = g->r;
    uint32_t aw = 1u << u->lpitch, ah = 0;
    for (uint32_t lev = 0; lev <= top; lev++)
        ah += 1u << (u->lheight > lev ? u->lheight - lev : 0);
    // Whatever the levels overlap on the GPU must be in VRAM first.
    for (uint32_t lev = 0; lev <= top; lev++) {
        uint32_t base, bytes, w, h;
        tex_level(u, lev, &base, &bytes, &w, &h);
        if (!(base & 0x2000000u))
            r128_vram_access(r, base, bytes, false);
    }
    if (g->lost)
        return false;
    uint32_t words[3] = {e->id, aw, ah};
    if (!emit(g, R128GPU_R_TEX, words, 3))
        return false;
    uint32_t rows_per = (512u << 10) / (aw * 8u);
    if (!rows_per)
        rows_per = 1;
    uint32_t row = 0;
    for (uint32_t lev = 0; lev <= top; lev++) {
        uint32_t base, bytes, w, h;
        tex_level(u, lev, &base, &bytes, &w, &h);
        uint32_t lw = u->lpitch > lev ? u->lpitch - lev : 0;
        for (uint32_t y = 0; y < h; y += rows_per) {
            uint32_t n = h - y < rows_per ? h - y : rows_per;
            uint32_t at = reserve(g, R128GPU_R_TEX_UPLOAD, 8u + 16u + w * n * 8u);
            if (at == UINT32_MAX)
                return false;
            uint32_t *p = (uint32_t *)payload(g, at);
            p[0] = e->id;
            p[1] = row + y;
            p[2] = w;
            p[3] = n;
            uint32_t *t = p + 4;
            // Each texel twice: converted as the walker converts it (alpha
            // kept; TEX_MAP_AEN is the draw's, applied in the shader) and
            // raw, for the colour key.
            for (uint32_t k = 0; k < n; k++)
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t raw = tex_raw(r, u->fmt, base, lw, x, y + k);
                    *t++ = r128_3d_texel_argb(NULL, u->fmt, raw, true);
                    *t++ = raw;
                }
            g->n_tex_upload_bytes += w * n * 8u;
        }
        row += h;
    }
    e->bytes = aw * ah * 8u;
    g->tex_bytes += e->bytes;
    return true;
}

// The atlas for unit `u` under the snapshot, uploaded if stale.  0 when
// the GPU is gone or the cache is full.
static uint32_t tex_resolve(r128_gpu_t *g, const tex_unit_t *u) {
    uint32_t top = u->lsize - u->lmin;
    uint32_t offs[11];
    bool agp = false;
    for (uint32_t i = 0; i < 11; i++)
        offs[i] = i <= top ? u->off[i] & 0x3FFFFFFu : 0;
    for (uint32_t i = 0; i <= top; i++)
        agp |= (offs[i] & 0x2000000u) != 0;
    gtex_t *e = NULL, *free_slot = NULL;
    for (int i = 0; i < R128GPU_MAX_TEX; i++) {
        gtex_t *t = &g->tex[i];
        if (!t->id) {
            if (!free_slot)
                free_slot = t;
            continue;
        }
        if (t->fmt == u->fmt && t->lpitch == u->lpitch && t->lheight == u->lheight && t->top == top &&
            !memcmp(t->off, offs, sizeof(offs))) {
            e = t;
            break;
        }
    }
    if (e) {
        bool stale;
        if (e->agp) {
            stale = false;
            if (e->checked_frame != g->frame) {
                uint64_t h = tex_hash(g->r, u, top);
                stale = h != e->hash;
                e->hash = h;
                e->checked_frame = g->frame;
            }
        } else {
            stale = tex_newest_gen(g, u, top) > e->gen_seen;
        }
        if (stale) {
            e->gen_seen = g->r->gpu_gen;
            g->tex_bytes -= e->bytes;
            if (!tex_upload(g, u, top, e))
                return 0;
        }
        e->last_use = g->frame;
        return e->id;
    }
    uint32_t aw = 1u << u->lpitch, ah = 0;
    for (uint32_t lev = 0; lev <= top; lev++)
        ah += 1u << (u->lheight > lev ? u->lheight - lev : 0);
    tex_trim(g, aw * ah * 8u);
    if (!free_slot) {
        for (int i = 0; i < R128GPU_MAX_TEX && !free_slot; i++)
            if (g->tex[i].last_use < g->frame) {
                tex_free(g, &g->tex[i]);
                free_slot = &g->tex[i];
            }
        if (!free_slot)
            return 0;
    }
    e = free_slot;
    memset(e, 0, sizeof(*e));
    e->id = ++g->next_id;
    e->fmt = u->fmt;
    e->lpitch = u->lpitch;
    e->lheight = u->lheight;
    e->top = top;
    memcpy(e->off, offs, sizeof(offs));
    e->agp = agp;
    e->gen_seen = g->r->gpu_gen;
    if (agp) {
        e->hash = tex_hash(g->r, u, top);
        e->checked_frame = g->frame;
    }
    if (!tex_upload(g, u, top, e)) {
        e->id = 0;
        return 0;
    }
    e->last_use = g->frame;
    g->n_tex_create++;
    return e->id;
}

// ============================================================
// Batches
// ============================================================

// The whole-channel write masks of a colour datatype, R G B A.
static void channel_masks(uint32_t type, uint32_t m[4]) {
    switch (type) {
    case 3:
        m[0] = 0x7C00u, m[1] = 0x03E0u, m[2] = 0x001Fu, m[3] = 0x8000u;
        break;
    case 4:
        m[0] = 0xF800u, m[1] = 0x07E0u, m[2] = 0x001Fu, m[3] = 0;
        break;
    case 6:
        m[0] = 0x00FF0000u, m[1] = 0x0000FF00u, m[2] = 0x000000FFu, m[3] = 0xFF000000u;
        break;
    case 7:
        m[0] = 0xE0u, m[1] = 0x1Cu, m[2] = 0x03u, m[3] = 0;
        break;
    case 15:
        m[0] = 0x0F00u, m[1] = 0x00F0u, m[2] = 0x000Fu, m[3] = 0xF000u;
        break;
    default:
        m[0] = m[1] = m[2] = m[3] = 0;
        break;
    }
}

// A blend factor in R128GPU_BF_* terms; false when WebGPU has none.
static bool blend_code(uint32_t f, bool for_dst, uint32_t *out) {
    if (f <= 9) {
        *out = f; // the codes are the register's
        return true;
    }
    if (f == 10) {
        *out = R128GPU_BF_SRC_A_SAT;
        return !for_dst;
    }
    *out = R128GPU_BF_ZERO; // 11/12 only mean something as the source; 13-15 read 0
    return true;
}

// Can the GPU draw this batch?  Returns FB_COUNT when it can, else the
// reason; fills the pipeline key.
static int classify(const st_t *s, uint32_t prim, uint32_t *key) {
    if (prim < 4 || prim > 7)
        return FB_PRIM;
    uint32_t ff = FPU_FRONT_FN(s->fpu), bf = FPU_BACK_FN(s->fpu);
    if (ff == CULL_FN_POINTS || ff == CULL_FN_LINES || bf == CULL_FN_POINTS || bf == CULL_FN_LINES)
        return FB_PRIM;
    if (s->rop != 0xCCu)
        return FB_ROP;
    if (s->brush_masks)
        return FB_STIPPLE;
    if (s->dst_bpp == 1 && s->dst_type != 7)
        return FB_FORMAT;
    uint32_t k = 0;
    bool z_en = (s->tex_cntl & TC_Z_EN) != 0;
    if ((s->tex_cntl & TC_STENCIL_EN) && s->z_max == 0xFFFFFFu)
        return FB_STENCIL;
    if (z_en) {
        if (s->z_bytes != 2)
            return FB_ZDEPTH;
        k |= R128GPU_PK_DEPTH | (ZS_Z_TEST(s->zs) << R128GPU_PK_DFUNC_SHIFT);
        if (s->tex_cntl & TC_Z_MASK)
            k |= R128GPU_PK_DEPTH_WRITE;
    }
    if (s->tex_cntl & TC_ALPHA_EN) {
        uint32_t fn = MISC_COMB_FN(s->misc);
        if (fn == 1 || fn == 3)
            return FB_BLEND;
        uint32_t fs = MISC_BLND_SRC(s->misc), fd = MISC_BLND_DST(s->misc), cs, cd;
        if (fs == 11 || fs == 12) {
            cs = fs == 11 ? R128GPU_BF_SRC_A : R128GPU_BF_ONE_MINUS_SRC_A;
            cd = fs == 11 ? R128GPU_BF_ONE_MINUS_SRC_A : R128GPU_BF_SRC_A;
        } else if (!blend_code(fs, false, &cs) || !blend_code(fd, true, &cd)) {
            return FB_BLEND;
        }
        k |= R128GPU_PK_BLEND | (cs << R128GPU_PK_SRC_SHIFT) | (cd << R128GPU_PK_DST_SHIFT);
        if (fn == 2)
            k |= R128GPU_PK_SUBTRACT;
    }
    uint32_t m[4];
    channel_masks(s->dst_type, m);
    uint32_t wm = 0;
    for (int c = 0; c < 4; c++) {
        if (!m[c])
            continue; // a channel the format does not store stays 255
        uint32_t got = s->write_mask & m[c];
        if (got == m[c])
            wm |= 1u << c;
        else if (got)
            return FB_WMASK;
    }
    k |= wm << R128GPU_PK_WMASK_SHIFT;
    *key = k;
    return FB_COUNT;
}

static void build_uniform(r128_gpu_t *g, const st_t *s) {
    uint32_t *u = g->uni;
    memset(u, 0, R128GPU_U_BYTES);
    u[R128GPU_U_TEX_CNTL] = s->tex_cntl;
    u[R128GPU_U_MISC] = s->misc;
    u[R128GPU_U_SCALE] = s->scale;
    u[R128GPU_U_ZS] = s->zs;
    u[R128GPU_U_CONSTANT] = s->constant;
    u[R128GPU_U_FOG_COLOR] = s->fog_color;
    u[R128GPU_U_KEY] = s->key;
    u[R128GPU_U_KEY_MASK] = s->key_mask;
    u[R128GPU_U_DST_TYPE] = s->dst_type;
    u[R128GPU_U_FLAGS] = ((s->tex_cntl & TC_ALPHA_EN) ? R128GPU_F_BLENDING : 0u) | (g->bd ? R128GPU_F_DEPTH : 0u);
    u[R128GPU_U_Z_BITS] = 16;
    u[R128GPU_U_AUX_CNTL] = s->aux_cntl;
    for (int k = 0; k < 3; k++)
        for (int e = 0; e < 4; e++)
            u[R128GPU_U_AUX + 4 * k + e] = (uint32_t)s->aux[k][e];
    for (int t = 0; t < 2; t++) {
        const tex_unit_t *x = &s->t[t];
        uint32_t *w = u + (t ? R128GPU_U_TEX1 : R128GPU_U_TEX0);
        w[R128GPU_UT_ON] = x->on ? 1u : 0u;
        if (!x->on)
            continue;
        w[R128GPU_UT_CNTL] = x->cntl;
        w[R128GPU_UT_COMB] = x->comb;
        w[R128GPU_UT_FMT] = x->fmt;
        w[R128GPU_UT_LPITCH] = x->lpitch;
        w[R128GPU_UT_LHEIGHT] = x->lheight;
        w[R128GPU_UT_TOP] = x->lsize - x->lmin;
        w[R128GPU_UT_BORDER] = x->border;
        w[R128GPU_UT_FLAGS] = x->persp ? R128GPU_TF_PERSP : 0u;
    }
    memcpy(u + R128GPU_U_FOGTABLE, s->fog_table, 256);
    float tw = (float)g->bc->w, th = (float)g->bc->h;
    memcpy(&u[R128GPU_U_TARGET_W], &tw, 4);
    memcpy(&u[R128GPU_U_TARGET_H], &th, 4);
}

bool r128_gpu_batch_begin(r128_gpu_t *g, st_t *s, uint32_t prim, const r128_vertex_t *v, uint32_t n) {
    if (!g || g->lost)
        return false;
    if (!g->engaged) {
        if (g->cooldown || !wants_engage(g, s))
            return false;
        engage(g);
    }
    // Any 3D keeps the takeover engaged, drawn here or by the walker: what
    // the walker's batches cost in readbacks is the storm rule's business.
    g->idle_vbls = 0;
    uint32_t key = 0;
    int why = classify(s, prim, &key);
    if (why != FB_COUNT) {
        g->n_fallback[why]++;
        return false;
    }
    // The rows the batch can reach: its vertices' extent, clipped to the
    // scissor — which also decides how tall the surfaces must be.
    double ymax = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double y = (double)v[i].y + s->win_y;
        if (isfinite(y) && y > ymax)
            ymax = y;
    }
    int32_t need = (int32_t)ceil(ymax) + 1;
    if (need > s->sc_b + 1)
        need = s->sc_b + 1;
    const display_t *d = &g->r->display;
    if (wants_engage(g, s) && d->height && need < (int32_t)d->height)
        need = (int32_t)d->height; // a screen-shaped surface holds a screen
    if (need < 1)
        need = 1;
    surf_t *bc = surf_get(g, s->dst_base, s->dst_stride, s->dst_bpp, s->dst_type, false, (uint32_t)need);
    surf_t *bd = NULL;
    if (bc && (key & R128GPU_PK_DEPTH)) {
        bd = surf_get(g, s->z_base, s->z_stride, 2, 0, true, bc->h);
        if (bd && bd->h > bc->h)
            bc = surf_get(g, s->dst_base, s->dst_stride, s->dst_bpp, s->dst_type, false, bd->h);
        if (!bd || !bc || bd->w != bc->w || bd->h != bc->h) {
            g->n_fallback[FB_SIZE]++;
            return false;
        }
    }
    if (!bc) {
        g->n_fallback[FB_SIZE]++;
        return false;
    }
    g->bc = bc;
    g->bd = bd;
    uint32_t tex_id[2] = {0, 0};
    for (int t = 0; t < 2; t++) {
        if (!s->t[t].on)
            continue;
        tex_id[t] = tex_resolve(g, &s->t[t]);
        if (!tex_id[t]) {
            g->n_fallback[FB_TEX]++;
            return false;
        }
    }
    // Uploads after the texture fences: a fence may have read a surface
    // row back and a texture conversion never writes VRAM.
    if (!upload_rows(g, bc) || (bd && !upload_rows(g, bd)) || g->lost)
        return false;
    int32_t x0 = s->sc_l < 0 ? 0 : s->sc_l, y0 = s->sc_t < 0 ? 0 : s->sc_t;
    int32_t x1 = s->sc_r + 1 > (int32_t)bc->w ? (int32_t)bc->w : s->sc_r + 1;
    int32_t y1 = s->sc_b + 1 > (int32_t)bc->h ? (int32_t)bc->h : s->sc_b + 1;
    g->sc_x0 = x0, g->sc_y0 = y0, g->sc_x1 = x1, g->sc_y1 = y1;
    g->batch_empty = x0 >= x1 || y0 >= y1;
    g->by0 = INT32_MAX;
    g->by1 = -1;
    g->write_color = ((key >> R128GPU_PK_WMASK_SHIFT) & 0xFu) != 0;
    g->write_depth = (key & R128GPU_PK_DEPTH_WRITE) != 0;
    memset(&g->hdr, 0, sizeof(g->hdr));
    g->hdr.color_id = bc->id;
    g->hdr.depth_id = bd ? bd->id : 0u;
    g->hdr.tex_id[0] = tex_id[0];
    g->hdr.tex_id[1] = tex_id[1];
    g->hdr.pipe_key = key;
    g->hdr.sx0 = x0, g->hdr.sy0 = y0, g->hdr.sx1 = x1, g->hdr.sy1 = y1;
    build_uniform(g, s);
    g->n_batches++;
    return true;
}

// Open a DRAW record for the batch's header and uniform (closing the last).
static bool open_draw(r128_gpu_t *g) {
    close_draw(g);
    uint32_t len =
        8u + (uint32_t)sizeof(r128gpu_draw_hdr_t) + R128GPU_U_BYTES + R128GPU_MAX_VERTS * R128GPU_VERTEX_BYTES;
    uint32_t at = reserve(g, R128GPU_R_DRAW, len);
    if (at == UINT32_MAX)
        return false;
    g->draw_open = true;
    g->draw_off = at;
    g->draw_hdr = g->hdr;
    g->draw_hdr.n_verts = 0;
    memcpy(g->draw_uni, g->uni, R128GPU_U_BYTES);
    memcpy(payload(g, at) + sizeof(r128gpu_draw_hdr_t), g->uni, R128GPU_U_BYTES);
    return true;
}

static bool draw_matches(const r128_gpu_t *g) {
    return g->draw_open && g->draw_hdr.n_verts + 3u <= R128GPU_MAX_VERTS &&
           !memcmp(&g->draw_hdr, &g->hdr, offsetof(r128gpu_draw_hdr_t, n_verts)) &&
           !memcmp(g->draw_uni, g->uni, R128GPU_U_BYTES);
}

static void put_vertex(float *o, const svtx_t *v) {
    // Positions on the walker's coverage grid (1/16 pixel).
    o[0] = (float)(floor(v->x * 16.0 + 0.5) / 16.0);
    o[1] = (float)(floor(v->y * 16.0 + 0.5) / 16.0);
    o[2] = (float)v->a[A_Z];
    for (int k = 0; k < 4; k++)
        o[3 + k] = (float)v->a[A_R + k];
    for (int k = 0; k < 3; k++)
        o[7 + k] = (float)v->a[A_SR + k];
    o[10] = (float)v->a[A_FOG];
    for (int k = 0; k < 6; k++)
        o[11 + k] = (float)v->a[A_S0 + k];
    o[17] = o[18] = o[19] = 0.0f;
}

void r128_gpu_tri(r128_gpu_t *g, const st_t *s, const svtx_t *a, const svtx_t *b, const svtx_t *c) {
    (void)s;
    if (g->batch_empty || g->lost)
        return;
    if (!draw_matches(g) && !open_draw(g))
        return;
    float *o = (float *)(payload(g, g->draw_off) + sizeof(r128gpu_draw_hdr_t) + R128GPU_U_BYTES +
                         g->draw_hdr.n_verts * R128GPU_VERTEX_BYTES);
    const svtx_t *vv[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        put_vertex(o + i * R128GPU_VERTEX_FLOATS, vv[i]);
        double y = vv[i]->y;
        int32_t yi = (int32_t)floor(y);
        if (yi < g->by0)
            g->by0 = yi;
        if (yi > g->by1)
            g->by1 = yi;
    }
    g->draw_hdr.n_verts += 3;
    g->n_tris++;
}

void r128_gpu_batch_end(r128_gpu_t *g, const st_t *s) {
    (void)s;
    if (g->lost || g->batch_empty || g->by1 < g->by0)
        return;
    int32_t y0 = g->by0 < g->sc_y0 ? g->sc_y0 : g->by0;
    int32_t y1 = g->by1 + 1 > g->sc_y1 ? g->sc_y1 : g->by1 + 1;
    if (y0 >= y1)
        return;
    if (g->write_color) {
        mark_gpu_newer(g->bc, (uint32_t)y0, (uint32_t)y1);
        bump_pages(g, g->bc->base + (uint32_t)y0 * g->bc->stride, g->bc->base + (uint32_t)y1 * g->bc->stride);
    }
    if (g->bd && g->write_depth) {
        mark_gpu_newer(g->bd, (uint32_t)y0, (uint32_t)y1);
        bump_pages(g, g->bd->base + (uint32_t)y0 * g->bd->stride, g->bd->base + (uint32_t)y1 * g->bd->stride);
    }
}

// ============================================================
// 2D fills
// ============================================================

bool r128_gpu_fill(r128_gpu_t *g, uint32_t base, uint32_t stride, uint32_t bpp, int32_t x0, int32_t y0, int32_t x1,
                   int32_t y1, uint32_t value) {
    if (!g || !g->engaged || g->lost || x0 >= x1 || y0 >= y1 || x0 < 0 || y0 < 0)
        return false;
    uint32_t a0 = base + (uint32_t)y0 * stride + (uint32_t)x0 * bpp;
    uint32_t a1 = base + (uint32_t)(y1 - 1) * stride + (uint32_t)x1 * bpp;
    if (a1 <= g->r->gpu_lo || a0 >= g->r->gpu_hi)
        return false;
    for (int i = 0; i < R128GPU_MAX_SURF; i++) {
        surf_t *s = &g->surf[i];
        if (!s->id || s->stride != stride || a0 < s->base || a1 > surf_end(s))
            continue;
        if (bpp % s->bpp)
            return false;
        // The fill value as surface pixels: every sub-pixel of a wider
        // fill must be the same.
        uint32_t per = bpp / s->bpp, smask = s->bpp >= 4 ? 0xFFFFFFFFu : (1u << (8u * s->bpp)) - 1u;
        uint32_t sv = value & smask;
        for (uint32_t k = 1; k < per; k++)
            if (((value >> (8u * s->bpp * k)) & smask) != sv)
                return false;
        uint32_t off = a0 - s->base;
        if ((off % s->stride) % s->bpp)
            return false;
        uint32_t sx0 = (off % s->stride) / s->bpp, sy0 = off / s->stride;
        uint32_t sx1 = sx0 + (uint32_t)(x1 - x0) * per, sy1 = sy0 + (uint32_t)(y1 - y0);
        if (sx1 > s->w || sy1 > s->h)
            return false;
        if (!upload_rows(g, s))
            return false;
        uint32_t words[7] = {s->depth ? 0u : s->id, s->depth ? s->id : 0u, sx0, sy0, sx1, sy1, sv};
        if (!emit(g, R128GPU_R_FILL, words, 7))
            return false;
        mark_gpu_newer(s, sy0, sy1);
        bump_pages(g, s->base + sy0 * s->stride, s->base + sy1 * s->stride);
        g->n_fills++;
        return true;
    }
    return false;
}

// ============================================================
// Vertical blank
// ============================================================

bool r128_gpu_vblank(r128_gpu_t *g) {
    if (!g || g->lost)
        return false;
    if (!g->engaged) {
        if (g->cooldown)
            g->cooldown--;
        return false;
    }
    g->frame++;
    close_draw(g);
    if (g->bands_frame > R128GPU_STORM_BANDS)
        g->storm_frames++;
    else
        g->storm_frames = 0;
    g->bands_frame = 0;
    if (g->storm_frames >= R128GPU_STORM_FRAMES) {
        g->n_storm++;
        LOG(1, "Rage 128 webgpu: readback storm — back to the software rasteriser for a while");
        r128_gpu_disengage(g, false);
        g->cooldown = R128GPU_COOLDOWN_VBLS;
        return false;
    }
    if (++g->idle_vbls > R128GPU_IDLE_VBLS) {
        r128_gpu_disengage(g, false);
        return false;
    }
    // Present when the scanout IS a colour surface (and no hardware
    // cursor needs compositing).
    rage128_t *r = g->r;
    const display_t *d = &r->display;
    uint32_t type = scan_dst_type(r);
    surf_t *hit = NULL;
    if (type && !(r->reg[R_CRTC_GEN_CNTL / 4] & CRTC_CUR_EN)) {
        for (int i = 0; i < R128GPU_MAX_SURF && !hit; i++) {
            surf_t *s = &g->surf[i];
            if (s->id && !s->depth && s->base == r->scan_base && s->stride == d->stride && s->fmt == type &&
                s->h >= d->height && s->w >= d->width)
                hit = s;
        }
    }
    if (!hit) {
        if (g->presenting) {
            g->presenting = false;
            emit_mode(g, false);
        }
        publish(g);
        return false;
    }
    if (!upload_rows(g, hit))
        return false;
    // The display table: the DAC's direct-colour table, then the CRT's
    // response — what the WebGL renderer composes (em_video.c).
    uint8_t lut[3][256];
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) {
            uint8_t x = d->dac_lut ? d->dac_lut[c][v] : (uint8_t)v;
            lut[c][v] = d->crt_response ? d->crt_response[c][x] : x;
        }
    if (!g->lut_sent || memcmp(lut, g->lut, sizeof(lut))) {
        close_draw(g);
        uint32_t at = reserve(g, R128GPU_R_LUT, 8u + 768u);
        if (at == UINT32_MAX)
            return false;
        memcpy(payload(g, at), lut, 768);
        memcpy(g->lut, lut, sizeof(lut));
        g->lut_sent = true;
    }
    if (!g->presenting) {
        g->presenting = true;
        emit_mode(g, true);
    }
    uint32_t bits[3] = {type == 4   ? 5u
                        : type == 3 ? 5u
                                    : 8u,
                        type == 4   ? 6u
                        : type == 3 ? 5u
                                    : 8u,
                        type == 4   ? 5u
                        : type == 3 ? 5u
                                    : 8u};
    uint32_t words[6] = {hit->id, d->width, d->height, bits[0], bits[1], bits[2]};
    emit(g, R128GPU_R_PRESENT, words, 6);
    publish(g);
    g->n_presents++;
    return true;
}

// ============================================================
// Lifetime and statistics
// ============================================================

bool r128_gpu_engaged(const r128_gpu_t *g) {
    return g && g->engaged;
}

const char *r128_gpu_stats(r128_gpu_t *g, char *buf, size_t n) {
    if (!g) {
        if (n)
            buf[0] = 0;
        return buf;
    }
    int k =
        snprintf(buf, n,
                 "engaged=%d presenting=%d lost=%d engages=%llu disengages=%llu storms=%llu batches=%llu "
                 "tris=%llu draws=%llu fills=%llu presents=%llu readback_bands=%llu readback_rows=%llu "
                 "upload_rows=%llu tex_create=%llu tex_upload_bytes=%llu tex_evict=%llu tex_bytes=%llu",
                 g->engaged, g->presenting, g->lost, (unsigned long long)g->n_engage,
                 (unsigned long long)g->n_disengage, (unsigned long long)g->n_storm, (unsigned long long)g->n_batches,
                 (unsigned long long)g->n_tris, (unsigned long long)g->n_draws, (unsigned long long)g->n_fills,
                 (unsigned long long)g->n_presents, (unsigned long long)g->n_readback_bands,
                 (unsigned long long)g->n_readback_rows, (unsigned long long)g->n_upload_rows,
                 (unsigned long long)g->n_tex_create, (unsigned long long)g->n_tex_upload_bytes,
                 (unsigned long long)g->n_tex_evict, (unsigned long long)g->tex_bytes);
    for (int i = 0; i < FB_COUNT && k > 0 && (size_t)k < n; i++)
        k += snprintf(buf + k, n - (size_t)k, " fallback_%s=%llu", k_fb_names[i], (unsigned long long)g->n_fallback[i]);
    if (g->ctrl && k > 0 && (size_t)k < n)
        snprintf(buf + k, n - (size_t)k, " gpu_frames=%u gpu_draws=%u gpu_flushes=%u gpu_pipelines=%u gpu_readbacks=%u",
                 gload(g, R128GPU_C_STAT_FRAMES), gload(g, R128GPU_C_STAT_DRAWS), gload(g, R128GPU_C_STAT_FLUSHES),
                 gload(g, R128GPU_C_STAT_PIPELINES), gload(g, R128GPU_C_STAT_READBACKS));
    return buf;
}

r128_gpu_t *r128_gpu_create(rage128_t *r, int engage_policy) {
    if (!gs_v2gpu_available()) {
        LOG(1, "Rage 128 raster=webgpu: no WebGPU transport on this host — the software rasteriser draws");
        return NULL;
    }
    r128_gpu_t *g = (r128_gpu_t *)calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->r = r;
    g->policy = engage_policy;
    g->tex = (gtex_t *)calloc(R128GPU_MAX_TEX, sizeof(gtex_t));
    g->page_gen = (uint32_t *)calloc((r->vram_size >> R128GPU_PAGE_SHIFT) + 1u, sizeof(uint32_t));
    uint32_t ctrl_bytes = R128GPU_CTRL_WORDS * 4u, ring_size = R128GPU_RING_BYTES;
    g->rb_size = R128GPU_RB_BYTES;
    g->region = (uint8_t *)calloc(1, 64u + ctrl_bytes + ring_size + g->rb_size);
    if (!g->tex || !g->page_gen || !g->region) {
        free(g->tex);
        free(g->page_gen);
        free(g->region);
        free(g);
        return NULL;
    }
    uintptr_t base = ((uintptr_t)g->region + 63u) & ~(uintptr_t)63u;
    g->ctrl = (volatile uint32_t *)base;
    mbx_ring_init(&g->ring, g->ctrl, R128GPU_C_HEAD, R128GPU_C_TAIL, (uint8_t *)(base + ctrl_bytes), ring_size);
    g->rb = g->ring.buf + ring_size;
    for (int i = 0; i < R128GPU_CTRL_WORDS; i++)
        g->ctrl[i] = 0;
    g->ctrl[R128GPU_C_MAGIC] = R128GPU_MAGIC;
    g->ctrl[R128GPU_C_VERSION] = R128GPU_PROTOCOL_VERSION;
    g->ctrl[R128GPU_C_RING_OFF] = ctrl_bytes;
    g->ctrl[R128GPU_C_RING_SIZE] = ring_size;
    g->ctrl[R128GPU_C_RB_OFF] = ctrl_bytes + ring_size;
    g->ctrl[R128GPU_C_RB_SIZE] = g->rb_size;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!gs_v2gpu_attach((void *)g->ctrl, ctrl_bytes + ring_size + g->rb_size)) {
        LOG(0, "Rage 128 raster=webgpu: the host refused to attach a GPU worker — the software rasteriser draws");
        free(g->tex);
        free(g->page_gen);
        free(g->region);
        free(g);
        return NULL;
    }
    double t0 = now_ms();
    while (gload(g, R128GPU_C_STATUS) != R128GPU_STATUS_ATTACHED) {
        if (now_ms() - t0 >= (double)R128GPU_ATTACH_MS) {
            LOG(0, "Rage 128 raster=webgpu: the GPU worker did not attach within %u ms — the software rasteriser draws",
                R128GPU_ATTACH_MS);
            gs_v2gpu_detach((void *)g->ctrl);
            free(g->tex);
            free(g->page_gen);
            free(g->region);
            free(g);
            return NULL;
        }
        gs_v2gpu_wait(&g->ctrl[R128GPU_C_STATUS], R128GPU_STATUS_DETACHED, 20);
        gs_v2gpu_keepalive();
    }
    g->attached = true;
    LOG(1, "Rage 128 raster=webgpu: GPU worker attached (op ring %u KB)", g->ring.size >> 10);
    return g;
}

void r128_gpu_destroy(r128_gpu_t *g) {
    if (!g)
        return;
    if (g->engaged)
        r128_gpu_disengage(g, true);
    if (g->attached && !g->lost) {
        uint32_t seq = ++g->seq;
        gstore(g, R128GPU_C_REQ, seq);
        emit(g, R128GPU_R_SHUTDOWN, &seq, 1);
        wait_ack(g, seq);
    }
    if (g->attached)
        gs_v2gpu_detach((void *)g->ctrl);
    g->r->gpu_page_gen = NULL;
    g->r->gpu_lo = g->r->gpu_hi = 0;
    free(g->region);
    free(g->tex);
    free(g->page_gen);
    free(g);
}
