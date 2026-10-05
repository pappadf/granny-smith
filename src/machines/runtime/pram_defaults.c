// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pram_defaults.c
// See pram_defaults.h.  The values are what each ROM's own `_InitUtil`
// writes into an invalid XPRAM (pram.md §4), measured per ROM; the store
// then gets what a System that has booted leaves (the no-wait bit in $01
// everywhere, rtc.h; MMFlags bit 5 on the PDM family).

#include "pram_defaults.h"

// PRAMInitTbl, $76..$89 (pram.md §4.2): default OS Macintosh, default
// startup device the SCSI driver refnum for id 0 ($FFFFFFDF).
static const uint8_t k_startmgr_std[PRAM_STARTMGR_LEN] = {
    0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xDF, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// The IIci and IIsi ROMs differ in one byte: $81 = $80, the default video
// device (pram.md §7) -- the built-in video.
static const uint8_t k_startmgr_iici[PRAM_STARTMGR_LEN] = {
    0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xDF, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// SysParam as each ROM's PRAMInit leaves it (logical $00..$13): measured by
// booting each ROM from a blank store and reading the bytes back.  The first
// sixteen are the same on every ROM -- valid ($A8), both serial ports 9600
// 8N2 and unused, Geneva, auto-key $63 -- and the last four differ by ROM
// generation: volume and mouse tracking ($10), double-click and caret blink
// ($11), disk cache ($12) and the misc flags ($13).
#define SYSPARAM_COMMON 0xA8, 0x00, 0x00, 0x00, 0xCC, 0x0A, 0xCC, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x63, 0x00
// Plus, SE/30, IIx, IIcx.
static const uint8_t k_sysparam_ii[20] = {SYSPARAM_COMMON, 0x03, 0x88, 0x00, 0x4C};
// IIci, IIsi, IIfx, Quadra 700 and 900.
static const uint8_t k_sysparam_32[20] = {SYSPARAM_COMMON, 0x13, 0x88, 0x00, 0x4C};
// Quadra 950, Quadra 840AV / Centris 660AV.
static const uint8_t k_sysparam_q950[20] = {SYSPARAM_COMMON, 0x13, 0x88, 0x00, 0xCC};
// Power Macintosh 6100/7100/8100 and the 7500/8500/9500 (the TNT ROM, in
// its NVRAM's XPRAM partition; the Network Server ROM runs no Mac OS and
// writes none, so it takes the TNT's).
static const uint8_t k_sysparam_ppc[20] = {SYSPARAM_COMMON, 0x1B, 0x88, 0x03, 0xCC};
// Power Macintosh G3: a larger disk cache.
static const uint8_t k_sysparam_g3[20] = {SYSPARAM_COMMON, 0x1B, 0x88, 0x08, 0xCC};

#define TOKEN_NUMC 0x4E754D63u // 'NuMc'
#define TOKEN_BUGS 0x42756773u // 'Bugs'

// The Plus ROM validates XPRAM with 'Bugs' and writes no Start Manager
// table (it has no Start Manager of that kind) nor MMFlags.
const pram_defaults_t pram_defaults_plus = {
    .xpram_token = TOKEN_BUGS,
    .startmgr = NULL,
    .mmflags = 0x00,
    .sysparam = k_sysparam_ii,
};

// MMFlags $00: measured on every one of these ROMs (pram.md used to say $80).
// Their ROMs are not 32-bit clean: 32-bit addressing needs MODE32.
const pram_defaults_t pram_defaults_mac_ii = {
    .xpram_token = TOKEN_NUMC,
    .startmgr = k_startmgr_std,
    .mmflags = 0x00,
    .sysparam = k_sysparam_ii,
    .addressing = PRAM_ADDRESSING_MODE32,
};

// The IIfx and the Quadra 700/900 ROMs: the Mac II store with the later
// SysParam.
const pram_defaults_t pram_defaults_iifx = {
    .xpram_token = TOKEN_NUMC,
    .startmgr = k_startmgr_std,
    .mmflags = 0x00,
    .sysparam = k_sysparam_32,
    .addressing = PRAM_ADDRESSING_SELECTABLE,
};

const pram_defaults_t pram_defaults_q950 = {
    .xpram_token = TOKEN_NUMC,
    .startmgr = k_startmgr_std,
    .mmflags = 0x00,
    .sysparam = k_sysparam_q950,
    .addressing = PRAM_ADDRESSING_SELECTABLE,
};

const pram_defaults_t pram_defaults_iici = {
    .xpram_token = TOKEN_NUMC,
    .startmgr = k_startmgr_iici,
    .mmflags = 0x00,
    .sysparam = k_sysparam_32,
    .addressing = PRAM_ADDRESSING_SELECTABLE,
};

const pram_defaults_t pram_defaults_av = {
    .xpram_token = TOKEN_NUMC,
    .startmgr = k_startmgr_std,
    .mmflags = 0x05,
    .sysparam = k_sysparam_q950,
    .addressing = PRAM_ADDRESSING_SELECTABLE,
};

// Bit 5 (D-2a): on blank MMFlags Mac OS 8.1 selects its DR emulator, sets
// the bit and soft-restarts -- the double boot, two chimes, on every fresh
// machine.  7.5 and 8.1 both leave it set once booted (7.5: $65 measured),
// so a store that has booted before has it.
const pram_defaults_t pram_defaults_pdm = {
    .xpram_token = TOKEN_NUMC,
    .startmgr = k_startmgr_std,
    .mmflags = 0x05,
    .mmflags_booted = 0x20,
    .sysparam = k_sysparam_ppc,
};

// The Open Firmware machines keep this PRAM in their 8 KB NVRAM, not in
// Cuda (of_nvram.h).  Measured the same way: each ROM booted from a blank
// store, the XPRAM read back after its cold init.  Both TNT ROMs (v1, v2)
// write the same bytes, as do the G3 Rev A and Rev C ROMs.  Neither writes
// the 68k default startup device: the TNT ROM leaves $78..$7B zero (SCSI
// target 0), the G3 ROM writes $6666, "no default".
static const uint8_t k_startmgr_tnt[PRAM_STARTMGR_LEN] = {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t k_startmgr_g3[PRAM_STARTMGR_LEN] = {
    0x00, 0x01, 0x00, 0x00, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const pram_byte_t k_extra_tnt[] = {
    {0xB1, 0x30}
};
static const pram_byte_t k_extra_g3[] = {
    {0xB1, 0x70}
};

const pram_defaults_t pram_defaults_tnt = {
    .xpram_token = TOKEN_NUMC,
    .startmgr = k_startmgr_tnt,
    .mmflags = 0x25,
    .extra = k_extra_tnt,
    .n_extra = 1,
    .sysparam = k_sysparam_ppc,
};

const pram_defaults_t pram_defaults_g3 = {
    .xpram_token = TOKEN_NUMC,
    .startmgr = k_startmgr_g3,
    .mmflags = 0x25,
    .extra = k_extra_g3,
    .n_extra = 1,
    .sysparam = k_sysparam_g3,
};
