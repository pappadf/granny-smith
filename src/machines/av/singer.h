// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// singer.h
// The Singer codec + PSC sound frame engine — the AV family's sound
// datapath.  Contract: local/gs-docs/840av_660av/docs/singer.md (register
// map §1, sndComCtl §2, singerCtl/singerStat §3, behavioural model §7) +
// the frame-tick gating facts in local/gs-docs/dsp3210-plaintalk/
// (supermario-board-wiring.md §B2, rtm-rom-host-side.md §2/§5).
//
// The engine runs a scheduler event at the programmed frame cadence
// (`sndSize` sample frames at the `pSndRate` codec rate, phase-locked to
// the same time formula as the PSC's `sndPhase` free-runner).  Per frame,
// when enabled: copy the guest's `sndOutBase` half-buffer to the host
// audio stream (attenuation per `singerCtl`), fill the `sndInBase` half
// from the audio-in source, raise PSC-VIA2 IFR bit 6 (`PSCSNDFRM`) and
// pulse DSP EXT1 — both gated by `pFrmIntEn` — and detect frame overruns
// (`pdspFrameOvr` + L5 bit 1) when the previous tick went unserviced.

#ifndef GS_MACHINES_AV_SINGER_H
#define GS_MACHINES_AV_SINGER_H

#include "system_config.h"

#include <stdbool.h>
#include <stdint.h>

struct av_singer;
typedef struct av_singer av_singer_t;

// singerStat presentation (singer.md §3 + gap-closure B4): BI1/BI3 set —
// the DSP driver's 3-way input-source decode reads "source 1", the
// microphone presentation — BI4 set for the shipped output-port choice,
// and pValidData so recorded A/D data reads as valid.  Board straps, not
// chip state; both AV boards present the same value.
#define AV_SINGER_STAT 0x0040000Du

// === Lifecycle ==============================================================

av_singer_t *av_singer_init(config_t *cfg, checkpoint_t *cp);
void av_singer_delete(av_singer_t *s);
void av_singer_checkpoint(av_singer_t *s, checkpoint_t *cp);

#endif // GS_MACHINES_AV_SINGER_H
