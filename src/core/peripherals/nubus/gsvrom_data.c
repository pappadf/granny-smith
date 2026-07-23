// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gsvrom_data.c
// The GS generic declaration ROM: runtime record generation + the
// embedded 68K code fragments.  The fragments are assembled by the core
// build (vrom68k.mk → build/vrom68k/gsvrom_fragments.h — generated,
// never committed); this file is the only translation unit that
// includes them.  gsvrom_generate turns a card kind's monitor table
// into a finished image via the declrom builder — the C monitors[]
// array is the single source of truth for mode geometry
// (proposal-nubus-runtime-vrom §3.3/§3.5).

#include "card.h"
#include "declrom.h"
#include "gsvrom.h"
#include "gsvrom_fragments.h"
#include "log.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("nubus");

// Per-personality fragment table, indexed by [personality][kind].
static const struct {
    const uint8_t *bytes;
    size_t size;
} s_frags[4][3] = {
    [GSVROM_JMFB] = {{gsvrom_frag_jmfb_init, sizeof(gsvrom_frag_jmfb_init)},
                     {gsvrom_frag_jmfb_drvr, sizeof(gsvrom_frag_jmfb_drvr)},
                     {NULL, 0}                                                 },
    [GSVROM_BOOGIE] = {{gsvrom_frag_boogie_init, sizeof(gsvrom_frag_boogie_init)},
                     {gsvrom_frag_boogie_drvr, sizeof(gsvrom_frag_boogie_drvr)},
                     {NULL, 0}                                                 },
    [GSVROM_MDCGC] = {{gsvrom_frag_mdcgc_init, sizeof(gsvrom_frag_mdcgc_init)},
                     {gsvrom_frag_mdcgc_drvr, sizeof(gsvrom_frag_mdcgc_drvr)},
                     {gsvrom_frag_mdcgc_sinit, sizeof(gsvrom_frag_mdcgc_sinit)}},
    [GSVROM_SE30] = {{gsvrom_frag_se30_init, sizeof(gsvrom_frag_se30_init)},
                     {gsvrom_frag_se30_drvr, sizeof(gsvrom_frag_se30_drvr)},
                     {NULL, 0}                                                 },
};

const uint8_t *gsvrom_frag(gsvrom_personality_t p, gsvrom_frag_kind_t k, size_t *out_size) {
    if ((size_t)p >= 4 || (size_t)k >= 3)
        return NULL;
    if (out_size)
        *out_size = s_frags[p][k].size;
    return s_frags[p][k].bytes;
}

// === Per-personality identity + mode rules ==================================
// The values the assembled images carried (tools/vrom history): board
// identity per proposal-generic-nubus-vrom §5.2 (byte-authentic where
// Mac-side software matches on them), video sResource names the OS
// pattern-matches, and each card's framebuffer window geometry.

typedef struct {
    const char *board_name; // board sRsrcName
    uint16_t board_id; // BoardId (load-bearing, §3.5)
    const char *rev_level; // VendorInfo RevLevel string
    const char *vid_name; // functional sResource name
    uint16_t drhw; // sRsrcType DrHW
    uint32_t fb_minor; // MinorBaseOS (std-slot framebuffer offset)
} gsvrom_traits_t;

// The GC accelerator's host driver version-gates on this exact string
// ("MDC 8<bullet>24 GC 1.1" with the MacRoman bullet $A5).
static const char gc_rev_level[] = "MDC 8\xA5"
                                   "24 GC 1.1";

static const gsvrom_traits_t s_traits[4] = {
    [GSVROM_JMFB] = {"GS Generic Display (8*24)",    0x0027, "1.0",        "Display_Video_Apple_MDC",    0x0019, 0xA00},
    [GSVROM_BOOGIE] = {"GS Generic Display (24AC)",    0x05FA, "1.0",        "Display_Video_Apple_Boogie", 0x002B, 0    },
    [GSVROM_MDCGC] = {"GS Generic Display (8*24 GC)", 0x002C, gc_rev_level, "Display_Video_Apple_MDCGC",  0x001D, 0xA00},
    [GSVROM_SE30] = {"GS Generic Display (SE/30)",   0x000C, "1.0",        "Display_Video_Apple_SE30",   0x0009, 0    },
};

// GC address-space constants (ops_mdcgc.s equivalents).
#define GC_FB_OFFSET   0x11400u // framebuffer offset within card DRAM
#define GC_DRAM_SIZE   0x200000u // 2 MB card DRAM
#define GC_ROW_BYTES   1024u // fixed row pitch at every indexed depth
#define GC_DEFER_SPID  0xA0u // the 32-bit (super-slot) sResource family
#define SE30_FB_OFFSET 0x8040u // primary buffer offset (vpBaseOffset)

// Fill one declrom_vidmode_t for `bpp` under this personality's rules.
static declrom_vidmode_t make_mode(gsvrom_personality_t p, const nubus_monitor_t *m, int bpp) {
    declrom_vidmode_t v = {
        .base_offset = (p == GSVROM_SE30) ? SE30_FB_OFFSET : 0,
        .row_bytes = (p == GSVROM_MDCGC) ? GC_ROW_BYTES : (uint16_t)(m->width * (uint32_t)bpp / 8),
        .width = (uint16_t)m->width,
        .height = (uint16_t)m->height,
        .pixel_type = 0, // chunky indexed
        .pixel_size = (uint16_t)bpp,
        .cmp_count = 1,
        .cmp_size = (uint16_t)bpp,
        .dev_type = 0, // settable CLUT
        .page_count = 1,
    };
    if (p == GSVROM_BOOGIE && bpp >= 16) {
        // The 24AC's direct-RGB depths: 16 bpp = 3×5, 32 bpp = 3×8.
        v.pixel_type = 0x10; // chunky direct
        v.cmp_count = 3;
        v.cmp_size = (bpp == 16) ? 5 : 8;
        v.dev_type = 2; // direct
    }
    return v;
}

// The window length the sResource declares (the assembled images'
// VS*Len values): bytes at the deepest declared depth.
static uint32_t window_length(gsvrom_personality_t p, const nubus_monitor_t *m) {
    switch (p) {
    case GSVROM_JMFB:
        return m->width * m->height; // 8 bpp max
    case GSVROM_BOOGIE:
        return m->width * m->height * 4; // 32 bpp max
    case GSVROM_MDCGC:
        return GC_ROW_BYTES * m->height; // fixed-pitch boot window
    case GSVROM_SE30:
        return SE30_FB_OFFSET + (m->width / 8) * m->height; // through FB end
    }
    return 0;
}

declrom_builder_t *gsvrom_generate(gsvrom_personality_t p, const nubus_monitor_t *monitors) {
    if ((size_t)p >= 4 || !monitors)
        return NULL;
    const gsvrom_traits_t *t = &s_traits[p];
    declrom_builder_t *b = declrom_builder_new(0x8000);
    if (!b)
        return NULL;

    // Board identity + vendor strings + slot-PRAM defaults (savedMode =
    // $80, 1 bpp; PrimaryInit fills the sister ids once sense is known).
    static const uint8_t pram_init[6] = {0x80, 0, 0, 0, 0, 0};
    bool ok = declrom_set_board(b, t->board_name, t->board_id);
    declrom_set_vendor(b, "granny-smith", t->rev_level, "gsvrom");
    declrom_set_pram_init(b, pram_init);

    // Splice the personality's code fragments.
    size_t n = 0;
    const uint8_t *frag = gsvrom_frag(p, GSVROM_FRAG_INIT, &n);
    ok = ok && frag && declrom_add_exec(b, DECLROM_PRIMARY_INIT, frag, n);
    frag = gsvrom_frag(p, GSVROM_FRAG_DRVR, &n);
    ok = ok && frag && declrom_add_drvr(b, frag, n);
    frag = gsvrom_frag(p, GSVROM_FRAG_SINIT, &n);
    if (frag)
        ok = ok && declrom_add_exec(b, DECLROM_SECONDARY_INIT, frag, n);

    // One functional video sResource per monitor, records straight from
    // the monitors[] row: spID from srsrc_sister, modes from depths[].
    for (const nubus_monitor_t *m = monitors; ok && m->id; m++) {
        declrom_vidmode_t modes[8];
        size_t nmodes = 0;
        for (const int *d = m->depths; d && *d && nmodes < 8; d++)
            modes[nmodes++] = make_mode(p, m, *d);
        declrom_vidsrsrc_t v = {
            .name = t->vid_name,
            .drhw = t->drhw,
            .flags = 2, // fOpenAtStart
            .use_major = false,
            .base = t->fb_minor,
            .length = window_length(p, m),
            .modes = modes,
            .mode_count = nmodes,
        };
        ok = ok && declrom_add_video_srsrc(b, m->srsrc_sister, &v);
        if (ok && p == GSVROM_MDCGC) {
            // The GC's 32-bit sister family: same modes, framebuffer in
            // super-slot DRAM (gsvrom.s VSA0GC).
            v.flags = 2 | 4; // fOpenAtStart + f32BitMode
            v.use_major = true;
            v.base = 0xC000000 + GC_FB_OFFSET;
            v.length = GC_DRAM_SIZE - GC_FB_OFFSET;
            ok = declrom_add_video_srsrc(b, GC_DEFER_SPID, &v);
        }
    }

    if (!ok || !declrom_finalise(b, 0x0F)) {
        LOG(0, "gsvrom: generation failed for personality %d", (int)p);
        declrom_builder_free(b);
        return NULL;
    }
    return b;
}
