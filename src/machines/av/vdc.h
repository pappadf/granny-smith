// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// vdc.h
// The AV video digitizer's chip pair — behavioral models of the Philips
// SAA7191B "DMSD" colour decoder (I2C slave $8A/$8B) and the SAA7186 "VDC"
// video scaler (I2C slave $B8/$B9) — plus the frame engine that writes
// captured fields into CIVIC VRAM at $50200800.  Contract:
// docs/machines/av/vdc.md (ROM-verified register maps,
// probes and the golden open sequence).
//
// Model shape (proposal-av-video-in.md §2):
//   * the two register files are write-sinks plus two synthesized status
//     bytes — the guest's 'i2c ' component serves subaddressed reads from
//     its own RAM shadow, so only $8B/$B9 status reads are load-bearing on
//     the wire (the Enabler 088 build's dead shadow cache makes register
//     read-backs reach us too; they are served from the files)
//   * the frame engine is a scheduler event at NTSC field cadence: when the
//     CIVIC gates are open (VDCClk 0, BusSize 0) and the VDC's VPE bit is
//     set, each firing pulls a 640x480 RGBA host frame, applies the VDC
//     window/decimation, packs to the FS format and writes it to VRAM with
//     the VidInSize stride, then latches the CIVIC field interrupt
//   * host sources: a deterministic pattern generator, a loaded PNG frame,
//     or the platform webcam through the gs_video_in_* seam (system.h) —
//     selected via the machine.videoin object-model node

#ifndef GS_MACHINES_AV_VDC_H
#define GS_MACHINES_AV_VDC_H

#include "system_config.h"

#include <stdbool.h>
#include <stdint.h>

struct av_vdc;
typedef struct av_vdc av_vdc_t;

// The video-in frame buffer: VRAM offset of $50200800 in the 2 MB aperture.
#define AV_VDC_VRAM_OFFSET 0x100800u

// Host frame contract: the SAA7191B is the square-pixel DMSD whose NTSC
// active picture is exactly 640x480 — every host source delivers that.
#define AV_VDC_SRC_W 640
#define AV_VDC_SRC_H 480

// === Lifecycle ==============================================================

av_vdc_t *av_vdc_init(config_t *cfg, checkpoint_t *cp);
void av_vdc_delete(av_vdc_t *vdc);
void av_vdc_checkpoint(av_vdc_t *vdc, checkpoint_t *cp);

// === I2C register access (wired from Cuda pseudo-command $22) ===============

// True for the four addresses on the bus: DMSD $8A/$8B, VDC $B8/$B9.
bool av_vdc_i2c_slave_known(uint8_t slave);

// A completed I2C write to `slave` (even address): data[0] is the
// subaddress, data[1..] the payload with subaddress auto-increment —
// exactly the byte stream Cuda puts on the wire.
void av_vdc_i2c_write(av_vdc_t *vdc, uint8_t slave, const uint8_t *data, int len);

// A completed I2C read from `slave` (odd address): without a subaddress the
// one-byte status register; with one, the register file from `sub` up.
// Returns the byte count placed in `out` (bounded by `maxlen`).
int av_vdc_i2c_read(av_vdc_t *vdc, uint8_t slave, bool has_sub, uint8_t sub, uint8_t *out, int maxlen);

// === Host source control (the machine.videoin surface + tests) ==============

// Select the host video source: "none" | "pattern" | "file" | "host".
// "file" requires a frame loaded first (machine.videoin.load); returns 0 on
// success, -1 on an unknown name.
int av_vdc_set_source(av_vdc_t *vdc, const char *name);

// True when the selected source reports a signal (drives DMSD HLCK).
bool av_vdc_connected(av_vdc_t *vdc);

// === CIVIC hooks ============================================================

// CIVIC's VDCClk slot changed (1 = clock off).  Tracks the capture-engine
// gate and pushes host camera-lifecycle notifications (gs_video_in_state).
void av_vdc_clock_gate(av_vdc_t *vdc, bool clock_off);

#endif // GS_MACHINES_AV_VDC_H
