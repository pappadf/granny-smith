// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sound.c
// Implements the Macintosh Plus sound subsystem.

#include "sound.h"
#include "audio_out.h"
#include "log.h"
#include "memory.h"
#include "object.h"
#include "platform.h"
#include "scheduler.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

LOG_USE_CATEGORY_NAME("sound");

// Forward declaration — class descriptor is at the bottom of the file but
// sound_init / sound_delete reference it.
extern const class_desc_t sound_class;

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// System 6 starts at an offset of 90 words into the sound buffer, selected
// based on the machine type byte from ROM. With the progressive scan below
// this is the scan-position anchor: the word the PWM scan reads next at the
// moment the VBL interrupt fires (empirically validated against the System 6
// Sound Driver's write phase).
#define VBL_OFFSET 90

#define BUF_SIZE 370

// PWM scan output rate: 370 samples per VBL at ~60.15 Hz (~22.255 kHz)
#define SOUND_SRC_RATE_HZ 22255

// The scan advances one word per horizontal scanline (352 CPU cycles: 704
// pixel clocks at half the 15.6672 MHz dot clock). Reading the whole buffer
// once per VBL tears against apps that stream into the buffer mid-frame,
// and even coarse sub-frame batches audibly crackle: MusicWorks' write
// frontier moves at nearly scan speed, so a batch that reads N words early
// flips back and forth across the frontier while the two crawl alongside
// each other, turning the single boundary sample real hardware produces
// into a burst of old/new alternations. The scan therefore runs at true
// per-scanline resolution: one word per event, 370 per frame, 352 cycles
// apart — the same event rate the ASC producer already runs (22,255/s).
#define SCAN_BATCH_WORDS  1
#define SCAN_BATCHES      370
#define SCAN_BATCH_CYCLES 352

// Producer push batch, mirroring the ASC's (asc.c ASC_PUSH_BATCH)
#define SOUND_PUSH_BATCH 64

struct sound {
    // Plain data first (for checkpointing via offsetof)
    unsigned int volume;
    bool enabled;
    uint16_t scan_idx; // next buffer word the PWM scan reads (0..369)
    uint16_t scan_left; // scan batch events remaining in the current frame
    uint64_t scan_anchor; // scheduler cycle count at the frame's VBL
    // Pointers / transient state last (excluded from checkpoint)
    uint16_t *buffer;
    memory_map_t *mem; // Memory map for buffer access
    scheduler_t *scheduler; // drives the sub-frame scan events
    struct object *object; // object-tree node; lifetime tied to this sound
    // Producer push batch (transient — not checkpointed; a restore only
    // loses <64 frames of host-side audio, never guest state)
    int out_count;
    int16_t out_buf[SOUND_PUSH_BATCH];
};

static void sound_flush(sound_t *sound);

// Selects either main or alternate sound buffer from RAM.
// The sound scanner reads from the top of *installed* RAM (the ROM places
// the buffer at MemTop-$300), so derive the address from the machine's RAM
// size — hardcoding the 4 MB maximum reads out of bounds on 1/2/2.5 MB
// machines (ram_native_pointer does not fold addresses through the RAM
// mirror the way CPU accesses do).
void sound_use_buffer(sound_t *sound, bool main) {
    uint32_t top = memory_ram_size(sound->mem);
    if (main) {
        // [2] the address of the main sound buffer is top of ram - 0x300
        sound->buffer = (uint16_t *)ram_native_pointer(sound->mem, top - 0x300);
    } else {
        // [1] the address of the alternate sound buffer is SoundBase-$5C00
        sound->buffer = (uint16_t *)ram_native_pointer(sound->mem, top - 0x300 - 0x5c00);
    }
}

// Sets the sound volume (0-7)
void sound_volume(sound_t *sound, unsigned int volume) {
    assert(volume < 8);

    sound->volume = volume;
}

// Enables or disables the sound output
void sound_enable(sound_t *sound, bool enabled) {
    if (sound->enabled && !enabled) {
        LOG(2, "sound disabled");
        sound->enabled = false;
        sound_flush(sound); // deliver any tail frames before going quiet
        return;
    }
    if (!sound->enabled && enabled)
        LOG(2, "sound enabled");

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
    return sound ? SOUND_SRC_RATE_HZ : 0u;
}
void sound_mute(sound_t *sound, bool muted) {
    if (!sound)
        return;
    sound_enable(sound, !muted);
}

// Flushes the pending push batch to the shared host audio stream
static void sound_flush(sound_t *sound) {
    if (sound->out_count <= 0)
        return;
    audio_out_push(sound->out_buf, sound->out_count, (int)sound->volume);
    sound->out_count = 0;
}

static void sound_scan_batch_event(void *source, uint64_t data);

// Reads the next SCAN_BATCH_WORDS buffer words at their (batched) scanline
// time — the progressive PWM scan. The high byte of each word is the sound
// sample (8-bit offset binary); the low byte drives the disk PWM (not
// modeled here). A mid-frame disable gates the output but the scan position
// keeps advancing, like the hardware's.
static void sound_scan_batch(sound_t *sound) {
    if (sound->enabled) {
        for (int i = 0; i < SCAN_BATCH_WORDS; i++) {
            uint8_t b = BE16(sound->buffer[sound->scan_idx]) >> 8;
            sound->out_buf[sound->out_count++] = (int16_t)(((int)b - 128) << 8);
            if (sound->out_count >= SOUND_PUSH_BATCH)
                sound_flush(sound);
            sound->scan_idx = (uint16_t)((sound->scan_idx + 1) % BUF_SIZE);
        }
    } else {
        sound->scan_idx = (uint16_t)((sound->scan_idx + SCAN_BATCH_WORDS) % BUF_SIZE);
    }

    if (sound->scan_left > 0)
        sound->scan_left--;
    if (sound->scan_left > 0) {
        if (sound->scheduler) {
            // Schedule against the frame anchor, not relative to this event's
            // fire time: events land on instruction boundaries, so a relative
            // +352 accumulates a few cycles of lag per event — enough for the
            // chain to overrun the frame and lose its tail words to the next
            // VBL's re-anchor (heard as a splice click every frame).
            uint64_t slot = sound->scan_anchor + (uint64_t)SCAN_BATCH_CYCLES * (SCAN_BATCHES - sound->scan_left);
            uint64_t now = scheduler_cpu_cycles(sound->scheduler);
            uint64_t delay = (slot > now) ? slot - now : 1;
            scheduler_new_cpu_event(sound->scheduler, &sound_scan_batch_event, sound, 0, delay, 0);
        }
    } else {
        // Frame's last batch: deliver the partial push batch at the frame
        // boundary so capture/host never lag a frame behind
        sound_flush(sound);
    }
}

// Scheduler callback for the sub-frame scan chain
static void sound_scan_batch_event(void *source, uint64_t data) {
    (void)data;
    sound_scan_batch((sound_t *)source);
}

// Anchors the PWM scan at the VBL interrupt: the scan reads word VBL_OFFSET
// next, then walks the whole buffer (wrapping 369 -> 0) as 37 batches spread
// across the frame. The chain runs whether or not sound is enabled — the
// hardware scan never stops, and the enable bit gates the output inside each
// batch, so a mid-frame enable/disable takes effect within 10 samples
// instead of being quantized to the frame.
void sound_vbl(sound_t *restrict sound) {
    if (sound->scheduler)
        remove_event(sound->scheduler, &sound_scan_batch_event, sound);

    assert(sound->buffer != NULL);
    sound->scan_idx = VBL_OFFSET;
    sound->scan_left = SCAN_BATCHES;
    if (sound->scheduler)
        sound->scan_anchor = scheduler_cpu_cycles(sound->scheduler);
    sound_scan_batch(sound);
}

// Initializes the sound subsystem
sound_t *sound_init(memory_map_t *map, scheduler_t *scheduler, checkpoint_t *checkpoint) {
    sound_t *sound = (sound_t *)malloc(sizeof(sound_t));

    if (sound == NULL)
        return NULL;

    memset(sound, 0, sizeof(sound_t));

    // Store memory map reference
    sound->mem = map;
    sound->scheduler = scheduler;
    if (scheduler)
        scheduler_new_event_type(scheduler, "sound", sound, "pwm_scan", &sound_scan_batch_event);

    // Default to a reasonable audible volume unless/until the guest sets it via VIA.
    // This avoids a confusing "no sound" experience at cold boot if the ROM hasn't
    // written the volume register yet.
    sound->volume = 4;

    sound_use_buffer(sound, true);

    // Open the shared host audio stream: mono int16 at the PWM scan rate
    audio_out_open(SOUND_SRC_RATE_HZ, 1);

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
    if (sound->object) {
        object_set_label(sound->object, "Sound");
        object_set_order(sound->object, 110);
        object_attach(machine_object(), sound->object);
        // Deterministic capture sink for golden-WAV tests (sound.capture.*)
        audio_out_capture_attach(sound->object);
    }

    return sound;
}

// Frees resources associated with the sound subsystem
void sound_delete(sound_t *sound) {
    if (!sound)
        return;
    if (sound->object) {
        audio_out_capture_detach();
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

// `sound.match(reference)` — sample-exact compare of the last capture against
// a golden PCM WAV (the audio analog of screen.match). Delegates to the
// shared capture sink in audio_out.c; a mismatch returns val_err so the
// headless script runner fails the integration test.
static value_t sound_method_match(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    return audio_out_match_value(argv[0].s);
}

static const arg_decl_t sound_mute_args[] = {
    {.name = "muted", .kind = V_BOOL, .doc = "true to mute, false to unmute"},
};

static const arg_decl_t sound_match_args[] = {
    {.name = "reference", .kind = V_STRING, .doc = "Reference WAV path (PCM int16)"},
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
    {.kind = M_METHOD,
     .name = "match",
     .doc = "Compare the last capture against a reference WAV (true if identical)",
     .method = {.args = sound_match_args, .nargs = 1, .result = V_BOOL, .fn = sound_method_match}},
};

const class_desc_t sound_class = {
    .name = "sound",
    .members = sound_members,
    .n_members = sizeof(sound_members) / sizeof(sound_members[0]),
};
