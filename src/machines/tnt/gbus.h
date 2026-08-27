// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gbus.h
// The GBUS island — Grand Central's Generic Bus, and the whole of what
// makes an Apple Network Server a server rather than a Power Macintosh.
//
// Apple, "Network Server Hardware Developer Notes", 1996, §4.2.1: Grand
// Central "provides six chip selects and write enable which the Network
// Server uses for devices such as NVRAM, Ethernet PROM, board registers,
// and the LCD."  On a 7500/8500/9500 those chip selects are mostly idle;
// on the ANS they carry the front-panel LCD, the keyswitch, the hardwired
// box identifier, the environmental ("Safe Server") status register, the
// Ethernet address PROM and the interprocessor doorbell.
//
// The island decode is `1_dddd_RRRR_RRRR_rrrr`, so device `dddd` occupies
// offset `dddd`*$1000 + $10000.  The 0x0-0x7 half is the Macintosh boards'
// and is already modelled (grand_central.c); this file owns the ANS half:
//
//   0x9  $19000  Ethernet address PROM (+ the MP doorbell)
//   0xA  $1A000  Board Register 1 — BoxID + keyswitch + TwoSuppliesH
//   0xC  $1C000  LCD command / $1C010 LCD data / $1C020 timebase enable
//   0xD  $1D000  NVRAM high address    (already modelled)
//   0xE  $1E000  Board Register 2 — the environmental status halfword
//   0xF  $1F000  NVRAM data            (already modelled)
//
// EVERY board-register bit is ACTIVE LOW unless its name ends in `H`, so
// "everything healthy" is all-ones, not zero.  And Board Register 2 is
// POLLED, never signalled: "Network Server does not implement an interrupt
// for these functions.  Software can implement a daemon that does
// background reads of this register (say every 30 seconds or so)."  There
// is therefore no event plumbing here at all, and none is needed.

#ifndef GS_MACHINES_TNT_GBUS_H
#define GS_MACHINES_TNT_GBUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct config;
struct object;
typedef struct config config_t;

// === GBUS island offsets (relative to $F3000000) ============================
#define ANS_OFF_EPROM 0x19000u // Ethernet address PROM + MP doorbell
#define ANS_OFF_BREG1 0x1A000u // Board Register 1 (== the Macintosh BoxID)
#define ANS_OFF_LCD   0x1C000u // GBUS device 3: LCD + timebase enable
#define ANS_OFF_BREG2 0x1E000u // Board Register 2 (environment)

// GBUS device 3's four register centres.
#define ANS_LCD_CMD  0x00u // R (Command) register — WRITE ONLY
#define ANS_LCD_DATA 0x10u // S (Data) register — WRITE ONLY
#define ANS_LCD_TBEN 0x20u // bit 15 gates the 604 timebases (MP sync)
#define ANS_LCD_MISC 0x30u // undocumented; POST writes $FFFF once (see gbus.c)

// === Board Register 1, the ANS top byte (little-endian bit numbering) =======
// b10 not connected / b11 BoxId0 = 1 / b12 BoxId1 = 0 / b13 Keyswitch
// ServiceL / b14 Keyswitch LockedL / b15 TwoSuppliesH.  Bits 0-9 are the
// Macintosh boards' and are contributed by grand_central.c.
#define ANS_BREG1_SERVICE_L 0x2000u // active low: 0 = the key is in Service
#define ANS_BREG1_LOCKED_L  0x4000u // active low: 0 = the key is Locked
#define ANS_BREG1_TWO_PSU_H 0x8000u // active HIGH: 1 = redundant supplies

// === Board Register 2, the "Safe Server" environmental byte =================
// All active low, so a healthy machine reads them all as ones.
#define ANS_ENV_FAN_DRIVE 0x0100u // b8  FanFailDrive
#define ANS_ENV_FAN_PROC  0x0200u // b9  FanFailProcessor
#define ANS_ENV_TEMP_FAIL 0x0400u // b10 TempFailProcessor (check-over-temp's mask)
#define ANS_ENV_TEMP_WARN 0x0800u // b11 TempWarnProcessor
#define ANS_ENV_PSU_LEFT  0x1000u // b12 FailPowSupplyLeft
#define ANS_ENV_PSU_RIGHT 0x2000u // b13 FailPowSupplyRight
#define ANS_ENV_HOT_LEFT  0x4000u // b14 powSupplyHotLeft
#define ANS_ENV_HOT_RIGHT 0x8000u // b15 powSupplyHotRight

// The front panel's three-position keyswitch — the one SOFTWARE reads.
// (The REAR keyswitch is a separate physical switch that gates power and
// the logic-board drawer; it is `rear_locked` below, and it is a power-on
// PRECONDITION rather than something the guest can see.)
typedef enum ans_keyswitch {
    ANS_KEY_LOCKED = 0, // Open Firmware "prevents all parameter and NVRAM resets"
    ANS_KEY_SERVICE, // Cmd-Opt-P-R "will erase all AIX-related booting parameters",
                     // and a never-booted machine hunts for an Install CD
    ANS_KEY_NORMAL, // Cmd-Opt-P-R erases "the Macintosh parameter RAM only"
} ans_keyswitch_t;

// === Ethernet address PROM ==================================================
// Sixteen bytes on a $10 stride: group ID high/mid/low at $00/$10/$20,
// sequencing address at $30/$40/$50, $AA at $60 signifying normal bit
// ordering, an inverted-XOR checksum at $70, the same six bytes INVERTED at
// $80-$D0, $55 at $E0 for reverse bit ordering, and a checksum at $F0.
// (Apple prints the base as `xF0319000`, which is inconsistent with Grand
// Central at $F3000000 — a typo for $F3019000, confirmed by §4.2
// independently citing "offset x19_000".)
#define ANS_EPROM_CELLS 16

// === GBUS state =============================================================
typedef struct tnt_gbus {
    uint8_t keyswitch; // ans_keyswitch_t — the FRONT switch, software-visible
    uint8_t rear_locked; // the REAR switch: a power-on precondition
    uint16_t env_faults; // set bit = that fault is INJECTED (register shows ~this)
    uint16_t tb_enable; // $1C020 store-and-readback (bit 15 = timebases run)
    uint16_t misc; // $1C030 store-and-readback (undocumented; see gbus.c)
    uint8_t eprom[ANS_EPROM_CELLS]; // the sixteen PROM cells, in cell order
    uint32_t doorbell; // accesses to $19000 space (the SecToPri_Int doorbell)
} tnt_gbus_t;

// === LCD state (lcd.c) ======================================================
// A Hitachi HD44780-class character controller behind two WRITE-ONLY
// registers.  Geometry is 4 lines x 20 columns — which the shipping ROM
// proves rather than assumes: its POST strings are padded to exactly 20
// characters and its line-select commands are $80 / $C0 / $94 / $D4, the
// canonical 4x20 DDRAM map (line 0 at $00, line 1 at $40, line 2 at $14,
// line 3 at $54).  Modelling the DDRAM directly rather than four separate
// line buffers gets the controller's famous non-contiguous wrap right for
// free.
#define ANS_LCD_LINES 4
#define ANS_LCD_COLS  20
#define ANS_LCD_DDRAM 128

typedef struct tnt_lcd {
    uint8_t ddram[ANS_LCD_DDRAM]; // display data RAM (ASCII, space-filled)
    uint8_t addr; // the address counter (0..127)
    bool cgram; // the counter currently addresses CGRAM, not DDRAM
    bool entry_inc; // entry mode: the counter increments after each write
    bool display_on;
    uint8_t last_cmd; // most recent command byte (diagnostics)
    uint32_t writes; // data writes since power-on
    uint32_t commands; // command writes since power-on
} tnt_lcd_t;

// === gbus.c =================================================================

void tnt_gbus_init(config_t *cfg); // power-on state + machine.board node
void tnt_gbus_reset(config_t *cfg); // power-on register state (keyswitch survives)
void tnt_gbus_teardown(config_t *cfg);

// The ANS bits Board Register 1 adds on top of the Macintosh BoxID straps:
// the keyswitch pair and TwoSuppliesH.  grand_central.c ORs this in.
uint32_t tnt_gbus_boxid_bits(config_t *cfg);

// Island access for the ANS GBUS blocks.  Byte-wide cells decode bytes;
// the 32-bit forms follow the family's little-endian register convention
// (the guest reads them with lwbrx, so the model swaps at the bus edge).
uint8_t tnt_gbus_read8(config_t *cfg, uint32_t offset);
void tnt_gbus_write8(config_t *cfg, uint32_t offset, uint8_t value);
uint32_t tnt_gbus_read32(config_t *cfg, uint32_t offset);
void tnt_gbus_write32(config_t *cfg, uint32_t offset, uint32_t value);

// === lcd.c ==================================================================

void tnt_lcd_init(config_t *cfg); // power-on state + machine.lcd node
void tnt_lcd_reset(config_t *cfg);
void tnt_lcd_teardown(config_t *cfg);
// GBUS device 3, offsets $00 (command) / $10 (data) / $20 / $30.
uint8_t tnt_lcd_read8(config_t *cfg, uint32_t offset);
void tnt_lcd_write8(config_t *cfg, uint32_t offset, uint8_t value);
// One display line as a NUL-terminated 20-character string (trailing
// spaces trimmed).  `line` is 0..3; out of range yields an empty string.
void tnt_lcd_line(config_t *cfg, int line, char *out, size_t out_size);

#endif // GS_MACHINES_TNT_GBUS_H
