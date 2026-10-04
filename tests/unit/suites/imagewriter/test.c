// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The ImageWriter interpreter, sheet raster and PDF writer, driven directly:
// byte strings in, parser state and dots out.  Positions are checked to the
// pixel, because the point of exact positioning is that a column lands where
// the arithmetic says.

#include "inflate.h"
#include "iw_interp.h"
#include "pdf_writer.h"
#include "test_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Fixture ---------------------------------------------------------------------

static iw_config_t g_cfg;
static iw_interp_t g_in;
static int g_pages; // sheets handed over
static uint32_t g_last_dots; // dots on the last sheet handed over
static char g_reply[32];
static size_t g_reply_len;

static void on_page(void *ctx, const iw_page_t *page) {
    (void)ctx;
    g_pages++;
    g_last_dots = page->dots;
}

static void on_reply(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    for (size_t i = 0; i < len && g_reply_len < sizeof(g_reply) - 1; i++)
        g_reply[g_reply_len++] = (char)data[i];
    g_reply[g_reply_len] = '\0';
}

// An ImageWriter II with a colour ribbon on 8.5 x 11 fanfold, column 0 at the
// sheet's left edge and top of form at its top edge, square dots: positions
// map to pixels with nothing in between.
static void setup(iw_model_t model) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.model = model;
    g_cfg.dip1 = 0x20;
    g_cfg.dip2 = 0x03;
    g_cfg.color_ribbon = model == IW_MODEL_IW2;
    g_cfg.paper_width_in = 8.5;
    g_cfg.paper_height_in = 11.0;
    g_cfg.left_offset_in = 0.0;
    g_cfg.tof_offset_144 = 0;
    g_cfg.dpi = 288;
    g_cfg.dot_shape = IW_DOT_SQUARE;
    g_cfg.printable_width_in = 8.0;
    iw_interp_free(&g_in);
    iw_interp_hooks_t hooks = {.ctx = NULL, .page_done = on_page, .reply = on_reply};
    iw_interp_init(&g_in, &g_cfg, &hooks);
    g_pages = 0;
    g_last_dots = 0;
    g_reply_len = 0;
    g_reply[0] = '\0';
}

static void feed(const char *s, size_t n) {
    iw_interp_feed(&g_in, (const uint8_t *)s, n);
}
#define FEED(lit) feed((lit), sizeof(lit) - 1)

// The black plane's pixel (x, y) on the sheet in progress
static bool px(int x, int y) {
    const iw_page_t *p = &g_in.page;
    const uint8_t *plane = p->nplanes == 4 ? p->plane[3] : p->plane[0];
    if (!plane || x < 0 || y < 0 || (uint32_t)x >= p->w || (uint32_t)y >= p->h)
        return false;
    return (plane[(size_t)y * p->stride + (x >> 3)] >> (7 - (x & 7))) & 1;
}

// Pixel of band `bit` (IW_INK_Y/M/C)
static bool px_band(int bit, int x, int y) {
    const iw_page_t *p = &g_in.page;
    int i = bit == IW_INK_Y ? 0 : bit == IW_INK_M ? 1 : 2;
    const uint8_t *plane = p->plane[i];
    if (!plane)
        return false;
    return (plane[(size_t)y * p->stride + (x >> 3)] >> (7 - (x & 7))) & 1;
}

// --- PDF writer --------------------------------------------------------------------

// Find `needle` in `hay` (binary-safe); NULL when absent.
static const uint8_t *find(const uint8_t *hay, size_t n, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp(hay + i, needle, k) == 0)
            return hay + i;
    return NULL;
}

TEST(test_pdf_structure) {
    // A 16 x 4 pixel stencil with a diagonal
    uint8_t bits[4 * 2] = {0x80, 0x00, 0x40, 0x00, 0x20, 0x00, 0x10, 0x01};
    uint8_t *a = NULL, *b = NULL;
    size_t alen = 0, blen = 0;
    for (int round = 0; round < 2; round++) {
        pdf_writer_t *w = pdf_writer_new("Test (job)", "Granny Smith");
        ASSERT_TRUE(w);
        ASSERT_TRUE(pdf_writer_add_mono_page(w, 612, 792, 16, 4, bits, 2));
        ASSERT_TRUE(pdf_writer_add_mono_page(w, 612, 792, 16, 4, bits, 2));
        ASSERT_EQ_INT(pdf_writer_pages(w), 2);
        ASSERT_TRUE(pdf_writer_finish(w, round ? &b : &a, round ? &blen : &alen));
        pdf_writer_free(w);
    }
    // Deterministic: the same pages give the same bytes
    ASSERT_TRUE(alen == blen && memcmp(a, b, alen) == 0);
    ASSERT_TRUE(memcmp(a, "%PDF-1.4", 8) == 0);
    ASSERT_TRUE(find(a, alen, "/Count 2"));
    ASSERT_TRUE(find(a, alen, "(Test \\(job\\))"));
    // startxref points at the table; every entry at its object
    const uint8_t *sx = find(a, alen, "startxref\n");
    ASSERT_TRUE(sx);
    size_t xref = strtoul((const char *)sx + 10, NULL, 10);
    ASSERT_TRUE(xref < alen && memcmp(a + xref, "xref\n0 ", 7) == 0);
    unsigned nobj = (unsigned)strtoul((const char *)a + xref + 7, NULL, 10);
    ASSERT_EQ_INT(nobj, 4 + 3 * 2);
    const char *entries = strchr((const char *)a + xref + 5, '\n') + 1;
    for (unsigned i = 1; i < nobj; i++) {
        size_t off = strtoul(entries + 20 * i, NULL, 10);
        char want[24];
        snprintf(want, sizeof(want), "%u 0 obj", i);
        ASSERT_TRUE(off < alen && memcmp(a + off, want, strlen(want)) == 0);
    }
    // The image stream inflates back to the stencil
    const uint8_t *st = find(a, alen, "stream\n");
    const uint8_t *len_at = find(a, alen, "/FlateDecode /Length ");
    ASSERT_TRUE(st && len_at);
    long zlen = strtol((const char *)len_at + 21, NULL, 10);
    uint8_t out[8];
    ASSERT_EQ_INT(inflate_zlib(st + 7, (size_t)zlen, out, sizeof(out)), 8);
    ASSERT_TRUE(memcmp(out, bits, 8) == 0);
    free(a);
    free(b);
}

TEST(test_pdf_serialise) {
    uint8_t bits[2] = {0xF0, 0x0F};
    pdf_writer_t *w = pdf_writer_new("T", "P");
    ASSERT_TRUE(pdf_writer_add_mono_page(w, 72, 72, 8, 2, bits, 1));
    uint8_t *ser = NULL;
    size_t serlen = 0;
    ASSERT_TRUE(pdf_writer_serialise(w, &ser, &serlen));
    pdf_writer_t *w2 = pdf_writer_deserialise(ser, serlen);
    ASSERT_TRUE(w2);
    ASSERT_TRUE(pdf_writer_add_mono_page(w, 72, 72, 8, 2, bits, 1));
    ASSERT_TRUE(pdf_writer_add_mono_page(w2, 72, 72, 8, 2, bits, 1));
    uint8_t *a, *b;
    size_t alen, blen;
    ASSERT_TRUE(pdf_writer_finish(w, &a, &alen));
    ASSERT_TRUE(pdf_writer_finish(w2, &b, &blen));
    // A restored writer continues exactly where the original was
    ASSERT_TRUE(alen == blen && memcmp(a, b, alen) == 0);
    // Malformed data is refused
    ser[0] ^= 0xFF;
    ASSERT_TRUE(pdf_writer_deserialise(ser, serlen) == NULL);
    pdf_writer_free(w);
    pdf_writer_free(w2);
    free(a);
    free(b);
    free(ser);
}

// --- Interpreter: settings ---------------------------------------------------------

TEST(test_power_on_defaults) {
    setup(IW_MODEL_IW2);
    // DIP 1-6 closed: elite; 6 lpi; switches per Table A-5
    ASSERT_EQ_INT(g_in.st.pitch, IW_PITCH_ELITE);
    ASSERT_EQ_INT(g_in.st.line_spacing, 24);
    ASSERT_EQ_INT(g_in.st.page_len, 1584);
    ASSERT_EQ_INT(g_in.st.soft_a, IW_SWA_NO_SELECT | IW_SWA_PRINT_LFFF);
    ASSERT_EQ_INT(g_in.st.soft_b, IW_SWB_7BIT | IW_SWB_NO_PERF);
    ASSERT_EQ_INT(g_in.st.quality, IW_QUALITY_DRAFT);
}

TEST(test_pitch_and_spacing) {
    setup(IW_MODEL_IW2);
    FEED("\x1bN");
    ASSERT_EQ_INT(g_in.st.pitch, IW_PITCH_PICA);
    FEED("\x1bP");
    ASSERT_EQ_INT(g_in.st.pitch, IW_PITCH_PROP_ELITE);
    FEED("\x1b"
         "e");
    ASSERT_EQ_INT(g_in.st.pitch, IW_PITCH_SEMICONDENSED);
    FEED("\x1bT16");
    ASSERT_EQ_INT(g_in.st.line_spacing, 16);
    FEED("\x1b"
         "B");
    ASSERT_EQ_INT(g_in.st.line_spacing, 18);
    FEED("\x1bH0792");
    ASSERT_EQ_INT(g_in.st.page_len, 792);
    // Leading spaces count as zeros
    FEED("\x1bT 9");
    ASSERT_EQ_INT(g_in.st.line_spacing, 9);
}

TEST(test_soft_switch_masks) {
    setup(IW_MODEL_IW2);
    // ESC D sets only A's 0xF7 and B's 0x25 bits
    FEED("\x1b"
         "D\xff\xff");
    ASSERT_EQ_INT(g_in.st.soft_a, 0xF7);
    ASSERT_EQ_INT(g_in.st.soft_b, 0x25);
    FEED("\x1bZ\xff\xff");
    ASSERT_EQ_INT(g_in.st.soft_a, 0x00);
    ASSERT_EQ_INT(g_in.st.soft_b, 0x00);
}

TEST(test_tabs) {
    setup(IW_MODEL_IW2);
    FEED("\x1bN\x1b(010,020,005.");
    ASSERT_EQ_INT(g_in.st.n_tabs, 3);
    ASSERT_EQ_INT(g_in.st.tabs[0], 5);
    ASSERT_EQ_INT(g_in.st.tabs[2], 20);
    FEED("\t");
    ASSERT_EQ_INT(g_in.st.head_x, 5 * (IW_UNITS_PER_INCH / 10));
    FEED("\t");
    ASSERT_EQ_INT(g_in.st.head_x, 10 * (IW_UNITS_PER_INCH / 10));
    FEED("\x1b)010.");
    ASSERT_EQ_INT(g_in.st.n_tabs, 2);
    FEED("\x1bu030");
    ASSERT_EQ_INT(g_in.st.n_tabs, 3);
    FEED("\x1b"
         "0");
    ASSERT_EQ_INT(g_in.st.n_tabs, 0);
}

// --- Interpreter: dots -------------------------------------------------------------

// One graphics column with only the top wire at dot column 100 in each pitch
// lands at round(100 * dot * 288 / U) -- including 107.2 dpi.
TEST(test_graphics_column_positions) {
    static const char *pitch_cmd[] = {"n", "N", "E", "e", "q", "Q", "p", "P"};
    static const double dpi[] = {72, 80, 96, 107.2, 120, 136, 144, 160};
    for (int i = 0; i < 8; i++) {
        setup(IW_MODEL_IW2);
        char buf[32];
        int n = snprintf(buf, sizeof(buf),
                         "\x1b%s\x1b"
                         "F0100\x1bG0001\x01\r",
                         pitch_cmd[i]);
        feed(buf, (size_t)n);
        int want = (int)(100.0 / dpi[i] * 288.0 + 0.5);
        ASSERT_TRUE(px(want, 0));
        ASSERT_TRUE(!px(want - 1, 0));
        ASSERT_TRUE(px(want + 3, 3)); // a 4 x 4 square dot
        ASSERT_TRUE(!px(want + 4, 0));
    }
}

// Pins are 1/72 in apart; a 1/144 in feed between two bands interleaves them.
TEST(test_interleave) {
    setup(IW_MODEL_IW2);
    // Wires 1 and 2, then 1/144 lower wire 1 again
    FEED("\x1bT01\x1bG0001\x03\n\x1bG0001\x01\r");
    ASSERT_TRUE(px(0, 0)); // wire 1, first band: rows 0..3
    ASSERT_TRUE(px(0, 4)); // wire 2: rows 4..7
    ASSERT_TRUE(px(0, 2 + 3)); // second band's wire 1 starts at row 2
    ASSERT_EQ_INT(g_in.st.y, 1);
}

// A graphics payload is opaque: bytes that look like ESC, CR or CAN are columns.
TEST(test_graphics_opaque) {
    setup(IW_MODEL_IW2);
    FEED("\x1bG0003\x1b\r\x18\x1bN");
    ASSERT_EQ_INT(g_in.st.n_marks, 3);
    ASSERT_EQ_INT(g_in.st.pitch, IW_PITCH_PICA); // the ESC N after the payload ran
}

// The Lisa Office System's job opening: a run of CANs flushes a graphics
// command an aborted job left incomplete, and the next command is heard.
TEST(test_can_run_recovers_truncated_graphics) {
    setup(IW_MODEL_IW2);
    FEED("\x1bG0020\x01\x01\x01");
    ASSERT_EQ_INT(g_in.st.ps, IW_PS_GRAPHICS);
    char cans[40];
    memset(cans, 0x18, sizeof(cans));
    feed(cans, sizeof(cans));
    ASSERT_EQ_INT(g_in.st.ps, IW_PS_NORMAL);
    ASSERT_EQ_INT(g_in.st.n_marks, 0); // CAN erased the staged line
    FEED("\x1bQ");
    ASSERT_EQ_INT(g_in.st.pitch, IW_PITCH_ULTRACONDENSED);
}

TEST(test_cr_lf_and_can) {
    setup(IW_MODEL_IW2);
    FEED("AB");
    ASSERT_TRUE(g_in.st.n_marks > 0);
    FEED("\x18");
    ASSERT_EQ_INT(g_in.st.n_marks, 0);
    ASSERT_EQ_INT(g_in.st.head_x, 0);
    FEED("A\r");
    ASSERT_EQ_INT(g_in.st.n_marks, 0); // printed
    ASSERT_EQ_INT(g_in.st.y, 0); // CR alone does not feed
    FEED("\n");
    ASSERT_EQ_INT(g_in.st.y, 24);
    FEED("\x1br\n");
    ASSERT_EQ_INT(g_in.st.y, 0);
    // Auto LF after CR (soft switch A-8)
    FEED("\x1b"
         "f\x1b"
         "D\x80\x00\r");
    ASSERT_EQ_INT(g_in.st.y, 24);
}

TEST(test_backspace_overprint) {
    setup(IW_MODEL_IW2);
    FEED("\x1bN"
         "A\x08_");
    // The underscore went back over the A: the head is one character in
    ASSERT_EQ_INT(g_in.st.head_x, IW_UNITS_PER_INCH / 10);
}

TEST(test_form_feed_and_pages) {
    setup(IW_MODEL_IW2);
    FEED("\x1bG0001\x01\f");
    ASSERT_EQ_INT(g_in.st.y, 1584);
    ASSERT_EQ_INT(g_pages, 0); // nothing printed on the next sheet yet
    FEED("\x1bG0001\x01\r");
    ASSERT_EQ_INT(g_pages, 1); // the first sheet went out when the second was printed on
    iw_interp_end_job(&g_in);
    ASSERT_EQ_INT(g_pages, 2);
    // A form feed at top of form advances a whole form; blank sheets make no pages
    FEED("\f\f\x1bG0001\x01\r");
    iw_interp_end_job(&g_in);
    ASSERT_EQ_INT(g_pages, 3);
}

TEST(test_reverse_clamped_at_sheet_top) {
    setup(IW_MODEL_IW2);
    FEED("\x1br\x1bT50\n");
    ASSERT_EQ_INT(g_in.st.y, 0); // cannot roll above the first sheet
}

TEST(test_perforation_skip) {
    setup(IW_MODEL_IW2);
    // Skip on (B-3 open); 6 lpi: line 60 (1440/144 in) is the first within half
    // an inch of the end of the form plus the half inch above top of form
    FEED("\x1bZ\x00\x04");
    for (int i = 0; i < 59; i++)
        FEED("\n");
    ASSERT_EQ_INT(g_in.st.y, 59 * 24);
    FEED("\n");
    // Skipped to the next top of form
    ASSERT_EQ_INT(g_in.st.y, 1584);
    // Off: line 60 is a line like any other
    setup(IW_MODEL_IW2);
    for (int i = 0; i < 60; i++)
        FEED("\n");
    ASSERT_EQ_INT(g_in.st.y, 1440);
}

TEST(test_colour) {
    setup(IW_MODEL_IW2);
    FEED("\x1bK4\x1bG0001\x01\r"); // orange: yellow and magenta
    ASSERT_TRUE(px_band(IW_INK_Y, 0, 0));
    ASSERT_TRUE(px_band(IW_INK_M, 0, 0));
    ASSERT_TRUE(!px_band(IW_INK_C, 0, 0));
    ASSERT_TRUE(!px(0, 0));
    ASSERT_EQ_INT(g_in.page.inks, IW_INK_Y | IW_INK_M);
}

TEST(test_self_id) {
    setup(IW_MODEL_IW2);
    FEED("\x1b?");
    ASSERT_TRUE(strcmp(g_reply, "IW10C") == 0);
    setup(IW_MODEL_IW1);
    FEED("\x1b?");
    ASSERT_EQ_INT((int)g_reply_len, 0);
}

TEST(test_eighth_bit) {
    setup(IW_MODEL_IW2);
    // Ignored by default: $9B is ESC
    FEED("\x9bN");
    ASSERT_EQ_INT(g_in.st.pitch, IW_PITCH_PICA);
    // Included: $9B prints nothing and is not ESC
    FEED("\x1bZ\x00\x20\x9bQ");
    ASSERT_EQ_INT(g_in.st.pitch, IW_PITCH_PICA);
}

TEST(test_custom_characters) {
    setup(IW_MODEL_IW2);
    // 8-dot set; 'A' becomes a 3-column bar on wires 1..8, then fetch it
    FEED("\x1b-\x1bI"
         "AC\xff\x00\xff\x04\x1b'");
    ASSERT_EQ_INT(g_in.st.custom['A'].width, 3);
    FEED("\x1bN"
         "A");
    ASSERT_EQ_INT(g_in.st.head_x, 3 * (IW_UNITS_PER_INCH / 80));
    FEED("\r");
    ASSERT_TRUE(px(0, 0) && px(0, 28)); // column 0: all eight wires
    ASSERT_TRUE(!px(4, 0)); // column 1: none
}

TEST(test_deterministic_page) {
    // The same stream twice gives the same sheet
    uint8_t *a = NULL, *b = NULL;
    size_t alen = 0, blen = 0;
    for (int r = 0; r < 2; r++) {
        setup(IW_MODEL_IW2);
        g_cfg.dot_shape = IW_DOT_DISC;
        iw_interp_reconfigure(&g_in);
        FEED("\x1bN\x1b!Hello\x1b\"\r\n\x1bP\x1bG0004\x0f\xf0\x55\xaa\r");
        ASSERT_TRUE(iw_page_serialise(&g_in.page, r ? &b : &a, r ? &blen : &alen));
    }
    ASSERT_TRUE(alen && alen == blen && memcmp(a, b, alen) == 0);
    // And it survives a round trip
    iw_page_t copy = {0};
    iw_page_setup(&copy, 8.5, 11.0, 288, 4, IW_DOT_DISC);
    ASSERT_TRUE(iw_page_deserialise(&copy, a, alen));
    ASSERT_EQ_INT(copy.dots, g_in.page.dots);
    ASSERT_TRUE(memcmp(copy.plane[3], g_in.page.plane[3], (size_t)copy.stride * copy.h) == 0);
    iw_page_clear(&copy);
    free(a);
    free(b);
}

// The generated tables: draft draws in half columns, a descender reaches
// wire 9, and a language replaces its codes.
TEST(test_rom_glyphs) {
    iw_font_sel_t sel = {.quality = IW_QUALITY_DRAFT};
    iw_glyph_t g;
    ASSERT_TRUE(iw_font_glyph(&sel, 'L', &g));
    ASSERT_TRUE(g.half && !g.placeholder);
    ASSERT_EQ_INT(g.n, 16);
    ASSERT_EQ_INT(g.cols[0], 0x7F); // the stem, wires 1-7
    for (int i = 0; i + 1 < g.n; i++)
        ASSERT_TRUE((g.cols[i] & g.cols[i + 1]) == 0); // never two neighbouring half columns
    sel.quality = IW_QUALITY_CORRESPONDENCE;
    ASSERT_TRUE(iw_font_glyph(&sel, 'p', &g));
    ASSERT_TRUE(!g.half && g.n == 8);
    ASSERT_TRUE(g.cols[1] & 0x100); // the stem reaches wire 9
    iw_glyph_t us, de;
    ASSERT_TRUE(iw_font_glyph(&sel, '[', &us));
    sel.language = 4; // German: [ is A-umlaut
    ASSERT_TRUE(iw_font_glyph(&sel, '[', &de));
    ASSERT_TRUE(memcmp(us.cols, de.cols, sizeof(us.cols)) != 0);
    // Nothing prints for a control code
    ASSERT_TRUE(!iw_font_glyph(&sel, 0x07, &g));
}

// NLQ prints a second pass; the proportional sets have their own widths
// (the Technical Reference's chart: space 7, A 16, digits 12).
TEST(test_rom_glyphs_nlq_proportional) {
    iw_font_sel_t sel = {.quality = IW_QUALITY_NLQ};
    iw_glyph_t g;
    ASSERT_TRUE(iw_font_glyph(&sel, 'B', &g));
    ASSERT_TRUE(g.half && !g.placeholder);
    ASSERT_EQ_INT(g.n, 16);
    ASSERT_EQ_INT(g.n2, 16);
    int p1 = 0, p2 = 0;
    for (int i = 0; i < 16; i++) {
        p1 |= g.cols[i];
        p2 |= g.pass2[i];
    }
    ASSERT_TRUE(p1 && p2);
    sel.proportional = true;
    static const struct {
        uint8_t code, width;
    } widths[] = {
        {' ', 7 },
        {'!', 7 },
        {'"', 10},
        {'#', 14},
        {'0', 12},
        {'A', 16},
        {'g', 12}
    };
    for (int q = 0; q < 2; q++) {
        sel.quality = q ? IW_QUALITY_NLQ : IW_QUALITY_CORRESPONDENCE;
        for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
            ASSERT_TRUE(iw_font_glyph(&sel, widths[i].code, &g));
            ASSERT_TRUE(!g.half && !g.placeholder);
            ASSERT_EQ_INT(g.n, widths[i].width);
            ASSERT_EQ_INT(g.n2, q ? widths[i].width : 0);
        }
    }
    // MouseText has no proportional shapes: the fixed ones, 16 columns
    sel.mousetext = true;
    ASSERT_TRUE(iw_font_glyph(&sel, '@', &g));
    ASSERT_EQ_INT(g.n, 16);
}

// The original ImageWriter's own sets: 8 fixed columns, proportional
// widths, its languages (no Danish: that switch setting is American).
TEST(test_rom_glyphs_iw1) {
    iw_font_sel_t sel = {.iw1 = true};
    iw_glyph_t g, us, de;
    ASSERT_TRUE(iw_font_glyph(&sel, 'H', &g));
    ASSERT_TRUE(!g.placeholder && !g.half && g.n == 8);
    ASSERT_TRUE(g.cols[1] == 0x7F); // the left stem, wires 1-7
    ASSERT_TRUE(iw_font_glyph(&sel, 'g', &g));
    int low = 0;
    for (int i = 0; i < g.n; i++)
        low |= g.cols[i] & 0x100;
    ASSERT_TRUE(low); // the descender reaches wire 9
    ASSERT_TRUE(iw_font_glyph(&sel, '[', &us));
    sel.language = 4; // German: [ is A-umlaut
    ASSERT_TRUE(iw_font_glyph(&sel, '[', &de));
    ASSERT_TRUE(memcmp(us.cols, de.cols, sizeof(us.cols)) != 0);
    sel.language = 2; // American on the original ImageWriter
    ASSERT_TRUE(iw_font_glyph(&sel, '[', &de));
    ASSERT_TRUE(memcmp(us.cols, de.cols, sizeof(us.cols)) == 0);
    sel.language = 0;
    sel.proportional = true;
    ASSERT_TRUE(iw_font_glyph(&sel, 'i', &g));
    int wi = g.n;
    ASSERT_TRUE(iw_font_glyph(&sel, 'W', &g));
    ASSERT_TRUE(!g.placeholder && g.n > wi);
}

// The original ImageWriter's EVFU, with the manual's own example: a 12-line
// form, tabs B on lines 3 and 6, C on 4 and 7, D on 6 and 7, E on 9, bottom
// of form on 10 (lines counted from 1 at top of form).
TEST(test_iw1_evfu) {
    setup(IW_MODEL_IW1);
    int64_t ls = g_in.st.line_spacing;
    // Power-on: a B stop every six lines
    FEED("\x0b");
    ASSERT_EQ_INT(g_in.st.y - g_in.st.tof, 6 * ls);
    FEED("\x1d"
         "A@@@B@D@@@J@L@@@P@C@@@@@A@\x1e");
    ASSERT_EQ_INT(g_in.st.n_evfu, 12);
    ASSERT_EQ_INT(g_in.st.page_len, 12 * ls);
    int64_t top = g_in.st.y; // programming sets top of form here
    FEED("\x1f"
         "C"); // to line 4
    ASSERT_EQ_INT(g_in.st.y - top, 3 * ls);
    FEED("\x1f"
         "B"); // to line 6
    ASSERT_EQ_INT(g_in.st.y - top, 5 * ls);
    FEED("\x1f"
         "E"); // to line 9
    ASSERT_EQ_INT(g_in.st.y - top, 8 * ls);
    FEED("\x1f"
         "D"); // no D left: to the bottom of form, line 10
    ASSERT_EQ_INT(g_in.st.y - top, 9 * ls);
    FEED("\x1f"
         "A"); // the next top of form
    ASSERT_EQ_INT(g_in.st.y - top, 12 * ls);
    FEED("\x1f"
         "B\x1f"
         "B"); // lines 3 and 6 of the next form
    ASSERT_EQ_INT(g_in.st.y - top, 17 * ls);
    FEED("\x0c"); // form feed: the next top of form
    ASSERT_EQ_INT(g_in.st.y - top, 24 * ls);
    // ESC c keeps the tabs; GS 0 restores the power-on ones
    FEED("\x1b"
         "c");
    ASSERT_EQ_INT(g_in.st.n_evfu, 12);
    FEED("\x1d"
         "0");
    ASSERT_EQ_INT(g_in.st.n_evfu, 0);
    ASSERT_EQ_INT(g_in.st.page_len, 11 * 144);
}

int main(void) {
    RUN(test_pdf_structure);
    RUN(test_pdf_serialise);
    RUN(test_power_on_defaults);
    RUN(test_pitch_and_spacing);
    RUN(test_soft_switch_masks);
    RUN(test_tabs);
    RUN(test_graphics_column_positions);
    RUN(test_interleave);
    RUN(test_graphics_opaque);
    RUN(test_can_run_recovers_truncated_graphics);
    RUN(test_cr_lf_and_can);
    RUN(test_backspace_overprint);
    RUN(test_form_feed_and_pages);
    RUN(test_reverse_clamped_at_sheet_top);
    RUN(test_perforation_skip);
    RUN(test_colour);
    RUN(test_self_id);
    RUN(test_eighth_bit);
    RUN(test_custom_characters);
    RUN(test_deterministic_page);
    RUN(test_rom_glyphs);
    RUN(test_rom_glyphs_nlq_proportional);
    RUN(test_rom_glyphs_iw1);
    RUN(test_iw1_evfu);
    iw_interp_free(&g_in);
    fprintf(stderr, "All imagewriter tests passed\n");
    return 0;
}
