// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// awacs.c
// Shared AWACS/ASCO codec semantics — see awacs.h.

#include "awacs.h"

#include "audio_out.h" // audio_atten_1p5db_x65536, the ASCO 2300 D/A ladder

void awacs_speaker_gains(const uint16_t regs[AWACS_CODEC_REGS], uint32_t *gl_x65536, uint32_t *gr_x65536, bool *mute) {
    *mute = (regs[1] & AWACS_R1_SPEAKER_MUTE) != 0;
    *gl_x65536 = audio_atten_1p5db_x65536[(regs[4] >> 6) & 15u];
    *gr_x65536 = audio_atten_1p5db_x65536[regs[4] & 15u];
}

void awacs_output_gains(const uint16_t regs[AWACS_CODEC_REGS], uint32_t *gl_x65536, uint32_t *gr_x65536, bool *mute) {
    if (!(regs[1] & AWACS_R1_SPEAKER_MUTE)) {
        awacs_speaker_gains(regs, gl_x65536, gr_x65536, mute);
        return;
    }
    *mute = (regs[1] & AWACS_R1_HEADPHONE_MUTE) != 0;
    *gl_x65536 = audio_atten_1p5db_x65536[(regs[2] >> 6) & 15u];
    *gr_x65536 = audio_atten_1p5db_x65536[regs[2] & 15u];
}
