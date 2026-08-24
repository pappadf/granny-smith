// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// awacs.h
// The AWACS codec's shared "expanded command set" semantics — the ITT
// ASCO 2300-family converter behind both the PDM sound block (AMIC face,
// machines/pdm/awacs.c) and the TNT sound block (Grand Central face +
// DBDMA, machines/tnt/awacs.c).  Each family owns its software-visible
// register file, command transport and DMA datapath; what the chip
// itself defines — the shadow-register meanings and the D/A attenuation
// law — lives here (the 53C96 chip-in-core / glue-in-family split).
//
// Register truth: the ITT ASCO 2300 codec datasheet, the shipping PDM
// and TNT ROM sound drivers, and the Linux/NetBSD awacs drivers.

#ifndef GS_PERIPHERALS_AWACS_H
#define GS_PERIPHERALS_AWACS_H

#include <stdbool.h>
#include <stdint.h>

// Expanded-command shadow registers (write-only on hardware; families
// keep the 12-bit shadows in their own checkpointed state):
//   0  input mux / gain          1  mutes, rate, loopthrough
//   2  volume A (headphones)     4  volume C (speaker)
//   5-7  Screamer extensions (stored, not interpreted)
#define AWACS_CODEC_REGS 8

// Register-1 bits this model interprets.
#define AWACS_R1_SPEAKER_MUTE 0x0080u

// D/A attenuation ladder: 4-bit code, 0 = loudest, -1.5 dB per step, as
// x65536 gains (the ASCO 2300 ladder — identical to Singer's; the chime
// volume law (7-vol)*2 rides on it).
extern const uint32_t awacs_atten_x65536[16];

// The speaker output path ("volume C", register 4: left code in bits
// 9:6, right in 3:0) plus the register-1 speaker mute, as x65536 gains.
void awacs_speaker_gains(const uint16_t regs[AWACS_CODEC_REGS], uint32_t *gl_x65536, uint32_t *gr_x65536, bool *mute);

#endif // GS_PERIPHERALS_AWACS_H
