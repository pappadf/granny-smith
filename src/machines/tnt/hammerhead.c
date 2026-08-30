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
//   +$E0  L2 config/status, byte — bit $80 = cache present, bits 1:0 a size
//         code.  Reading $00 makes the ROM's .L2DataCacheTest skip the whole
//         test, the safest no-L2 presentation (decoded from the shipping
//         ROM's HWInit/POST disassembly).  The SIZE CODE is no longer
//         unattested: the Network Server ROM prints the size it decoded on
//         the front-panel LCD, so sweeping the register and reading line 3
//         gives the encoding directly —
//
//             $80 -> `0512KB Level 2 Cache`   $82 -> `1024KB Level 2 Cache`
//             $81 -> `0256KB Level 2 Cache`   $83 -> `4096KB Level 2 Cache`
//
//         and bit $04 is ignored ($84-$87 repeat $80-$83).  So bits 1:0 are
//         the size and 512 KB — not 256 — is code zero.
//   +$F0  L2 flush/fill strobe, byte — written $80 / $00 around the fill
//         walk; accepted with no side effect.
//
//   +$1C0..+$4F0  the DRAM BANK BASE registers, a pair per bank for 26
//         banks (Apple, "Power Macintosh 7500 and 8500 Computers"
//         Developer Note, 1995: software initialises "the address mode
//         bits in the bank base registers" while sizing RAM).  Fitted to
//         the Network Server ROM's POST (the sizing loop at $FFF04468 and
//         the pair-merging pass at $FFF046C0): bank k has its "A" word at
//         +$1C0+$20k and its "B" word at +$1D0+$20k; B[31:24] is the base
//         address in 4 MB units and A[24] is address bit 30, so the bank
//         decodes from base = (B[31:24] << 22) | (A[24] << 30).  A[25] is
//         toggled around the probe (an address-mode bit, indifferent here)
//         and A[26] marks a bank interleaved with its neighbour (POST
//         sets it on equal-sized adjacent banks; the union of the two is
//         contiguous either way, so it changes nothing in the decode).
//         POST's power-on table puts bank k at k x 64 MB, probes each
//         window top-down in 1 MB steps for the highest address that
//         holds a pattern and then upward for the alias, and re-bases
//         the banks it found contiguously from 0 -- so a bank must show
//         its DIMM, ALIASED, through a 64 MB window at whatever base its
//         registers name, and an empty bank must read nothing back
//         (tnt_hh_remap).  The merging pass then pairs adjacent
//         equal-sized banks (2k, 2k+1): the LOWER bank's A[26] is set and
//         the UPPER bank's base is rewritten to the END of the pair, so
//         the pair decodes as one region of twice the size from the lower
//         base -- interleaved on the board, contiguous to software.  The sizes it finds are cached in NVRAM
//         ($1048 + 8k: base, size) for Open Firmware's `dimm-sizes` and
//         the diagnostic utility's hardware configuration.
//
// Everything else is accept-and-readback, and every access is logged.

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

// The bits of +$E0 that are a hardware strap rather than software state:
// $80 = cache present, $03 = the size code.
#define HH_L2CFG_STRAP_MASK 0x83u

// The machine-identification register at +$20: bit 30 marks the 9500
// (the 68k identification at ROM $FFC1484E tests it before BoxID), and
// bit 31 marks the 7500/8500 class — Open Firmware folds the top byte
// into its model selector as (b>>5)|((b>>1)&8) ($80 -> 7500/8500,
// $40 -> 9500) and falls back to "AAPL,????" (no display nodes) when
// neither bit is set.  The +$30 top byte feeds the AAPL,cpu-id low
// nibble the same way (>>4); it is unattested and currently zero.
#define HH_REG_MACHID 0x20u

// The bank base register pairs.
#define HH_BANK_A(k)     (0x1C0u + 0x20u * (k)) // A word: bit 24 = base bit 30, 25 = mode, 26 = interleaved
#define HH_BANK_A_ILV    0x04000000u // A[26]: this bank and the next form an interleaved pair
#define HH_BANK_B(k)     (0x1D0u + 0x20u * (k)) // B word: bits 31:24 = base >> 22
#define HH_BANK_REGS_END 0x500u
#define HH_BANK_WINDOW   0x04000000u // 64 MB: the most a bank decodes, and POST's probe window
#define HH_DECODE_TOP    0x80000000u // DRAM decode space; PCI and the islands live above

// Where the DIMM slots sit among the banks (the ROM's `set-dimm-sizes`:
// slot pair i is banks 2+4i..5+4i, side A = the even banks, side B = the
// odd ones, each DIMM's two sides summed).  A DIMM here occupies its
// first bank whole; the second stays empty.
#define HH_DIMM_PAIRS            4
#define HH_DIMM_BANK(pair, side) (2u + 4u * (pair) + (side))

// The +$E0 strap for this board's cache DIMM: presence plus the size code
// decoded above.  A board with no L2 reads $00, which is what makes the
// ROM's .L2DataCacheTest skip the whole test — the Macintosh boards' answer
// and, until the Network Servers, the only one this model ever gave.
static uint8_t hh_l2cfg_strap(config_t *cfg) {
    switch (tnt_board(cfg)->l2_kb) {
    case 256u:
        return 0x81u;
    case 512u:
        return 0x80u;
    case 1024u:
        return 0x82u;
    case 4096u:
        return 0x83u;
    case 0u:
        return 0x00u; // no cache DIMM fitted
    default:
        LOG(0, "board declares an L2 size of %u KB, which has no +$E0 size code; reporting no cache",
            tnt_board(cfg)->l2_kb);
        return 0x00u;
    }
}

// Carve the profile's RAM into DIMM pairs (Apple fits the Network Server's
// slots in matched pairs): the largest power-of-two DIMM that fits half
// the remainder, up to 64 MB, per pair -- 64 MB is 32+32 in slot pair 1,
// 48 MB is 16+16 then 8+8, 512 MB fills all four pairs with 64+64.
static void hh_bank_inventory(config_t *cfg) {
    tnt_hammerhead_t *hh = &tnt_st(cfg)->hh;
    uint32_t rest = cfg->ram_size;
    uint32_t off = 0;
    for (unsigned pair = 0; pair < HH_DIMM_PAIRS && rest >= 0x200000u; pair++) {
        uint32_t half = rest / 2;
        uint32_t dimm = HH_BANK_WINDOW;
        while (dimm > half)
            dimm >>= 1;
        for (unsigned side = 0; side < 2; side++) {
            hh->bank_size[HH_DIMM_BANK(pair, side)] = dimm;
            hh->bank_host_off[HH_DIMM_BANK(pair, side)] = off;
            off += dimm;
        }
        rest -= 2 * dimm;
    }
    if (rest)
        LOG(0, "RAM size $%X does not decompose into DIMM pairs; $%X unmapped", cfg->ram_size, rest);
}

static uint32_t hh_bank_base(const tnt_hammerhead_t *hh, unsigned k) {
    uint32_t a = hh->reg[HH_BANK_A(k) >> 4];
    uint32_t b = hh->reg[HH_BANK_B(k) >> 4];
    return ((b >> 24) << 22) | ((a & 0x01000000u) ? 0x40000000u : 0u);
}

// Rebuild the DRAM decode from the bank registers: every bank with a DIMM
// shows it, aliased, through a window from its base up to the next bank's
// base or 64 MB, whichever comes first; everything else in the decode
// space reads nothing (cleared pages read 0, which fails POST's pattern
// compare -- the "empty banks must not echo" requirement the sizing loop
// depends on).
void tnt_hh_remap(config_t *cfg) {
    tnt_hammerhead_t *hh = &tnt_st(cfg)->hh;
    uint8_t *ram = ram_native_pointer(cfg->mem_map, 0);
    for (uint32_t p = 0; p < (HH_DECODE_TOP >> PAGE_SHIFT); p++)
        tnt_clear_page(p);
    for (unsigned k = 0; k < TNT_HH_BANKS; k++) {
        uint32_t size = hh->bank_size[k];
        // An interleaved pair is decoded by its lower bank, as one region
        // of both banks' storage (adjacent in host RAM by construction);
        // the upper bank's own base register then only marks the end.
        bool ilv_lower = (hh->reg[HH_BANK_A(k) >> 4] & HH_BANK_A_ILV) && k + 1 < TNT_HH_BANKS &&
                         hh->bank_size[k + 1] == size && hh->bank_host_off[k + 1] == hh->bank_host_off[k] + size;
        bool ilv_upper = k > 0 && (hh->reg[HH_BANK_A(k - 1) >> 4] & HH_BANK_A_ILV) && hh->bank_size[k - 1] == size &&
                         hh->bank_host_off[k] == hh->bank_host_off[k - 1] + size;
        if (ilv_lower)
            size *= 2;
        if (!size || ilv_upper)
            continue;
        uint32_t base = hh_bank_base(hh, k);
        if (base >= HH_DECODE_TOP)
            continue;
        uint32_t window = ilv_lower ? 2 * HH_BANK_WINDOW : HH_BANK_WINDOW;
        for (unsigned j = 0; j < TNT_HH_BANKS; j++) {
            if (j == k)
                continue;
            uint32_t other = hh_bank_base(hh, j);
            if (other > base && other - base < window)
                window = other - base;
        }
        if (base + window > HH_DECODE_TOP)
            window = HH_DECODE_TOP - base;
        uint8_t *host = ram + hh->bank_host_off[k];
        uint32_t first = base >> PAGE_SHIFT;
        for (uint32_t p = 0; p < (window >> PAGE_SHIFT); p++)
            tnt_fill_page(first + p, host + ((p << PAGE_SHIFT) % size), true);
        LOG(3, "bank %u: $%X bytes at $%08X (window $%X)", k, size, base, window);
    }
}

void tnt_hh_init(config_t *cfg) {
    tnt_hammerhead_t *hh = &tnt_st(cfg)->hh;
    memset(hh, 0, sizeof(*hh));
    hh_bank_inventory(cfg);
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
    hh->reg[HH_REG_L2CFG >> 4] = (uint32_t)hh_l2cfg_strap(cfg) << 24;
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
    // +$E0's presence bit and size code are a STRAP — what the cache DIMM
    // socket reports, not something software sets.  The ROM writes the
    // register (it drops $70 in there mid-test) and reads it straight back,
    // so a plain store-and-readback would erase the cache the machine has
    // and the size report would come out as `0000KB`.  The strapped bits
    // therefore survive every write; the rest latch normally.
    if (lane == 0 && (offset >> 4) == (HH_REG_L2CFG >> 4)) {
        uint8_t strap = hh_l2cfg_strap(cfg);
        value = (uint8_t)((value & (uint8_t)~HH_L2CFG_STRAP_MASK) | (strap & HH_L2CFG_STRAP_MASK));
    }
    uint32_t *reg = &hh->reg[offset >> 4];
    uint32_t shift = 8 * (3 - lane);
    *reg = (*reg & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    LOG(2, "write +$%03X = $%02X (reg now $%08X)", offset, value, *reg);
    // The identifier keeps its identity upper halfword whatever is
    // written (accept-and-readback everywhere else).
    if ((offset >> 4) == (HH_REG_ID >> 4))
        hh->reg[HH_REG_ID >> 4] = (hh->reg[HH_REG_ID >> 4] & 0x0000FFFFu) | (tnt_board(cfg)->hh_id & 0xFFFF0000u);
    // A bank base moved: the DRAM decode follows it.
    if (offset >= HH_BANK_A(0) && offset < HH_BANK_REGS_END && lane == 0)
        tnt_hh_remap(cfg);
}
