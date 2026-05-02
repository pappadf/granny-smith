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

// === Type Definitions ===
struct sound;
typedef struct sound sound_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

sound_t *sound_init(memory_map_t *map, checkpoint_t *checkpoint);

void sound_delete(sound_t *sound);

void sound_checkpoint(sound_t *restrict sound, checkpoint_t *checkpoint);

// === Operations ===

void sound_buffer(sound_t *restrict sound, uint16_t *buffer);

void sound_use_buffer(sound_t *restrict sound, bool main);

void sound_volume(sound_t *restrict sound, unsigned int volume);

void sound_enable(sound_t *restrict sound, bool enabled);

void sound_vbl(sound_t *restrict sound);

void validate_sound(sound_t *restrict sound);

// === M7f — object-model accessors ===========================================
//
// Read-only views over the sound subsystem used by the `sound` object
// class. `mute(bool)` is a thin wrapper over sound_enable so users
// don't have to remember the inverted semantics ("mute true" → enabled
// false). `sample_rate` is the legacy 22.255 kHz PWM rate hardcoded in
// the platform layer; we expose it as an attribute for parity with the
// proposal §5.4 listing.

bool sound_get_enabled(const sound_t *sound);
unsigned sound_get_volume(const sound_t *sound);
unsigned sound_get_sample_rate(const sound_t *sound);
void sound_mute(sound_t *sound, bool muted);

#endif // SOUND_H
