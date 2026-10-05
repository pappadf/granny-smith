// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// config_seed.c
// See config_seed.h.

#include "config_seed.h"

#include "machine_profile.h"
#include "nubus.h"
#include "rtc.h"
#include "system_config.h"

#include <string.h>

// SysParam, the original 20-byte parameter RAM, as the extended RTC (and an
// XPRAM image) holds it: logical bytes $00-$0F at $10-$1F, $10-$13 at
// $08-$0B (the RTC's two command groups).  The seed writes the bytes the
// machine's own ROM's PRAMInit writes into an invalid store (its
// pram_defaults_t, measured per ROM), with byte $03, the serial-port use, the
// one record a configuration chooses.  Writing the whole block valid is what
// keeps PRAMInit from overwriting the choice.

// Serial-port use: the printer port (port B) in use by AppleTalk.
#define SPCONFIG_PORTB_MASK 0x0Fu
#define SPCONFIG_USE_ATALK  0x01u
#define SYSPARAM_PHYS_HI    0x10u
#define SYSPARAM_PHYS_LO    0x08u
#define SYSPARAM_SPCONFIG   0x03u

// The slot records (sPRAMRec): 8 bytes per NuBus slot from $9, at $46.
#define SLOT_PRAM_BASE 0x46u

// The configuration's AppleTalk choice, or -1 when it has no such option.
static int appletalk_choice(const struct config *cfg) {
    const char *v = machine_build_opts_option(&cfg->build_opts, "appletalk");
    if (!v)
        return -1;
    return strcmp(v, "active") == 0;
}

// The SysParam block with the AppleTalk choice, into a 256-byte image.
static void sysparam_image(uint8_t img[256], const uint8_t sysparam[20], bool active) {
    memcpy(img + SYSPARAM_PHYS_HI, sysparam, 16);
    memcpy(img + SYSPARAM_PHYS_LO, sysparam + 16, 4);
    uint8_t *sp = img + SYSPARAM_PHYS_HI + SYSPARAM_SPCONFIG;
    *sp = (uint8_t)((*sp & ~SPCONFIG_PORTB_MASK) | (active ? SPCONFIG_USE_ATALK : 0));
}

void mac_seed_xpram_appletalk(uint8_t xpram[256], const struct config *cfg, const pram_defaults_t *pram) {
    int a = appletalk_choice(cfg);
    if (a >= 0 && pram && pram->sysparam)
        sysparam_image(xpram, pram->sysparam, a == 1);
}

int mac_seed_startup_scsi_id(const struct config *cfg, const char *bus_id) {
    const machine_startup_t *s = &cfg->build_opts.startup;
    if (!cfg->build_opts.storage_given)
        return -2;
    if (s->none || !s->bus[0] || strcmp(s->bus, bus_id) != 0)
        return -1;
    return s->unit;
}

// Start Manager record (PRAMInitTbl, pram.md §4.2): $77 the default OS, $78-$7B
// the default startup device as the SCSI driver refnum -(33 + id).
#define PRAM_DEFAULT_OS   0x77u
#define PRAM_BOOT_REFNUM  0x78u
#define PRAM_OS_MACINTOSH 0x01u

void mac_seed_rtc_pram(struct config *cfg) {
    rtc_t *rtc = cfg->rtc;
    if (!rtc)
        return;
    const pram_defaults_t *pd = cfg->machine->pram;
    int a = appletalk_choice(cfg);
    if (a >= 0 && pd && pd->sysparam) {
        uint8_t img[256];
        for (int i = 0; i < 256; i++)
            img[i] = rtc_pram_read(rtc, (uint8_t)i);
        sysparam_image(img, pd->sysparam, a == 1);
        for (unsigned i = 0; i < 16; i++)
            rtc_pram_write(rtc, (uint8_t)(SYSPARAM_PHYS_HI + i), img[SYSPARAM_PHYS_HI + i]);
        for (unsigned i = 0; i < 4; i++)
            rtc_pram_write(rtc, (uint8_t)(SYSPARAM_PHYS_LO + i), img[SYSPARAM_PHYS_LO + i]);
    }
    // The addressing mode: MMFlags bit 0, as the Memory control panel saves
    // it.  32-bit writes what a System booted that way leaves ($05), so
    // 7.6 and later find it set and do not rewrite it and restart.
    const char *addressing = machine_build_opts_option(&cfg->build_opts, "addressing");
    if (addressing) {
        uint8_t mm = rtc_pram_read(rtc, PRAM_MMFLAGS);
        if (strcmp(addressing, "32") == 0)
            mm |= PRAM_MMFLAGS_32BIT_BOOTED;
        else
            mm &= (uint8_t)~PRAM_MMFLAGS_32BIT;
        rtc_pram_write(rtc, PRAM_MMFLAGS, mm);
    }
    // The built-in video's startup video mode, and each NuBus card's: the
    // slot PRAM record the Monitors control panel would have saved (the
    // profile and the card know the format).
    const machine_build_opts_t *o = &cfg->build_opts;
    if (o->builtin_startup.slot >= 0x9 && o->builtin_startup.slot <= 0xE)
        for (int i = 0; i < 8; i++)
            rtc_pram_write(rtc, (uint8_t)(SLOT_PRAM_BASE + (o->builtin_startup.slot - 0x9) * 8 + i),
                           o->builtin_startup.record[i]);
    for (int slot = 0x9; cfg->nubus && slot <= 0xE; slot++) {
        uint8_t rec[8];
        if (!nubus_startup_record(cfg->nubus, slot, rec))
            continue;
        for (int i = 0; i < 8; i++)
            rtc_pram_write(rtc, (uint8_t)(SLOT_PRAM_BASE + (slot - 0x9) * 8 + i), rec[i]);
    }
    // A ROM with no Start Manager table (the Plus's) has no record to seed.
    if (pd && !pd->startmgr)
        return;
    int id = mac_seed_startup_scsi_id(cfg, "scsi");
    if (id == -2)
        return;
    // "No default": the refnum of no driver, which the Start Manager answers
    // by searching every drive.
    uint32_t refnum = id >= 0 ? (uint32_t)(-33 - id) : 0u;
    rtc_pram_write(rtc, PRAM_DEFAULT_OS, PRAM_OS_MACINTOSH);
    for (int i = 0; i < 4; i++)
        rtc_pram_write(rtc, (uint8_t)(PRAM_BOOT_REFNUM + i), (uint8_t)(refnum >> (24 - 8 * i)));
}
