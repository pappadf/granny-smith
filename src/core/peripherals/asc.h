// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// asc.h
// Public interface for Apple Sound Chip (ASC) emulation (SE/30).

#ifndef ASC_H
#define ASC_H

// === Includes ===

#include "common.h"
#include "memory.h"
#include "scheduler.h"
#include "via.h"

#include <stdint.h>

// === Forward Declarations ===

struct asc;

// === Type Definitions ===

// Opaque handle for the ASC peripheral state
typedef struct asc asc_t;

// Board-level fold of the ASC's stereo output to the (mono) internal
// speaker: IIx/IIcx wire the speaker to the left channel; the SE/30 board
// sums both channels. (Headphone-jack stereo switching is deliberately not
// modeled — see proposal-sound-support-all-models §9.)
typedef enum asc_mix {
    ASC_MIX_CH_A = 0, // speaker = left channel (IIx, IIcx)
    ASC_MIX_SUM = 1, // speaker = left + right analog mix, averaged (SE/30)
} asc_mix_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Creates and initialises an ASC instance, registering it in the memory map
asc_t *asc_init(memory_map_t *map, scheduler_t *scheduler, checkpoint_t *checkpoint);

// Frees all resources associated with an ASC instance
void asc_delete(asc_t *asc);

// Saves ASC state to a checkpoint
void asc_checkpoint(asc_t *restrict asc, checkpoint_t *checkpoint);

// === Wiring ===

// Connects the ASC interrupt output to VIA2 CB1 (called after VIA2 creation).
// Convenience wrapper over asc_set_irq_handler for the GLUE machines.
void asc_set_via(asc_t *asc, via_t *via);

// Installs a chipset-specific interrupt sink; `fn` receives the logical IRQ
// level (true = asserted). RBV machines wire rbv_set_snd_irq (RvIFR bit 4);
// OSS machines route to their per-source mask model. The chip model itself
// stays chipset-agnostic.
void asc_set_irq_handler(asc_t *asc, void (*fn)(void *ctx, bool active), void *ctx);

// Selects the board's speaker mix (see asc_mix_t). Not checkpointed —
// machines call this unconditionally right after asc_init.
void asc_set_mix(asc_t *asc, asc_mix_t mix);

// === Operations ===

// Get the memory-mapped I/O interface for machine-level address decode
const memory_interface_t *asc_get_memory_interface(asc_t *asc);

#endif // ASC_H
