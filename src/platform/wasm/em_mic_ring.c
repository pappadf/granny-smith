// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_mic_ring.c
// See em_mic_ring.h.

#include "em_mic_ring.h"

mic_take_t mic_ring_take(uint32_t wr, uint32_t rd, uint32_t ring_len, uint32_t need) {
    uint32_t half = ring_len / 2;
    if (need == 0)
        need = 1;
    if (need > half)
        need = half; // the caller's resampler stretches what is missing
    uint32_t avail = wr - rd;
    mic_take_t t = {.ok = false, .overrun = false, .start = rd, .need = need};
    if (avail > half && avail > need) {
        t.start = wr - need;
        avail = need;
        t.overrun = true;
    }
    t.ok = avail >= need;
    return t;
}
