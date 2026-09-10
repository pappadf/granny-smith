// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sound_surface.c
// The shared `machine.sound` class — see sound_surface.h for why there is one.

#include "sound_surface.h"

#include "audio_out.h"
#include "machine.h"
#include "object.h"
#include "value.h"

#include <stdlib.h>

// The surface is copied into the object's instance data so a caller may hand
// us a stack literal at construction.
static sound_surface_t *surf(struct object *self) {
    return (sound_surface_t *)object_data(self);
}

static value_t snd_sample_rate(struct object *self, const member_t *m) {
    (void)m;
    sound_surface_t *s = surf(self);
    return val_uint(4, s->sample_rate(s->ctx));
}

static value_t snd_volume(struct object *self, const member_t *m) {
    (void)m;
    sound_surface_t *s = surf(self);
    return val_uint(1, s->volume(s->ctx));
}

// `enabled` is the output GATE, and is the inverse of muted.  It is not
// out_enabled: a machine can have its DMA running into a muted output, which
// is exactly what the Sound control panel's mute checkbox does.
static value_t snd_enabled_get(struct object *self, const member_t *m) {
    (void)m;
    sound_surface_t *s = surf(self);
    return val_bool(!s->muted(s->ctx));
}

static value_t snd_enabled_set(struct object *self, const member_t *m, value_t v) {
    (void)m;
    sound_surface_t *s = surf(self);
    if (!s->set_muted)
        return val_err("enabled: this engine's gate is guest-controlled and cannot be set from here");
    s->set_muted(s->ctx, !v.b);
    return val_none();
}

static value_t snd_volume_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    sound_surface_t *s = surf(self);
    if (!s->set_volume)
        return val_err("volume: this engine's level is a codec register the guest driver owns");
    if (in.u >= 8)
        return val_err("sound.volume: must be 0..7 (got %llu)", (unsigned long long)in.u);
    s->set_volume(s->ctx, (uint32_t)in.u);
    return val_none();
}

static value_t snd_out_enabled(struct object *self, const member_t *m) {
    (void)m;
    sound_surface_t *s = surf(self);
    return val_bool(s->out_enabled(s->ctx));
}

static value_t snd_in_enabled(struct object *self, const member_t *m) {
    (void)m;
    sound_surface_t *s = surf(self);
    return val_bool(s->in_enabled(s->ctx));
}

static value_t snd_frames(struct object *self, const member_t *m) {
    (void)m;
    sound_surface_t *s = surf(self);
    return val_uint(8, s->frames(s->ctx));
}

static value_t snd_peak(struct object *self, const member_t *m) {
    (void)m;
    sound_surface_t *s = surf(self);
    return val_int(s->peak(s->ctx));
}

static value_t snd_overruns(struct object *self, const member_t *m) {
    (void)m;
    sound_surface_t *s = surf(self);
    return val_uint(8, s->overruns(s->ctx));
}

static value_t snd_method_mute(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    sound_surface_t *s = surf(self);
    if (!s->set_muted)
        return val_err("mute: this engine's gate is guest-controlled and cannot be set from here");
    s->set_muted(s->ctx, argv[0].b);
    return val_none();
}

static value_t snd_method_match(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    return audio_out_match_value(argv[0].s);
}

static const arg_decl_t snd_mute_args[] = {
    {.name = "muted", .kind = V_BOOL, .doc = "true to mute, false to unmute"},
};

static const arg_decl_t snd_match_args[] = {
    {.name = "reference", .kind = V_STRING, .doc = "Reference WAV path (PCM int16)"},
};

static const member_t sound_members[] = {
    {.kind = M_ATTR,
     .name = "sample_rate",
     .doc = "Output sample rate in Hz",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = snd_sample_rate, .set = NULL}},
    {.kind = M_ATTR,
     .name = "volume",
     .doc = "Output level 0..7 (the Sound control panel scale)",
     .attr = {.type = V_UINT, .get = snd_volume, .set = snd_volume_set}},
    {.kind = M_ATTR,
     .name = "enabled",
     .doc = "Sound output gate — false when muted",
     .attr = {.type = V_BOOL, .get = snd_enabled_get, .set = snd_enabled_set}},
    {.kind = M_ATTR,
     .name = "out_enabled",
     .doc = "Output engine running (DMA armed / PWM feeding the host)",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = snd_out_enabled, .set = NULL}},
    {.kind = M_ATTR,
     .name = "in_enabled",
     .doc = "Sound-input engine running (false where input is not modelled yet)",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = snd_in_enabled, .set = NULL}},
    {.kind = M_ATTR,
     .name = "frames",
     .doc = "Output frames rendered to the host since power-on",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = snd_frames, .set = NULL}},
    {.kind = M_ATTR,
     .name = "peak",
     .doc = "Loudest |sample| pushed to the host since power-on (0 = silence)",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = snd_peak, .set = NULL}},
    {.kind = M_ATTR,
     .name = "overruns",
     .doc = "Engine over/underruns since power-on",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = snd_overruns, .set = NULL}},
    {.kind = M_METHOD,
     .name = "mute",
     .doc = "Mute or unmute the sound output",
     .method = {.args = snd_mute_args, .nargs = 1, .result = V_NONE, .fn = snd_method_mute}},
    {.kind = M_METHOD,
     .name = "match",
     .doc = "Compare the last capture against a reference WAV (true if identical)",
     .method = {.args = snd_match_args, .nargs = 1, .result = V_BOOL, .fn = snd_method_match}},
};

static const class_desc_t sound_surface_class = {
    .name = "sound",
    .members = sound_members,
    .n_members = sizeof sound_members / sizeof sound_members[0],
};

struct object *sound_object_new(const sound_surface_t *s) {
    if (!s)
        return NULL;
    sound_surface_t *copy = (sound_surface_t *)malloc(sizeof(*copy));
    if (!copy)
        return NULL;
    *copy = *s;

    struct object *obj = object_new(&sound_surface_class, copy, "sound");
    if (!obj) {
        free(copy);
        return NULL;
    }
    object_set_label(obj, "Sound");
    object_set_order(obj, 110);
    object_attach(machine_object(), obj);
    // Deterministic capture sink for golden-WAV tests (sound.capture.*).
    audio_out_capture_attach(obj);
    return obj;
}

void sound_object_delete(struct object *obj) {
    if (!obj)
        return;
    void *copy = object_data(obj);
    // Capture owns its own node and deletes it; everything else attached here
    // is the engine's own detail child (machine.sound.asc), which object_delete
    // would NOT tear down -- it detaches the node from its parent but leaves
    // that node's children pointing at memory it is about to free.  Across a
    // run that reboots repeatedly (boot-config does it 28 times) those stale
    // children accumulate with dangling parents.  object_delete_tree is
    // post-order and takes the subtree.
    audio_out_capture_detach();
    object_detach(obj);
    object_delete_tree(obj);
    free(copy);
}
