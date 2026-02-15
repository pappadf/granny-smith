// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sound.c
// Implements the Macintosh Plus sound subsystem.

#include "sound.h"
#include "memory.h"
#include "platform.h"
#include "system.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct sound {
    // Plain data first (for checkpointing via offsetof)
    unsigned int volume;
    bool enabled;
    // Pointers last (excluded from checkpoint)
    uint16_t *buffer;
    memory_map_t *mem; // Memory map for buffer access
};

// System 6 starts at an offset of 90 words into the sound buffer, selected
// based on the machine type byte from ROM (see docs/hw/sound.md for details)
#define VBL_OFFSET 90

#define BUF_SIZE 370

// Selects either main or alternate sound buffer from RAM
void sound_use_buffer(sound_t *sound, bool main) {
    if (main) {
        // [2] the address of the main sound buffer is top of ram - 0x300
        sound->buffer = (uint16_t *)ram_native_pointer(sound->mem, 0x400000 - 0x300);
    } else {
        // [1] the address of the alternate sound buffer is SoundBase-$5C00
        sound->buffer = (uint16_t *)ram_native_pointer(sound->mem, 0x400000 - 0x300 - 0x5c00);
    }
}

// Sets the sound volume (0-7)
void sound_volume(sound_t *sound, unsigned int volume) {
    assert(volume < 8);

    sound->volume = volume;
}

// Enables or disables the sound output
void sound_enable(sound_t *sound, bool enabled) {
    if (sound->enabled && !enabled)
        printf("sound disabled\n");
    if (!sound->enabled && enabled)
        printf("sound enabled\n");

    sound->enabled = enabled;
}

// Processes sound buffer during vertical blank interval
void sound_vbl(sound_t *restrict sound) {
    if (!sound->enabled)
        return;

    assert(sound->buffer != NULL);

    uint8_t buf[BUF_SIZE];

    // The "native" sound buffer is in big endian, and high order byte is the sound
    for (int i = 0; i < (BUF_SIZE - VBL_OFFSET); i++)
        buf[i] = BE16(sound->buffer[VBL_OFFSET + i]) >> 8;

    for (int i = BUF_SIZE - VBL_OFFSET; i < BUF_SIZE; i++)
        buf[i] = BE16(sound->buffer[i - (BUF_SIZE - VBL_OFFSET)]) >> 8;

    // All buffers are forwarded unconditionally. The platform layer (em_audio.c)
    // detects silence and drops silent buffers when the ring buffer already meets
    // the target depth, preventing unbounded latency growth when the emulator
    // runs faster than real-time.
    platform_play_8bit_pwm(buf, BUF_SIZE, sound->volume);
}

// Initializes the sound subsystem
sound_t *sound_init(memory_map_t *map, checkpoint_t *checkpoint) {
    sound_t *sound = (sound_t *)malloc(sizeof(sound_t));

    if (sound == NULL)
        return NULL;

    memset(sound, 0, sizeof(sound_t));

    // Store memory map reference
    sound->mem = map;

    // Default to a reasonable audible volume unless/until the guest sets it via VIA.
    // This avoids a confusing "no sound" experience at cold boot if the ROM hasn't
    // written the volume register yet.
    sound->volume = 4;

    sound_use_buffer(sound, true);

    platform_init_sound();

    // Load from checkpoint if provided
    if (checkpoint) {
        // Read contiguous plain-data portion of sound_t (everything before pointer(s)).
        // Do NOT restore buffer contents here; RAM is checkpointed separately and
        // already contains both main and alternate buffers. VIA outputs will also
        // re-drive buffer selection, volume and enable after device init.
        size_t data_size = offsetof(sound_t, buffer);
        system_read_checkpoint_data(checkpoint, sound, data_size);
    }

    return sound;
}

// Frees resources associated with the sound subsystem
void sound_delete(sound_t *sound) {
    if (!sound)
        return;
    free(sound);
}

// Saves sound state to a checkpoint
void sound_checkpoint(sound_t *restrict sound, checkpoint_t *checkpoint) {
    if (!sound || !checkpoint)
        return;
    // Write contiguous plain-data portion of sound_t in one operation
    size_t data_size = offsetof(sound_t, buffer);
    system_write_checkpoint_data(checkpoint, sound, data_size);
    // Do not write buffer contents; RAM is serialized by memory_map_checkpoint.
}
