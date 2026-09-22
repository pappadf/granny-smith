// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sound.h
// Public interface for sound emulation.

#ifndef SOUND_H
#define SOUND_H

// === Includes ===
#include "common.h"

#include <stdbool.h>
#include <stdint.h>

// === Forward Declarations ===
struct memory;
typedef struct memory memory_map_t;
struct scheduler;

// === Type Definitions ===
struct sound;
typedef struct sound sound_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

sound_t *sound_init(memory_map_t *map, struct scheduler *scheduler, checkpoint_t *checkpoint);

void sound_delete(sound_t *sound);

void sound_checkpoint(sound_t *restrict sound, checkpoint_t *checkpoint);

// === Operations ===

void sound_use_buffer(sound_t *restrict sound, bool main);

void sound_volume(sound_t *restrict sound, unsigned int volume);

void sound_enable(sound_t *restrict sound, bool enabled);

void sound_vbl(sound_t *restrict sound);

// === Object-model accessors =================================================
//
// Read-only views over the sound subsystem, read by the `sound_surface_t`
// vtable this module fills in (see sound_surface.h) rather than by a
// hand-rolled class of its own.  `sample_rate` is the legacy 22.255 kHz PWM
// rate hardcoded in the platform layer; it is exposed as an attribute so the
// Plus reports the same vocabulary as every other engine.
//
// There is no `sound_mute()`: the surface's set_muted hook goes straight to
// plus_snd_set_muted -> sound_enable(), which is the inverted-semantics
// wrapper a mute helper would have been.

bool sound_get_enabled(const sound_t *sound);
unsigned sound_get_volume(const sound_t *sound);
unsigned sound_get_sample_rate(const sound_t *sound);

#endif // SOUND_H
