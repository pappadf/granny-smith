// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// pram_image.c
// pram_defaults_apply (rtc.h): write a family's power-up PRAM into a
// 256-byte image.  Kept apart from the RTC chip model because the Open
// Firmware machines apply the same defaults to their NVRAM's PRAM partition
// (of_nvram.h), where no RTC is involved.

#include "rtc.h"

#include <string.h>

void pram_defaults_apply(uint8_t pram[256], const pram_defaults_t *d) {
    if (!pram || !d)
        return;
    pram[0x0C] = (uint8_t)(d->xpram_token >> 24);
    pram[0x0D] = (uint8_t)(d->xpram_token >> 16);
    pram[0x0E] = (uint8_t)(d->xpram_token >> 8);
    pram[0x0F] = (uint8_t)d->xpram_token;
    if (d->startmgr)
        memcpy(pram + PRAM_STARTMGR_BASE, d->startmgr, PRAM_STARTMGR_LEN);
    pram[PRAM_MMFLAGS] = d->mmflags | d->mmflags_booted;
    for (uint8_t i = 0; i < d->n_extra; i++)
        pram[d->extra[i].addr] = d->extra[i].value;
    pram[PRAM_STARTMGR_WAIT] |= PRAM_STARTMGR_NO_WAIT;
}
