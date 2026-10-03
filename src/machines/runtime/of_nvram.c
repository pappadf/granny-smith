// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// of_nvram.c
// See of_nvram.h.

#include "of_nvram.h"

#include "pram_defaults.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// === The Open Firmware partition ============================================
//
// A 0x5C-byte header at $1800, then a free gap, then the strings packed
// downward to $1FFF.  All fields big-endian; string addresses are absolute.
//
//   +$00 u16 magic $1275      +$06 u16 here (first byte after the header)
//   +$02 u8  version (5)      +$08 u16 top (lowest string byte)
//   +$03 u8  pages (8 x 256)  +$0A u16 next (0)
//   +$04 u16 checksum         +$0C u32 flags (booleans, bits 31..24)
//   +$10 u32 numbers[9]       +$34 {u16 addr, u16 len} strings[10]

#define OF_MAGIC     0x1275u
#define OF_VERSION   5u
#define OF_PAGES     (OF_NVRAM_OF_SIZE / 256u)
#define OF_HDR_SIZE  0x5Cu
#define OF_FLAGS_OFF 0x0Cu
#define OF_NUMS_OFF  0x10u
#define OF_STRS_OFF  0x34u
#define OF_N_BOOLS   8
#define OF_N_NUMBERS 9
#define OF_N_STRINGS 10

static const char *const k_var_names[] = {
    // booleans, flags bit 31 down to bit 24
    "little-endian?", "real-mode?", "auto-boot?", "diag-switch?", "fcode-debug?", "oem-banner?", "oem-logo?",
    "use-nvramrc?",
    // numbers
    "real-base", "real-size", "virt-base", "virt-size", "load-base", "pci-probe-list", "screen-#columns",
    "screen-#rows", "selftest-#megs",
    // strings
    "boot-device", "boot-file", "diag-device", "diag-file", "input-device", "output-device", "oem-banner", "oem-logo",
    "nvramrc", "boot-command"};

#define OF_N_VARS (sizeof(k_var_names) / sizeof(k_var_names[0]))

// OF 1.0.5, as its format pass writes a blank store on both TNT ROMs; the
// same values Apple prints as the defaults of a 9500 (Technical Note
// TN1061, p. 3).
static const of_nvram_of_defaults_t k_of_105 = {
    .flags = 0x20000000u, // auto-boot? true
    .numbers = {0xFFFFFFFFu, 0x100000u, 0xFFFFFFFFu, 0x100000u, 0x4000u, 0xFFFFFFFFu, 100u, 40u, 0u},
    .strings = {"/AAPL,ROM", NULL, "fd:diags", NULL, "ttya", "ttya", NULL, NULL, NULL, "boot"},
};

// OF 2.4 (G3 Rev C).  OF 2.0f1 (Rev A) writes the same except diag-device
// "fd:diags"; it accepts this store as it stands.
static const of_nvram_of_defaults_t k_of_24 = {
    .flags = 0x20000000u,
    .numbers = {0xFFFFFFFFu, 0x100000u, 0xFFFFFFFFu, 0x100000u, 0x4000u, 0xFFFFFFFFu, 100u, 40u, 0u},
    .strings = {"/AAPL,ROM", NULL, "fd:\\diags", NULL, "kbd", "screen", NULL, NULL, NULL, "boot"},
};

const of_nvram_defaults_t of_nvram_defaults_tnt = {.pram = &pram_defaults_tnt, .of = &k_of_105, .startup_partition = 0};
const of_nvram_defaults_t of_nvram_defaults_g3 = {.pram = &pram_defaults_g3, .of = &k_of_24, .startup_partition = 1};

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Sum of the partition as 1024 big-endian words.
static uint32_t of_word_sum(const uint8_t *of) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < OF_NVRAM_OF_SIZE; i += 2)
        s += rd16(of + i);
    return s;
}

// The checksum is stored as ~(sum mod $FFFF) over the partition with the
// field zeroed.  The firmware's own generator folds the carry once, which
// can leave a sum its verifier reads as $10000; the mod form always folds
// to $FFFF, under one fold or two, so every reader accepts it.
static void of_set_checksum(uint8_t *of) {
    wr16(of + 4, 0);
    uint32_t s = of_word_sum(of);
    wr16(of + 4, (~(s % 0xFFFFu)) & 0xFFFFu);
}

bool of_nvram_of_valid(const uint8_t nv[OF_NVRAM_SIZE]) {
    const uint8_t *of = nv + OF_NVRAM_OF;
    if (rd16(of) != OF_MAGIC)
        return false;
    uint32_t s = of_word_sum(of);
    uint32_t f = (s >> 16) + (s & 0xFFFFu);
    f = (f >> 16) + (f & 0xFFFFu);
    return f == 0xFFFFu;
}

// Write the strings downward from $2000 in slot order -- the firmware's own
// packing -- and the header around them.  `strs[i]` may be NULL (empty).
// Returns false if they do not fit above the header.
static bool of_pack(uint8_t *of, uint32_t flags, const uint32_t numbers[OF_N_NUMBERS],
                    const char *const strs[OF_N_STRINGS], const size_t lens[OF_N_STRINGS]) {
    size_t total = 0;
    for (int i = 0; i < OF_N_STRINGS; i++)
        total += lens[i];
    if (total > OF_NVRAM_OF_SIZE - OF_HDR_SIZE)
        return false;
    uint8_t heap[OF_NVRAM_OF_SIZE];
    uint32_t top = OF_NVRAM_OF + OF_NVRAM_OF_SIZE; // absolute
    uint16_t addr[OF_N_STRINGS];
    for (int i = 0; i < OF_N_STRINGS; i++) {
        top -= (uint32_t)lens[i];
        if (lens[i])
            memcpy(heap + (top - OF_NVRAM_OF), strs[i], lens[i]);
        addr[i] = (uint16_t)top;
    }
    memset(of, 0, top - OF_NVRAM_OF);
    memcpy(of + (top - OF_NVRAM_OF), heap + (top - OF_NVRAM_OF), OF_NVRAM_OF + OF_NVRAM_OF_SIZE - top);
    wr16(of + 0, OF_MAGIC);
    of[2] = OF_VERSION;
    of[3] = OF_PAGES;
    wr16(of + 6, OF_NVRAM_OF + OF_HDR_SIZE);
    wr16(of + 8, top);
    wr16(of + 0x0A, 0);
    wr32(of + OF_FLAGS_OFF, flags);
    for (int i = 0; i < OF_N_NUMBERS; i++)
        wr32(of + OF_NUMS_OFF + 4 * i, numbers[i]);
    for (int i = 0; i < OF_N_STRINGS; i++) {
        wr16(of + OF_STRS_OFF + 4 * i, addr[i]);
        wr16(of + OF_STRS_OFF + 4 * i + 2, (uint32_t)lens[i]);
    }
    of_set_checksum(of);
    return true;
}

void of_nvram_factory(uint8_t nv[OF_NVRAM_SIZE], const of_nvram_defaults_t *d) {
    memset(nv, 0, OF_NVRAM_SIZE);
    if (!d)
        return;
    pram_defaults_apply(nv + OF_NVRAM_XPRAM, d->pram);
    if (d->of) {
        size_t lens[OF_N_STRINGS];
        for (int i = 0; i < OF_N_STRINGS; i++)
            lens[i] = d->of->strings[i] ? strlen(d->of->strings[i]) : 0;
        of_pack(nv + OF_NVRAM_OF, d->of->flags, d->of->numbers, d->of->strings, lens);
    }
}

const char *const *of_nvram_var_names(size_t *count) {
    if (count)
        *count = OF_N_VARS;
    return k_var_names;
}

static int var_slot(const char *name) {
    for (size_t i = 0; i < OF_N_VARS; i++)
        if (strcmp(k_var_names[i], name) == 0)
            return (int)i;
    return -1;
}

of_var_kind_t of_nvram_getenv(const uint8_t nv[OF_NVRAM_SIZE], const char *name, char *buf, size_t buflen) {
    int slot = name ? var_slot(name) : -1;
    if (slot < 0 || !of_nvram_of_valid(nv) || !buf || !buflen)
        return OF_VAR_NONE;
    const uint8_t *of = nv + OF_NVRAM_OF;
    if (slot < OF_N_BOOLS) {
        bool on = (rd32(of + OF_FLAGS_OFF) >> (31 - slot)) & 1u;
        snprintf(buf, buflen, "%s", on ? "true" : "false");
        return OF_VAR_BOOL;
    }
    slot -= OF_N_BOOLS;
    if (slot < OF_N_NUMBERS) {
        snprintf(buf, buflen, "%x", (unsigned)rd32(of + OF_NUMS_OFF + 4 * slot));
        return OF_VAR_NUMBER;
    }
    slot -= OF_N_NUMBERS;
    uint32_t a = rd16(of + OF_STRS_OFF + 4 * slot);
    uint32_t n = rd16(of + OF_STRS_OFF + 4 * slot + 2);
    if (n && (a < OF_NVRAM_OF || a + n > OF_NVRAM_SIZE))
        n = 0; // a damaged entry reads as empty rather than out of bounds
    size_t c = n < buflen - 1 ? n : buflen - 1;
    memcpy(buf, nv + a, c);
    buf[c] = '\0';
    return OF_VAR_STRING;
}

const char *of_nvram_setenv(uint8_t nv[OF_NVRAM_SIZE], const char *name, const char *value) {
    int slot = name ? var_slot(name) : -1;
    if (slot < 0)
        return "no such Open Firmware variable";
    if (!value)
        return "no value";
    if (!of_nvram_of_valid(nv))
        return "the Open Firmware partition is not valid";
    uint8_t *of = nv + OF_NVRAM_OF;
    uint32_t flags = rd32(of + OF_FLAGS_OFF);
    uint32_t numbers[OF_N_NUMBERS];
    for (int i = 0; i < OF_N_NUMBERS; i++)
        numbers[i] = rd32(of + OF_NUMS_OFF + 4 * i);
    // Copy the strings out before repacking overwrites the heap.
    char *copies[OF_N_STRINGS] = {0};
    const char *strs[OF_N_STRINGS];
    size_t lens[OF_N_STRINGS];
    for (int i = 0; i < OF_N_STRINGS; i++) {
        uint32_t a = rd16(of + OF_STRS_OFF + 4 * i);
        uint32_t n = rd16(of + OF_STRS_OFF + 4 * i + 2);
        if (n && (a < OF_NVRAM_OF || a + n > OF_NVRAM_SIZE))
            n = 0;
        copies[i] = malloc(n + 1);
        if (!copies[i]) {
            for (int j = 0; j < i; j++)
                free(copies[j]);
            return "out of memory";
        }
        memcpy(copies[i], nv + a, n);
        strs[i] = copies[i];
        lens[i] = n;
    }
    const char *err = NULL;
    if (slot < OF_N_BOOLS) {
        uint32_t bit = 1u << (31 - slot);
        if (strcasecmp(value, "true") == 0)
            flags |= bit;
        else if (strcasecmp(value, "false") == 0)
            flags &= ~bit;
        else
            err = "a boolean takes true or false";
    } else if (slot < OF_N_BOOLS + OF_N_NUMBERS) {
        char *end = NULL;
        unsigned long v = strtoul(value, &end, 16);
        if (!*value || (end && *end))
            err = "a number takes a hex value";
        else
            numbers[slot - OF_N_BOOLS] = (uint32_t)v;
    } else {
        int s = slot - OF_N_BOOLS - OF_N_NUMBERS;
        strs[s] = value;
        lens[s] = strlen(value);
    }
    if (!err && !of_pack(of, flags, numbers, strs, lens))
        err = "the strings do not fit in the 2 KB partition";
    for (int i = 0; i < OF_N_STRINGS; i++)
        free(copies[i]);
    return err;
}

// === Mac OS settings in the store ============================================

#define XP_STARTUP (OF_NVRAM_XPRAM + 0x78u)

int of_nvram_startup_scsi(const uint8_t nv[OF_NVRAM_SIZE]) {
    const uint8_t *p = nv + XP_STARTUP;
    if (p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] >= 0xD9 && p[3] <= 0xDF)
        return 0xDF - p[3]; // the 68k form: SCSI driver refnum ~(32 + id)
    if (p[2] == 0 && p[3] == 0 && p[1] <= 1 && p[0] < 7 * 8)
        return p[0] >> 3;
    return -1;
}

void of_nvram_set_startup_scsi(uint8_t nv[OF_NVRAM_SIZE], int id, const of_nvram_defaults_t *d) {
    uint8_t *p = nv + XP_STARTUP;
    if (id < 0 || id > 6) {
        p[0] = p[1] = 0;
        p[2] = p[3] = 0x66; // NoDefaultVal: search for any startup device
        return;
    }
    p[0] = (uint8_t)(id << 3);
    p[1] = d ? d->startup_partition : 0;
    p[2] = p[3] = 0;
}

// The end of the record list, as an offset from $1402; 0 for an empty area.
static uint32_t nr_used(const uint8_t nv[OF_NVRAM_SIZE]) {
    int32_t end = (int32_t)rd16(nv + OF_NVRAM_NR) - (int32_t)OF_NVRAM_NR;
    if (end < 2 || end >= 0x400)
        return 0;
    return (uint32_t)end - 2;
}

uint8_t *of_nvram_nr_find(uint8_t nv[OF_NVRAM_SIZE], const char name[4]) {
    uint32_t used = nr_used(nv);
    for (uint32_t off = 0; off + OF_NVRAM_NR_RECORD <= used; off += OF_NVRAM_NR_RECORD) {
        uint8_t *r = nv + OF_NVRAM_NR + 2 + off;
        if (r[6] == 4 && memcmp(r + 7, name, 4) == 0)
            return r + 12;
    }
    return NULL;
}

bool of_nvram_nr_add(uint8_t nv[OF_NVRAM_SIZE], const uint8_t location[6], const char name[4],
                     const uint8_t data[OF_NVRAM_NR_DATA]) {
    uint32_t used = nr_used(nv);
    if (2 + used + OF_NVRAM_NR_RECORD > 0x400)
        return false;
    uint8_t *r = nv + OF_NVRAM_NR + 2 + used;
    memcpy(r, location, 6);
    r[6] = 4;
    memcpy(r + 7, name, 4);
    r[11] = OF_NVRAM_NR_DATA;
    memcpy(r + 12, data, OF_NVRAM_NR_DATA);
    wr16(nv + OF_NVRAM_NR, OF_NVRAM_NR + 2 + used + OF_NVRAM_NR_RECORD);
    return true;
}
