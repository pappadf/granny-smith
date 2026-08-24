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
// (the 68k identification at ROM $FFC1484E tests it before BoxID).
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
    uint32_t *reg = &hh->reg[offset >> 4];
    uint32_t shift = 8 * (3 - lane);
    *reg = (*reg & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    LOG(2, "write +$%03X = $%02X (reg now $%08X)", offset, value, *reg);
    // The identifier keeps its identity upper halfword whatever is
    // written (accept-and-readback everywhere else).
    if ((offset >> 4) == (HH_REG_ID >> 4))
        hh->reg[HH_REG_ID >> 4] = (hh->reg[HH_REG_ID >> 4] & 0x0000FFFFu) | (tnt_board(cfg)->hh_id & 0xFFFF0000u);
}
