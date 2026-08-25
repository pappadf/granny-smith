// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// awacs.c
// Shared AWACS/ASCO codec semantics — see awacs.h.

#include "awacs.h"

// The ASCO 2300 D/A attenuation ladder (0 = loudest, -1.5 dB per step).
const uint32_t awacs_atten_x65536[16] = {
    65536, 55142, 46396, 39037, 32846, 27636, 23253, 19565, 16462, 13851, 11654, 9806, 8250, 6942, 5841, 4915,
};

void awacs_speaker_gains(const uint16_t regs[AWACS_CODEC_REGS], uint32_t *gl_x65536, uint32_t *gr_x65536, bool *mute) {
    *mute = (regs[1] & AWACS_R1_SPEAKER_MUTE) != 0;
    *gl_x65536 = awacs_atten_x65536[(regs[4] >> 6) & 15u];
    *gr_x65536 = awacs_atten_x65536[regs[4] & 15u];
}
