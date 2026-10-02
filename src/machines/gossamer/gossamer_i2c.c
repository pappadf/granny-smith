// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gossamer_i2c.c
// The I2C bus behind Cuda on the beige G3 — the only I2C master the ROM
// and every driver use (Apple AppleCuda IIC calls; NetBSD cuda.c).  It
// carries:
//
//   * three SDRAM SPD EEPROMs, one per DIMM slot, at 7-bit $50/$51/$52
//     (8-bit $A0/$A2/$A4).  The boot program sizes RAM from them and from
//     nothing else: an empty slot NAKs (Cuda answers with an error
//     packet), a populated one returns the JEDEC SDRAM SPD bytes the ROM
//     turns into Grackle bank registers.  "A machine with no valid SPD
//     data does not size its RAM";
//   * the personality card's ID EEPROM at 7-bit $53 (8-bit $A6), from
//     which Open Firmware builds the /perch node (Whisper: `0F AA 55 AA
//     "Whisper" 00 .. 02`, the rest of the first 32 bytes zero, then $FF);
//   * the TDA7433 tone/volume chip at 8-bit $8A, written by every audio
//     driver when /perch exists — a write sink here.
//
// The SPD image is the JEDEC 168-pin SDRAM layout (bytes 0-63), built
// from the DIMM geometry the profile's RAM size decomposes into: PC66
// class parts (10 ns tCK, CAS latency 2/3 — the developer note's "100
// MHz/10 ns or faster ... CAS latency of 3 ... burst length at least 4"),
// four internal banks, 64 data bits, no parity/ECC.

#include "gossamer.h"

#include "log.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("i2c");

#define SPD_ADDR0    0xA0u // 8-bit address of DIMM 0's SPD; DIMM k at +2k
#define PERCH_ADDR   0xA6u
#define TDA7433_ADDR 0x8Au

// One standard DIMM type, by size: rows/columns of its 64-Mbit/128-Mbit
// SDRAM parts, sides, bytes per side.  4 internal banks, x8 organisation
// (8 chips per 64-bit side).
typedef struct dimm_type {
    uint32_t mb;
    uint8_t sides, rows, cols;
} dimm_type_t;

static const dimm_type_t k_dimm_types[] = {
    {256, 2, 12, 10}, // 2 x 128 MB (128-Mbit parts)
    {128, 2, 12, 9 }, // 2 x 64 MB (64-Mbit x8)
    {64,  1, 12, 9 }, // 1 x 64 MB
    {32,  1, 12, 8 }, // 1 x 32 MB (64-Mbit x16)
};

// Bytes per side of a geometry: 2^(rows+cols) locations x 4 banks x 8 bytes.
static uint32_t side_bytes(uint8_t rows, uint8_t cols) {
    return (1u << (rows + cols)) * 4u * 8u;
}

// Decompose the RAM size into at most three DIMMs, largest first.
static void carve_dimms(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    memset(st->dimm, 0, sizeof(st->dimm));
    uint32_t left_mb = cfg->ram_size >> 20;
    for (unsigned k = 0; k < GOS_DIMMS && left_mb; k++) {
        for (size_t t = 0; t < sizeof(k_dimm_types) / sizeof(k_dimm_types[0]); t++) {
            const dimm_type_t *d = &k_dimm_types[t];
            // Take the largest module that fits and leaves a remainder the
            // remaining slots can still hold (<= 256 MB each).
            unsigned slots_after = GOS_DIMMS - k - 1;
            if (d->mb > left_mb || left_mb - d->mb > slots_after * 256u)
                continue;
            st->dimm[k].sides = d->sides;
            st->dimm[k].rows = d->rows;
            st->dimm[k].cols = d->cols;
            st->dimm[k].side_bytes = side_bytes(d->rows, d->cols);
            left_mb -= d->mb;
            break;
        }
    }
    if (left_mb)
        LOG(0, "RAM size %u MB does not decompose into three SDRAM DIMMs; %u MB is not presented", cfg->ram_size >> 20,
            left_mb);
}

// The JEDEC SDRAM SPD bytes for one DIMM.
static void build_spd(uint8_t *spd, const gos_dimm_t *d) {
    memset(spd, 0xFF, GOS_SPD_SIZE);
    memset(spd, 0, 128);
    uint32_t mb_side = d->side_bytes >> 20;
    spd[0] = 0x80; // bytes written by the manufacturer
    spd[1] = 0x08; // log2(total EEPROM bytes) = 256
    spd[2] = 0x04; // memory type: SDRAM
    spd[3] = d->rows; // row address bits
    spd[4] = d->cols; // column address bits
    spd[5] = d->sides; // module rows (physical banks)
    spd[6] = 64; // data width
    spd[7] = 0;
    spd[8] = 0x01; // LVTTL
    spd[9] = 0xA0; // tCK at the highest CAS latency: 10.0 ns
    spd[10] = 0x60; // tAC: 6 ns
    spd[11] = 0x00; // no parity / ECC
    spd[12] = 0x80; // refresh: 15.625 us, self-refresh
    spd[13] = 0x08; // primary SDRAM width x8
    spd[14] = 0x00; // no ECC width
    spd[15] = 0x01; // tCCD 1 clock
    spd[16] = 0x8F; // burst lengths 1, 2, 4, 8, full page
    spd[17] = 0x04; // four internal banks
    spd[18] = 0x06; // CAS latencies 2 and 3
    spd[19] = 0x01; // CS latency 0
    spd[20] = 0x01; // WE latency 0
    spd[21] = 0x00; // unbuffered
    spd[22] = 0x0E; // precharge-all, auto-precharge, early RAS#
    spd[23] = 0xA0; // tCK at CL-1
    spd[24] = 0x60;
    spd[27] = 0x14; // tRP 20 ns
    spd[28] = 0x10; // tRRD 16 ns
    spd[29] = 0x14; // tRCD 20 ns
    spd[30] = 0x32; // tRAS 50 ns
    // Module-row density: one bit per power of two from 4 MB (bit 0).
    uint8_t dens = 0;
    for (unsigned b = 0; b < 8; b++)
        if ((4u << b) == mb_side)
            dens = (uint8_t)(1u << b);
    spd[31] = dens;
    spd[32] = 0x20; // command/address setup
    spd[33] = 0x10;
    spd[34] = 0x20; // data setup
    spd[35] = 0x10;
    spd[62] = 0x12; // SPD revision 1.2
    uint32_t sum = 0;
    for (int i = 0; i < 63; i++)
        sum += spd[i];
    spd[63] = (uint8_t)sum;
    memcpy(&spd[73], "GS-SDRAM-PC66", 13); // module part number (ASCII)
}

void gos_i2c_init(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    gos_i2c_t *bus = &st->i2c;
    carve_dimms(cfg);
    memset(bus, 0, sizeof(*bus));
    for (unsigned k = 0; k < GOS_DIMMS; k++) {
        if (st->dimm[k].side_bytes)
            build_spd(bus->spd[k], &st->dimm[k]);
        LOG(2, "DIMM %u: %u side(s) of %u MB (rows %u cols %u)", k, st->dimm[k].sides, st->dimm[k].side_bytes >> 20,
            st->dimm[k].rows, st->dimm[k].cols);
    }
    const uint8_t *id = gos_board(cfg)->perch_eeprom;
    memset(bus->perch, 0xFF, sizeof(bus->perch));
    if (id) {
        memset(bus->perch, 0, 32);
        memcpy(bus->perch, id, 16);
        bus->perch_present = true;
    }
}

// Which EEPROM an 8-bit address names (NULL = nobody answers).
static const uint8_t *eeprom_at(gossamer_state_t *st, uint8_t addr8) {
    uint8_t a = addr8 & 0xFEu;
    if (a >= SPD_ADDR0 && a < SPD_ADDR0 + 2u * GOS_DIMMS) {
        unsigned k = (a - SPD_ADDR0) >> 1;
        return st->dimm[k].side_bytes ? st->i2c.spd[k] : NULL;
    }
    if (a == PERCH_ADDR && st->i2c.perch_present)
        return st->i2c.perch;
    return NULL;
}

int gos_i2c_read(void *ctx, uint8_t addr8, bool has_sub, uint8_t sub, uint8_t *out, int max) {
    config_t *cfg = (config_t *)ctx;
    gossamer_state_t *st = gos_st(cfg);
    const uint8_t *rom = eeprom_at(st, addr8);
    if (!rom) {
        LOG(3, "read $%02X: no device (NAK)", addr8);
        return -1;
    }
    if (has_sub)
        st->i2c.ptr = sub;
    // An EEPROM streams sequentially from its pointer (wrapping at 256)
    // for as long as the master keeps clocking; the host stops the Cuda
    // transfer once it has its count.  Serve as much as the reply buffer
    // holds: Open Firmware's /perch probe reads more than 32 bytes and
    // treats a short reply as a missing card.
    int n = max < GOS_SPD_SIZE ? max : GOS_SPD_SIZE;
    for (int i = 0; i < n; i++)
        out[i] = rom[(uint8_t)(st->i2c.ptr + i)];
    LOG(3, "read $%02X sub $%02X -> $%02X $%02X $%02X $%02X", addr8, st->i2c.ptr, out[0], n > 1 ? out[1] : 0,
        n > 2 ? out[2] : 0, n > 3 ? out[3] : 0);
    return n;
}

bool gos_i2c_write(void *ctx, uint8_t addr8, const uint8_t *data, int len) {
    config_t *cfg = (config_t *)ctx;
    gossamer_state_t *st = gos_st(cfg);
    if ((addr8 & 0xFEu) == TDA7433_ADDR) {
        if (len >= 2 && (data[0] & 7u) < sizeof(st->i2c.tda7433))
            st->i2c.tda7433[data[0] & 7u] = data[1];
        LOG(3, "TDA7433 write (%d bytes)", len);
        return true;
    }
    if (!eeprom_at(st, addr8)) {
        LOG(3, "write $%02X: no device (NAK)", addr8);
        return false;
    }
    // A write to an EEPROM sets its address pointer; data bytes after it
    // would program the part, which the write-protected SPDs ignore.  The
    // boot program's lone write — $E2 into register 3 of device $A0 at ROM
    // $FFF03334 — lands here and changes nothing.
    if (len >= 1)
        st->i2c.ptr = data[0];
    LOG(2, "write $%02X sub $%02X (%d data bytes ignored: write-protected)", addr8, len >= 1 ? data[0] : 0,
        len > 1 ? len - 1 : 0);
    return true;
}
