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

// Set the BRG source clock frequencies (Hz) for accurate baud-rate timing.
// pclk_hz: PCLK input (e.g. 7833600 for C8M); rtxc_hz: RTxC input (e.g. 3686400).
void scc_set_clocks(scc_t *restrict scc, uint32_t pclk_hz, uint32_t rtxc_hz);

void scc_reset(scc_t *restrict scc);

void scc_set_dcd_a(scc_t *restrict scc, unsigned char val);

void scc_set_dcd_b(scc_t *restrict scc, unsigned char val);

void scc_dcd(scc_t *restrict scc, unsigned int ch, unsigned int dcd);

int scc_sdlc_send(scc_t *restrict scc, uint8_t *buf, size_t len);

// Get the memory-mapped I/O interface for machine-level address decode
const memory_interface_t *scc_get_memory_interface(scc_t *scc);

// Enable/disable external loopback (port A TX → port B RX, port B TX → port A RX)
void scc_set_external_loopback(scc_t *scc, bool enabled);

// Query external loopback state
bool scc_get_external_loopback(scc_t *scc);

#endif // SCC_H
