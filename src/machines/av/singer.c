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

#include <stdio.h>
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

// Host audio source selection (machine.audioin.source).
typedef enum {
    AIN_SRC_NONE = 0, // no microphone (default; input records silence)
    AIN_SRC_TONE, // deterministic sawtooth, pure function of the sample counter
    AIN_SRC_WAV, // a WAV loaded via machine.audioin.load (position checkpointed)
    AIN_SRC_HOST, // the platform microphone through the gs_audio_in seam
} ain_src_t;

struct av_singer {
    // --- plain data (checkpointed up to `cfg`) ---
    uint64_t frames; // frame ticks serviced since power-on
    uint32_t overruns; // frame boundaries that passed unserviced
    uint32_t open_rate; // rate the host stream was opened at
    uint16_t last_com; // previous sndComCtl (edge-triggered logging)

    // machine.audioin state (the microphone-source surface)
    uint8_t ain_src; // ain_src_t
    uint16_t ain_gain; // input gain in percent (100 = unity)
    uint64_t ain_samples; // sample frames pulled since power-on (tone phase)
    uint32_t wav_pos; // playback position in the loaded WAV (frames)
    uint32_t wav_frames; // loaded WAV length in frames (0 = none)

    // --- pointers (not checkpointed) ---
    config_t *cfg;
    int16_t *stage; // staging buffer for one half-buffer (stereo)
    int16_t *wav; // loaded WAV, interleaved stereo at the codec rate
    struct object *object; // the machine.sound node
    struct object *ain_object; // the machine.audioin node
};

extern const class_desc_t av_singer_sound_class;
extern const class_desc_t av_audioin_class;

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
        if (!mute) {
            // Reads may address ROM: CycloneBeep plays the chime PCM
            // straight out of the ROM image at $408C5D24.
            l = (int16_t)mmu_read_physical_uint16(g_mmu, addr);
            r = (int16_t)mmu_read_physical_uint16(g_mmu, addr + 2);
            l = (int16_t)(((int32_t)l * (int32_t)gl) >> 16);
            r = (int16_t)(((int32_t)r * (int32_t)gr) >> 16);
        }
        s->stage[i * 2] = l;
        s->stage[i * 2 + 1] = r;
    }
}

// True when the selected source reports a signal (the singerStat mic
// presentation and the Speech Setup detection read this indirectly).
static bool singer_ain_connected(av_singer_t *s) {
    switch (s->ain_src) {
    case AIN_SRC_TONE:
        return true;
    case AIN_SRC_WAV:
        return s->wav_frames != 0;
    case AIN_SRC_HOST:
        return gs_audio_in_connected();
    default:
        return false;
    }
}

// Produce nframes stereo sample pairs from the selected source into the
// staging buffer.  Deterministic sources are pure functions of the
// checkpointed counters; the host microphone is checkpoint-ephemeral.
static void singer_ain_pull(av_singer_t *s, uint32_t nframes, uint32_t rate) {
    memset(s->stage, 0, (size_t)nframes * 2 * sizeof(int16_t));
    switch (s->ain_src) {
    case AIN_SRC_TONE: {
        // 600 Hz sawtooth, +-12000, integer math only (byte-determinism
        // across hosts — no libm in the sample path).
        uint32_t period = rate / 600;
        if (period == 0)
            period = 1;
        for (uint32_t i = 0; i < nframes; i++) {
            uint32_t ph = (uint32_t)((s->ain_samples + i) % period);
            int16_t v = (int16_t)((int32_t)ph * 24000 / (int32_t)period - 12000);
            s->stage[i * 2] = v;
            s->stage[i * 2 + 1] = v;
        }
        break;
    }
    case AIN_SRC_WAV: {
        for (uint32_t i = 0; i < nframes && s->wav_pos < s->wav_frames; i++, s->wav_pos++) {
            s->stage[i * 2] = s->wav[(size_t)s->wav_pos * 2];
            s->stage[i * 2 + 1] = s->wav[(size_t)s->wav_pos * 2 + 1];
        }
        break;
    }
    case AIN_SRC_HOST:
        gs_audio_in_frames(s->stage, nframes, rate);
        break;
    default:
        break;
    }
    if (s->ain_gain != 100) {
        for (uint32_t i = 0; i < nframes * 2; i++) {
            int32_t v = (int32_t)s->stage[i] * s->ain_gain / 100;
            if (v > 32767)
                v = 32767;
            if (v < -32768)
                v = -32768;
            s->stage[i] = (int16_t)v;
        }
    }
    s->ain_samples += nframes;
}

// Fill the guest's input half-buffer from the selected audio-in source
// (silence when none — the "mic absent" presentation).
static void singer_fill_input(av_singer_t *s, uint32_t base, uint32_t nframes) {
    if (base >= 0x40000000u)
        return; // never DMA into ROM/NuBus space
    singer_ain_pull(s, nframes, singer_rate(s));
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
    // The half that just FINISHED playing/recording.  Hardware touches
    // samples progressively across the frame; emitting the half at the
    // END of its window reads the state the guest had staged while it
    // "played" — CycloneBeep re-points sndOutBase just after the phase
    // wraps, and a start-of-frame snapshot would replay a stale half.
    uint32_t half = (uint32_t)((frame + 1) & 1);

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
// machine.audioin — the microphone-source surface
// ============================================================

static inline av_singer_t *singer_self(struct object *self) {
    return (av_singer_t *)object_data(self);
}

static const char *ain_src_name(uint8_t mode) {
    switch (mode) {
    case AIN_SRC_TONE:
        return "tone";
    case AIN_SRC_WAV:
        return "wav";
    case AIN_SRC_HOST:
        return "host";
    default:
        return "none";
    }
}

static int singer_ain_set_source(av_singer_t *s, const char *name) {
    if (strcmp(name, "none") == 0)
        s->ain_src = AIN_SRC_NONE;
    else if (strcmp(name, "tone") == 0)
        s->ain_src = AIN_SRC_TONE;
    else if (strcmp(name, "wav") == 0)
        s->ain_src = s->wav_frames ? AIN_SRC_WAV : AIN_SRC_NONE;
    else if (strcmp(name, "host") == 0)
        s->ain_src = AIN_SRC_HOST;
    else
        return -1;
    gs_audio_in_state(s->ain_src == AIN_SRC_HOST);
    return 0;
}

// Minimal PCM16 WAV reader: fmt + data chunks, mono or stereo, any rate
// (assets are prepared offline at the codec rate — TEST_DATA.md).
static int singer_load_wav(av_singer_t *s, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(fp);
        return -1;
    }
    uint16_t channels = 0, bits = 0;
    uint8_t chunk[8];
    long data_off = 0;
    uint32_t data_len = 0;
    while (fread(chunk, 1, 8, fp) == 8) {
        uint32_t len =
            (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8) | ((uint32_t)chunk[6] << 16) | ((uint32_t)chunk[7] << 24);
        if (memcmp(chunk, "fmt ", 4) == 0 && len >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, fp) != 16)
                break;
            channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            bits = (uint16_t)(fmt[14] | (fmt[15] << 8));
            if (len > 16)
                fseek(fp, (long)len - 16, SEEK_CUR);
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_off = ftell(fp);
            data_len = len;
            fseek(fp, (long)len, SEEK_CUR);
        } else {
            fseek(fp, (long)((len + 1) & ~1u), SEEK_CUR);
        }
    }
    if (!data_off || bits != 16 || (channels != 1 && channels != 2)) {
        fclose(fp);
        return -1;
    }
    uint32_t nframes = data_len / (channels * 2u);
    int16_t *buf = malloc((size_t)nframes * 2 * sizeof(int16_t));
    uint8_t *raw = malloc(data_len);
    if (!buf || !raw) {
        free(buf);
        free(raw);
        fclose(fp);
        return -1;
    }
    fseek(fp, data_off, SEEK_SET);
    if (fread(raw, 1, data_len, fp) != data_len) {
        free(buf);
        free(raw);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    for (uint32_t i = 0; i < nframes; i++) {
        int16_t l = (int16_t)(raw[i * channels * 2] | (raw[i * channels * 2 + 1] << 8));
        int16_t r = channels == 2 ? (int16_t)(raw[i * channels * 2 + 2] | (raw[i * channels * 2 + 3] << 8)) : l;
        buf[i * 2] = l;
        buf[i * 2 + 1] = r;
    }
    free(raw);
    free(s->wav);
    s->wav = buf;
    s->wav_frames = nframes;
    s->wav_pos = 0;
    return 0;
}

static value_t ain_attr_source_get(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_str(s ? ain_src_name(s->ain_src) : "none");
}

static value_t ain_attr_source_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    av_singer_t *s = singer_self(self);
    if (!s) {
        value_free(&in);
        return val_err("audioin not available");
    }
    if (singer_ain_set_source(s, in.s) < 0) {
        value_t e = val_err("audioin.source: want none|tone|wav|host, got '%s'", in.s);
        value_free(&in);
        return e;
    }
    value_free(&in);
    return val_none();
}

static value_t ain_attr_connected(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_bool(s && singer_ain_connected(s));
}

static value_t ain_attr_gain_get(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_uint(2, s ? s->ain_gain : 100);
}

static value_t ain_attr_gain_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    av_singer_t *s = singer_self(self);
    if (!s) {
        value_free(&in);
        return val_err("audioin not available");
    }
    if (in.u > 800) {
        value_free(&in);
        return val_err("audioin.gain: percent 0-800");
    }
    s->ain_gain = (uint16_t)in.u;
    value_free(&in);
    return val_none();
}

static value_t ain_attr_samples(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_uint(8, s ? s->ain_samples : 0);
}

static value_t ain_attr_position(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_uint(4, s ? s->wav_pos : 0);
}

static value_t ain_method_load(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    av_singer_t *s = singer_self(self);
    if (!s || argc < 1)
        return val_err("audioin not available");
    if (singer_load_wav(s, argv[0].s) < 0)
        return val_err("audioin.load: cannot load '%s' as a PCM16 WAV", argv[0].s);
    s->ain_src = AIN_SRC_WAV;
    return val_uint(4, s->wav_frames);
}

static value_t ain_method_rewind(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    av_singer_t *s = singer_self(self);
    if (!s)
        return val_err("audioin not available");
    s->wav_pos = 0;
    return val_none();
}

static const arg_decl_t ain_load_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "PCM16 WAV prepared at the codec rate (mono or stereo)"},
};

static const member_t av_audioin_members[] = {
    {.kind = M_ATTR,
     .name = "source",
     .doc = "Host audio source: none | tone | wav | host (microphone)",
     .attr = {.type = V_STRING, .get = ain_attr_source_get, .set = ain_attr_source_set}},
    {.kind = M_ATTR,
     .name = "connected",
     .doc = "True when the source reports a signal (the mic-present sense)",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = ain_attr_connected, .set = NULL}},
    {.kind = M_ATTR,
     .name = "gain",
     .doc = "Input gain in percent (100 = unity; bring-up level sweeps)",
     .attr = {.type = V_UINT, .get = ain_attr_gain_get, .set = ain_attr_gain_set}},
    {.kind = M_ATTR,
     .name = "samples",
     .doc = "Sample frames pulled from the source since power-on",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = ain_attr_samples, .set = NULL}},
    {.kind = M_ATTR,
     .name = "position",
     .doc = "Playback position in the loaded WAV (frames)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = ain_attr_position, .set = NULL}},
    {.kind = M_METHOD,
     .name = "load",
     .doc = "Load a PCM16 WAV and select it as the source; returns its length",
     .method = {.args = ain_load_args, .nargs = 1, .result = V_UINT, .fn = ain_method_load}},
    {.kind = M_METHOD,
     .name = "rewind",
     .doc = "Rewind the loaded WAV to its start",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = ain_method_rewind}},
};

const class_desc_t av_audioin_class = {
    .name = "audioin",
    .members = av_audioin_members,
    .n_members = sizeof(av_audioin_members) / sizeof(av_audioin_members[0]),
};

// ============================================================
// machine.sound — the object node
// ============================================================

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
        if (s->wav_frames) {
            s->wav = malloc((size_t)s->wav_frames * 2 * sizeof(int16_t));
            if (s->wav)
                system_read_checkpoint_data(cp, s->wav, (size_t)s->wav_frames * 2 * sizeof(int16_t));
        }
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

    if (!s->ain_gain)
        s->ain_gain = 100; // unity on a fresh machine
    s->ain_object = object_new(&av_audioin_class, s, "audioin");
    if (s->ain_object) {
        object_set_label(s->ain_object, "Audio In");
        object_set_order(s->ain_object, 126);
        object_attach(machine_object(), s->ain_object);
    }

    LOG(1, "Singer init (%u Hz, audioin %s)", s->open_rate, ain_src_name(s->ain_src));
    return s;
}

void av_singer_delete(av_singer_t *s) {
    if (!s)
        return;
    audio_out_capture_detach();
    if (s->ain_object) {
        object_detach(s->ain_object);
        object_delete(s->ain_object);
    }
    if (s->object) {
        object_detach(s->object);
        object_delete(s->object);
    }
    if (s->cfg && s->cfg->scheduler)
        remove_event(s->cfg->scheduler, &singer_frame_event, s);
    free(s->wav);
    free(s->stage);
    free(s);
}

void av_singer_checkpoint(av_singer_t *s, checkpoint_t *cp) {
    if (!s || !cp)
        return;
    size_t data_size = offsetof(av_singer_t, cfg);
    system_write_checkpoint_data(cp, s, data_size);
    // The loaded WAV is state (a restore must keep producing the same
    // samples); the host microphone is checkpoint-ephemeral.
    if (s->wav_frames && s->wav)
        system_write_checkpoint_data(cp, s->wav, (size_t)s->wav_frames * 2 * sizeof(int16_t));
}
