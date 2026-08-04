// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// singer.c
// Singer codec + PSC sound frame engine — see singer.h.  The structural
// twin of the VDC field engine: a cadenced scheduler event, guest-
// programmed geometry (sndSize/pSndRate/bases), a host seam (audio out
// via audio_out.h, audio in via the gs_audio_in seam), and interrupt
// lines (PSC-VIA2 bit 6 + DSP EXT1, both gated by pFrmIntEn).
//
// The engine is phase-locked to the same absolute time formula as the
// PSC's free-running sndPhase (psc.c psc_snd_phase): sample index
// s(t) = floor(t * rate / 1e9), frame index f = s / sndSize, and the
// event fires at each frame boundary — so the play position the guest
// polls and the frame interrupts it receives can never drift apart.

#include "singer.h"

#include "av.h"
#include "dsp.h"
#include "psc.h"

#include "dsp3210.h" // DSP3210_VEC_EXT1

#include "audio_out.h"
#include "log.h"
#include "machine_profile.h"
#include "mmu.h"
#include "object.h"
#include "scheduler.h"
#include "system.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("singer");

// sndComCtl fields (singer.md §2).
#define SND_FRM_INT_EN (1u << 6)
#define SND_IN_EN      (1u << 7)
#define SND_OUT_EN     (1u << 8)

// Largest half-buffer the engine will stage per frame (the shipped driver
// always programs 240; the register is 16-bit).
#define SINGER_MAX_FRAMES 0x4000u

struct av_singer {
    // --- plain data (checkpointed up to `cfg`) ---
    uint64_t frames; // frame ticks serviced since power-on
    uint32_t overruns; // frame boundaries that passed unserviced
    uint32_t open_rate; // rate the host stream was opened at
    uint16_t last_com; // previous sndComCtl (edge-triggered logging)

    // --- pointers (not checkpointed) ---
    config_t *cfg;
    int16_t *stage; // staging buffer for one half-buffer (stereo)
    struct object *object; // the machine.sound node
};

extern const class_desc_t av_singer_sound_class;

static void singer_frame_event(void *source, uint64_t data);

static inline av_state_t *singer_st(av_singer_t *s) {
    return (av_state_t *)s->cfg->machine_context;
}

// Codec rate from sndComCtl bits 9-10 (24/32/48 kHz; 3 reserved → 48).
static uint32_t singer_rate(av_singer_t *s) {
    static const uint32_t rates[4] = {24000, 32000, 48000, 48000};
    return rates[(av_psc_snd_read16(singer_st(s)->psc, 0x00) >> 9) & 3];
}

// Half-buffer size in sample frames; the power-on default cadence is the
// 240-frame (10 ms) value the shipped driver always programs.
static uint32_t singer_size(av_singer_t *s) {
    uint32_t n = av_psc_snd_read16(singer_st(s)->psc, 0x18);
    if (n == 0)
        n = 240;
    if (n > SINGER_MAX_FRAMES)
        n = SINGER_MAX_FRAMES;
    return n;
}

// ============================================================
// The frame engine
// ============================================================

// D/A attenuation ladder (singer.md §3): 1.5 dB steps as x65536 gains.
static const uint32_t singer_atten_x65536[16] = {
    65536, 55142, 46396, 39037, 32846, 27636, 23253, 19565, 16462, 13851, 11654, 9806, 8250, 6942, 5841, 4915,
};

// Stage the guest's output half-buffer with singerCtl attenuation/mute
// applied — the capture sink then records exactly what the codec drives.
static void singer_stage_output(av_singer_t *s, uint32_t base, uint32_t nframes) {
    uint32_t ctl = av_psc_snd_read32(singer_st(s)->psc, 0x04); // singerCtl
    uint32_t gl = singer_atten_x65536[(ctl >> 8) & 15]; // pLeftAtten
    uint32_t gr = singer_atten_x65536[(ctl >> 4) & 15]; // pRightAtten
    bool mute = (ctl & (1u << 22)) != 0;
    for (uint32_t i = 0; i < nframes; i++) {
        uint32_t addr = base + i * 4;
        int16_t l = 0, r = 0;
        if (!mute && addr < 0x40000000u) {
            l = (int16_t)mmu_read_physical_uint16(g_mmu, addr);
            r = (int16_t)mmu_read_physical_uint16(g_mmu, addr + 2);
            l = (int16_t)(((int32_t)l * (int32_t)gl) >> 16);
            r = (int16_t)(((int32_t)r * (int32_t)gr) >> 16);
        }
        s->stage[i * 2] = l;
        s->stage[i * 2 + 1] = r;
    }
}

// Fill the guest's input half-buffer from the host audio-in seam (silence
// until a source is connected — the "mic absent" presentation).
static void singer_fill_input(av_singer_t *s, uint32_t base, uint32_t nframes) {
    if (base >= 0x40000000u)
        return;
    memset(s->stage, 0, (size_t)nframes * 2 * sizeof(int16_t));
    gs_audio_in_frames(s->stage, nframes, singer_rate(s));
    for (uint32_t i = 0; i < nframes; i++) {
        mmu_write_physical_uint16(g_mmu, base + i * 4, (uint16_t)s->stage[i * 2]);
        mmu_write_physical_uint16(g_mmu, base + i * 4 + 2, (uint16_t)s->stage[i * 2 + 1]);
    }
}

static void singer_frame_event(void *source, uint64_t data) {
    (void)data;
    av_singer_t *s = (av_singer_t *)source;
    av_state_t *st = singer_st(s);
    uint32_t rate = singer_rate(s);
    uint32_t size = singer_size(s);
    uint16_t com = av_psc_snd_read16(st->psc, 0x00);

    // The frame index this boundary begins (phase-locked to sndPhase).
    double now_ns = scheduler_time_ns(s->cfg->scheduler);
    uint64_t sample = (uint64_t)(now_ns * (double)rate / 1e9);
    uint64_t frame = sample / size;
    uint32_t half = (uint32_t)(frame & 1);

    if (com & SND_OUT_EN) {
        uint32_t base = av_psc_snd_read32(st->psc, 0x14) + half * size * 4; // sndOutBase
        if (audio_out_rate() != rate)
            audio_out_set_rate(rate);
        singer_stage_output(s, base, size);
        audio_out_push(s->stage, (int)size, 7);
    }
    if (com & SND_IN_EN) {
        uint32_t base = av_psc_snd_read32(st->psc, 0x10) + half * size * 4; // sndInBase
        singer_fill_input(s, base, size);
    }

    if ((com ^ s->last_com) & (SND_FRM_INT_EN | SND_OUT_EN | SND_IN_EN))
        LOG(2, "sndComCtl now $%04X at frame %llu (int %d out %d in %d)", com, (unsigned long long)frame,
            !!(com & SND_FRM_INT_EN), !!(com & SND_OUT_EN), !!(com & SND_IN_EN));
    s->last_com = com;

    if (com & SND_FRM_INT_EN) {
        // Frame overrun: the previous tick's EXT1 is still latched — the
        // kernel never serviced it.  Sticky $21C bit 2 + L5 bit 1; the
        // host's FRMOVRNhndlr kills the DSP, no restart (B3).
        if (st->dsp && av_dsp_running(st->dsp) && av_dsp_ext1_pending(st->dsp)) {
            s->overruns++;
            LOG(1, "frame overrun (frame %llu)", (unsigned long long)frame);
            av_psc_dsp_frame_overrun(st->psc);
        }
        // The host IFR bit and the DSP tick are the SAME gated tick (B2).
        av_psc_via2_latch(st->psc, AV_PSC_VIA2_SNDFRM);
        if (st->dsp)
            av_dsp_irq(st->dsp, DSP3210_VEC_EXT1);
    }

    s->frames++;

    // Next boundary of the absolute formula (re-reads rate/size next tick
    // so reprogramming takes effect at a frame edge, like the hardware).
    double next_ns = (double)((frame + 1) * size) * 1e9 / (double)rate;
    double delta = next_ns - now_ns;
    if (delta < 1000.0)
        delta = 1000.0; // guard: never re-arm in the past
    scheduler_new_cpu_event(s->cfg->scheduler, &singer_frame_event, s, 0, 0, (uint64_t)delta);
}

// ============================================================
// machine.sound — the object node
// ============================================================

static inline av_singer_t *singer_self(struct object *self) {
    return (av_singer_t *)object_data(self);
}

static value_t snd_attr_rate(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_uint(4, s ? singer_rate(s) : 0);
}

static value_t snd_attr_out_enabled(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_bool(s && (av_psc_snd_read16(singer_st(s)->psc, 0x00) & SND_OUT_EN));
}

static value_t snd_attr_in_enabled(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_bool(s && (av_psc_snd_read16(singer_st(s)->psc, 0x00) & SND_IN_EN));
}

static value_t snd_attr_frames(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_uint(8, s ? s->frames : 0);
}

static value_t snd_attr_overruns(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_uint(4, s ? s->overruns : 0);
}

static value_t snd_method_match(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("match: want a golden WAV path");
    return audio_out_match_value(argv[0].s);
}

static const arg_decl_t snd_match_args[] = {
    {.name = "reference", .kind = V_STRING, .doc = "golden WAV to compare the last capture against"},
};

static const member_t av_singer_sound_members[] = {
    {.kind = M_ATTR,
     .name = "sample_rate",
     .doc = "Codec sample rate from sndComCtl (24000/32000/48000)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = snd_attr_rate, .set = NULL}},
    {.kind = M_ATTR,
     .name = "out_enabled",
     .doc = "pSndOutEn — sound output DMA running",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = snd_attr_out_enabled, .set = NULL}},
    {.kind = M_ATTR,
     .name = "in_enabled",
     .doc = "pSndInEn — sound input DMA running",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = snd_attr_in_enabled, .set = NULL}},
    {.kind = M_ATTR,
     .name = "frames",
     .doc = "Sound frames the engine has ticked since power-on",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = snd_attr_frames, .set = NULL}},
    {.kind = M_ATTR,
     .name = "overruns",
     .doc = "Frame boundaries that passed with the DSP tick unserviced",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = snd_attr_overruns, .set = NULL}},
    {.kind = M_METHOD,
     .name = "match",
     .doc = "Sample-exact compare of the last capture against a golden WAV",
     .method = {.args = snd_match_args, .nargs = 1, .result = V_BOOL, .fn = snd_method_match}},
};

const class_desc_t av_singer_sound_class = {
    .name = "sound",
    .members = av_singer_sound_members,
    .n_members = sizeof(av_singer_sound_members) / sizeof(av_singer_sound_members[0]),
};

// ============================================================
// Lifecycle
// ============================================================

av_singer_t *av_singer_init(config_t *cfg, checkpoint_t *cp) {
    av_singer_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->cfg = cfg;
    s->stage = calloc(SINGER_MAX_FRAMES, 2 * sizeof(int16_t));
    if (!s->stage) {
        free(s);
        return NULL;
    }

    if (cp) {
        size_t data_size = offsetof(av_singer_t, cfg);
        system_read_checkpoint_data(cp, s, data_size);
    }

    scheduler_new_event_type(cfg->scheduler, "singer", s, "frame", &singer_frame_event);
    if (!cp) {
        // First boundary of the absolute formula; restore re-arms via the
        // checkpointed event instead.
        scheduler_new_cpu_event(cfg->scheduler, &singer_frame_event, s, 0, 0, (uint64_t)(240.0 * 1e9 / 24000.0));
    }

    // The shared host stream + capture sink (stereo at the codec rate).
    s->open_rate = singer_rate(s);
    audio_out_open(s->open_rate, 2);

    s->object = object_new(&av_singer_sound_class, s, "sound");
    if (s->object) {
        object_set_label(s->object, "Sound");
        object_set_order(s->object, 110);
        object_attach(machine_object(), s->object);
        audio_out_capture_attach(s->object);
    }

    LOG(1, "Singer init (%u Hz)", s->open_rate);
    return s;
}

void av_singer_delete(av_singer_t *s) {
    if (!s)
        return;
    audio_out_capture_detach();
    if (s->object) {
        object_detach(s->object);
        object_delete(s->object);
    }
    if (s->cfg && s->cfg->scheduler)
        remove_event(s->cfg->scheduler, &singer_frame_event, s);
    free(s->stage);
    free(s);
}

void av_singer_checkpoint(av_singer_t *s, checkpoint_t *cp) {
    if (!s || !cp)
        return;
    size_t data_size = offsetof(av_singer_t, cfg);
    system_write_checkpoint_data(cp, s, data_size);
}
