// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iw_interp.c
// The ImageWriter command interpreter: see iw_interp.h.

#include "iw_interp.h"

#include "log.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("imagewriter");

// Control characters
#define BEL 0x07
#define BS  0x08
#define HT  0x09
#define LF  0x0A
#define VT  0x0B
#define FF  0x0C
#define CR  0x0D
#define SO  0x0E
#define SI  0x0F
#define DC1 0x11
#define DC3 0x13
#define CAN 0x18
#define ESC 0x1B
#define GS  0x1D
#define US  0x1F
#define EOT 0x04

// Second strike of a bold column: 1/160 in to the right
#define BOLD_OFFSET (IW_UNITS_PER_INCH / 160)

// One pitch: its dot spacing and the width of a character column
typedef struct {
    int32_t dot; // IW units between dot columns
    int32_t column; // IW units per character column (margins, tabs)
    bool proportional;
    const char *name;
} iw_pitch_desc_t;

// Fixed pitches print 8 dot columns per character; a proportional pitch's
// character column (for margins and tabs) is its pica or elite width.
static const iw_pitch_desc_t pitches[IW_PITCH_COUNT] = {
    [IW_PITCH_EXTENDED] = {IW_UNITS_PER_INCH / 72,      IW_UNITS_PER_INCH / 9,        false, "extended"          },
    [IW_PITCH_PICA] = {IW_UNITS_PER_INCH / 80,      IW_UNITS_PER_INCH / 10,       false, "pica"              },
    [IW_PITCH_ELITE] = {IW_UNITS_PER_INCH / 96,      IW_UNITS_PER_INCH / 12,       false, "elite"             },
    [IW_PITCH_SEMICONDENSED] = {IW_UNITS_PER_INCH / 536 * 5, IW_UNITS_PER_INCH / 536 * 40, false, "semicondensed"     },
    [IW_PITCH_CONDENSED] = {IW_UNITS_PER_INCH / 120,     IW_UNITS_PER_INCH / 15,       false, "condensed"         },
    [IW_PITCH_ULTRACONDENSED] = {IW_UNITS_PER_INCH / 136,     IW_UNITS_PER_INCH / 17,       false, "ultracondensed"    },
    [IW_PITCH_PROP_PICA] = {IW_UNITS_PER_INCH / 144,     IW_UNITS_PER_INCH / 10,       true,  "proportional pica" },
    [IW_PITCH_PROP_ELITE] = {IW_UNITS_PER_INCH / 160,     IW_UNITS_PER_INCH / 12,       true,  "proportional elite"},
};

const char *iw_pitch_name(unsigned pitch) {
    return pitch < IW_PITCH_COUNT ? pitches[pitch].name : "?";
}

uint32_t iw_interp_dot_pitch_units(const iw_interp_t *in) {
    return (uint32_t)pitches[in->st.pitch].dot;
}

// The DIP pitch (switches 1-6, 1-7): 10 cpi, 12 cpi, 17 cpi, 160 dpi proportional
static uint8_t dip_pitch(uint8_t dip1) {
    static const uint8_t map[4] = {IW_PITCH_PICA, IW_PITCH_ELITE, IW_PITCH_ULTRACONDENSED, IW_PITCH_PROP_ELITE};
    return map[(dip1 & IW_DIP1_PITCH) >> 5];
}

static bool is_iw2(const iw_interp_t *in) {
    return in->cfg->model == IW_MODEL_IW2;
}

// --- Paper -------------------------------------------------------------------

// The sheet length in 1/144 in: the form length on continuous paper, the
// paper itself when sheets are fed one at a time.
static int32_t sheet_len(const iw_interp_t *in) {
    if (in->cfg->cut_sheet)
        return (int32_t)(in->cfg->paper_height_in * 144.0 + 0.5);
    return in->st.page_len > 0 ? in->st.page_len : 1584;
}

// Give the (blank) sheet its geometry: the paper's width, the sheet length.
static void sheet_setup(iw_interp_t *in) {
    const iw_config_t *c = in->cfg;
    iw_page_setup(&in->page, c->paper_width_in, sheet_len(in) / 144.0, c->dpi, c->color_ribbon && is_iw2(in) ? 4 : 1,
                  c->dot_shape);
}

// The sheet is done: hand it over if anything is printed on it.
static void sheet_finish(iw_interp_t *in) {
    if (iw_page_marked(&in->page)) {
        in->st.pages_done++;
        if (in->hooks.page_done)
            in->hooks.page_done(in->hooks.ctx, &in->page);
    }
    iw_page_clear(&in->page);
}

// Make the sheet under print line `y` the current one, finishing the sheets
// the paper has moved past.
static void sheet_sync(iw_interp_t *in, int64_t y) {
    int32_t len = sheet_len(in);
    if (y < in->st.page_top + len)
        return;
    sheet_finish(in);
    while (y >= in->st.page_top + len)
        in->st.page_top += len;
    sheet_setup(in);
}

// Move the paper by `d` 1/144 in (negative: reverse).  It cannot roll back
// above the top of the sheet being printed: the sheets before it are out.
static void paper_move(iw_interp_t *in, int64_t d) {
    in->st.y += d;
    if (in->st.y < in->st.page_top) {
        LOG(2, "reverse feed clamped at the top of the sheet");
        in->st.y = in->st.page_top;
    }
}

// Perforation skip: from half an inch above the end of a form to half an
// inch below the next top of form, nothing is printed.
static bool perf_skip_on(const iw_interp_t *in) {
    if (is_iw2(in))
        return !(in->st.soft_b & IW_SWB_NO_PERF);
    return false;
}

static int64_t mod_pos(int64_t a, int64_t m) {
    int64_t r = a % m;
    return r < 0 ? r + m : r;
}

// One line feed's worth of motion (no printing).
static void line_motion(iw_interp_t *in, int32_t amount) {
    if (in->st.reverse) {
        paper_move(in, -amount);
        return;
    }
    paper_move(in, amount);
    if (perf_skip_on(in)) {
        int64_t len = in->st.page_len > 0 ? in->st.page_len : 1584;
        int64_t rel = mod_pos(in->st.y - in->st.tof, len);
        if (rel >= len - 144)
            paper_move(in, len - rel);
    }
}

// Form feed motion: on to the next top of form (a sheet feeder ejects).
static void form_motion(iw_interp_t *in) {
    if (in->cfg->cut_sheet) {
        int32_t len = sheet_len(in);
        sheet_finish(in);
        in->st.page_top += len;
        in->st.y = in->st.tof = in->st.page_top + in->cfg->tof_offset_144;
        sheet_setup(in);
        return;
    }
    int64_t len = in->st.page_len > 0 ? in->st.page_len : 1584;
    int64_t rel = mod_pos(in->st.y - in->st.tof, len);
    paper_move(in, len - rel);
}

// --- The line buffer -----------------------------------------------------------

// Stamp the staged line onto the sheet at the print line and empty it.
static void commit_line(iw_interp_t *in) {
    iw_state_t *st = &in->st;
    if (st->n_marks == 0) {
        st->line_has_text = false;
        return;
    }
    sheet_sync(in, st->y);
    const iw_config_t *c = in->cfg;
    int64_t left = (int64_t)(c->left_offset_in * IW_UNITS_PER_INCH + 0.5);
    int64_t dpi = in->page.dpi;
    for (uint32_t i = 0; i < st->n_marks; i++) {
        const iw_mark_t *m = &st->marks[i];
        // Rounded once from the exact position: long lines do not drift
        int64_t xu = left + m->x;
        int64_t px = (xu * dpi + IW_UNITS_PER_INCH / 2) / IW_UNITS_PER_INCH;
        for (int w = 0; w < 9; w++) {
            if (!(m->col & (1u << w)))
                continue;
            int64_t y144 = st->y - st->page_top + m->yoff + (int64_t)w * m->step;
            if (!iw_page_dot(&in->page, px, y144 * dpi / 144, m->mask))
                LOG(1, "out of memory for the sheet");
        }
    }
    st->n_marks = 0;
    st->line_has_text = false;
}

// Stage one dot column.
static void stage(iw_interp_t *in, int64_t x, uint16_t col, uint8_t step, int8_t yoff) {
    iw_state_t *st = &in->st;
    if (!col)
        return;
    if (x < 0 || x >= (int64_t)(in->cfg->printable_width_in * IW_UNITS_PER_INCH + 0.5))
        return; // off the carriage
    if (st->n_marks >= IW_LINE_MARKS)
        commit_line(in); // the buffer holds one line; print what it has, same place
    iw_mark_t *m = &st->marks[st->n_marks++];
    memset(m, 0, sizeof(*m));
    m->x = (int32_t)x;
    m->col = col;
    m->mask = st->color_mask;
    m->step = step;
    m->yoff = yoff;
}

// The right end of the carriage, IW units
static int64_t right_limit(const iw_interp_t *in) {
    return (int64_t)(in->cfg->printable_width_in * IW_UNITS_PER_INCH + 0.5);
}

// --- Print commands --------------------------------------------------------------

// Whether LF and FF print the staged line (soft switch A-7)
static bool lf_prints(const iw_interp_t *in) {
    return !is_iw2(in) || (in->st.soft_a & IW_SWA_PRINT_LFFF);
}

// Auto line feed after CR: soft switch A-8 (IW II), DIP 1-8 (IW I)
static bool auto_lf(const iw_interp_t *in) {
    if (is_iw2(in))
        return (in->st.soft_a & IW_SWA_AUTO_LF) != 0;
    return (in->cfg->dip1 & IW_DIP1_AUTO_LF) != 0;
}

static void do_lf(iw_interp_t *in) {
    if (lf_prints(in))
        commit_line(in);
    if (in->st.cr_before_lf)
        in->st.head_x = in->st.left_margin;
    line_motion(in, in->st.line_spacing);
}

static void do_cr(iw_interp_t *in) {
    commit_line(in);
    in->st.head_x = in->st.left_margin;
    if (auto_lf(in))
        line_motion(in, in->st.line_spacing);
}

static void do_ff(iw_interp_t *in) {
    if (lf_prints(in))
        commit_line(in);
    if (in->st.cr_before_lf)
        in->st.head_x = in->st.left_margin;
    form_motion(in);
}

// The line is full: print it, back to the margin, LF if A-6 says so (on
// the original ImageWriter the same switch bit, set at power-on).
static void line_full(iw_interp_t *in) {
    commit_line(in);
    in->st.head_x = in->st.left_margin;
    if (in->st.soft_a & IW_SWA_LF_FULL)
        line_motion(in, in->st.line_spacing);
}

// --- Characters ------------------------------------------------------------------

// The glyph for `code` with the current selections.  False when nothing prints.
static bool pick_glyph(iw_interp_t *in, uint8_t code, iw_glyph_t *g) {
    iw_state_t *st = &in->st;
    // Custom characters (IW II): ESC ' for the code itself, ESC * for the
    // high set reached through low codes
    if (is_iw2(in) && (st->attr & (IW_ATTR_CUSTOM | IW_ATTR_CUSTOM_HIGH))) {
        unsigned idx = code;
        if ((st->attr & IW_ATTR_CUSTOM_HIGH) && code >= 0x20 && code < 0x70)
            idx = code + 0x80u;
        const iw_custom_t *cc = &st->custom[idx];
        if (cc->width) {
            memset(g, 0, sizeof(*g));
            g->n = cc->width;
            for (int i = 0; i < cc->width; i++)
                g->cols[i] = (uint16_t)(cc->low_wires ? cc->cols[i] << 1 : cc->cols[i]);
            return true;
        }
    }
    iw_font_sel_t sel = {0};
    sel.iw1 = !is_iw2(in);
    sel.proportional = pitches[st->pitch].proportional;
    // Draft has no bold, double width, half height, scripts or proportional
    // spacing: those print in correspondence quality
    sel.quality = st->quality;
    if (sel.quality == IW_QUALITY_DRAFT &&
        (sel.proportional || (st->attr & (IW_ATTR_BOLD | IW_ATTR_DOUBLE_WIDTH | IW_ATTR_HALF_HEIGHT |
                                          IW_ATTR_SUPERSCRIPT | IW_ATTR_SUBSCRIPT))))
        sel.quality = IW_QUALITY_CORRESPONDENCE;
    // Both printers take the language from DIP 1-1 .. 1-3 at power-on and
    // change it with ESC D / ESC Z
    sel.language = (uint8_t)(st->soft_a & IW_SWA_LANGUAGE);
    sel.mousetext = (st->attr & IW_ATTR_MOUSETEXT) != 0;
    sel.slash_zero = is_iw2(in) && (st->soft_b & IW_SWB_SLASH_ZERO);
    if (!iw_font_glyph(&sel, code, g))
        return false;
    // A blank (the space) looks the same in any font
    bool inked = false;
    for (int i = 0; i < g->n; i++)
        inked |= g->cols[i] != 0;
    if (g->placeholder && inked && !st->placeholder_used) {
        st->placeholder_used = true;
        LOG(1, "text printed with the placeholder font: no glyph table for this character set");
    }
    return true;
}

// Stage a printable character at the head and advance.
static void put_char(iw_interp_t *in, uint8_t code) {
    iw_state_t *st = &in->st;
    iw_glyph_t g;
    if (!pick_glyph(in, code, &g))
        return;
    const iw_pitch_desc_t *p = &pitches[st->pitch];
    int rep = (st->attr & IW_ATTR_DOUBLE_WIDTH) ? 2 : 1;
    // Draft glyphs are drawn in half dot columns (each column's position is
    // computed from the start, so an odd dot pitch does not drift)
    int div = g.half ? 2 : 1;
    int64_t advance = (int64_t)g.n * rep * p->dot / div + (p->proportional ? st->prop_dot_space : 0) * p->dot;
    // Backspace: this character goes back over the previous one
    if (st->bs_pending) {
        st->bs_pending = false;
        st->head_x -= (int32_t)advance;
        if (st->head_x < st->left_margin)
            st->head_x = st->left_margin;
    }
    if (st->head_x + advance > right_limit(in))
        line_full(in);
    st->line_has_text = true;
    // Height: half height, superscript (top half), subscript (bottom half)
    uint8_t step = 2;
    int8_t yoff = 0;
    if (st->attr & IW_ATTR_SUPERSCRIPT) {
        step = 1;
    } else if (st->attr & IW_ATTR_SUBSCRIPT) {
        step = 1;
        yoff = 8;
    } else if (st->attr & IW_ATTR_HALF_HEIGHT) {
        step = 1;
        yoff = 6;
    }
    bool bold = (st->attr & IW_ATTR_BOLD) != 0;
    for (int i = 0; i < g.n; i++) {
        for (int r = 0; r < rep; r++) {
            int64_t x = st->head_x + (int64_t)(i * rep + r) * p->dot / div;
            stage(in, x, g.cols[i], step, yoff);
            if (bold)
                stage(in, x + BOLD_OFFSET, g.cols[i], step, yoff);
            // NLQ's second pass, 1/144 in lower
            if (i < g.n2) {
                stage(in, x, g.pass2[i], step, (int8_t)(yoff + 1));
                if (bold)
                    stage(in, x + BOLD_OFFSET, g.pass2[i], step, (int8_t)(yoff + 1));
            }
        }
    }
    // Underline: wire 9 under the whole advance
    if (st->attr & IW_ATTR_UNDERLINE) {
        int64_t n = advance / p->dot;
        for (int64_t i = 0; i < n; i++)
            stage(in, st->head_x + i * p->dot, 0x100, 2, 0);
    }
    st->head_x += (int32_t)advance;
}

// One graphics column at the head.
static void put_graphics(iw_interp_t *in, uint8_t b) {
    iw_state_t *st = &in->st;
    const iw_pitch_desc_t *p = &pitches[st->pitch];
    int rep = (st->attr & IW_ATTR_DOUBLE_WIDTH) ? 2 : 1;
    bool bold = (st->attr & IW_ATTR_BOLD) != 0;
    for (int r = 0; r < rep; r++) {
        int64_t x = st->head_x + (int64_t)r * p->dot;
        stage(in, x, b, 2, 0);
        if (bold)
            stage(in, x + BOLD_OFFSET, b, 2, 0);
    }
    st->head_x += rep * p->dot;
}

// --- Tabs --------------------------------------------------------------------------

static void tab_add(iw_state_t *st, uint16_t col) {
    if (col == 0)
        return;
    for (int i = 0; i < st->n_tabs; i++)
        if (st->tabs[i] == col)
            return;
    if (st->n_tabs >= IW_MAX_TABS)
        return;
    int i = st->n_tabs++;
    // Kept ascending
    while (i > 0 && st->tabs[i - 1] > col) {
        st->tabs[i] = st->tabs[i - 1];
        i--;
    }
    st->tabs[i] = col;
}

static void tab_remove(iw_state_t *st, uint16_t col) {
    for (int i = 0; i < st->n_tabs; i++) {
        if (st->tabs[i] != col)
            continue;
        memmove(&st->tabs[i], &st->tabs[i + 1], (size_t)(st->n_tabs - i - 1) * sizeof(st->tabs[0]));
        st->n_tabs--;
        return;
    }
}

static void do_ht(iw_interp_t *in) {
    iw_state_t *st = &in->st;
    int32_t colw = pitches[st->pitch].column;
    int32_t cur = st->head_x / colw;
    for (int i = 0; i < st->n_tabs; i++) {
        if (st->tabs[i] > cur) {
            st->head_x = st->tabs[i] * colw;
            return;
        }
    }
}

// --- Resets --------------------------------------------------------------------------

// The settings ESC c and power-on share (Table A-2).
static void soft_defaults(iw_interp_t *in) {
    iw_state_t *st = &in->st;
    const iw_config_t *c = in->cfg;
    st->quality = IW_QUALITY_DRAFT;
    st->pitch = is_iw2(in) ? dip_pitch(c->dip1) : IW_PITCH_ELITE;
    st->prop_dot_space = 0;
    st->attr = 0;
    st->left_margin = 0;
    st->head_x = 0;
    st->page_len = (c->dip1 & IW_DIP1_FORM12) ? 12 * 144 : 11 * 144;
    st->paper_out_sensor = true;
    st->line_spacing = 24;
    st->reverse = false;
    st->cr_before_lf = true;
    st->unidirectional = false;
    st->n_tabs = 0;
    st->color_mask = IW_INK_K;
    st->bs_pending = false;
    // Software switches from the DIPs
    st->soft_a = (uint8_t)((c->dip1 & IW_DIP1_LANGUAGE) | IW_SWA_NO_SELECT | IW_SWA_PRINT_LFFF |
                           ((c->dip1 & IW_DIP1_AUTO_LF) ? IW_SWA_AUTO_LF : 0) | (is_iw2(in) ? 0 : IW_SWA_LF_FULL));
    st->soft_b = (uint8_t)(IW_SWB_7BIT | ((c->dip1 & IW_DIP1_PERF) ? 0 : IW_SWB_NO_PERF));
    // IW I: vertical tabs every 6 lines
    st->n_evfu = 0;
}

void iw_interp_power_on(iw_interp_t *in) {
    iw_state_t *st = &in->st;
    memset(st, 0, sizeof(*st));
    soft_defaults(in);
    st->selected = true;
    st->ps = IW_PS_NORMAL;
    // The print line at power-on is the top of form; the sheet's top edge is
    // above it by the paper's top-of-form offset
    st->y = 0;
    st->tof = 0;
    st->page_top = -(int64_t)in->cfg->tof_offset_144;
    sheet_setup(in);
}

void iw_interp_init(iw_interp_t *in, const iw_config_t *cfg, const iw_interp_hooks_t *hooks) {
    memset(in, 0, sizeof(*in));
    in->cfg = cfg;
    if (hooks)
        in->hooks = *hooks;
    iw_interp_power_on(in);
}

void iw_interp_free(iw_interp_t *in) {
    iw_page_clear(&in->page);
}

void iw_interp_reconfigure(iw_interp_t *in) {
    iw_page_clear(&in->page);
    sheet_setup(in);
}

void iw_interp_end_job(iw_interp_t *in) {
    commit_line(in);
    sheet_finish(in);
    sheet_setup(in);
}

void iw_interp_begin_job(iw_interp_t *in) {
    in->st.pages_done = 0;
    in->st.placeholder_used = false;
}

// --- Commands ------------------------------------------------------------------------

// Parameter bytes an ESC command takes (digits and raw bytes together).
static uint8_t esc_param_count(uint8_t cmd) {
    switch (cmd) {
    case 'a':
    case 's':
    case 'l':
    case 'K':
        return 1;
    case 'T':
    case 'D':
    case 'Z':
        return 2;
    case 'L':
    case 'u':
    case 'g':
        return 3;
    case 'F':
    case 'G':
    case 'S':
    case 'H':
    case 'R': // nnn + the character
        return 4;
    case 'V': // nnnn + the column
        return 5;
    default:
        return 0;
    }
}

// A fixed-length ASCII decimal parameter: a space counts as 0, anything
// else by its low four bits (what the printer's masking does).
static uint32_t digits(const uint8_t *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        uint8_t d = (uint8_t)((p[i] - '0') & 0x0F);
        if (p[i] != ' ' && (p[i] < '0' || p[i] > '9'))
            LOG(3, "non-digit 0x%02X in a numeric parameter", p[i]);
        v = v * 10 + (d > 9 ? 9 : d);
    }
    return v;
}

// The ribbon bands for ESC K n: black, yellow, magenta, cyan, then the
// two-band colours orange (Y+M), green (Y+C) and purple (M+C)
static const uint8_t ribbon_colours[7] = {
    IW_INK_K, IW_INK_Y, IW_INK_M, IW_INK_C, IW_INK_Y | IW_INK_M, IW_INK_Y | IW_INK_C, IW_INK_M | IW_INK_C,
};

// Send the self-ID string (ESC ?): IW, the carriage width, C for a colour
// ribbon, F for a sheet feeder.
static void send_id(iw_interp_t *in) {
    if (!is_iw2(in) || !in->hooks.reply)
        return;
    char id[8] = "IW10";
    size_t n = 4;
    if (in->cfg->color_ribbon)
        id[n++] = 'C';
    if (in->cfg->sheet_feeder)
        id[n++] = 'F';
    in->hooks.reply(in->hooks.ctx, (const uint8_t *)id, n);
}

// Run ESC `cmd` with its parameters collected.
static void exec_esc(iw_interp_t *in) {
    iw_state_t *st = &in->st;
    const uint8_t *p = st->param;
    bool iw2 = is_iw2(in);
    switch (st->cmd) {
    // Pitch
    case 'n':
        st->pitch = IW_PITCH_EXTENDED;
        break;
    case 'N':
        st->pitch = IW_PITCH_PICA;
        break;
    case 'E':
        st->pitch = IW_PITCH_ELITE;
        break;
    case 'e':
        st->pitch = IW_PITCH_SEMICONDENSED;
        break;
    case 'q':
        st->pitch = IW_PITCH_CONDENSED;
        break;
    case 'Q':
        st->pitch = IW_PITCH_ULTRACONDENSED;
        break;
    case 'p':
        st->pitch = IW_PITCH_PROP_PICA;
        break;
    case 'P':
        st->pitch = IW_PITCH_PROP_ELITE;
        break;
    case 's':
        st->prop_dot_space = (uint8_t)digits(p, 1);
        break;
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
        // Insert that many dot spaces
        st->head_x += (st->cmd - '0') * pitches[st->pitch].dot;
        break;
    // Quality
    case 'a':
        if (iw2) {
            uint32_t q = digits(p, 1);
            if (q <= 2)
                st->quality = (uint8_t)q;
        }
        break;
    case 'm':
        if (iw2)
            st->quality = IW_QUALITY_CORRESPONDENCE;
        break;
    case 'M':
        if (iw2)
            st->quality = IW_QUALITY_NLQ;
        break;
    // Attributes
    case 'X':
        st->attr |= IW_ATTR_UNDERLINE;
        break;
    case 'Y':
        st->attr &= (uint16_t)~IW_ATTR_UNDERLINE;
        break;
    case '!':
        st->attr |= IW_ATTR_BOLD;
        break;
    case '"':
        st->attr &= (uint16_t)~IW_ATTR_BOLD;
        break;
    case 'w':
        if (iw2)
            st->attr |= IW_ATTR_HALF_HEIGHT;
        break;
    case 'W':
        st->attr &= (uint16_t)~IW_ATTR_HALF_HEIGHT;
        break;
    case 'x':
        if (iw2)
            st->attr = (uint16_t)((st->attr & ~IW_ATTR_SUBSCRIPT) | IW_ATTR_SUPERSCRIPT);
        break;
    case 'y':
        if (iw2)
            st->attr = (uint16_t)((st->attr & ~IW_ATTR_SUPERSCRIPT) | IW_ATTR_SUBSCRIPT);
        break;
    case 'z':
        st->attr &= (uint16_t) ~(IW_ATTR_SUPERSCRIPT | IW_ATTR_SUBSCRIPT);
        break;
    // Character sets
    case '&':
        if (iw2)
            st->attr |= IW_ATTR_MOUSETEXT;
        break;
    case '$':
        st->attr &= (uint16_t) ~(IW_ATTR_MOUSETEXT | IW_ATTR_CUSTOM | IW_ATTR_CUSTOM_HIGH);
        break;
    case '\'':
        if (iw2)
            st->attr = (uint16_t)((st->attr & ~IW_ATTR_CUSTOM_HIGH) | IW_ATTR_CUSTOM);
        break;
    case '*':
        if (iw2)
            st->attr = (uint16_t)((st->attr & ~IW_ATTR_CUSTOM) | IW_ATTR_CUSTOM_HIGH);
        break;
    case '-':
    case '+':
        // Choosing the maximum width erases the custom set and selects 8-bit data
        if (iw2) {
            memset(st->custom, 0, sizeof(st->custom));
            st->custom_wide = st->cmd == '+';
            st->soft_b &= (uint8_t)~IW_SWB_7BIT;
        }
        break;
    // Horizontal
    case 'L':
        st->left_margin = (int32_t)digits(p, 3) * pitches[st->pitch].column;
        break;
    case 'u':
        tab_add(st, (uint16_t)digits(p, 3));
        break;
    case '0':
        st->n_tabs = 0;
        break;
    case 'F':
        st->head_x = st->left_margin + (int32_t)digits(p, 4) * pitches[st->pitch].dot;
        break;
    case 'R': {
        uint32_t n = digits(p, 3);
        uint8_t c = p[3];
        if (c >= 0x20 && c != 0x7F)
            for (uint32_t i = 0; i < n; i++)
                put_char(in, c);
        break;
    }
    case '>':
        st->unidirectional = true;
        break;
    case '<':
        st->unidirectional = false;
        break;
    // Vertical
    case 'A':
        st->line_spacing = 24;
        break;
    case 'B':
        st->line_spacing = 18;
        break;
    case 'T': {
        uint32_t n = digits(p, 2);
        if (n > 0)
            st->line_spacing = (int32_t)n;
        break;
    }
    case 'f':
        st->reverse = false;
        break;
    case 'r':
        st->reverse = true;
        break;
    case 'H': {
        uint32_t n = digits(p, 4);
        if (n > 0)
            st->page_len = (int32_t)n;
        break;
    }
    case 'v':
        st->tof = st->y;
        break;
    case 'l':
        st->cr_before_lf = digits(p, 1) == 0;
        break;
    case 'o':
        st->paper_out_sensor = true;
        break;
    case 'O':
        st->paper_out_sensor = false;
        break;
    // Graphics
    case 'G':
    case 'S':
        st->gfx_left = digits(p, 4);
        if (st->gfx_left)
            st->ps = IW_PS_GRAPHICS;
        return;
    case 'g':
        st->gfx_left = digits(p, 3) * 8;
        if (st->gfx_left)
            st->ps = IW_PS_GRAPHICS;
        return;
    case 'V': {
        uint32_t n = digits(p, 4);
        for (uint32_t i = 0; i < n; i++)
            put_graphics(in, p[4]);
        break;
    }
    // Colour
    case 'K':
        if (iw2 && in->cfg->color_ribbon) {
            uint32_t c = digits(p, 1);
            if (c < 7)
                st->color_mask = ribbon_colours[c];
        }
        break;
    // Switches
    case 'D':
        st->soft_a |= (uint8_t)(p[0] & IW_SWA_MASK);
        st->soft_b |= (uint8_t)(p[1] & IW_SWB_MASK);
        break;
    case 'Z':
        st->soft_a &= (uint8_t) ~(p[0] & IW_SWA_MASK);
        st->soft_b &= (uint8_t) ~(p[1] & IW_SWB_MASK);
        break;
    case 'c':
        // Prints what is buffered, then resets the software settings; top of
        // form and the custom characters stay
        commit_line(in);
        soft_defaults(in);
        break;
    case '?':
        send_id(in);
        break;
    default:
        LOG(3, "unknown command ESC 0x%02X ignored", st->cmd);
        break;
    }
    st->ps = IW_PS_NORMAL;
}

// A control byte in normal state.
static void control(iw_interp_t *in, uint8_t b) {
    iw_state_t *st = &in->st;
    switch (b) {
    case BS:
        st->bs_pending = true;
        break;
    case HT:
        do_ht(in);
        break;
    case LF:
        do_lf(in);
        break;
    case VT:
        if (is_iw2(in)) {
            line_motion(in, 2); // a fixed 2/144 in micro-feed
        } else {
            // Next vertical tab stop: every 6 lines from top of form
            commit_line(in);
            int64_t lines = (st->y - st->tof) / (st->line_spacing ? st->line_spacing : 24);
            int64_t next = (lines / 6 + 1) * 6;
            paper_move(in, st->tof + next * st->line_spacing - st->y);
        }
        break;
    case FF:
        do_ff(in);
        break;
    case CR:
        do_cr(in);
        break;
    case SO:
        st->attr |= IW_ATTR_DOUBLE_WIDTH;
        break;
    case SI:
        st->attr &= (uint16_t)~IW_ATTR_DOUBLE_WIDTH;
        break;
    case DC1:
    case DC3:
        // Honoured only with the software select response enabled (A-5 open)
        if (!is_iw2(in) || !(st->soft_a & IW_SWA_NO_SELECT)) {
            bool sel = b == DC1;
            if (sel != st->selected) {
                st->selected = sel;
                if (in->hooks.select_changed)
                    in->hooks.select_changed(in->hooks.ctx, sel);
            }
        }
        break;
    case CAN:
        // Erase the line not yet printed
        st->n_marks = 0;
        st->line_has_text = false;
        st->head_x = st->left_margin;
        break;
    case ESC:
        st->ps = IW_PS_ESC;
        break;
    case US:
        if (is_iw2(in))
            st->ps = IW_PS_US;
        break;
    case GS:
        LOG(3, "GS (vertical format) ignored");
        break;
    default:
        break; // BEL and the rest print nothing
    }
}

// Whether the eighth bit is stripped before anything else looks at a byte
static bool ignore_8th_bit(const iw_interp_t *in) {
    if (is_iw2(in))
        return (in->st.soft_b & IW_SWB_7BIT) != 0;
    return (in->cfg->dip1 & IW_DIP1_PERF) != 0;
}

// One byte through the state machine.
static void feed_byte(iw_interp_t *in, uint8_t b) {
    iw_state_t *st = &in->st;
    switch ((iw_parse_state_t)st->ps) {
    case IW_PS_GRAPHICS:
        // Raw columns, counted and never interpreted
        put_graphics(in, b);
        if (--st->gfx_left == 0)
            st->ps = IW_PS_NORMAL;
        return;
    case IW_PS_PARAMS:
        st->param[st->nparam++] = b;
        if (st->nparam >= st->want)
            exec_esc(in);
        return;
    case IW_PS_CUSTOM_KEY:
        if ((b & 0x7F) == EOT) {
            st->ps = IW_PS_NORMAL;
            return;
        }
        st->custom_key = b;
        st->ps = IW_PS_CUSTOM_WIDTH;
        return;
    case IW_PS_CUSTOM_WIDTH: {
        uint8_t w = b & 0x7F;
        bool low = false;
        uint8_t width = 0;
        if (w >= 'A' && w <= 'P')
            width = (uint8_t)(w - 'A' + 1);
        else if (w >= 'a' && w <= 'p') {
            width = (uint8_t)(w - 'a' + 1);
            low = true;
        }
        uint8_t max = st->custom_wide ? 16 : 8;
        if (width == 0 || width > max) {
            LOG(2, "bad custom character width code 0x%02X: loading abandoned", b);
            st->ps = IW_PS_NORMAL;
            return;
        }
        iw_custom_t *cc = &st->custom[st->custom_key];
        memset(cc, 0, sizeof(*cc));
        cc->width = width;
        cc->low_wires = low;
        st->custom_left = width;
        st->nparam = 0;
        st->ps = IW_PS_CUSTOM_DATA;
        return;
    }
    case IW_PS_CUSTOM_DATA: {
        iw_custom_t *cc = &st->custom[st->custom_key];
        cc->cols[cc->width - st->custom_left] = b;
        if (--st->custom_left == 0)
            st->ps = IW_PS_CUSTOM_KEY;
        return;
    }
    default:
        break;
    }

    if (ignore_8th_bit(in))
        b &= 0x7F;

    switch ((iw_parse_state_t)st->ps) {
    case IW_PS_ESC: {
        st->cmd = b;
        st->nparam = 0;
        if (b == '(' || b == ')') {
            st->tab_value = 0;
            st->tab_digits = false;
            st->ps = IW_PS_TABLIST;
            return;
        }
        if (b == 'I' && is_iw2(in)) {
            st->ps = IW_PS_CUSTOM_KEY;
            return;
        }
        st->want = esc_param_count(b);
        if (st->want) {
            st->ps = IW_PS_PARAMS;
            return;
        }
        exec_esc(in);
        return;
    }
    case IW_PS_TABLIST:
        if (b >= '0' && b <= '9') {
            st->tab_value = (uint16_t)(st->tab_value * 10 + (b - '0'));
            st->tab_digits = true;
            return;
        }
        if (b == ' ')
            return;
        if (st->tab_digits) {
            if (st->cmd == '(')
                tab_add(st, st->tab_value);
            else
                tab_remove(st, st->tab_value);
        }
        st->tab_value = 0;
        st->tab_digits = false;
        if (b != ',')
            st->ps = IW_PS_NORMAL; // '.' ends the list (anything else too)
        return;
    case IW_PS_US: {
        uint8_t n = (uint8_t)((b - '0') & 0x0F);
        st->ps = IW_PS_NORMAL;
        if (lf_prints(in))
            commit_line(in);
        for (uint8_t i = 0; i < n; i++)
            line_motion(in, st->line_spacing);
        return;
    }
    default:
        break;
    }

    // Normal state.  Deselected (DC3), only DC1 is heard.
    if (!st->selected && b != DC1)
        return;
    if (b < 0x20 || b == 0x7F) {
        control(in, b);
        return;
    }
    if (b >= 0x80 && b < 0xA0)
        return; // with eight bits included, nothing prints here
    put_char(in, b);
}

void iw_interp_feed(iw_interp_t *in, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++)
        feed_byte(in, data[i]);
}
