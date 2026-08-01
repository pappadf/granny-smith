// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vdc.c
// SAA7191B DMSD + SAA7186 VDC models and the video-in frame engine — see
// vdc.h.  Register semantics from video-in.md §3/§4 (datasheet-verified);
// the frame engine implements §9.2 "to actually implement capture":
//
//   * once per NTSC field (59.94 Hz), when VDCClk == 0 (clock on),
//     BusSize == 0 (32-bit mode) and VDC $00 VPE == 1 (VRAM port enabled),
//     blit one field of the host frame into VRAM at $50200800 in the format
//     the VDC registers request, then latch CIVIC's VDC field interrupt
//   * geometry is the VDC window/decimation registers: XO/XS/XD, YO/YS/YD
//     (nearest-neighbour sampling stands in for the chip's decimation
//     filters — AFS/HF/VP filter selection is accepted but not modelled)
//   * formats: FS=00 → 1-5-5-5 ARGB two-per-longword with α = 0 (Apple
//     ships with the chroma keyer disabled — video-in.md §4.6); FS=11 →
//     8-bit greyscale with the $10 MCT polarity.  YUV formats (FS=01/10)
//     and the VBI bypass region are out of scope and logged.
//   * the writer always writes when the engine runs, even with no source
//     (black fields): Enabler 088's liveness probe stamps $0001FEFF at
//     $50200804 and fails if it survives a field (video-in.md §5.8)
//
// The window start registers are programmed relative to the DMSD raster:
// the NTSC active picture is (30, 16, 510, 656), so XO = 16 / YO = 15
// (field lines) address the top-left of the 640x480 host frame
// (video-in.md §7.5).

#include "vdc.h"

#include "av.h"
#include "civic.h"

#include "debug.h"
#include "log.h"
#include "machine_profile.h"
#include "object.h"
#include "scheduler.h"
#include "system.h"
#include "value.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("vdc");

// NTSC field cadence: 59.94 Hz.
#define AV_VDC_FIELD_NS 16683350.0

// DMSD ($8A) has 25 write registers, the VDC ($B8) 17 (video-in.md §2.4).
#define DMSD_REGS 25
#define VDC_REGS  17

// NTSC active-picture origin in DMSD raster coordinates: left = 16 pixels,
// top = 30 raster lines = 15 field lines (video-in.md §7.5, §4.4).
#define ACTIVE_LEFT      16
#define ACTIVE_TOP_FIELD 15

// Host video source selection (machine.videoin.source).
typedef enum {
    VDC_SRC_NONE = 0, // nothing plugged in (default; DMSD reports no lock)
    VDC_SRC_PATTERN, // deterministic colour bars + frame counter strip
    VDC_SRC_FILE, // a PNG loaded via machine.videoin.load
    VDC_SRC_HOST, // the platform webcam through the gs_video_in_* seam
} vdc_src_t;

struct av_vdc {
    // --- plain data (checkpointed up to the first pointer field) ---
    uint8_t dmsd[DMSD_REGS]; // SAA7191B register file ($00-$18)
    uint8_t vdc[VDC_REGS]; // SAA7186 register file ($00-$10)
    uint8_t src_mode; // vdc_src_t
    uint8_t field_parity; // OEF of the most recent field (status $B9 bit 1)
    uint8_t warned_fs; // FS value already warned about (edge-triggered logging)
    bool clock_on; // tracked VDCClk gate (for host lifecycle pushes)
    uint64_t src_seq; // source frame clock (drives the pattern generator)
    uint64_t fields; // fields actually written since power-on

    // --- pointers (not checkpointed) ---
    config_t *cfg;
    uint8_t *frame; // 640x480 RGBA staging buffer for the current field
    uint8_t *file_frame; // the loaded PNG frame (VDC_SRC_FILE); checkpointed when active
    struct object *object; // the machine.videoin node
};

extern const class_desc_t videoin_class;

static void vdc_field_event(void *source, uint64_t data);

static inline av_civic_t *vdc_civic(av_vdc_t *vdc) {
    av_state_t *st = (av_state_t *)vdc->cfg->machine_context;
    return st ? st->civic : NULL;
}

// ============================================================
// I2C register files + status bytes
// ============================================================

bool av_vdc_i2c_slave_known(uint8_t slave) {
    return slave == 0x8A || slave == 0x8B || slave == 0xB8 || slave == 0xB9;
}

// True when the guest-visible source reports a signal (DMSD HLCK).
bool av_vdc_connected(av_vdc_t *vdc) {
    switch (vdc->src_mode) {
    case VDC_SRC_PATTERN:
    case VDC_SRC_FILE:
        return true;
    case VDC_SRC_HOST:
        return gs_video_in_connected();
    default:
        return false;
    }
}

int av_vdc_set_source(av_vdc_t *vdc, const char *name) {
    if (strcmp(name, "none") == 0)
        vdc->src_mode = VDC_SRC_NONE;
    else if (strcmp(name, "pattern") == 0)
        vdc->src_mode = VDC_SRC_PATTERN;
    else if (strcmp(name, "file") == 0)
        vdc->src_mode = vdc->file_frame ? VDC_SRC_FILE : VDC_SRC_NONE;
    else if (strcmp(name, "host") == 0)
        vdc->src_mode = VDC_SRC_HOST;
    else
        return -1;
    return 0;
}

// DMSD status byte ($8B; video-in.md §3.1): STTC mirrors the programmed
// VTRC time constant; a connected source reads "locked, 60 Hz, colour"
// (HLCK = 0, FIDT = 1, CODE = 1), a missing one "PLL unlocked" ($40).
static uint8_t vdc_dmsd_status(av_vdc_t *vdc) {
    uint8_t sttc = (uint8_t)(vdc->dmsd[0x0D] & 0x80);
    return (uint8_t)(sttc | (av_vdc_connected(vdc) ? 0x21 : 0x40));
}

// VDC status byte ($B9; video-in.md §4.7): version ID nibble MUST read
// %0001, OEF is the detected field parity, SVP mirrors VPE taking effect.
static uint8_t vdc_vdc_status(av_vdc_t *vdc) {
    uint8_t svp = (uint8_t)((vdc->vdc[0x00] >> 4) & 1);
    return (uint8_t)(0x10 | ((vdc->field_parity & 1) << 1) | svp);
}

void av_vdc_i2c_write(av_vdc_t *vdc, uint8_t slave, const uint8_t *data, int len) {
    if (!vdc || len < 1)
        return; // no subaddress byte — nothing to latch
    uint8_t *regs = (slave == 0x8A) ? vdc->dmsd : vdc->vdc;
    int nregs = (slave == 0x8A) ? DMSD_REGS : VDC_REGS;
    uint8_t sub = data[0];
    // Subaddress auto-increment, exactly the chips' multi-byte write rule.
    for (int i = 1; i < len; i++) {
        int r = sub + (i - 1);
        if (r >= nregs) {
            LOG(1, "I2C write past $%02X register file (sub $%02X + %d) ignored", slave, sub, i - 1);
            break;
        }
        regs[r] = data[i];
        LOG(3, "%s[$%02X] = $%02X", slave == 0x8A ? "DMSD" : "VDC", r, data[i]);
    }
}

int av_vdc_i2c_read(av_vdc_t *vdc, uint8_t slave, bool has_sub, uint8_t sub, uint8_t *out, int maxlen) {
    if (!vdc || maxlen < 1)
        return 0;
    if (!has_sub) {
        // The status registers — the only reads real hardware ever serves
        // (shadowed register reads normally never reach the wire).
        out[0] = (slave == 0x8B) ? vdc_dmsd_status(vdc) : vdc_vdc_status(vdc);
        LOG(3, "%s status read = $%02X", slave == 0x8B ? "DMSD" : "VDC", out[0]);
        return 1;
    }
    // Subaddressed read: serve the register file from `sub` up (the Enabler
    // 088 'i2c ' build disables its shadow cache, so these DO reach us).
    const uint8_t *regs = (slave == 0x8B) ? vdc->dmsd : vdc->vdc;
    int nregs = (slave == 0x8B) ? DMSD_REGS : VDC_REGS;
    int n = 0;
    for (int r = sub; r < nregs && n < maxlen; r++)
        out[n++] = regs[r];
    return n;
}

// ============================================================
// Host frame sources
// ============================================================

// Deterministic test pattern: eight full-height colour bars, a scrolling
// gradient band, and a 32-bit binary frame-counter strip along the bottom.
// A pure function of `seq`, so goldens are byte-exact across runs and
// checkpoint/restore.
static void vdc_pattern_frame(uint8_t *rgba, uint64_t seq) {
    static const uint8_t bars[8][3] = {
        {255, 255, 255},
        {255, 255, 0  },
        {0,   255, 255},
        {0,   255, 0  },
        {255, 0,   255},
        {255, 0,   0  },
        {0,   0,   255},
        {0,   0,   0  },
    };
    for (int y = 0; y < AV_VDC_SRC_H; y++) {
        uint8_t *row = rgba + (size_t)y * AV_VDC_SRC_W * 4;
        for (int x = 0; x < AV_VDC_SRC_W; x++) {
            uint8_t r, g, b;
            if (y < 384) { // colour bars
                const uint8_t *c = bars[(x * 8) / AV_VDC_SRC_W];
                r = c[0];
                g = c[1];
                b = c[2];
            } else if (y < 448) { // gradient band scrolling with the frame clock
                uint8_t v = (uint8_t)((x + seq * 4) & 0xFF);
                r = g = b = v;
            } else { // frame counter: bit 31..0 of seq, MSB leftmost
                int bit = 31 - (x * 32) / AV_VDC_SRC_W;
                uint8_t v = ((seq >> bit) & 1) ? 255 : 0;
                r = g = b = v;
            }
            row[x * 4 + 0] = r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = b;
            row[x * 4 + 3] = 255;
        }
    }
}

// Fill the staging buffer with the current source frame; black when the
// source has nothing (the engine must keep writing — vdc.h header).
static void vdc_fill_frame(av_vdc_t *vdc) {
    switch (vdc->src_mode) {
    case VDC_SRC_PATTERN:
        vdc_pattern_frame(vdc->frame, vdc->src_seq);
        break;
    case VDC_SRC_FILE:
        if (vdc->file_frame) {
            memcpy(vdc->frame, vdc->file_frame, (size_t)AV_VDC_SRC_W * AV_VDC_SRC_H * 4);
            break;
        }
        memset(vdc->frame, 0, (size_t)AV_VDC_SRC_W * AV_VDC_SRC_H * 4);
        break;
    case VDC_SRC_HOST:
        if (gs_video_in_frame(vdc->frame) == 0)
            break;
        memset(vdc->frame, 0, (size_t)AV_VDC_SRC_W * AV_VDC_SRC_H * 4);
        break;
    default:
        memset(vdc->frame, 0, (size_t)AV_VDC_SRC_W * AV_VDC_SRC_H * 4);
        break;
    }
}

// ============================================================
// The frame engine
// ============================================================

// Assemble the 9/10-bit window parameters from their register spread
// (video-in.md §4.2: low 8 bits in the base register, overflow in $04/$08).
typedef struct {
    int xd, xs, xo; // output pixels, input pixels, window start
    int yd, ys, yo; // output lines, input lines, window start (field lines)
} vdc_window_t;

static vdc_window_t vdc_window(const av_vdc_t *vdc) {
    const uint8_t *r = vdc->vdc;
    vdc_window_t w;
    w.xd = r[0x01] | ((r[0x04] & 0x03) << 8);
    w.xs = r[0x02] | ((r[0x04] & 0x0C) << 6);
    w.xo = r[0x03] | ((r[0x04] & 0x10) << 4);
    w.yd = r[0x05] | ((r[0x08] & 0x03) << 8);
    w.ys = r[0x06] | ((r[0x08] & 0x0C) << 6);
    w.yo = r[0x07] | ((r[0x08] & 0x10) << 4);
    return w;
}

// Write one field of the staged host frame into VRAM per the VDC registers.
// `parity` is the field being delivered (0 = even).
static void vdc_write_field(av_vdc_t *vdc, av_civic_t *cv, int parity) {
    vdc_window_t w = vdc_window(vdc);
    uint8_t fs = (uint8_t)(vdc->vdc[0x00] & 0x03);
    uint8_t of = (uint8_t)((vdc->vdc[0x00] >> 5) & 0x03);
    bool mct = (vdc->vdc[0x10] & 0x10) != 0;
    uint32_t stride = av_civic_vidin_stride_big(cv) ? 1536u : 1024u;
    uint8_t *vram = av_civic_vram(cv);

    if (w.xd <= 0 || w.yd <= 0 || w.xs <= 0 || w.ys <= 0)
        return; // no window programmed — nothing to emit
    if (fs == 1 || fs == 2) {
        // Edge-triggered: a sustained YUV capture would otherwise log 60x/s.
        if (vdc->warned_fs != fs) {
            vdc->warned_fs = fs;
            LOG(1, "FS=%d (YUV) output format not modelled — fields dropped until it changes", fs);
        }
        return;
    }
    vdc->warned_fs = 0;
    if ((vdc->vdc[0x0A] | (vdc->vdc[0x0B] & 0x04)) != 0)
        LOG(4, "VBI bypass region programmed (VC != 0) — not modelled"); // per-field; quiet by default

    int bpp = (fs == 0) ? 2 : 1;
    for (int j = 0; j < w.yd; j++) {
        // Interlaced storage (OF=00) lands field lines on alternate VRAM
        // rows; single-field modes (OF=1x) and non-interlaced (01) pack
        // them consecutively (video-in.md §4.1).
        uint32_t row = (of == 0) ? (uint32_t)(j * 2 + parity) : (uint32_t)j;
        uint32_t off = AV_VDC_VRAM_OFFSET + row * stride;
        if (off + (uint32_t)(w.xd * bpp) > AV_CIVIC_VRAM_SIZE)
            break; // never run off the end of the aperture
        uint8_t *dst = vram + off;

        // Source line: window start + decimated position, in field lines,
        // then de-fielded into the 640x480 host frame.
        int fline = (w.yo - ACTIVE_TOP_FIELD) + (j * w.ys) / w.yd;
        int sline = fline * 2 + parity;
        if (sline < 0)
            sline = 0;
        if (sline >= AV_VDC_SRC_H)
            sline = AV_VDC_SRC_H - 1;
        const uint8_t *src = vdc->frame + (size_t)sline * AV_VDC_SRC_W * 4;

        for (int i = 0; i < w.xd; i++) {
            int sx = (w.xo - ACTIVE_LEFT) + (i * w.xs) / w.xd;
            if (sx < 0)
                sx = 0;
            if (sx >= AV_VDC_SRC_W)
                sx = AV_VDC_SRC_W - 1;
            const uint8_t *p = src + sx * 4;
            if (fs == 0) {
                // 1-5-5-5 ARGB, α = 0 (keyer disabled is Apple's shipping
                // state — video-in.md §4.6), 8→5 bit truncation, guest
                // big-endian byte order.
                uint16_t px = (uint16_t)(((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3));
                dst[i * 2 + 0] = (uint8_t)(px >> 8);
                dst[i * 2 + 1] = (uint8_t)px;
            } else {
                // FS=11: 8-bit luminance; MCT = 0 selects the inverse ramp.
                uint8_t y = (uint8_t)((77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8);
                dst[i] = mct ? y : (uint8_t)(255 - y);
            }
        }
    }
}

// The field event: runs at NTSC field cadence for the machine's lifetime;
// the CIVIC gates + VPE decide whether a field is actually delivered.
static void vdc_field_event(void *source, uint64_t data) {
    (void)data;
    av_vdc_t *vdc = (av_vdc_t *)source;
    av_civic_t *cv = vdc_civic(vdc);

    vdc->src_seq++; // the source's own frame clock keeps running

    bool vpe = (vdc->vdc[0x00] & 0x10) != 0;
    if (cv && vpe && !av_civic_vidin_clock_off(cv) && !av_civic_bus64(cv)) {
        uint8_t of = (uint8_t)((vdc->vdc[0x00] >> 5) & 0x03);
        // Field parity: alternating for both-fields modes, pinned for the
        // single-field modes (10 = odd only, 11 = even only — §4.2).
        int parity;
        if (of == 2)
            parity = 1;
        else if (of == 3)
            parity = 0;
        else
            parity = (int)(vdc->fields & 1);
        vdc_fill_frame(vdc);
        vdc_write_field(vdc, cv, parity);
        vdc->field_parity = (uint8_t)parity;
        vdc->fields++;
        av_civic_vdc_field(cv); // one interrupt per field
    }

    scheduler_new_cpu_event(vdc->cfg->scheduler, &vdc_field_event, vdc, 0, 0, (uint64_t)AV_VDC_FIELD_NS);
}

void av_vdc_clock_gate(av_vdc_t *vdc, bool clock_off) {
    if (!vdc)
        return;
    bool on = !clock_off;
    if (on == vdc->clock_on)
        return;
    vdc->clock_on = on;
    LOG(2, "VDC clock %s", on ? "on (capture running)" : "off");
    // Camera lifecycle: the host attaches/stops its capture with the guest.
    gs_video_in_state(on);
}

// ============================================================
// machine.videoin — the host source surface
// ============================================================

static const char *vdc_src_name(uint8_t mode) {
    switch (mode) {
    case VDC_SRC_PATTERN:
        return "pattern";
    case VDC_SRC_FILE:
        return "file";
    case VDC_SRC_HOST:
        return "host";
    default:
        return "none";
    }
}

static inline av_vdc_t *vdc_self(struct object *self) {
    return (av_vdc_t *)object_data(self);
}

static value_t videoin_attr_source_get(struct object *self, const member_t *m) {
    (void)m;
    av_vdc_t *vdc = vdc_self(self);
    return val_str(vdc ? vdc_src_name(vdc->src_mode) : "none");
}

static value_t videoin_attr_source_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    av_vdc_t *vdc = vdc_self(self);
    if (!vdc) {
        value_free(&in);
        return val_err("videoin not available");
    }
    if (av_vdc_set_source(vdc, in.s) < 0) {
        value_t e = val_err("videoin.source: want none|pattern|file|host, got '%s'", in.s);
        value_free(&in);
        return e;
    }
    value_free(&in);
    return val_none();
}

static value_t videoin_attr_connected(struct object *self, const member_t *m) {
    (void)m;
    av_vdc_t *vdc = vdc_self(self);
    return val_bool(vdc ? av_vdc_connected(vdc) : false);
}

static value_t videoin_attr_fields(struct object *self, const member_t *m) {
    (void)m;
    av_vdc_t *vdc = vdc_self(self);
    return val_uint(8, vdc ? vdc->fields : 0);
}

static value_t videoin_method_pattern(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    av_vdc_t *vdc = vdc_self(self);
    if (!vdc)
        return val_err("videoin not available");
    vdc->src_mode = VDC_SRC_PATTERN;
    return val_none();
}

static value_t videoin_method_load(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    av_vdc_t *vdc = vdc_self(self);
    if (!vdc || argc < 1)
        return val_err("videoin not available");
    if (!vdc->file_frame) {
        vdc->file_frame = malloc((size_t)AV_VDC_SRC_W * AV_VDC_SRC_H * 4);
        if (!vdc->file_frame)
            return val_err("videoin.load: out of memory");
    }
    if (debug_load_png_rgba(argv[0].s, AV_VDC_SRC_W, AV_VDC_SRC_H, vdc->file_frame) < 0) {
        free(vdc->file_frame);
        vdc->file_frame = NULL;
        return val_err("videoin.load: cannot load '%s' as a %dx%d PNG", argv[0].s, AV_VDC_SRC_W, AV_VDC_SRC_H);
    }
    vdc->src_mode = VDC_SRC_FILE;
    return val_none();
}

static const arg_decl_t videoin_load_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "640x480 PNG to use as the video source frame"},
};

static const member_t videoin_members[] = {
    {.kind = M_ATTR,
     .name = "source",
     .doc = "Host video source: none | pattern | file | host (webcam)",
     .attr = {.type = V_STRING, .get = videoin_attr_source_get, .set = videoin_attr_source_set}},
    {.kind = M_ATTR,
     .name = "connected",
     .doc = "True when the source reports a signal (drives the DMSD lock status)",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = videoin_attr_connected, .set = NULL}},
    {.kind = M_ATTR,
     .name = "fields",
     .doc = "Fields the capture engine has written since power-on",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = videoin_attr_fields, .set = NULL}},
    {.kind = M_METHOD,
     .name = "pattern",
     .doc = "Select the built-in deterministic test pattern as the source",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = videoin_method_pattern}},
    {.kind = M_METHOD,
     .name = "load",
     .doc = "Load a 640x480 PNG and select it as the source frame",
     .method = {.args = videoin_load_args, .nargs = 1, .result = V_NONE, .fn = videoin_method_load}},
};

const class_desc_t videoin_class = {
    .name = "videoin",
    .members = videoin_members,
    .n_members = sizeof(videoin_members) / sizeof(videoin_members[0]),
};

// ============================================================
// Lifecycle
// ============================================================

av_vdc_t *av_vdc_init(config_t *cfg, checkpoint_t *cp) {
    av_vdc_t *vdc = calloc(1, sizeof(*vdc));
    if (!vdc)
        return NULL;
    vdc->cfg = cfg;
    vdc->frame = calloc(1, (size_t)AV_VDC_SRC_W * AV_VDC_SRC_H * 4);
    if (!vdc->frame) {
        free(vdc);
        return NULL;
    }

    if (cp) {
        size_t data_size = offsetof(av_vdc_t, cfg);
        system_read_checkpoint_data(cp, vdc, data_size);
        if (vdc->src_mode == VDC_SRC_FILE) {
            vdc->file_frame = malloc((size_t)AV_VDC_SRC_W * AV_VDC_SRC_H * 4);
            if (vdc->file_frame)
                system_read_checkpoint_data(cp, vdc->file_frame, (size_t)AV_VDC_SRC_W * AV_VDC_SRC_H * 4);
        }
    }

    scheduler_new_event_type(cfg->scheduler, "vdc", vdc, "field", &vdc_field_event);
    scheduler_new_cpu_event(cfg->scheduler, &vdc_field_event, vdc, 0, 0, (uint64_t)AV_VDC_FIELD_NS);

    vdc->object = object_new(&videoin_class, vdc, "videoin");
    if (vdc->object) {
        object_set_label(vdc->object, "Video In");
        object_set_order(vdc->object, 125);
        object_attach(machine_object(), vdc->object);
    }

    LOG(1, "VDC init (SAA7191B + SAA7186, source %s)", vdc_src_name(vdc->src_mode));
    return vdc;
}

void av_vdc_delete(av_vdc_t *vdc) {
    if (!vdc)
        return;
    if (vdc->object) {
        object_detach(vdc->object);
        object_delete(vdc->object);
    }
    if (vdc->cfg && vdc->cfg->scheduler)
        remove_event(vdc->cfg->scheduler, &vdc_field_event, vdc);
    free(vdc->file_frame);
    free(vdc->frame);
    free(vdc);
}

void av_vdc_checkpoint(av_vdc_t *vdc, checkpoint_t *cp) {
    if (!vdc || !cp)
        return;
    size_t data_size = offsetof(av_vdc_t, cfg);
    system_write_checkpoint_data(cp, vdc, data_size);
    // The loaded source frame is state (a restore must keep producing the
    // same pixels); the webcam is host-ephemeral and outside scope.
    if (vdc->src_mode == VDC_SRC_FILE && vdc->file_frame)
        system_write_checkpoint_data(cp, vdc->file_frame, (size_t)AV_VDC_SRC_W * AV_VDC_SRC_H * 4);
}
