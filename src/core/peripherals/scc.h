// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scc.h
// Public interface for Serial Communications Controller (SCC) emulation.

#ifndef SCC_H
#define SCC_H

// === Includes ===
#include "common.h"
#include "memory.h"

// === Forward Declarations ===
struct scheduler;

// === Type Definitions ===
struct scc;
typedef struct scc scc_t;

// Callback for SCC interrupt line changes (per-instance routing)
typedef void (*scc_irq_fn)(void *context, bool active);

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Create an SCC instance with per-instance IRQ callback routing.
// irq_cb: called when the SCC interrupt line changes state
// cb_context: opaque pointer passed to the callback
scc_t *scc_init(memory_map_t *map, struct scheduler *scheduler, scc_irq_fn irq_cb, void *cb_context,
                checkpoint_t *checkpoint);

void scc_delete(scc_t *scc);

void scc_checkpoint(scc_t *restrict scc, checkpoint_t *checkpoint);

// === Operations ===

void scc_clock(scc_t *restrict scc, int n);

void scc_reset(scc_t *restrict scc);

void scc_set_dcd_a(scc_t *restrict scc, unsigned char val);

void scc_set_dcd_b(scc_t *restrict scc, unsigned char val);

void scc_dcd(scc_t *restrict scc, unsigned int ch, unsigned int dcd);

int scc_sdlc_send(scc_t *restrict scc, uint8_t *buf, size_t len);

#endif // SCC_H
