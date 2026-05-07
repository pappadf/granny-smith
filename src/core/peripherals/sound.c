// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sound.c
// Implements the Macintosh Plus sound subsystem.

#include "sound.h"
#include "memory.h"
#include "object.h"
#include "platform.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

// Forward declaration — class descriptor is at the bottom of the file but
// sound_init / sound_delete reference it.
extern const class_desc_t sound_class;

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
    struct object *object; // object-tree node; lifetime tied to this sound
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

// === M7f — read-only views and mute helper ==================================

bool sound_get_enabled(const sound_t *sound) {
    return sound ? sound->enabled : false;
}
unsigned sound_get_volume(const sound_t *sound) {
    return sound ? sound->volume : 0;
}
unsigned sound_get_sample_rate(const sound_t *sound) {
    // Plus PWM sound runs at 22.255 kHz (1 buffer / VBL × BUF_SIZE).
    // The rate is fixed by the hardware/host platform layer.
    (void)sound;
    return sound ? 22255u : 0u;
}
void sound_mute(sound_t *sound, bool muted) {
    if (!sound)
        return;
    sound_enable(sound, !muted);
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

    // Object-tree binding — instance_data is the sound itself.
    sound->object = object_new(&sound_class, sound, "sound");
    if (sound->object)
        object_attach(object_root(), sound->object);

    return sound;
}

// Frees resources associated with the sound subsystem
void sound_delete(sound_t *sound) {
    if (!sound)
        return;
    if (sound->object) {
        object_detach(sound->object);
        object_delete(sound->object);
        sound->object = NULL;
    }
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

// === Object-model class descriptor =========================================
//
// Plus's PWM sound module per proposal §5.4: `sound.enabled`,
// `sound.sample_rate`, `sound.volume` attributes plus `mute(bool)`
// method. SE/30 / IIcx use the Apple Sound Chip and don't populate
// `cfg->sound`; the object is only attached when the field is set.
//
// instance_data is the sound_t* itself; lifetime is tied to
// sound_init / sound_delete.

static sound_t *sound_self_from(struct object *self) {
    return (sound_t *)object_data(self);
}

static value_t sound_attr_enabled_get(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(sound_get_enabled(sound_self_from(self)));
}
static value_t sound_attr_enabled_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    sound_t *sound = sound_self_from(self);
    if (!sound) {
        value_free(&in);
        return val_err("sound not available");
    }
    sound_enable(sound, in.b);
    return val_none();
}

static value_t sound_attr_volume_get(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(1, sound_get_volume(sound_self_from(self)));
}
static value_t sound_attr_volume_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    sound_t *sound = sound_self_from(self);
    if (!sound)
        return val_err("sound not available");
    uint64_t v = in.u;
    if (v >= 8)
        return val_err("sound.volume: must be 0..7 (got %llu)", (unsigned long long)v);
    sound_volume(sound, (unsigned)v);
    return val_none();
}

static value_t sound_attr_sample_rate(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(4, sound_get_sample_rate(sound_self_from(self)));
}

static value_t sound_method_mute(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    sound_t *sound = sound_self_from(self);
    if (!sound)
        return val_err("sound not available");
    sound_mute(sound, argv[0].b);
    return val_none();
}

static const arg_decl_t sound_mute_args[] = {
    {.name = "muted", .kind = V_BOOL, .doc = "true to mute, false to unmute"},
};

static const member_t sound_members[] = {
    {.kind = M_ATTR,
     .name = "enabled",
     .doc = "Sound output gate (writable mirror of mute)",
     .flags = 0,
     .attr = {.type = V_BOOL, .get = sound_attr_enabled_get, .set = sound_attr_enabled_set}},
    {.kind = M_ATTR,
     .name = "volume",
     .doc = "Output level (0..7)",
     .flags = 0,
     .attr = {.type = V_UINT, .get = sound_attr_volume_get, .set = sound_attr_volume_set}},
    {.kind = M_ATTR,
     .name = "sample_rate",
     .doc = "Output sample rate in Hz",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = sound_attr_sample_rate, .set = NULL}},
    {.kind = M_METHOD,
     .name = "mute",
     .doc = "Mute or unmute the sound output",
     .method = {.args = sound_mute_args, .nargs = 1, .result = V_NONE, .fn = sound_method_mute}},
};

const class_desc_t sound_class = {
    .name = "sound",
    .members = sound_members,
    .n_members = sizeof(sound_members) / sizeof(sound_members[0]),
};
