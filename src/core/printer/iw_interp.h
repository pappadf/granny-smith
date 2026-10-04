// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iw_interp.h
// The ImageWriter command language: one byte stream in, sheets of dots out.
//
// The ImageWriter (I) and ImageWriter II speak one language -- ESC
// commands with fixed-length ASCII decimal parameters, 8-dot graphics
// columns with bit 0 at the top -- and the LocalTalk Option card carries the
// same stream in PAP data.  This interpreter is the printer behind all of
// them: it keeps the printer's settings (pitch, margins, tabs, line
// spacing, soft switches, ribbon colour), stages printable characters and
// graphics in a line buffer the way the printer does, and when a print
// command commits the line it stamps the line's dots onto the sheet at the
// current paper position.  Sheets are finished as the paper moves past
// them and handed to the owner, which turns them into the job's PDF.
//
// Positions are exact: horizontally in 1/IW_UNITS_PER_INCH in (a common
// multiple of every dot pitch, including 107.2 dpi), vertically in 1/144 in.
// Nothing here depends on host or guest time.
//
// Sources: Apple, ImageWriter II Technical Reference Manual (command set,
// App. A; software switches, Table A-5; custom characters, ch. 7; self ID,
// Table 6-7); Apple, ImageWriter User's Manual Part 1: Reference (the
// original ImageWriter).

#ifndef IW_INTERP_H
#define IW_INTERP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iw_font.h"
#include "iw_page.h"

// Horizontal position unit: 1/1640160 in (the LCM of 72, 80, 96, 120, 136,
// 144 and 160 dpi, times 67 for 107.2 dpi = 536/5)
#define IW_UNITS_PER_INCH 1640160

// Bounds
#define IW_MAX_TABS     32
#define IW_LINE_MARKS   8192
#define IW_EVFU_LINES   132
#define IW_PARAM_MAX    8
#define IW_CUSTOM_CODES 256

// Models
typedef enum {
    IW_MODEL_IW1 = 0, // ImageWriter (C. Itoh 8510 lineage)
    IW_MODEL_IW2, // ImageWriter II
    IW_MODEL_COUNT
} iw_model_t;

// Pitches (index into the pitch table)
typedef enum {
    IW_PITCH_EXTENDED = 0, // ESC n: 9 cpi, 72 dpi
    IW_PITCH_PICA, // ESC N: 10 cpi, 80 dpi
    IW_PITCH_ELITE, // ESC E: 12 cpi, 96 dpi
    IW_PITCH_SEMICONDENSED, // ESC e: 13.4 cpi, 107.2 dpi
    IW_PITCH_CONDENSED, // ESC q: 15 cpi, 120 dpi
    IW_PITCH_ULTRACONDENSED, // ESC Q: 17 cpi, 136 dpi
    IW_PITCH_PROP_PICA, // ESC p: proportional, 144 dpi
    IW_PITCH_PROP_ELITE, // ESC P: proportional, 160 dpi
    IW_PITCH_COUNT
} iw_pitch_t;

// Character attributes
#define IW_ATTR_UNDERLINE    0x0001
#define IW_ATTR_BOLD         0x0002
#define IW_ATTR_DOUBLE_WIDTH 0x0004
#define IW_ATTR_HALF_HEIGHT  0x0008
#define IW_ATTR_SUPERSCRIPT  0x0010
#define IW_ATTR_SUBSCRIPT    0x0020
#define IW_ATTR_MOUSETEXT    0x0040
#define IW_ATTR_CUSTOM       0x0080 // ESC ': custom characters for $20..$7E
#define IW_ATTR_CUSTOM_HIGH  0x0100 // ESC *: low codes fetch the high custom set

// Software switch bits (register A, register B), Table A-5
#define IW_SWA_LANGUAGE   0x07
#define IW_SWA_NO_SELECT  0x10 // A-5 closed: DC1/DC3 ignored
#define IW_SWA_LF_FULL    0x20 // A-6 closed: LF when the line is full
#define IW_SWA_PRINT_LFFF 0x40 // A-7 closed: CR, LF and FF print (open: CR only)
#define IW_SWA_AUTO_LF    0x80 // A-8 closed: LF after CR
#define IW_SWB_SLASH_ZERO 0x01 // B-1 closed: slashed zero
#define IW_SWB_NO_PERF    0x04 // B-3 closed: no perforation skip
#define IW_SWB_7BIT       0x20 // B-6 closed: ignore the eighth bit
#define IW_SWA_MASK       0xF7 // bits ESC D / ESC Z may change
#define IW_SWB_MASK       0x25

// DIP switches: bit n-1 of dip1/dip2 = switch 1-n / 2-n closed
#define IW_DIP1_LANGUAGE 0x07
#define IW_DIP1_FORM12   0x08 // 1-4: 12 in forms
#define IW_DIP1_PERF     0x10 // 1-5: perforation skip (IW II); ignore 8th bit (IW I)
#define IW_DIP1_PITCH    0x60 // 1-6, 1-7
#define IW_DIP1_AUTO_LF  0x80 // 1-8
#define IW_DIP2_BAUD     0x03 // 2-1, 2-2
#define IW_DIP2_XONXOFF  0x04 // 2-3
#define IW_DIP2_OPTION   0x08 // 2-4

// The fixed configuration the interpreter runs under: what the printer is
// and the paper in it.  Owned by the device; read on every page.
typedef struct {
    iw_model_t model;
    uint8_t dip1, dip2;
    bool color_ribbon;
    bool sheet_feeder;
    double paper_width_in, paper_height_in; // the sheet (PDF media box)
    double left_offset_in; // column 0's distance from the sheet's left edge
    int32_t tof_offset_144; // print line below the sheet's top edge at top of form
    bool cut_sheet; // each form feed ejects; the sheet is the page
    uint16_t dpi; // raster resolution
    uint8_t dot_shape; // IW_DOT_*
    double printable_width_in; // the carriage: 8.0 (10 in) or 13.6 (15 in)
} iw_config_t;

// One dot column staged in the line buffer.
typedef struct {
    int32_t x; // position in IW units from column 0
    uint16_t col; // wire pattern, bit 0 = wire 1
    uint8_t mask; // ribbon bands
    uint8_t step; // 1/144 in between wires: 2 normally, 1 for half-height
    int8_t yoff; // vertical offset, 1/144 in
    uint8_t pad[3];
} iw_mark_t;

// A custom character (ESC I).
typedef struct {
    uint8_t width; // columns, 0 = not defined
    bool low_wires; // printed with wires 2..9
    uint8_t cols[16];
} iw_custom_t;

// Parser states
typedef enum {
    IW_PS_NORMAL = 0,
    IW_PS_ESC, // after ESC
    IW_PS_PARAMS, // collecting a command's parameter bytes
    IW_PS_TABLIST, // ESC ( / ESC ): digits, commas, a period
    IW_PS_GRAPHICS, // raw graphics columns
    IW_PS_US, // CTRL-_: the line count
    IW_PS_CUSTOM_KEY, // ESC I: a key or CTRL-D
    IW_PS_CUSTOM_WIDTH, // ESC I: the width code
    IW_PS_CUSTOM_DATA, // ESC I: the columns
    IW_PS_GS, // IW I CTRL-]: programming the EVFU, or GS 0
    IW_PS_VTAB, // IW I CTRL-_: the tab channel letter
} iw_parse_state_t;

// IW I EVFU channels: A marks top and bottom of form, B .. F the tab stops
#define IW_EVFU_A 0x01
#define IW_EVFU_B 0x02

// Everything the printer remembers.  Plain data: a checkpoint writes it as
// one block.
typedef struct {
    // --- switches ---
    uint8_t soft_a, soft_b; // IW II software switch registers
    // --- character formatting ---
    uint8_t pitch; // iw_pitch_t
    uint8_t quality; // IW_QUALITY_*
    uint16_t attr; // IW_ATTR_*
    uint8_t prop_dot_space; // ESC s n: extra dots between proportional characters
    uint8_t color_mask; // ribbon bands, IW_INK_*
    // --- horizontal ---
    int32_t left_margin; // IW units
    int32_t head_x; // IW units from column 0
    uint8_t n_tabs;
    uint16_t tabs[IW_MAX_TABS]; // character columns, ascending
    bool unidirectional; // ESC > (no effect on the page)
    bool bs_pending; // CTRL-H: the next glyph backs up by its own advance
    // --- vertical (1/144 in) ---
    int64_t y; // print line, from the start of the job's paper
    int64_t tof; // top of form
    int64_t page_top; // top edge of the sheet being printed
    int32_t line_spacing; // ESC A / ESC B / ESC T
    int32_t page_len; // ESC H, DIP 1-4
    bool reverse; // ESC r
    bool cr_before_lf; // ESC l 0 (default)
    bool paper_out_sensor; // ESC o / ESC O
    uint8_t evfu[IW_EVFU_LINES]; // IW I EVFU: channels (IW_EVFU_*) per line from top of form
    uint8_t n_evfu; // the programmed form's length in lines; 0: power-on tabs
    uint8_t evfu_lines; // programming: lines received so far
    uint8_t evfu_c1; // programming: the first byte of a pair, 0 when none
    // --- parser ---
    uint8_t ps; // iw_parse_state_t
    uint8_t cmd; // current ESC command
    uint8_t param[IW_PARAM_MAX];
    uint8_t nparam, want;
    uint32_t gfx_left; // graphics columns still to come
    uint16_t tab_value; // ESC ( / ESC ): the number being read
    bool tab_digits; // a digit has been read for tab_value
    // --- custom characters ---
    bool custom_wide; // ESC +: 16-dot maximum (else 8)
    uint8_t custom_key, custom_left;
    iw_custom_t custom[IW_CUSTOM_CODES];
    // --- status ---
    bool selected; // CTRL-Q / CTRL-S, front panel
    // --- line buffer ---
    uint32_t n_marks;
    iw_mark_t marks[IW_LINE_MARKS];
    bool line_has_text; // the staged line holds printable characters
    // --- counters ---
    uint32_t pages_done; // sheets finished in this job
    bool placeholder_used; // a glyph came from the placeholder font this job
} iw_state_t;

// What the interpreter tells its owner.
typedef struct {
    void *ctx;
    // A sheet is finished (it has dots on it).  The owner adds it to the job.
    void (*page_done)(void *ctx, const iw_page_t *page);
    // The printer sends bytes back to the host (the ESC ? reply).
    void (*reply)(void *ctx, const uint8_t *data, size_t len);
    // The select state changed (DC1 / DC3).
    void (*select_changed)(void *ctx, bool selected);
} iw_interp_hooks_t;

typedef struct {
    const iw_config_t *cfg;
    iw_interp_hooks_t hooks;
    iw_state_t st;
    iw_page_t page; // the sheet being printed
} iw_interp_t;

// Set up an interpreter for `cfg` and power it on (iw_interp_power_on).
void iw_interp_init(iw_interp_t *in, const iw_config_t *cfg, const iw_interp_hooks_t *hooks);

// Free the sheet in progress.
void iw_interp_free(iw_interp_t *in);

// Power-on (hard reset, Table A-1): every setting from the DIP switches,
// custom characters cleared, the print line becomes top of form.  A sheet
// in progress is dropped.
void iw_interp_power_on(iw_interp_t *in);

// The configuration changed (paper, resolution): re-set the sheet.  A sheet
// in progress is dropped.
void iw_interp_reconfigure(iw_interp_t *in);

// Interpret `len` bytes.
void iw_interp_feed(iw_interp_t *in, const uint8_t *data, size_t len);

// The job is over: commit a staged line and finish the sheet in progress
// (handing it to page_done when it has dots).  The paper position is kept.
void iw_interp_end_job(iw_interp_t *in);

// A new job starts on fresh paper: the next sheet begins at the print line
// (the paper the last job used has been torn off), settings unchanged.
void iw_interp_begin_job(iw_interp_t *in);

// Read-outs
uint32_t iw_interp_dot_pitch_units(const iw_interp_t *in); // current graphics dot pitch
const char *iw_pitch_name(unsigned pitch);

#endif // IW_INTERP_H
