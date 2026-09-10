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
#include "sound_surface.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

LOG_USE_CATEGORY_NAME("sound");

// Forward declaration — class descriptor is at the bottom of the file but
// sound_init / sound_delete reference it.

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
    // Host-stream statistics, for machine.sound.  Transient like the batch
    // above: a restore resets them, which is why they count "since power-on"
    // and not "since boot".
    uint64_t frames_pushed;
    int32_t peak;
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
    for (int i = 0; i < sound->out_count; i++) {
        int32_t a = sound->out_buf[i] < 0 ? -(int32_t)sound->out_buf[i] : (int32_t)sound->out_buf[i];
        if (a > sound->peak)
            sound->peak = a;
    }
    sound->frames_pushed += (uint64_t)sound->out_count;
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

// === machine.sound surface (sound_surface.h) =================================
//
// The Plus PWM has no DMA engine and no input path: `out_enabled` is the gate
// itself (the buffer is scanned whenever sound is on), `in_enabled` is false
// because the machine has no sound-input hardware at all, and `overruns` is 0
// because there is no producer/consumer boundary to overrun -- the VBL scan
// pushes whatever the buffer holds.

static uint32_t plus_snd_sample_rate(void *ctx) {
    return sound_get_sample_rate((sound_t *)ctx);
}
static uint32_t plus_snd_volume(void *ctx) {
    return sound_get_volume((sound_t *)ctx);
}
static void plus_snd_set_volume(void *ctx, uint32_t v) {
    sound_volume((sound_t *)ctx, (unsigned)v);
}
static bool plus_snd_muted(void *ctx) {
    return !sound_get_enabled((sound_t *)ctx);
}
static void plus_snd_set_muted(void *ctx, bool muted) {
    sound_enable((sound_t *)ctx, !muted);
}
static bool plus_snd_out_enabled(void *ctx) {
    return sound_get_enabled((sound_t *)ctx);
}
static bool plus_snd_in_enabled(void *ctx) {
    (void)ctx;
    return false; // no sound-input hardware on a Plus
}
static uint64_t plus_snd_frames(void *ctx) {
    return ((sound_t *)ctx)->frames_pushed;
}
static int32_t plus_snd_peak(void *ctx) {
    return ((sound_t *)ctx)->peak;
}
static uint64_t plus_snd_overruns(void *ctx) {
    (void)ctx;
    return 0; // the VBL scan cannot overrun: it pushes what is there
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
    const sound_surface_t surface = {
        .sample_rate = plus_snd_sample_rate,
        .volume = plus_snd_volume,
        .muted = plus_snd_muted,
        .out_enabled = plus_snd_out_enabled,
        .in_enabled = plus_snd_in_enabled,
        .frames = plus_snd_frames,
        .peak = plus_snd_peak,
        .overruns = plus_snd_overruns,
        .set_muted = plus_snd_set_muted,
        .set_volume = plus_snd_set_volume,
        .ctx = sound,
    };
    sound->object = sound_object_new(&surface);

    return sound;
}

// Frees resources associated with the sound subsystem
void sound_delete(sound_t *sound) {
    if (!sound)
        return;
    if (sound->object) {
        sound_object_delete(sound->object);
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
