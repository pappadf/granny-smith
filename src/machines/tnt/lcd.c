// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lcd.c
// The Apple Network Server's front-panel character LCD — and, during POST,
// the machine's ONLY output device.
//
// > "Network Servers provides a WRITE ONLY LCD interface as two registers on
// >  GBUS device 3.  Software will have to provide a 1 microsecond (or
// >  longer) timer between write accesses.  Register 0 at address offset
// >  x1C000 is the R (Command) register for the device; register 1 at
// >  address offset x1C010 is the S (Data) register."
// >  (Apple, "Network Server Hardware Developer Notes", 1996, §4.2.3.)
//
// Building this FIRST is instrumentation, not thoroughness.  The Theory of
// Operations puts it ahead of memory: "It is the job of POST to initialize
// the hardware into a working state and establish a software path to the
// LCD.  The LCD is then written with progress reports on the state of the
// discovered hardware: DRAM; SRAM cache; and various fan, temperature, and
// power supply fail states."  An emulator that cannot accept LCD writes is
// blind during exactly the phase most likely to break — and because Apple
// published the exact strings POST writes, every ladder rung from S2 to S5
// can assert on documented text, positively and negatively.
//
// WHAT THE CONTROLLER IS.  Apple documents only the two register addresses
// and the settling time; the part behind them is not named.  The shipping
// ROM answers the question by its own initialisation sequence, which is the
// textbook Hitachi HD44780 power-on ritual and nothing else:
//
//     $30 $30 $30   function set, 8-bit interface, written three times
//     $38           function set: 8-bit, 2-line mode, 5x8 font
//     $08           display off
//     $0C           display on, cursor off, blink off
//     $06           entry mode: increment, no display shift
//     $38           function set again
//     $01           clear display
//
// and then addresses lines with $80 / $C0 / $94 / $D4 — the canonical 4x20
// DDRAM map ($00, $40, $14, $54).  That map settles the one geometry
// question the documents leave open: **the panel is 20 columns**, because
// $94 - $80 = $14 = 20 is exactly where line 0 ends.  Every POST string the
// ROM writes is padded to exactly 20 characters, which confirms it a second
// way.  (The `150 MHz 604, 50 MHz Bus` sample banner printed in "Setting Up
// the Network Server" is 23 characters and cannot be what the machine
// writes; the real ROM writes `150MHz 604, 50MHzBus`-shaped 20-character
// lines, e.g. `066MHz 604, 44MHzBus`.)
//
// Because the geometry is the HD44780's, the DDRAM is modelled directly
// rather than as four line buffers: that gets the controller's famously
// non-contiguous wrap (line 0 runs into line 2, line 1 into line 3) right
// for free, and it is what the hardware does.
//
// TIMING.  The 1 microsecond inter-write settle is a constraint the
// hardware documentation places on SOFTWARE, not on the device, and the ROM
// leaves a 1000x margin by spinning a `1ms` word between writes.  Modelling
// it would be modelling the guest's problem.
//
// READS.  The interface is WRITE ONLY.  Reads return zero and are logged;
// the 2.0 prototype ROM contains an `lcd-cmd@` word implying a read exists,
// so the log is how we would find out that it matters.

#include "tnt.h"

#include "log.h"
#include "object.h"
#include "value.h"

#include <stdio.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("lcd");

static inline tnt_lcd_t *lcd(config_t *cfg) {
    return cfg && cfg->machine_context ? &tnt_st(cfg)->lcd : NULL;
}

// The HD44780's 4x20 DDRAM map: line 0 at $00, line 1 at $40, line 2 at
// $14, line 3 at $54.  Returns the DDRAM base of a display line.
static const uint8_t lcd_line_base[ANS_LCD_LINES] = {0x00u, 0x40u, 0x14u, 0x54u};

// ============================================================
// Command decode
// ============================================================
// The HD44780 command set is a priority encoding on the highest set bit.
static void lcd_command(config_t *cfg, uint8_t cmd) {
    tnt_lcd_t *l = lcd(cfg);
    if (!l)
        return;
    l->commands++;
    l->last_cmd = cmd;
    if (cmd & 0x80u) { // Set DDRAM address
        l->addr = cmd & 0x7Fu;
        l->cgram = false;
    } else if (cmd & 0x40u) { // Set CGRAM address — the ROM never uses this
        l->addr = cmd & 0x3Fu;
        l->cgram = true;
        LOG(2, "set CGRAM address $%02X (no custom glyphs are modelled)", l->addr);
    } else if (cmd & 0x20u) { // Function set: interface width, lines, font
        LOG(4, "function set $%02X", cmd);
    } else if (cmd & 0x10u) { // Cursor or display shift
        LOG(2, "cursor/display shift $%02X (not modelled)", cmd);
    } else if (cmd & 0x08u) { // Display on/off control
        l->display_on = (cmd & 0x04u) != 0;
        LOG(4, "display %s", l->display_on ? "on" : "off");
    } else if (cmd & 0x04u) { // Entry mode set
        l->entry_inc = (cmd & 0x02u) != 0;
        LOG(4, "entry mode: address %s", l->entry_inc ? "increments" : "decrements");
    } else if (cmd & 0x02u) { // Return home
        l->addr = 0;
        l->cgram = false;
    } else if (cmd & 0x01u) { // Clear display
        memset(l->ddram, ' ', sizeof(l->ddram));
        l->addr = 0;
        l->cgram = false;
    } else {
        LOG(1, "unknown LCD command $%02X", cmd);
    }
}

// A character write lands at the address counter, which then steps.  CGRAM
// writes are accepted and discarded — no custom glyph the ROM defines could
// change what a text assertion reads.
static void lcd_data(config_t *cfg, uint8_t ch) {
    tnt_lcd_t *l = lcd(cfg);
    if (!l)
        return;
    l->writes++;
    if (!l->cgram)
        l->ddram[l->addr & (ANS_LCD_DDRAM - 1u)] = ch;
    l->addr = (uint8_t)((l->addr + (l->entry_inc ? 1 : -1)) & (ANS_LCD_DDRAM - 1u));
}

// ============================================================
// Register interface (GBUS device 3)
// ============================================================

void tnt_lcd_write8(config_t *cfg, uint32_t offset, uint8_t value) {
    switch (offset & 0x30u) {
    case ANS_LCD_CMD:
        lcd_command(cfg, value);
        return;
    case ANS_LCD_DATA:
        lcd_data(cfg, value);
        return;
    case ANS_LCD_TBEN:
        // The register is a halfword; the guest may reach it a byte at a
        // time, so merge into the shadow rather than replacing it.
        tnt_gbus_tben_write(cfg, (uint16_t)((tnt_gbus_tben_read(cfg) & (offset & 1u ? 0xFF00u : 0x00FFu)) |
                                            ((uint16_t)value << (offset & 1u ? 0 : 8))));
        return;
    default: // ANS_LCD_MISC
        tnt_gbus_misc_write(cfg, (uint16_t)((tnt_gbus_misc_read(cfg) & (offset & 1u ? 0xFF00u : 0x00FFu)) |
                                            ((uint16_t)value << (offset & 1u ? 0 : 8))));
        return;
    }
}

uint8_t tnt_lcd_read8(config_t *cfg, uint32_t offset) {
    switch (offset & 0x30u) {
    case ANS_LCD_TBEN:
        return (uint8_t)(tnt_gbus_tben_read(cfg) >> (offset & 1u ? 0 : 8));
    case ANS_LCD_MISC:
        return (uint8_t)(tnt_gbus_misc_read(cfg) >> (offset & 1u ? 0 : 8));
    default:
        // WRITE ONLY (§4.2.3).  Expecting readback would invent state the
        // hardware does not have, so this reads zero and says so.
        LOG(1, "read of the WRITE-ONLY LCD +$%02X -> 0", offset & 0x3Fu);
        return 0;
    }
}

// ============================================================
// Text extraction
// ============================================================

void tnt_lcd_line(config_t *cfg, int line, char *out, size_t out_size) {
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    tnt_lcd_t *l = lcd(cfg);
    if (!l || line < 0 || line >= ANS_LCD_LINES)
        return;
    size_t n = ANS_LCD_COLS;
    if (n > out_size - 1)
        n = out_size - 1;
    const uint8_t *src = l->ddram + lcd_line_base[line];
    for (size_t i = 0; i < n; i++) {
        uint8_t c = src[i];
        out[i] = (c >= 0x20u && c < 0x7Fu) ? (char)c : ' ';
    }
    out[n] = '\0';
    // Trim trailing spaces: POST pads every string to the full width, and a
    // test that had to count the padding would be asserting on the padding.
    while (n > 0 && out[n - 1] == ' ')
        out[--n] = '\0';
}

// ============================================================
// machine.lcd — the object node
// ============================================================
// `line` is a METHOD rather than an indexed child: a display line carries
// exactly one value, so `machine.lcd.line(2)` says what `line[2].text`
// would, without four child objects to own and tear down.

static value_t lcd_attr_text(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    char buf[ANS_LCD_LINES * (ANS_LCD_COLS + 1) + 1];
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; i < ANS_LCD_LINES; i++) {
        char line[ANS_LCD_COLS + 1];
        tnt_lcd_line(cfg, i, line, sizeof(line));
        int n = snprintf(buf + used, sizeof(buf) - used, "%s%s", i ? "\n" : "", line);
        if (n > 0)
            used += (size_t)n;
    }
    return val_str(buf);
}

static value_t lcd_attr_cursor(struct object *self, const member_t *m) {
    (void)m;
    tnt_lcd_t *l = lcd((config_t *)object_data(self));
    return val_uint(1, l ? l->addr : 0);
}

static value_t lcd_attr_writes(struct object *self, const member_t *m) {
    (void)m;
    tnt_lcd_t *l = lcd((config_t *)object_data(self));
    return val_uint(4, l ? l->writes : 0);
}

static value_t lcd_attr_commands(struct object *self, const member_t *m) {
    (void)m;
    tnt_lcd_t *l = lcd((config_t *)object_data(self));
    return val_uint(4, l ? l->commands : 0);
}

static value_t lcd_attr_on(struct object *self, const member_t *m) {
    (void)m;
    tnt_lcd_t *l = lcd((config_t *)object_data(self));
    return val_bool(l && l->display_on);
}

static value_t lcd_method_line(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    if (argc < 1)
        return val_err("line: want a line number 0..%d", ANS_LCD_LINES - 1);
    bool ok = false;
    int64_t n = val_as_i64(&argv[0], &ok);
    if (!ok || n < 0 || n >= ANS_LCD_LINES)
        return val_err("line: %lld is outside 0..%d", (long long)n, ANS_LCD_LINES - 1);
    char buf[ANS_LCD_COLS + 1];
    tnt_lcd_line(cfg, (int)n, buf, sizeof(buf));
    return val_str(buf);
}

static const arg_decl_t lcd_line_args[] = {
    {.name = "line", .kind = V_INT, .doc = "display line, 0 (top) to 3 (bottom)"},
};

static const member_t tnt_lcd_members[] = {
    {.kind = M_ATTR,
     .name = "text",
     .doc = "All four display lines, newline-separated, trailing padding trimmed",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = lcd_attr_text, .set = NULL}},
    {.kind = M_ATTR,
     .name = "cursor",
     .doc = "The controller's address counter (DDRAM address)",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = lcd_attr_cursor, .set = NULL}},
    {.kind = M_ATTR,
     .name = "writes",
     .doc = "Character writes to the data register since power-on",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = lcd_attr_writes, .set = NULL}},
    {.kind = M_ATTR,
     .name = "commands",
     .doc = "Command-register writes since power-on",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = lcd_attr_commands, .set = NULL}},
    {.kind = M_ATTR,
     .name = "on",
     .doc = "Display enabled (HD44780 display on/off control)",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = lcd_attr_on, .set = NULL}},
    {.kind = M_METHOD,
     .name = "line",
     .doc = "One display line as a string (0 = top)",
     .method = {.args = lcd_line_args, .nargs = 1, .result = V_STRING, .fn = lcd_method_line}},
};

static const class_desc_t tnt_lcd_class = {
    .name = "lcd",
    .members = tnt_lcd_members,
    .n_members = sizeof(tnt_lcd_members) / sizeof(tnt_lcd_members[0]),
};

// ============================================================
// Lifecycle
// ============================================================

void tnt_lcd_reset(config_t *cfg) {
    tnt_lcd_t *l = lcd(cfg);
    if (!l)
        return;
    // Power-on: blank panel, display off, counter at home, incrementing.
    // POST's own init sequence sets all of this again; the state here is
    // what a panel that has never been written looks like.
    memset(l->ddram, ' ', sizeof(l->ddram));
    l->addr = 0;
    l->cgram = false;
    l->entry_inc = true;
    l->display_on = false;
    l->last_cmd = 0;
    l->writes = 0;
    l->commands = 0;
}

void tnt_lcd_init(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_lcd_reset(cfg);
    st->lcd_object = object_new(&tnt_lcd_class, cfg, "lcd");
    if (st->lcd_object) {
        object_set_label(st->lcd_object, "Front Panel LCD");
        object_set_order(st->lcd_object, 121);
        object_attach(machine_object(), st->lcd_object);
    }
}

void tnt_lcd_teardown(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (st && st->lcd_object) {
        object_detach(st->lcd_object);
        object_delete(st->lcd_object);
        st->lcd_object = NULL;
    }
}
