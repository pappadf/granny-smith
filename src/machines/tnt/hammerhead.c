// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// hammerhead.c
// Hammerhead (343S1142) — system-bus, DRAM, ROM and L2 cache controller;
// the north bridge of the TNT machines.  No operating system ever programs
// it: the ROM's Open Firmware sizes memory itself and every OS then reads
// the device tree, so the emulation contract is a benign register file
// whose handful of attested offsets return sane values.
//
// The attested registers (all sources agree on $10 centres, big-endian on
// the processor bus — this is the one non-little-endian block on the
// machine):
//   +$00  part identifier, 32-bit — the ROM's POST reads it and requires
//         the upper halfword to be $3001.
//   +$90  ArbConfig, byte — bus arbitration; bit $02 "TwoCPU" reads clear
//         on a uniprocessor (OSF/Apple MkLinux DR3, MPPlugIn.h).
//   +$B0  WhoAmI, byte — $10 = primary CPU, $08 = secondary (same source).
//   +$C0  IntReg, byte — inter-processor interrupt; writable, and a no-op
//         with one CPU.
//   +$E0  L2 config/status, byte — bit $80 = cache present, low 3 bits a
//         size code.  Reading $00 makes the ROM's .L2DataCacheTest skip the
//         whole test, the safest no-L2 presentation (decoded from the
//         shipping ROM's HWInit/POST disassembly).
//   +$F0  L2 flush/fill strobe, byte — written $80 / $00 around the fill
//         walk; accepted with no side effect.
//
// Everything else is accept-and-readback.  The DRAM bank base registers
// exist (Apple, "Power Macintosh 7500 and 8500 Computers" Developer Note,
// 1995: software initialises "the address mode bits in the bank base
// registers" while sizing RAM) but their offsets are unattested — risk R1
// of the proposal.  Every access is therefore logged, so ladder rung T5
// (Open Firmware memory sizing) records the sequence the model must be
// fitted to.

#include "tnt.h"

#include "log.h"
#include "ppc.h" // r24 (the 68k PC) annotates the R1 access log

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("hammerhead");

// Attested offsets (from the Hammerhead window base $F8000000)
#define HH_REG_ID        0x00u // part identifier (32-bit)
#define HH_REG_ARBCONFIG 0x90u // ArbConfig: $02 = TwoCPU
#define HH_REG_WHOAMI    0xB0u // WhoAmI: $10 = primary CPU
#define HH_REG_INTREG    0xC0u // inter-processor interrupt
#define HH_REG_L2CFG     0xE0u // L2 present/size
#define HH_REG_L2STROBE  0xF0u // L2 flush/fill strobe

// The machine-identification register at +$20: bit 30 marks the 9500
// (the 68k identification at ROM $FFC1484E tests it before BoxID), and
// bit 31 marks the 7500/8500 class — Open Firmware folds the top byte
// into its model selector as (b>>5)|((b>>1)&8) ($80 -> 7500/8500,
// $40 -> 9500) and falls back to "AAPL,????" (no display nodes) when
// neither bit is set.  The +$30 top byte feeds the AAPL,cpu-id low
// nibble the same way (>>4); it is unattested and currently zero.
#define HH_REG_MACHID 0x20u

void tnt_hh_init(config_t *cfg) {
    tnt_hammerhead_t *hh = &tnt_st(cfg)->hh;
    memset(hh, 0, sizeof(*hh));
    // Power-on values for the registers software reads before writing.
    // Byte-wide registers live in LANE 0 of their $10-centre longword
    // (bits 31-24 — the byte a lbz of the register address returns).
    // The part identifier's FIRST BYTE is the model-family discriminator
    // the shipping ROM dispatches on ($39 = TNT, $3001xxxx = the 7200 /
    // Catalyst — decoded from the identification routine at $FFC14844;
    // the dossier's "$3001 required" reading was the Catalyst branch).
    hh->reg[HH_REG_ID >> 4] = tnt_board(cfg)->hh_id;
    hh->reg[HH_REG_MACHID >> 4] = tnt_board(cfg)->hh_r20;
    hh->reg[HH_REG_ARBCONFIG >> 4] = 0x00u; // TwoCPU clear: uniprocessor
    hh->reg[HH_REG_WHOAMI >> 4] = 0x10u << 24; // primary CPU
    hh->reg[HH_REG_L2CFG >> 4] = 0x00u; // no L2: POST skips the test
    // TEMP diagnostic (604 boot-wall hunt): present an L2 module.  Byte
    // registers live in lane 0, so the value is shifted to bits 31-24.
    // Bit $80 = present, low 3 bits = size code (encoding unattested; probe
    // with values $80-$87 and read what OF puts in the tree).  The probe
    // sequence observed live (read $81 / strobe $80 / WRITE +$E0=$70 /
    // read-back / strobe $00) shows +$E0 is read back after a write, so
    // while the override is armed the register is STICKY: reads always
    // return the env value (hardware-status semantics), writes are dropped.
    {
        const char *s = getenv("GS_HH_L2CFG");
        if (s) {
            hh->reg[HH_REG_L2CFG >> 4] = ((uint32_t)strtoul(s, NULL, 16) & 0xFFu) << 24;
            hh->l2cfg_sticky = true;
        }
    }
}

// Byte read.  The register file is 128 x 32-bit on $10 centres; within a
// centre the register occupies the FIRST longword (byte lanes 0-3,
// big-endian), and the attested byte registers are read at lane 0.  Bytes
// beyond +3 of a centre read zero.
uint8_t tnt_hh_read(config_t *cfg, uint32_t offset) {
    tnt_hammerhead_t *hh = &tnt_st(cfg)->hh;
    if (offset >= TNT_HH_REGS * 0x10u) {
        LOG(2, "read beyond the register file: +$%03X", offset);
        return 0;
    }
    uint32_t lane = offset & 0xFu;
    uint32_t value = hh->reg[offset >> 4];
    uint8_t b = (lane < 4) ? (uint8_t)(value >> (8 * (3 - lane))) : 0;
    // R1 instrumentation: every Hammerhead access is loggable so the T5
    // memory-sizing sequence can be recorded and the model fitted to it.
    LOG(2, "read  +$%03X -> $%02X (reg $%08X) r24=$%08X", offset, b, value, ppc_get_gpr(cfg->ppc, 24));
    return b;
}

void tnt_hh_write(config_t *cfg, uint32_t offset, uint8_t value) {
    tnt_hammerhead_t *hh = &tnt_st(cfg)->hh;
    if (offset >= TNT_HH_REGS * 0x10u) {
        LOG(2, "write beyond the register file: +$%03X = $%02X", offset, value);
        return;
    }
    uint32_t lane = offset & 0xFu;
    if (lane >= 4) {
        LOG(2, "write to dead lane +$%03X = $%02X", offset, value);
        return;
    }
    if (hh->l2cfg_sticky && (offset >> 4) == (HH_REG_L2CFG >> 4)) {
        LOG(2, "write +$%03X = $%02X dropped (GS_HH_L2CFG sticky)", offset, value);
        return;
    }
    uint32_t *reg = &hh->reg[offset >> 4];
    uint32_t shift = 8 * (3 - lane);
    *reg = (*reg & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    LOG(2, "write +$%03X = $%02X (reg now $%08X)", offset, value, *reg);
    // The identifier keeps its identity upper halfword whatever is
    // written (accept-and-readback everywhere else).
    if ((offset >> 4) == (HH_REG_ID >> 4))
        hh->reg[HH_REG_ID >> 4] = (hh->reg[HH_REG_ID >> 4] & 0x0000FFFFu) | (tnt_board(cfg)->hh_id & 0xFFFF0000u);
}
