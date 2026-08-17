// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mace.h
// MACE Ethernet MAC (Am79C940 core inside Curio) — a register stub with no
// wire behind it, plus the Apple address PROM at $50F08000.  Contract:
// the AV MACE hardware notes §2 (register map) and §4 (PROM).
//
// The `.ENET` driver runs three loopback self-tests at open; with no
// datapath they fail and the driver does not load.  That is harmless for
// booting (IMPLEMENTATION.md §7) and is the documented Phase-G contract.

#ifndef GS_MACHINES_AV_MACE_H
#define GS_MACHINES_AV_MACE_H

#include "system_config.h"

#include <stdint.h>

struct av_mace;
typedef struct av_mace av_mace_t;

// === Lifecycle ==============================================================

av_mace_t *av_mace_init(config_t *cfg, checkpoint_t *cp);
void av_mace_delete(av_mace_t *mace);
void av_mace_checkpoint(av_mace_t *mace, checkpoint_t *cp);

// === I/O island handlers ====================================================

// MACE registers (island $1C000, $10 stride).
uint8_t av_mace_read(config_t *cfg, uint32_t addr);
void av_mace_write(config_t *cfg, uint32_t addr, uint8_t value);

// Apple Ethernet address PROM (island $08000): 8 bytes at $x1 of each
// 16-byte group, XOR of all eight == $FF.
uint8_t av_mace_prom_read(config_t *cfg, uint32_t addr);
void av_mace_prom_write(config_t *cfg, uint32_t addr, uint8_t value);

#endif // GS_MACHINES_AV_MACE_H
