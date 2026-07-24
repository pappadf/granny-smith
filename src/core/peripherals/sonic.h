// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sonic.h
// National Semiconductor DP83932 SONIC Ethernet controller — the Quadra
// generation's NIC (proposal-machine-quadra-700-900-950.md §11), machine-
// independent so the later NIC-equipped machines can reuse it.
//
// Phase F (v1) scope: the full 16-bit register file with the semantics
// Apple's ROM self-tests pin (OS/StartMgr/UnivTestEnv/SONIC_*.c — register
// bit-march quirks, CAM load/readback via descriptor DMA, interrupt
// mask/status gating, MAC/ENDEC/transceiver loopback through the real
// RRA/RDA/TDA linked-list buffer management, DP83932B datasheet §3/§4).
// There is no wire: non-loopback transmissions complete successfully into
// the void and nothing is ever received (§11.3 — bridging SONIC to a
// network is a separate proposal).

#ifndef SONIC_H
#define SONIC_H

#include "checkpoint.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct sonic sonic_t;

// INT output callback (level; active-high argument — wired active-low to
// VIA2 PA0 on the Quadras by the machine).
typedef void (*sonic_irq_cb)(void *context, bool active);

// Bus-master memory hooks: SONIC DMAs descriptors and packet data to/from
// guest-PHYSICAL memory (no IOMMU on this family — ref §16.3).  Width is
// 1, 2, or 4 bytes; values are big-endian guest data in host integers.
// When no hooks are installed the chip uses the machine bus
// (mmu_read_physical_* / mmu_write_physical_*); unit tests install mock
// hooks over a flat buffer.
typedef uint32_t (*sonic_mem_read_fn)(void *context, uint32_t phys, unsigned width);
typedef void (*sonic_mem_write_fn)(void *context, uint32_t phys, uint32_t value, unsigned width);

sonic_t *sonic_init(checkpoint_t *cp);
void sonic_delete(sonic_t *s);
void sonic_checkpoint(sonic_t *s, checkpoint_t *cp);

void sonic_set_irq_callback(sonic_t *s, sonic_irq_cb cb, void *context);
void sonic_set_memory_hooks(sonic_t *s, sonic_mem_read_fn rd, sonic_mem_write_fn wr, void *context);

// Register file access (reg = RA5..RA0, i.e. the byte offset already
// divided by the board's 4-byte register spacing).
uint16_t sonic_reg_read(sonic_t *s, uint32_t reg);
void sonic_reg_write(sonic_t *s, uint32_t reg, uint16_t value);

// Hardware reset (power-on / RESET line).
void sonic_hard_reset(sonic_t *s);

#endif // SONIC_H
