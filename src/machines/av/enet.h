// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// enet.h
// The AV Quadras' built-in Ethernet: the shared Curio MACE core (core/
// peripherals/mace.c) at $50F1C000 on its level-3 interrupt, fed by the
// PSC's two Ethernet DMA channels -- ENetWr (2) for transmit, ENetRd (1) in
// chain mode for receive -- plus Apple's station-address PROM at $50F08000,
// with the AAUI port cabled to the emulator's AppleTalk network (EtherTalk).
// Contract: docs/internals/machines/av/enet.md.

#ifndef GS_MACHINES_AV_ENET_H
#define GS_MACHINES_AV_ENET_H

#include "checkpoint.h"
#include "system_internal.h"

#include <stdint.h>

struct av_enet;
typedef struct av_enet av_enet_t;

// === Lifecycle ==============================================================

// Builds the core and its wiring (after the PSC); with `cp` the core's state
// is read back.
av_enet_t *av_enet_init(config_t *cfg, checkpoint_t *cp);
void av_enet_delete(av_enet_t *enet);
void av_enet_checkpoint(av_enet_t *enet, checkpoint_t *cp);
// The MACE's RESET pin (the board reset).
void av_enet_reset(av_enet_t *enet);

// === I/O island handlers ====================================================

// MACE registers (island $1C000, $10 stride).
uint8_t av_enet_mace_read(config_t *cfg, uint32_t win_off, uint32_t addr);
// The same register without its read side effects (board_io_range_t.peek_fn).
uint8_t av_enet_mace_peek(config_t *cfg, uint32_t win_off, uint32_t addr);
void av_enet_mace_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);

// Apple Ethernet address PROM (island $08000): 8 bytes at $x1 of each
// 16-byte group, XOR of all eight == $FF.
uint8_t av_enet_prom_read(config_t *cfg, uint32_t win_off, uint32_t addr);
void av_enet_prom_write(config_t *cfg, uint32_t win_off, uint32_t addr, uint8_t value);

// The PSC's Ethernet-channel touch hook (av_psc_chan_touch_fn): the guest
// programmed channel 1 or 2, so a waiting frame may move.
void av_enet_dma_touch(void *ctx);

#endif // GS_MACHINES_AV_ENET_H
