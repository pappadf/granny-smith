// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// dsp.h
// The AV family's DSP3210 instance: binds the generic core
// (src/core/cpu/dsp3210/) to the board — guest-physical bus hooks, the
// PSC dspOverRun reset latch, burst execution on the scheduler
// (heterogeneous multi-CPU design: the 68040 owns time, the DSP rides it
// in quantum bursts), the BIO0→PSC-L5 doorbell, and the `machine.dsp`
// object node.  Contract: local/gs-docs/840av_660av/docs/dsp3210.md §8 +
// the gap-closure findings in local/gs-docs/dsp3210-plaintalk/.

#ifndef GS_MACHINES_AV_DSP_H
#define GS_MACHINES_AV_DSP_H

#include "system_config.h"

#include <stdbool.h>
#include <stdint.h>

struct av_dsp;
typedef struct av_dsp av_dsp_t;

// === Lifecycle ==============================================================

av_dsp_t *av_dsp_init(config_t *cfg, checkpoint_t *cp);
void av_dsp_delete(av_dsp_t *dsp);
void av_dsp_checkpoint(av_dsp_t *dsp, checkpoint_t *cp);

// === Board wiring ===========================================================

// dspOverRun ($21C) write from the PSC: bit 0 pdspReset (1 = hold), bit 1
// pdspResetEn, bit 2 pdspFrameOvr.  A bit-0 clear write releases the DSP
// (the power-on release finds the latch already 0 — hardware reset held
// the chip until then) and starts execution from external physical 0
// (StartProcessorRoutine's 7-word bootstrap); a bit-0 set write re-holds.
void av_dsp_overrun_write(av_dsp_t *dsp, uint8_t bits, uint8_t written);

// av_psc_dsp_fn adapter (ctx = the av_dsp_t) for av_psc_set_dsp_hook.
void av_dsp_overrun_hook(void *ctx, uint8_t bits, uint8_t written);

// Assert an external interrupt pin (DSP3210_VEC_EXT0/EXT1).  The Singer
// frame engine pulses EXT1 (IR1N) once per sound frame while `pFrmIntEn`
// is set; waking a parked core re-arms its burst promptly.
void av_dsp_irq(av_dsp_t *dsp, int vector);

// True when the frame engine should count a frame as unserviced: the
// previous EXT1 request is still latched (the kernel never woke for it).
bool av_dsp_ext1_pending(av_dsp_t *dsp);

// True once the RTM has started the DSP and it has not been re-held.
bool av_dsp_running(av_dsp_t *dsp);

#endif // GS_MACHINES_AV_DSP_H
