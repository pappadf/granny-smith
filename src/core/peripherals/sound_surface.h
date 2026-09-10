// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sound_surface.h
// The one `machine.sound` contract, shared by every sound engine in the tree.
//
// Five engines used to define their own class: the Plus PWM, the ASC, the AV's
// Singer, and the AWACS on the PDM and TNT.  The review's F-34 read that as
// five copies of one class.  It is not quite -- the five member lists really do
// differ -- but the differences turned out to be mostly OUR gaps rather than
// the hardware's:
//
//   * Volume exists on all five.  The Plus and ASC pass a real 0..7 level to
//     audio_out_push(); the AWACS and Singer machines apply their attenuation
//     ladder during their own mixing and push a hardcoded 7 ("attenuation
//     already applied"), so the value never had to leave the device and nobody
//     surfaced it.  awacs_speaker_gains() has been computing gain AND mute on
//     every render all along.
//   * Sound input exists on all five.  Only the AV models it.  The TNT's own
//     register notes document an "Input SubFrame Select" field; the path is
//     simply not written yet.
//
// So the surface declares the WHOLE vocabulary and every machine presents it.
// A device that does not model something reports the honest value -- input DMA
// that is not running reads false -- rather than the member being absent.  That
// is the point: `machine.sound.in_enabled` reading false on a Power Mac is a
// visible gap that someone can close, where a missing attribute was invisible
// for years.
//
// Chip-register detail that is genuinely specific to one engine does NOT live
// here; it hangs off a child object, the way `capture` already does.

#ifndef GS_CORE_PERIPHERALS_SOUND_SURFACE_H
#define GS_CORE_PERIPHERALS_SOUND_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

struct object;

// What an engine tells the shared class about itself.  Every getter is
// required: an engine that does not model a thing returns the value that is
// true of a machine where that thing is idle (false / 0), not a "no answer".
typedef struct sound_surface {
    uint32_t (*sample_rate)(void *ctx); // output rate in Hz
    uint32_t (*volume)(void *ctx); // 0..7, the Sound control panel scale
    bool (*muted)(void *ctx); // output gate closed
    bool (*out_enabled)(void *ctx); // output engine/DMA running
    bool (*in_enabled)(void *ctx); // input engine/DMA running
    uint64_t (*frames)(void *ctx); // output frames rendered since power-on
    int32_t (*peak)(void *ctx); // loudest |sample| pushed to the host
    uint64_t (*overruns)(void *ctx); // engine over/underruns since power-on

    // Optional writers.  NULL where the guest's own mixer is the only thing
    // that may change the value, which is the case on every engine whose level
    // is a codec register the driver owns.  Without them `enabled` and
    // `volume` are read-only and say so when written.
    void (*set_muted)(void *ctx, bool muted);
    void (*set_volume)(void *ctx, uint32_t vol_0_7);

    void *ctx;
} sound_surface_t;

// Map a codec attenuation-ladder index onto the 0..7 Sound control panel
// scale.  The AWACS and Singer ladders are both 1.5 dB per step with $0 =
// 0 dB (loudest) and $F = -22.5 dB (singer.md §3; the ASCO 2300 ladder in
// awacs.c is the same shape), and the Mac's slider is 3 dB per step -- so two
// ladder steps make one slider step, and the mapping is exact across the
// whole range rather than a fitted approximation.
static inline uint32_t sound_volume_from_atten(unsigned atten_0_15) {
    if (atten_0_15 > 15u)
        atten_0_15 = 15u;
    return 7u - (atten_0_15 / 2u);
}

// Create `machine.sound` over `s`.  The surface is copied, so the caller may
// pass a stack literal.  Attaches the shared `capture` child too.
struct object *sound_object_new(const sound_surface_t *s);

// Tear it down (detach + delete, including the capture child).
void sound_object_delete(struct object *obj);

#endif // GS_CORE_PERIPHERALS_SOUND_SURFACE_H
