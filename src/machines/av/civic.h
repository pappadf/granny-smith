// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// civic.h
// CIVIC (Cyclone Integrated Video Interfaces Controller, 343S1096) — the AV
// frame-buffer / video-timing controller — plus its downstream Sebastian
// RAMDAC/CLUT (343S0704) and the Endeavor/Clifton/PUMA pixel-clock
// synthesizer latches.  Contracts: local/gs-docs/840av_660av/docs/civic.md,
// sebastian.md, endeavor-clifton-puma.md.
//
// The one thing to get right first (civic.md §2): CIVIC's register
// interface is BIT-SERIAL — one bit per longword, only D[0] meaningful,
// LSB at the lowest address, stride 4.  A 12-bit register at $380 occupies
// $380..$3AC.  Five 1-bit registers are also poked as plain longwords;
// modelling every longword slot as one stored bit serves both styles.

#ifndef GS_MACHINES_AV_CIVIC_H
#define GS_MACHINES_AV_CIVIC_H

#include "display.h"
#include "system_config.h"

#include <stdbool.h>
#include <stdint.h>

struct av_civic;
typedef struct av_civic av_civic_t;

// VRAM: 2 MB at $50100000 (the 'Nano' probe at base+2MB-8 sizes it).
#define AV_CIVIC_VRAM_BASE 0x50100000u
#define AV_CIVIC_VRAM_SIZE 0x00200000u

// === Lifecycle ==============================================================

av_civic_t *av_civic_init(config_t *cfg, checkpoint_t *cp);
void av_civic_delete(av_civic_t *cv);
void av_civic_checkpoint(av_civic_t *cv, checkpoint_t *cp);

// Map VRAM (direct pages + bus-resolver host region) and the low CIVIC
// register alias at $50036000.  Called from build_devices after the bus
// resolver exists.
void av_civic_install_memory(config_t *cfg, av_civic_t *cv);

// === I/O island handlers ====================================================

// CIVIC serial registers (island $36000; also aliased at $50036000).
uint8_t av_civic_read(config_t *cfg, uint32_t addr);
void av_civic_write(config_t *cfg, uint32_t addr, uint8_t value);

// Sebastian RAMDAC (island $30800: index/data/PCBR, $10 stride).
uint8_t av_civic_seb_read(config_t *cfg, uint32_t addr);
void av_civic_seb_write(config_t *cfg, uint32_t addr, uint8_t value);

// Endeavor/Clifton/PUMA clock synthesizer (island $2E000).
uint8_t av_civic_clk_read(config_t *cfg, uint32_t addr);
void av_civic_clk_write(config_t *cfg, uint32_t addr, uint8_t value);

// === Display ================================================================

// The scanout display (substrate .display hook).
display_t *av_civic_display(av_civic_t *cv);

#endif // GS_MACHINES_AV_CIVIC_H
