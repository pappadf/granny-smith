// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// qd3d_raster.c
// The QuickDraw 3D Accelerator Card's renderer -- the executor of the raster
// seam (qd3d_raster.h) -- and the backends that decide where it runs.
//
// The renderer works a scanline at a time, as the ASIC does.  Every live
// object that covers the scanline adds a fragment to each pixel it covers;
// each pixel keeps its fragments sorted front to back (an opaque fragment
// drops everything behind it) and composites them, CSG operands through the
// truth table, over the background register.  The finished scanline is
// appended to the frame's output stream, which the card streams through the
// descriptor list when the frame completes.

#include "qd3d_raster.h"

#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// The thread backend builds everywhere the core builds: native pthreads,
// and Emscripten's pthreads-on-Web-Workers, created on demand at the
// card's first boot exactly as the Voodoo2's (voodoo2_raster.c).  A build
// can leave it out with -DGS_QD3D_THREAD_BACKEND=0 (EXTRA_CFLAGS): no
// pthread code is compiled, raster=thread falls back to the walker with a
// log line, and the default backend becomes the walker.
#ifndef GS_QD3D_THREAD_BACKEND
#define GS_QD3D_THREAD_BACKEND 1
#endif
#define Q3_HAVE_THREAD_BACKEND GS_QD3D_THREAD_BACKEND
#if Q3_HAVE_THREAD_BACKEND
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#endif

LOG_USE_CATEGORY_NAME("qd3d");

// Object header bits the renderer reads.
#define HDR_BACKFACING 0x04000000u
#define HDR_CSG        0x01000000u
#define HDR_OPAQUE     0x00200000u

// Fragment flags: an opaque layer hides everything behind it; a CSG
// operand's surface enters (front face) or leaves (back face) operand ID.
#define FRAG_OPAQUE    0x80u
#define FRAG_CSG       0x40u
#define FRAG_BACKFACE  0x20u
#define FRAG_CSG_ID(f) ((f) & 7u)

// ============================================================
// The target
// ============================================================

bool q3_target_init(q3_target_t *tgt, const uint32_t *tex) {
    memset(tgt, 0, sizeof(*tgt));
    tgt->tex = tex;
    tgt->frags = (q3_frag_t *)calloc((size_t)Q3_MAX_WIDTH * Q3_LAYERS, sizeof(*tgt->frags));
    tgt->nfrags = (uint8_t *)calloc(Q3_MAX_WIDTH, 1);
    if (!tgt->frags || !tgt->nfrags) {
        q3_target_free(tgt);
        return false;
    }
    return true;
}

void q3_target_free(q3_target_t *tgt) {
    free(tgt->objs);
    free(tgt->live);
    free(tgt->frags);
    free(tgt->nfrags);
    free(tgt->out);
    memset(tgt, 0, sizeof(*tgt));
}

bool q3_target_reserve(q3_target_t *tgt, size_t len) {
    if (len <= tgt->out_cap)
        return true;
    size_t cap = tgt->out_cap ? tgt->out_cap : 65536u;
    while (cap < len)
        cap *= 2;
    uint8_t *p = (uint8_t *)realloc(tgt->out, cap);
    if (!p)
        return false;
    tgt->out = p;
    tgt->out_cap = cap;
    return true;
}

// ============================================================
// Textures
// ============================================================

// Where one object's map lives, level by level, for the scanline being
// drawn.  Mip levels are stored level by level for all maps: register
// $48+4L is the base of global level L (256² down to 1²), a 128² map's own
// level 0 living in global level 1, and a map's levels follow its slot.
// `off` is the level's base relative to the SRAM ($800000), `slotbase` the
// texel index of the map's first texel within the level.
typedef struct q3_texview {
    const uint32_t *tex;
    int size, levels;
    uint32_t off[Q3_MAP_LEVELS];
    uint32_t slotbase[Q3_MAP_LEVELS];
} q3_texview_t;

static void texview_init(q3_texview_t *tv, const q3_draw_state_t *st, const q3_target_t *tgt, const q3_obj_t *o) {
    tv->tex = tgt->tex;
    tv->size = o->tex_size;
    tv->levels = o->tex_levels;
    for (int level = 0; level < o->tex_levels && level < Q3_MAP_LEVELS; level++) {
        int global = level + (o->tex_size == 128 ? 1 : 0);
        uint32_t s = (uint32_t)(o->tex_size >> level);
        if (s < 1)
            s = 1;
        tv->off[level] = global < Q3_MAP_LEVELS ? st->mapbase[global] - Q3_TEX_BASE : 0xFFFFFFFFu;
        tv->slotbase[level] = (uint32_t)o->tex_slot * s * s;
    }
}

// One texel of mip level `level` (`s` texels square), wrapping u and v.
// Row 0 is the image's bottom row.  Outside the SRAM reads as 0.
static inline uint32_t tex_fetch(const q3_texview_t *tv, int level, int s, int x, int y) {
    x &= s - 1;
    y &= s - 1;
    uint32_t idx = (tv->off[level] + (tv->slotbase[level] + (uint32_t)y * (uint32_t)s + (uint32_t)x) * 4u) >> 2;
    return idx < Q3_TEX_TEXELS ? tv->tex[idx] : 0;
}

// floorf, without the library call where the answer is a small integer:
// below 2^23 in magnitude the truncation is exact, and one step down fixes
// a negative fraction.  Anything else (large, infinite, NaN) asks floorf.
static inline float floor_fast(float f) {
    if (!(fabsf(f) < 8388608.0f))
        return floorf(f);
    float t = (float)(int32_t)f;
    return t > f ? t - 1.0f : t;
}

// Bilinear sample of one mip level, as floats 0..255.
static inline void tex_bilinear(const q3_texview_t *tv, int level, float u, float v, float rgb[3]) {
    int s = tv->size >> level;
    if (s < 1)
        s = 1;
    float fu = u * (float)s - 0.5f, fv = v * (float)s - 0.5f;
    float iu = floor_fast(fu), iv = floor_fast(fv);
    float wu = fu - iu, wv = fv - iv;
    int x = (int)iu, y = (int)iv;
    uint32_t t00 = tex_fetch(tv, level, s, x, y), t10 = tex_fetch(tv, level, s, x + 1, y);
    uint32_t t01 = tex_fetch(tv, level, s, x, y + 1), t11 = tex_fetch(tv, level, s, x + 1, y + 1);
    for (int c = 0; c < 3; c++) {
        int sh = 16 - 8 * c;
        float a = (float)((t00 >> sh) & 0xFF), b = (float)((t10 >> sh) & 0xFF);
        float cc = (float)((t01 >> sh) & 0xFF), d = (float)((t11 >> sh) & 0xFF);
        rgb[c] = (a + (b - a) * wu) * (1.0f - wv) + (cc + (d - cc) * wu) * wv;
    }
}

// Trilinear sample: the footprint (texture units per pixel, normalised)
// picks the level, and the two nearest levels are blended.
static inline void tex_sample(const q3_texview_t *tv, float u, float v, float footprint, float rgb[3]) {
    // A footprint of at most one texel is level 0 -- exactly where the
    // logarithm is not positive -- so it needs no log2f.
    float texels = footprint * (float)tv->size;
    float lambda = footprint > 0.0f && texels > 1.0f ? log2f(texels) : 0.0f;
    float maxl = (float)(tv->levels - 1);
    if (lambda <= 0.0f || tv->levels <= 1) {
        tex_bilinear(tv, 0, u, v, rgb);
        return;
    }
    if (lambda >= maxl) {
        tex_bilinear(tv, tv->levels - 1, u, v, rgb);
        return;
    }
    int l0 = (int)lambda;
    float w = lambda - (float)l0;
    float c0[3], c1[3];
    tex_bilinear(tv, l0, u, v, c0);
    tex_bilinear(tv, l0 + 1, u, v, c1);
    for (int c = 0; c < 3; c++)
        rgb[c] = c0[c] + (c1[c] - c0[c]) * w;
}

// ============================================================
// Fragments
// ============================================================

// Is depth `a` nearer than `b`?  With perspective depth the fields hold
// 1/w (larger is nearer), otherwise z (smaller is nearer).
static inline bool nearer(bool persp, float a, float b) {
    return persp ? a > b : a < b;
}

// Add a fragment to pixel `x`: hidden behind an opaque layer it is
// dropped; an opaque fragment drops everything behind it.
static void frag_add(q3_target_t *tgt, bool persp, uint32_t x, const q3_frag_t *f) {
    q3_frag_t *list = &tgt->frags[x * Q3_LAYERS];
    uint32_t n = tgt->nfrags[x];
    for (uint32_t i = 0; i < n; i++)
        if ((list[i].flags & FRAG_OPAQUE) && !nearer(persp, f->depth, list[i].depth))
            return;
    if (f->flags & FRAG_OPAQUE) {
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++)
            if (nearer(persp, list[i].depth, f->depth))
                list[k++] = list[i];
        n = k;
    }
    if (n < Q3_LAYERS) {
        list[n++] = *f;
    } else {
        // Full: replace the farthest layer if this one is nearer.
        uint32_t far = 0;
        for (uint32_t i = 1; i < n; i++)
            if (nearer(persp, list[far].depth, list[i].depth))
                far = i;
        if (nearer(persp, f->depth, list[far].depth))
            list[far] = *f;
    }
    tgt->nfrags[x] = (uint8_t)n;
}

// A fragment's flags from its object's header.  The plug-in marks an
// object opaque only when all its vertex alphas are 255 and it is no CSG
// operand, so CSG surfaces never hide what is behind them.
static uint8_t frag_flags(uint32_t header) {
    uint8_t f = (header & HDR_OPAQUE) ? FRAG_OPAQUE : 0u;
    if (header & HDR_CSG)
        f |= (uint8_t)(FRAG_CSG | (header & 7u) | ((header & HDR_BACKFACING) ? FRAG_BACKFACE : 0u));
    return f;
}

static inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// ============================================================
// Drawing
// ============================================================

// Rasterise one triangle on scanline `y`: the pixels whose centres lie from
// the left edge up to but excluding the right edge.  The card samples at
// pixel centres (13.3 coordinates, centres at .5); vertically the plug-in's
// own bucketing fixes the rule as yTop < centre <= yBottom, horizontally
// this is the usual left-inclusive convention.  (Apple's software engine
// truncates x to whole pixels instead, so its edges can differ by a column.)
static void draw_tri(const q3_draw_state_t *st, q3_target_t *tgt, const q3_obj_t *o, int y, uint32_t width) {
    int yc = y * 8 + 4; // the scanline centre in 13.3
    float xs[3];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        int ya = o->y13[i], yb = o->y13[j];
        float xa = o->x13[i], xb = o->x13[j];
        if (ya > yb) {
            int t = ya;
            ya = yb;
            yb = t;
            float tx = xa;
            xa = xb;
            xb = tx;
        }
        if (ya < yc && yc <= yb)
            xs[n++] = (xa + (xb - xa) * (float)(yc - ya) / (float)(yb - ya)) / 8.0f;
    }
    if (n < 2)
        return;
    float xl = xs[0] < xs[1] ? xs[0] : xs[1];
    float xr = xs[0] < xs[1] ? xs[1] : xs[0];
    int px0 = (int)ceilf(xl - 0.5f), px1 = (int)ceilf(xr - 0.5f) - 1;
    if (px0 < 0)
        px0 = 0;
    if (px1 >= (int)width)
        px1 = (int)width - 1;
    if (px0 > px1)
        return;
    bool persp = st->persp;
    // Each attribute is its plane evaluated at the pixel centre,
    // a0 + dx * cx + dy * cy; the dy term is the scanline's constant.
    float cy = (float)y + 0.5f - o->y0;
    float ady[Q3_A_NATTR];
    for (int k = 0; k < Q3_A_NATTR; k++)
        ady[k] = o->dy[k] * cy;
    q3_frag_t f;
    f.flags = frag_flags(o->header);
    if (o->kind == Q3_OBJ_TEXTURED) {
        q3_texview_t tv;
        texview_init(&tv, st, tgt, o);
        for (int px = px0; px <= px1; px++) {
            float cx = (float)px + 0.5f - o->x0;
            float depth = o->a0[Q3_A_DEPTH] + o->dx[Q3_A_DEPTH] * cx + ady[Q3_A_DEPTH];
            float alpha = o->a0[Q3_A_ALPHA] + o->dx[Q3_A_ALPHA] * cx + ady[Q3_A_ALPHA];
            float u = o->a0[Q3_A_U] + o->dx[Q3_A_U] * cx + ady[Q3_A_U];
            float v = o->a0[Q3_A_V] + o->dx[Q3_A_V] * cx + ady[Q3_A_V];
            float fp = o->a0[Q3_A_LOD] + o->dx[Q3_A_LOD] * cx + ady[Q3_A_LOD];
            float kd = o->a0[Q3_A_KD] + o->dx[Q3_A_KD] * cx + ady[Q3_A_KD];
            float ks = o->a0[Q3_A_KS] + o->dx[Q3_A_KS] * cx + ady[Q3_A_KS];
            f.depth = depth;
            f.a = clamp01(alpha);
            // Perspective: u, v and the footprint carry 1/w factors the
            // card divides out per pixel.
            if (persp && depth != 0.0f) {
                float w = 1.0f / depth;
                u *= w;
                v *= w;
                fp *= w * w;
            }
            float rgb[3];
            tex_sample(&tv, u, v, fabsf(fp), rgb);
            kd = kd < 0.0f ? 0.0f : kd;
            ks = ks < 0.0f ? 0.0f : ks;
            f.r = rgb[0] / 255.0f * kd + ks;
            f.g = rgb[1] / 255.0f * kd + ks;
            f.b = rgb[2] / 255.0f * kd + ks;
            frag_add(tgt, persp, (uint32_t)px, &f);
        }
    } else {
        for (int px = px0; px <= px1; px++) {
            float cx = (float)px + 0.5f - o->x0;
            f.depth = o->a0[Q3_A_DEPTH] + o->dx[Q3_A_DEPTH] * cx + ady[Q3_A_DEPTH];
            f.a = clamp01(o->a0[Q3_A_ALPHA] + o->dx[Q3_A_ALPHA] * cx + ady[Q3_A_ALPHA]);
            f.r = o->a0[Q3_A_R] + o->dx[Q3_A_R] * cx + ady[Q3_A_R];
            f.g = o->a0[Q3_A_G] + o->dx[Q3_A_G] * cx + ady[Q3_A_G];
            f.b = o->a0[Q3_A_B] + o->dx[Q3_A_B] * cx + ady[Q3_A_B];
            frag_add(tgt, persp, (uint32_t)px, &f);
        }
    }
}

// One row of a bitmap tile: set bits draw the tile's colour.
static void draw_bitmap(const q3_draw_state_t *st, q3_target_t *tgt, const q3_obj_t *o, int y, uint32_t width) {
    uint32_t bits = o->mask[y - o->by];
    q3_frag_t f;
    // A tile is never marked opaque, but a fully opaque colour hides what
    // is behind it all the same.
    f.flags = (uint8_t)(frag_flags(o->header) | ((o->colour >> 24) == 0xFFu ? FRAG_OPAQUE : 0u));
    f.depth = o->a0[Q3_A_DEPTH];
    f.a = (float)(o->colour >> 24) / 255.0f;
    f.r = (float)((o->colour >> 16) & 0xFFu) / 255.0f;
    f.g = (float)((o->colour >> 8) & 0xFFu) / 255.0f;
    f.b = (float)(o->colour & 0xFFu) / 255.0f;
    for (int i = 0; i < 32 && bits; i++, bits <<= 1) {
        int px = o->bx + i;
        if ((bits & 0x80000000u) && px >= 0 && px < (int)width)
            frag_add(tgt, st->persp, (uint32_t)px, &f);
    }
}

// Saturate a 0..1 float to a byte.
static inline uint8_t to_byte(float v) {
    if (v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

// Pack one pixel into the output stream: big-endian x1555 or xRGB8888.
static inline uint8_t *put_pixel(uint8_t *out, bool dest16, uint8_t R, uint8_t G, uint8_t B) {
    if (dest16) {
        uint16_t p = (uint16_t)(((R >> 3) << 10) | ((G >> 3) << 5) | (B >> 3));
        *out++ = (uint8_t)(p >> 8);
        *out++ = (uint8_t)p;
    } else {
        *out++ = 0;
        *out++ = R;
        *out++ = G;
        *out++ = B;
    }
    return out;
}

// Render scanline `y` from the live objects and append it to the output.
static void render_scanline(const q3_draw_state_t *st, q3_target_t *tgt, int y, uint32_t width) {
    if (width > Q3_MAX_WIDTH)
        width = Q3_MAX_WIDTH;
    bool persp = st->persp;
    memset(tgt->nfrags, 0, width);
    // Retire objects above this scanline; draw the rest that cover it.
    uint32_t k = 0;
    for (uint32_t i = 0; i < tgt->n_live; i++) {
        const q3_obj_t *o = &tgt->objs[tgt->live[i]];
        if (o->last < y)
            continue;
        tgt->live[k++] = tgt->live[i];
        if (o->first > y)
            continue;
        if (o->kind == Q3_OBJ_BITMAP)
            draw_bitmap(st, tgt, o, y, width);
        else
            draw_tri(st, tgt, o, y, width);
    }
    tgt->n_live = k;
    size_t bytes = (size_t)width * (st->dest16 ? 2u : 4u);
    if (tgt->out_failed || !q3_target_reserve(tgt, tgt->out_len + bytes)) {
        tgt->out_failed = true;
        return;
    }
    // Composite each pixel front to back over the background.
    uint32_t bg = st->background;
    uint32_t csg = st->csg;
    float bgr = (float)((bg >> 16) & 0xFFu) / 255.0f, bgg = (float)((bg >> 8) & 0xFFu) / 255.0f,
          bgb = (float)(bg & 0xFFu) / 255.0f;
    // A pixel nothing covers is the background (0 + 1 x background, exactly).
    uint8_t bgR = to_byte(bgr), bgG = to_byte(bgg), bgB = to_byte(bgb);
    uint8_t *out = tgt->out + tgt->out_len;
    for (uint32_t x = 0; x < width; x++) {
        uint32_t n = tgt->nfrags[x];
        if (n == 0) {
            out = put_pixel(out, st->dest16, bgR, bgG, bgB);
            continue;
        }
        q3_frag_t *list = &tgt->frags[x * Q3_LAYERS];
        // insertion sort, nearest first
        for (uint32_t i = 1; i < n; i++) {
            q3_frag_t t = list[i];
            uint32_t j = i;
            while (j > 0 && nearer(persp, t.depth, list[j - 1].depth)) {
                list[j] = list[j - 1];
                j--;
            }
            list[j] = t;
        }
        // Colours are premultiplied, RAVE's default blend (the card ignores
        // the Blend tag): each layer adds its colour and lets 1 - alpha of
        // what is behind it through.  CSG: walking front to back from
        // outside every operand, a front face enters its operand and a
        // back face leaves it; the 5-bit inside state indexes the truth
        // table, and an operand's surface is part of the result only where
        // the table's answer changes.
        float r = 0, g = 0, b = 0, t = 1.0f;
        uint32_t state = 0;
        for (uint32_t i = 0; i < n && t > 0.0f; i++) {
            if (list[i].flags & FRAG_CSG) {
                uint32_t before = (csg >> state) & 1u, bit = 1u << FRAG_CSG_ID(list[i].flags);
                state = (list[i].flags & FRAG_BACKFACE) ? (state & ~bit) : (state | bit);
                if (((csg >> state) & 1u) == before)
                    continue;
            }
            r += t * list[i].r;
            g += t * list[i].g;
            b += t * list[i].b;
            t *= 1.0f - list[i].a;
        }
        r += t * bgr;
        g += t * bgg;
        b += t * bgb;
        out = put_pixel(out, st->dest16, to_byte(r), to_byte(g), to_byte(b));
    }
    tgt->out_len += bytes;
}

// Append one object to the frame and to the live list.
static void add_object(q3_target_t *tgt, const q3_obj_t *o) {
    if (tgt->n_objs == tgt->cap_objs) {
        uint32_t cap = tgt->cap_objs ? tgt->cap_objs * 2 : 1024;
        q3_obj_t *p = (q3_obj_t *)realloc(tgt->objs, cap * sizeof(*p));
        uint32_t *l = (uint32_t *)realloc(tgt->live, cap * sizeof(*l));
        if (p)
            tgt->objs = p;
        if (l)
            tgt->live = l;
        if (!p || !l)
            return; // dropped: out of memory
        tgt->cap_objs = tgt->cap_live = cap;
    }
    tgt->objs[tgt->n_objs] = *o;
    tgt->live[tgt->n_live++] = tgt->n_objs++;
}

void q3_raster_execute(const q3_draw_state_t *st, q3_target_t *tgt, const q3_cmd_t *cmd) {
    switch (cmd->kind) {
    case Q3_CMD_FRAME:
        tgt->n_objs = tgt->n_live = 0;
        tgt->out_len = 0;
        tgt->out_failed = false;
        break;
    case Q3_CMD_OBJECT:
        add_object(tgt, &cmd->u.obj);
        break;
    case Q3_CMD_SCANLINE:
        render_scanline(st, tgt, cmd->u.line.y, cmd->u.line.width);
        break;
    default:
        break;
    }
}

// ============================================================
// Backends -- WHERE the executor runs
// ============================================================

#define Q3_STATE_SLOTS 1024u // draw-state ring (power of two)
#define Q3_QUEUE_DEPTH 4096u // thread backend command ring (power of two)

typedef enum q3_backend_kind {
    Q3_BACKEND_SW,
    Q3_BACKEND_NULL,
    Q3_BACKEND_THREAD,
} q3_backend_kind_t;

#if Q3_HAVE_THREAD_BACKEND
// The worker thread and its bounded single-producer/single-consumer ring,
// as the Voodoo2's: `head` counts commands submitted, `tail` commands
// RETIRED (executed to completion).  Both sides sleep on condition
// variables only when they have announced they are about to (the
// idle/waiting flags), so the steady state makes no system calls.
typedef struct q3_thread {
    q3_cmd_t *q;
    _Atomic uint64_t head, tail;
    _Atomic int worker_idle; // the worker is (about to be) asleep
    _Atomic int producer_waiting; // the producer is (about to be) asleep
    pthread_mutex_t mu;
    pthread_cond_t cv_work, cv_done;
    bool stop;
    bool started;
    pthread_t thr;
    // GS_QD3D_STATS=1: the decomposition printed at teardown.
    uint64_t n_submit, n_sync, n_sync_wait, n_room_wait, n_state_wait, n_worker_sleep;
    uint64_t wait_ns;
    double worker_cpu_s;
} q3_thread_t;

// Monotonic nanoseconds, for the stats.
static uint64_t q3_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

struct q3_raster {
    q3_backend_kind_t kind;
    q3_target_t *tgt;
    q3_state_build_fn build;
    void *ctx;
    // The draw-state ring: a slot is rewritten only once every command
    // that referenced it has retired (st_seq holds 1 + the sequence
    // number of the last such command; 0 = never used).
    q3_draw_state_t *st;
    uint64_t *st_seq;
    uint32_t st_cur;
    bool st_dirty;
#if Q3_HAVE_THREAD_BACKEND
    q3_thread_t th;
#endif
};

#if Q3_HAVE_THREAD_BACKEND
// One iteration of a busy-wait: a pause hint where the ISA has one.
static inline void q3_cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

// How long a dry worker spins before it sleeps: a frame's commands arrive
// in a burst while the card walks the control program, so a short spin
// catches the next one; between frames the worker sleeps.
#define Q3_WORKER_SPIN 4096

// The worker: drain the ring, execute in order, retire, sleep when dry.
static void *q3_worker_main(void *arg) {
    q3_raster_t *r = (q3_raster_t *)arg;
    q3_thread_t *th = &r->th;
    for (;;) {
        uint64_t t = atomic_load_explicit(&th->tail, memory_order_relaxed);
        uint64_t h = atomic_load_explicit(&th->head, memory_order_acquire);
        for (int spin = 0; h == t && spin < Q3_WORKER_SPIN; spin++) {
            q3_cpu_relax();
            h = atomic_load_explicit(&th->head, memory_order_acquire);
        }
        if (h == t) {
            th->n_worker_sleep++;
            pthread_mutex_lock(&th->mu);
            atomic_store_explicit(&th->worker_idle, 1, memory_order_seq_cst);
            // Re-check under the lock: a producer that stored head after
            // our first load either sees worker_idle and signals, or its
            // store is visible here (Dekker on head/worker_idle).
            while (atomic_load_explicit(&th->head, memory_order_seq_cst) == t && !th->stop)
                pthread_cond_wait(&th->cv_work, &th->mu);
            atomic_store_explicit(&th->worker_idle, 0, memory_order_seq_cst);
            bool stop = th->stop && atomic_load_explicit(&th->head, memory_order_seq_cst) == t;
            pthread_mutex_unlock(&th->mu);
            if (stop) {
                struct timespec ts = {0, 0}; // stays zero where the clock is unsupported (wasm)
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
                th->worker_cpu_s = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
                return NULL;
            }
            continue;
        }
        const q3_cmd_t *cmd = &th->q[t & (Q3_QUEUE_DEPTH - 1u)];
        q3_raster_execute(&r->st[cmd->state], r->tgt, cmd);
        atomic_store_explicit(&th->tail, t + 1u, memory_order_seq_cst);
        if (atomic_load_explicit(&th->producer_waiting, memory_order_seq_cst)) {
            pthread_mutex_lock(&th->mu);
            pthread_cond_broadcast(&th->cv_done);
            pthread_mutex_unlock(&th->mu);
        }
    }
}

// Block the producer until `tail` reaches at least `want`.
static void q3_thread_wait_tail(q3_thread_t *th, uint64_t want) {
    if (atomic_load_explicit(&th->tail, memory_order_acquire) >= want)
        return;
    uint64_t t0 = q3_now_ns();
    pthread_mutex_lock(&th->mu);
    atomic_store_explicit(&th->producer_waiting, 1, memory_order_seq_cst);
    while (atomic_load_explicit(&th->tail, memory_order_seq_cst) < want)
        pthread_cond_wait(&th->cv_done, &th->mu);
    atomic_store_explicit(&th->producer_waiting, 0, memory_order_seq_cst);
    pthread_mutex_unlock(&th->mu);
    th->wait_ns += q3_now_ns() - t0;
}

static bool q3_thread_start(q3_raster_t *r) {
    q3_thread_t *th = &r->th;
    th->q = (q3_cmd_t *)calloc(Q3_QUEUE_DEPTH, sizeof(q3_cmd_t));
    if (!th->q)
        return false;
    atomic_store(&th->head, 0);
    atomic_store(&th->tail, 0);
    atomic_store(&th->worker_idle, 0);
    atomic_store(&th->producer_waiting, 0);
    pthread_mutex_init(&th->mu, NULL);
    pthread_cond_init(&th->cv_work, NULL);
    pthread_cond_init(&th->cv_done, NULL);
    th->stop = false;
    if (pthread_create(&th->thr, NULL, q3_worker_main, r) != 0) {
        free(th->q);
        th->q = NULL;
        return false;
    }
    th->started = true;
    return true;
}

static void q3_thread_stop(q3_raster_t *r) {
    q3_thread_t *th = &r->th;
    if (!th->started)
        return;
    q3_thread_wait_tail(th, atomic_load(&th->head));
    pthread_mutex_lock(&th->mu);
    th->stop = true;
    pthread_cond_broadcast(&th->cv_work);
    pthread_mutex_unlock(&th->mu);
    pthread_join(th->thr, NULL);
    const char *stats = getenv("GS_QD3D_STATS");
    if (stats && *stats && *stats != '0')
        LOG(0,
            "raster=thread stats: %llu commands, %llu syncs (%llu waited), %llu room waits, %llu state waits, "
            "%.3f s waited total, %llu worker sleeps, worker cpu %.1f s",
            (unsigned long long)th->n_submit, (unsigned long long)th->n_sync, (unsigned long long)th->n_sync_wait,
            (unsigned long long)th->n_room_wait, (unsigned long long)th->n_state_wait, (double)th->wait_ns * 1e-9,
            (unsigned long long)th->n_worker_sleep, th->worker_cpu_s);
    pthread_mutex_destroy(&th->mu);
    pthread_cond_destroy(&th->cv_work);
    pthread_cond_destroy(&th->cv_done);
    free(th->q);
    th->q = NULL;
    th->started = false;
}
#endif

q3_raster_t *q3_raster_create(const char *kind, q3_target_t *tgt, q3_state_build_fn build, void *ctx) {
    q3_raster_t *r = (q3_raster_t *)calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->tgt = tgt;
    r->build = build;
    r->ctx = ctx;
    r->st = (q3_draw_state_t *)calloc(Q3_STATE_SLOTS, sizeof(q3_draw_state_t));
    r->st_seq = (uint64_t *)calloc(Q3_STATE_SLOTS, sizeof(uint64_t));
    if (!r->st || !r->st_seq) {
        free(r->st);
        free(r->st_seq);
        free(r);
        return NULL;
    }
    r->st_dirty = true;
    r->kind = Q3_BACKEND_SW;
    if (kind && strcmp(kind, "null") == 0) {
        r->kind = Q3_BACKEND_NULL;
    } else if (kind && strcmp(kind, "thread") == 0) {
#if Q3_HAVE_THREAD_BACKEND
        if (q3_thread_start(r))
            r->kind = Q3_BACKEND_THREAD;
        else
            LOG(0, "raster=thread: could not start the worker thread -- using the synchronous walker");
#else
        LOG(1, "raster=thread: this build compiled the thread backend out (GS_QD3D_THREAD_BACKEND=0) -- using the "
               "synchronous walker");
#endif
    } else if (kind && strcmp(kind, "sw") != 0) {
        LOG(0, "unknown raster backend '%s' -- using the synchronous walker", kind);
    }
    return r;
}

void q3_raster_destroy(q3_raster_t *r) {
    if (!r)
        return;
#if Q3_HAVE_THREAD_BACKEND
    if (r->kind == Q3_BACKEND_THREAD)
        q3_thread_stop(r);
#endif
    free(r->st);
    free(r->st_seq);
    free(r);
}

const char *q3_raster_name(const q3_raster_t *r) {
    switch (r->kind) {
    case Q3_BACKEND_NULL:
        return "null";
    case Q3_BACKEND_THREAD:
        return "thread";
    default:
        return "sw";
    }
}

void q3_raster_state_dirty(q3_raster_t *r) {
    r->st_dirty = true;
}

void q3_raster_sync(q3_raster_t *r) {
#if Q3_HAVE_THREAD_BACKEND
    if (r->kind == Q3_BACKEND_THREAD) {
        uint64_t h = atomic_load_explicit(&r->th.head, memory_order_relaxed);
        r->th.n_sync++;
        if (atomic_load_explicit(&r->th.tail, memory_order_acquire) < h)
            r->th.n_sync_wait++;
        q3_thread_wait_tail(&r->th, h);
    }
#else
    (void)r;
#endif
}

void q3_raster_submit(q3_raster_t *r, q3_cmd_t *cmd) {
    uint64_t seq = 0;
#if Q3_HAVE_THREAD_BACKEND
    if (r->kind == Q3_BACKEND_THREAD)
        seq = atomic_load_explicit(&r->th.head, memory_order_relaxed);
#endif
    if (r->st_dirty) {
        // Take the next ring slot -- after everything that read it has
        // retired -- and let the producer fill it from the live registers.
        uint32_t next = (r->st_cur + 1u) & (Q3_STATE_SLOTS - 1u);
#if Q3_HAVE_THREAD_BACKEND
        if (r->kind == Q3_BACKEND_THREAD && r->st_seq[next]) {
            if (atomic_load_explicit(&r->th.tail, memory_order_acquire) < r->st_seq[next])
                r->th.n_state_wait++;
            q3_thread_wait_tail(&r->th, r->st_seq[next]);
        }
#endif
        r->build(r->ctx, &r->st[next]);
        r->st_cur = next;
        r->st_dirty = false;
    }
    cmd->state = (uint16_t)r->st_cur;
    r->st_seq[r->st_cur] = seq + 1u;
    switch (r->kind) {
    case Q3_BACKEND_NULL:
        if (cmd->kind == Q3_CMD_OBJECT)
            return; // the null backend draws nothing: every frame is the background
        q3_raster_execute(&r->st[r->st_cur], r->tgt, cmd);
        return;
    case Q3_BACKEND_SW:
        q3_raster_execute(&r->st[r->st_cur], r->tgt, cmd);
        return;
    case Q3_BACKEND_THREAD: {
#if Q3_HAVE_THREAD_BACKEND
        q3_thread_t *th = &r->th;
        // Room: the slot we are about to write must have been retired.
        th->n_submit++;
        if (seq >= Q3_QUEUE_DEPTH) {
            if (atomic_load_explicit(&th->tail, memory_order_acquire) < seq + 1u - Q3_QUEUE_DEPTH)
                th->n_room_wait++;
            q3_thread_wait_tail(th, seq + 1u - Q3_QUEUE_DEPTH);
        }
        th->q[seq & (Q3_QUEUE_DEPTH - 1u)] = *cmd;
        atomic_store_explicit(&th->head, seq + 1u, memory_order_seq_cst);
        if (atomic_load_explicit(&th->worker_idle, memory_order_seq_cst)) {
            pthread_mutex_lock(&th->mu);
            pthread_cond_signal(&th->cv_work);
            pthread_mutex_unlock(&th->mu);
        }
#endif
        return;
    }
    }
}
