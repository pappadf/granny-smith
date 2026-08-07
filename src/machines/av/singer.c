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

#include <math.h>
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

    // The codec A/D gain the guest last selected (singerCtl bits 12-19),
    // in x65536 fixed point.  The meter reports it because the guest's own
    // AGC drives it: a recording that comes back flat and full-scale is
    // explained by the ladder sitting at its top, and cannot be told apart
    // from a source problem without seeing where the guest parked it.
    uint32_t ain_adgain;
    // Last input-engine configuration logged, so a change is reported once
    // rather than every 10 ms frame.  PlainTalk recognition works through
    // this same engine, so what the Sound cdev's 8-bit recorder programs
    // DIFFERENTLY from what the recognizer programs is the whole question.
    uint32_t dbg_in_base, dbg_in_frames, dbg_in_rate, dbg_in_ctl;
    // Input level meter (machine.audioin.monitor/level/peak).  Measured on
    // exactly the samples the DMA is about to deposit — after the source,
    // the harness gain, the codec's A/D gain ladder and the dither — so it
    // reports what the GUEST actually receives, not what a source intended.
    // That distinction is the whole point: the guest's recorder gains its
    // input up hard, so a path delivering nothing comes back as full-scale
    // white noise, indistinguishable by ear from one delivering rubbish.
    // One number here separates them.
    uint8_t ain_monitor; // periodic level logging
    int32_t ain_peak; // peak |sample| in the last completed window
    int32_t ain_level; // RMS in the last completed window
    double ain_sumsq; // accumulator for the window in progress
    uint32_t ain_acc_n;
    int32_t ain_acc_peak;
    uint32_t wav_pos; // playback position in the loaded WAV (frames)
    uint32_t wav_frames; // loaded WAV length in frames (0 = none)

    // --- pointers (not checkpointed) ---
    config_t *cfg;
    int16_t *stage; // staging buffer for one half-buffer (stereo)
    int16_t *wav; // loaded WAV, interleaved stereo at the codec rate
    struct object *object; // the machine.sound node
    struct object *ain_object; // the machine.audioin node

    // machine.audioin.capture — the host's ear on what the SOURCE delivered.
    //
    // The mirror of machine.sound.capture, and the tool for a class of
    // failure that is otherwise unreachable: something that only misbehaves
    // with a real microphone, in a real browser, in front of a real person.
    // Nobody can hand over their microphone, but they can hand over the
    // samples it became — and because this records the source stream (see
    // singer_ain_capture), the WAV it writes goes straight back in through
    // machine.audioin.load and reproduces the session headlessly.
    //
    // Deliberately NOT checkpointed, and deliberately after `cfg`: a live
    // capture is a debugging session, not machine state, and restoring one
    // with a NULL buffer would be a crash waiting to happen.
    int16_t *ain_cap; // interleaved stereo, malloc'd, or NULL
    size_t ain_cap_n; // int16 samples used
    size_t ain_cap_max; // int16 samples allocated
    uint32_t ain_cap_rate; // codec rate latched at start
    uint8_t ain_cap_active;
    uint8_t ain_cap_full; // hit the cap; reported once
    struct object *ain_cap_object;

    // machine.audioin.advise — a once-a-second verdict on whether the
    // incoming audio is in the shape PlainTalk's recognizer wants.  State
    // for two bandpass filters and the per-second accumulators; see
    // singer_ain_advise.
    uint8_t ain_advise;
    uint32_t ain_adv_rate; // rate the biquads were designed for
    double adv_lo_b0, adv_lo_b1, adv_lo_b2, adv_lo_a1, adv_lo_a2;
    double adv_hi_b0, adv_hi_b1, adv_hi_b2, adv_hi_a1, adv_hi_a2;
    double adv_lo_x1, adv_lo_x2, adv_lo_y1, adv_lo_y2;
    double adv_hi_x1, adv_hi_x2, adv_hi_y1, adv_hi_y2;
    double adv_lo_e, adv_hi_e; // band energy, voiced blocks only
    double adv_sumsq; // voiced RMS accumulator
    uint32_t adv_voiced_n; // voiced sample count this second
    uint32_t adv_n; // samples this second
    int32_t adv_peak; // source peak this second
    double adv_floor; // running noise-floor estimate (RMS counts)
};

extern const class_desc_t av_singer_sound_class;
extern const class_desc_t av_audioin_class;
extern const class_desc_t av_audioin_capture_class;

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

// A/D gain ladder (singer.md §3, singerCtl bits 12-19): the same 1.5 dB
// steps upwards, 0 dB to +22.5 dB.  The driver's `singerCtlInit` selects
// +7.5 dB on both channels, and the speech front end's AGC drives this
// field, so an unmodelled A/D gain leaves every recorded level wrong.
static const uint32_t singer_adgain_x65536[16] = {
    65536,  77890,  92572,  110022, 130762, 155410, 184706, 219523,
    260904, 310084, 368536, 438006, 520571, 618700, 735326, 873937,
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

// The converter's noise floor.  A real Singer digitises its analogue front
// end continuously, so the captured stream is never a run of mathematically
// exact zeros — there is always about an LSB of converter and thermal noise,
// on both channels independently.  Modelling it is not cosmetic: Apple's DSP
// speech front end normalises each frame by its own envelope, and a frame of
// exact zeros drives that normalisation through a division by zero.  The
// resulting infinities land in the endpoint detector's running cepstral mean,
// which saturates and never recovers, so the recognizer stops detecting
// speech for the rest of the session (debug-plaintalk, rung 7).
//
// Deterministic — a pure function of the checkpointed sample counter, so
// captures stay byte-identical across hosts and across checkpoint restore.
static void singer_ain_dither(av_singer_t *s, uint32_t nframes) {
    for (uint32_t i = 0; i < nframes * 2; i++) {
        uint32_t h = (uint32_t)((s->ain_samples * 2 + i) * 2654435761u);
        h ^= h >> 13;
        h *= 1274126177u;
        h ^= h >> 16;
        int32_t v = (int32_t)s->stage[i] + (int32_t)(h % 3u) - 1; // -1, 0, +1
        if (v > 32767)
            v = 32767;
        if (v < -32768)
            v = -32768;
        s->stage[i] = (int16_t)v;
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
    singer_ain_dither(s, nframes);
    s->ain_samples += nframes;
}

static const char *ain_src_name(uint8_t mode);

// ============================================================
// machine.audioin.advise — is this audio in the shape Casper wants?
// ============================================================
//
// A once-a-second verdict on the incoming signal, for the case that is
// otherwise unanswerable from the outside: the user speaks, the level meter
// waves, and nothing is ever recognised.  The meter above says how LOUD the
// input is; this says whether it is USABLE, which is a different question
// and the one people actually have.
//
// WHERE THE NUMBERS COME FROM — read this before trusting a verdict.  They
// are calibrated against exactly two signals: the synthesized asset that
// the recognizer does accept (tests/data/speech/sr-open-the-trash.wav, and
// suite-av's av-sr-command row proves it), and one real user recording that
// it rejected 12 times out of 12 across two corrections.  That is a sample
// of two.  The level and clipping tests rest on documented behaviour and
// are solid; the SPECTRAL test is a heuristic drawn from a single failing
// example, so treat "tilt" as a hint about microphone technique, never as a
// specification.  Passing every check here does NOT promise recognition —
// the same recording passed all of them, spectrum-matched to the asset, and
// was still rejected.
//
//   level    Voiced RMS of the SOURCE, ahead of the codec's A/D gain.  The
//            PlainTalk contract puts typical voiced speech at 100-200 mVpp
//            (TIL15884), which is ~1400 counts of voiced RMS here; the
//            asset that works measures 1143.  Rung 7 found assets 6-12 dB
//            hot were rejected because the guest's AGC winds the codec gain
//            down in response (sr-test-audio-assets.md §2), so the window
//            is deliberately tight.
//   clip     Source peak times the CURRENT A/D gain ladder setting.  This
//            catches the trap a plain level meter cannot: a source that
//            looks fine on its own (peak 21145, no railed samples) is hard
//            clipped by the time it reaches the converter, because the
//            ladder multiplies it by 2.37x at the driver's default.
//   tilt     Energy in 1-3 kHz relative to 200-600 Hz.  REPORTED, NOT
//            JUDGED, and that is a finding rather than laziness.  Offline,
//            averaged over a whole utterance, this separated the two
//            signals cleanly (asset -11 dB, rejected recording -20 dB).
//            Measured here — per second, energy-weighted, over voiced
//            blocks only — the ranges OVERLAP: the asset reads -7.0 and
//            -3.5, the rejected recording -5.3 and -9.7.  No threshold
//            separates them, so no verdict is issued.  The number is still
//            worth printing (a persistently low figure across many seconds
//            does indicate proximity effect), but a confident flag here
//            would be a detector that produces plausible wrong answers,
//            and this investigation has already been bitten twice by
//            exactly that.
#define ADV_LEVEL_LO   500.0 // voiced RMS counts: quieter than this is thin
#define ADV_LEVEL_HI   2500.0 // ...louder than this is the "hot" band
#define ADV_LEVEL_WANT 1200.0 // mid-window; the working asset sits at 1143
#define ADV_CLIP_CEIL  31000 // counts after the A/D gain

// RBJ constant-0-dB-peak bandpass.
static void adv_bp(double fc, double q, uint32_t rate, double *b0, double *b1, double *b2, double *a1, double *a2) {
    double w0 = 2.0 * M_PI * fc / (double)rate;
    double alpha = sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha;
    *b0 = alpha / a0;
    *b1 = 0.0;
    *b2 = -alpha / a0;
    *a1 = (-2.0 * cos(w0)) / a0;
    *a2 = (1.0 - alpha) / a0;
}

static void adv_design(av_singer_t *s, uint32_t rate) {
    // 200-600 Hz and 1-3 kHz, as geometric centre + matching Q.
    adv_bp(346.0, 0.87, rate, &s->adv_lo_b0, &s->adv_lo_b1, &s->adv_lo_b2, &s->adv_lo_a1, &s->adv_lo_a2);
    adv_bp(1732.0, 0.87, rate, &s->adv_hi_b0, &s->adv_hi_b1, &s->adv_hi_b2, &s->adv_hi_a1, &s->adv_hi_a2);
    s->ain_adv_rate = rate;
    s->adv_lo_x1 = s->adv_lo_x2 = s->adv_lo_y1 = s->adv_lo_y2 = 0.0;
    s->adv_hi_x1 = s->adv_hi_x2 = s->adv_hi_y1 = s->adv_hi_y2 = 0.0;
}

// Fold one staged half-buffer in, and emit a verdict once a second.  Called
// on the SOURCE samples, ahead of the A/D gain, because that is the signal
// the user can actually do something about.
static void singer_ain_advise(av_singer_t *s, uint32_t nframes, uint32_t rate) {
    if (!s->ain_advise)
        return;
    if (s->ain_adv_rate != rate)
        adv_design(s, rate);

    // This block's RMS decides whether it counts as speech. Room tone must
    // not drive the level or the tilt: a quiet room would otherwise read as
    // "too quiet" forever, and its spectrum is not the speaker's.
    double sum2 = 0.0;
    int32_t pk = 0;
    for (uint32_t i = 0; i < nframes; i++) {
        double v = (double)s->stage[i * 2];
        sum2 += v * v;
        int32_t a = s->stage[i * 2] < 0 ? -s->stage[i * 2] : s->stage[i * 2];
        if (a > pk)
            pk = a;
    }
    double rms = sqrt(sum2 / (double)nframes);
    if (s->adv_floor <= 0.0)
        s->adv_floor = rms;
    else if (rms < s->adv_floor)
        s->adv_floor += (rms - s->adv_floor) * 0.05;
    else
        s->adv_floor += (rms - s->adv_floor) * 0.002;
    if (s->adv_floor < 4.0)
        s->adv_floor = 4.0;
    int voiced = rms > s->adv_floor * 6.0;

    if (pk > s->adv_peak)
        s->adv_peak = pk;
    s->adv_n += nframes;

    if (voiced) {
        s->adv_sumsq += sum2;
        s->adv_voiced_n += nframes;
        for (uint32_t i = 0; i < nframes; i++) {
            double x = (double)s->stage[i * 2];
            double yl = s->adv_lo_b0 * x + s->adv_lo_b1 * s->adv_lo_x1 + s->adv_lo_b2 * s->adv_lo_x2 -
                        s->adv_lo_a1 * s->adv_lo_y1 - s->adv_lo_a2 * s->adv_lo_y2;
            s->adv_lo_x2 = s->adv_lo_x1;
            s->adv_lo_x1 = x;
            s->adv_lo_y2 = s->adv_lo_y1;
            s->adv_lo_y1 = yl;
            double yh = s->adv_hi_b0 * x + s->adv_hi_b1 * s->adv_hi_x1 + s->adv_hi_b2 * s->adv_hi_x2 -
                        s->adv_hi_a1 * s->adv_hi_y1 - s->adv_hi_a2 * s->adv_hi_y2;
            s->adv_hi_x2 = s->adv_hi_x1;
            s->adv_hi_x1 = x;
            s->adv_hi_y2 = s->adv_hi_y1;
            s->adv_hi_y1 = yh;
            s->adv_lo_e += yl * yl;
            s->adv_hi_e += yh * yh;
        }
    }

    if (s->adv_n < rate)
        return;

    // A second with little speech in it gets no verdict: "silence" once a
    // second is noise in the log, none of the tests mean anything without a
    // voice to measure, and a second holding only the first syllable reads
    // as "too quiet" when the utterance is fine.
    if (s->adv_voiced_n >= rate / 5) { // at least 200 ms of speech
        double vrms = sqrt(s->adv_sumsq / (double)s->adv_voiced_n);
        double lvl_db = 20.0 * log10(vrms / ADV_LEVEL_WANT);
        double tilt = (s->adv_lo_e > 0.0 && s->adv_hi_e > 0.0) ? 10.0 * log10(s->adv_hi_e / s->adv_lo_e) : 0.0;
        int64_t after_gain = ((int64_t)s->adv_peak * (int64_t)s->ain_adgain) >> 16;

        const char *lvl = "ok";
        if (vrms > ADV_LEVEL_HI)
            lvl = "HOT";
        else if (vrms < ADV_LEVEL_LO)
            lvl = "QUIET";
        const char *clip = after_gain >= ADV_CLIP_CEIL ? "CLIPS" : "ok";

        char advice[192];
        advice[0] = 0;
        if (after_gain >= ADV_CLIP_CEIL || vrms > ADV_LEVEL_HI)
            snprintf(advice, sizeof advice, " -> too loud: move back from the mic, or lower the input level");
        else if (vrms < ADV_LEVEL_LO)
            snprintf(advice, sizeof advice, " -> too quiet: move closer, or raise the input level");
        else
            snprintf(advice, sizeof advice, " -> in the window the recognizer wants");

        LOG(1,
            "audioin advice: level %.0f rms (%+.1f dB vs target, %s)  peak %d x%.2f = %lld (%s)  "
            "tilt %+.1f dB (fyi)%s",
            vrms, lvl_db, lvl, s->adv_peak, (double)s->ain_adgain / 65536.0, (long long)after_gain, clip, tilt, advice);
    }

    s->adv_lo_e = s->adv_hi_e = 0.0;
    s->adv_sumsq = 0.0;
    s->adv_voiced_n = 0;
    s->adv_n = 0;
    s->adv_peak = 0;
}

// Two minutes of stereo at the highest codec rate (48 kHz) — 23 MB. Long
// enough for any spoken diagnostic, bounded so a capture left running in a
// browser tab cannot grow until the tab dies.
#define AIN_CAP_MAX_SAMPLES ((size_t)48000 * 2 * 120)

// Append one staged half-buffer to the capture.
//
// Called from singer_fill_input BEFORE the codec's A/D gain, which is the
// whole point: this records what the SOURCE delivered, which is exactly
// what machine.audioin.load feeds back in. Capturing after the A/D gain
// would look more like "what the guest heard", but replaying such a file
// would apply the ladder a second time and land 2.37x hot at the default
// setting — a trap that would quietly invalidate every comparison made
// with it. The harness gain and the converter's dither ARE included; both
// are re-applied on replay, so the dither differs by its ±1 LSB and
// nothing else does.
static void singer_ain_capture(av_singer_t *s, uint32_t nframes) {
    if (!s->ain_cap_active)
        return;
    size_t want = (size_t)nframes * 2;
    if (s->ain_cap_n + want > AIN_CAP_MAX_SAMPLES) {
        if (!s->ain_cap_full) {
            s->ain_cap_full = 1;
            LOG(1, "audioin capture: reached the %zu-second cap, still recording nothing further",
                AIN_CAP_MAX_SAMPLES / (2 * 48000));
        }
        return;
    }
    if (s->ain_cap_n + want > s->ain_cap_max) {
        size_t grow = s->ain_cap_max ? s->ain_cap_max * 2 : (size_t)48000 * 2 * 4;
        while (grow < s->ain_cap_n + want)
            grow *= 2;
        int16_t *p = realloc(s->ain_cap, grow * sizeof(int16_t));
        if (!p) {
            LOG(0, "audioin capture: out of memory at %zu samples, stopping", s->ain_cap_n);
            s->ain_cap_active = 0;
            return;
        }
        s->ain_cap = p;
        s->ain_cap_max = grow;
    }
    memcpy(s->ain_cap + s->ain_cap_n, s->stage, want * sizeof(int16_t));
    s->ain_cap_n += want;
}

// Fold one staged half-buffer into the level meter, and emit a line about
// once a second while `machine.audioin.monitor` is on.
static void singer_ain_meter(av_singer_t *s, uint32_t nframes, uint32_t rate) {
    for (uint32_t i = 0; i < nframes; i++) {
        int32_t v = s->stage[i * 2];
        int32_t a = v < 0 ? -v : v;
        if (a > s->ain_acc_peak)
            s->ain_acc_peak = a;
        s->ain_sumsq += (double)v * (double)v;
    }
    s->ain_acc_n += nframes;
    // A quarter-second window: the attributes stay responsive enough to
    // watch live, and a full second would not even complete inside a short
    // scripted run (which is how this was first found reporting zero).
    if (s->ain_acc_n < rate / 4)
        return;
    s->ain_peak = s->ain_acc_peak;
    s->ain_level = (int32_t)(sqrt(s->ain_sumsq / (double)s->ain_acc_n) + 0.5);
    if (s->ain_monitor) { // one line per quarter-second window
        // dBFS is the readable form; the raw counts matter because the
        // codec's own dither floor is +-1, so anything at or under ~2 counts
        // means NO audio is arriving, however loud it sounds afterwards.
        double pk = s->ain_peak > 0 ? 20.0 * log10((double)s->ain_peak / 32768.0) : -999.0;
        double rm = s->ain_level > 0 ? 20.0 * log10((double)s->ain_level / 32768.0) : -999.0;
        // The platform's own view of the capture, when it has one: the
        // guest-side level says audio is missing, never which side lost it.
        char host[192];
        if (!gs_audio_in_debug(host, sizeof host))
            host[0] = 0;
        LOG(1, "audioin[%s]: peak %5d (%6.1f dBFS)  rms %5d (%6.1f dBFS) adgain %.2fx %s %s", ain_src_name(s->ain_src),
            s->ain_peak, pk, s->ain_level, rm, (double)s->ain_adgain / 65536.0, host,
            // The dither floor measures 3 counts once the codec's +7.5 dB
            // A/D gain is applied, so anything at or under that is silence.
            s->ain_peak <= 4 ? "<- SILENCE: no audio is reaching the guest" : "");
    }
    s->ain_sumsq = 0.0;
    s->ain_acc_n = 0;
    s->ain_acc_peak = 0;
}

// Fill the guest's input half-buffer from the selected audio-in source
// (silence when none — the "mic absent" presentation).
static void singer_fill_input(av_singer_t *s, uint32_t base, uint32_t nframes) {
    if (base >= 0x40000000u)
        return; // never DMA into ROM/NuBus space
    singer_ain_pull(s, nframes, singer_rate(s));
    // The codec's A/D gain sits ahead of the converter, so it scales what
    // the DMA deposits (singer.md §3, singerCtl bits 12-19).  Read it BEFORE
    // the source taps below: both of them report on the pre-gain signal, and
    // the advisory needs the ladder setting to predict clipping at the
    // converter.
    uint32_t ctl = av_psc_snd_read32(singer_st(s)->psc, 0x04); // singerCtl
    uint32_t agl = singer_adgain_x65536[(ctl >> 16) & 15]; // pLeftGain
    s->ain_adgain = agl;
    uint32_t agr = singer_adgain_x65536[(ctl >> 12) & 15]; // pRightGain

    // Both taps see the SOURCE stream, ahead of the A/D gain — see the
    // functions.  Hooking them after it made the advisory measure a gained
    // signal and then apply the gain a second time, so it called the very
    // asset the recognizer accepts "too loud".
    singer_ain_capture(s, nframes);
    singer_ain_advise(s, nframes, singer_rate(s));

    if (agl != 65536 || agr != 65536) {
        for (uint32_t i = 0; i < nframes; i++) {
            int64_t l = ((int64_t)s->stage[i * 2] * agl) >> 16;
            int64_t r = ((int64_t)s->stage[i * 2 + 1] * agr) >> 16;
            s->stage[i * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            s->stage[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
    }
    if (base != s->dbg_in_base || nframes != s->dbg_in_frames || singer_rate(s) != s->dbg_in_rate ||
        ctl != s->dbg_in_ctl) {
        s->dbg_in_base = base;
        s->dbg_in_frames = nframes;
        s->dbg_in_rate = singer_rate(s);
        s->dbg_in_ctl = ctl;
        LOG(2, "input engine: base $%08X %u frames @%u Hz singerCtl $%08X (adgain %.2fx) sndComCtl $%04X", base,
            nframes, singer_rate(s), ctl, (double)agl / 65536.0, av_psc_snd_read16(singer_st(s)->psc, 0x00));
    }
    singer_ain_meter(s, nframes, singer_rate(s));
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
    // Host capture lifecycle, on the GUEST's own gate — the mirror of the
    // VDC clock driving gs_video_in_state.  A browser platform attaches its
    // microphone track here, so the recording indicator is lit only while
    // the guest is genuinely recording.  This must NOT be driven from the
    // machine.audioin source setter: that merely echoes back the selection
    // the caller just made, which re-enters the frontend's own stream
    // reconciliation and races it.
    if ((com ^ s->last_com) & SND_IN_EN)
        gs_audio_in_state((com & SND_IN_EN) != 0);
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
            av_dsp_ext1_tick(st->dsp);
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

// inject = load, plus the platform monitor hook: in the browser the same
// file is also played through the host speakers so an audience hears what
// the guest was just fed.  `load` stays silent — tests use it.
static value_t ain_method_inject(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    av_singer_t *s = singer_self(self);
    if (!s || argc < 1)
        return val_err("audioin not available");
    if (singer_load_wav(s, argv[0].s) < 0)
        return val_err("audioin.inject: cannot load '%s' as a PCM16 WAV", argv[0].s);
    s->ain_src = AIN_SRC_WAV;
    gs_audio_in_injected(argv[0].s);
    return val_uint(4, s->wav_frames);
}

static value_t ain_attr_advise_get(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_bool(s && s->ain_advise);
}

static value_t ain_attr_advise_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    av_singer_t *s = singer_self(self);
    if (!s) {
        value_free(&in);
        return val_err("audioin not available");
    }
    s->ain_advise = in.b ? 1 : 0;
    s->adv_n = s->adv_voiced_n = 0;
    s->adv_peak = 0;
    s->adv_sumsq = s->adv_lo_e = s->adv_hi_e = s->adv_floor = 0.0;
    value_free(&in);
    return val_none();
}

static value_t ain_attr_monitor_get(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_bool(s && s->ain_monitor);
}

static value_t ain_attr_monitor_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    av_singer_t *s = singer_self(self);
    if (!s)
        return val_err("audioin not available");
    s->ain_monitor = in.b ? 1 : 0;
    return val_none();
}

static value_t ain_attr_level(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_int(s ? s->ain_level : 0);
}

static value_t ain_attr_peak(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = singer_self(self);
    return val_int(s ? s->ain_peak : 0);
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
     .name = "advise",
     .doc = "Judge the incoming audio ~1/s (level, clipping, spectrum); needs debug.log singer \"level=1\"",
     .attr = {.type = V_BOOL, .get = ain_attr_advise_get, .set = ain_attr_advise_set}},
    {.kind = M_ATTR,
     .name = "monitor",
     .doc = "Log the input level ~1/s; needs the singer category on: debug.log singer \"level=1\"",
     .attr = {.type = V_BOOL, .get = ain_attr_monitor_get, .set = ain_attr_monitor_set}},
    {.kind = M_ATTR,
     .name = "level",
     .doc = "RMS of the last second of input, in int16 counts (the codec's dither floor is ~1)",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = ain_attr_level, .set = NULL}},
    {.kind = M_ATTR,
     .name = "peak",
     .doc = "Peak |sample| of the last second of input, in int16 counts",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = ain_attr_peak, .set = NULL}},
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
     .doc = "Inject a PCM16 WAV as the microphone: selects the wav source and feeds it "
            "from the top to whatever is listening; returns its length in frames", .method = {.args = ain_load_args, .nargs = 1, .result = V_UINT, .fn = ain_method_load}},
    {.kind = M_METHOD,
     .name = "rewind",
     .doc = "Replay the loaded WAV from its start (no reload)",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = ain_method_rewind}},
    {.kind = M_METHOD,
     .name = "inject",
     .doc = "Like load, and additionally monitors the file through the host "
            "speakers (browser) so an audience hears what the guest was fed", .method = {.args = ain_load_args, .nargs = 1, .result = V_UINT, .fn = ain_method_inject}},
};

const class_desc_t av_audioin_class = {
    .name = "audioin",
    .members = av_audioin_members,
    .n_members = sizeof(av_audioin_members) / sizeof(av_audioin_members[0]),
};

// ============================================================
// machine.audioin.capture — record what the source delivered
// ============================================================

static value_t ain_cap_attr_active(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = (av_singer_t *)object_data(self);
    return val_bool(s && s->ain_cap_active);
}

static value_t ain_cap_attr_frames(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = (av_singer_t *)object_data(self);
    return val_uint(8, s ? (uint64_t)(s->ain_cap_n / 2) : 0);
}

static value_t ain_cap_attr_seconds(struct object *self, const member_t *m) {
    (void)m;
    av_singer_t *s = (av_singer_t *)object_data(self);
    uint32_t rate = s && s->ain_cap_rate ? s->ain_cap_rate : 24000;
    return val_float(s ? (double)(s->ain_cap_n / 2) / (double)rate : 0.0);
}

static value_t ain_cap_method_start(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    av_singer_t *s = (av_singer_t *)object_data(self);
    if (!s)
        return val_err("audioin not available");
    s->ain_cap_n = 0;
    s->ain_cap_full = 0;
    s->ain_cap_rate = singer_rate(s);
    s->ain_cap_active = 1;
    LOG(1, "audioin capture: start (%u Hz stereo, source %s)", s->ain_cap_rate, ain_src_name(s->ain_src));
    return val_bool(true);
}

static value_t ain_cap_method_stop(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    av_singer_t *s = (av_singer_t *)object_data(self);
    if (!s)
        return val_err("audioin not available");
    s->ain_cap_active = 0;
    uint64_t frames = s->ain_cap_n / 2;
    if (argc >= 1 && argv[0].kind == V_STRING && argv[0].s && *argv[0].s) {
        if (!s->ain_cap_n)
            return val_err("audioin.capture.stop: nothing captured — was a source connected?");
        if (audio_wav_write(argv[0].s, s->ain_cap, s->ain_cap_n, s->ain_cap_rate, 2) < 0)
            return val_err("audioin.capture.stop: cannot write '%s'", argv[0].s);
        LOG(1, "audioin capture: wrote %llu frames to %s", (unsigned long long)frames, argv[0].s);
    }
    return val_uint(8, frames);
}

static const arg_decl_t ain_cap_stop_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Write the capture here as a PCM16 WAV (replayable with audioin.load)"},
};

static const member_t ain_capture_members[] = {
    {.kind = M_ATTR,
     .name = "active",
     .doc = "True while a capture is recording",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = ain_cap_attr_active, .set = NULL}},
    {.kind = M_ATTR,
     .name = "frames",
     .doc = "Sample frames accumulated in the current or last capture",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = ain_cap_attr_frames, .set = NULL}},
    {.kind = M_ATTR,
     .name = "seconds",
     .doc = "Length of the current or last capture, in seconds",
     .flags = VAL_RO,
     .attr = {.type = V_FLOAT, .get = ain_cap_attr_seconds, .set = NULL}},
    {.kind = M_METHOD,
     .name = "start",
     .doc = "Begin recording what the audio-in source delivers",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = ain_cap_method_start}},
    {.kind = M_METHOD,
     .name = "stop",
     .doc = "Stop recording; with a path, write it as a WAV. Returns frames",
     .method = {.args = ain_cap_stop_args, .nargs = 1, .result = V_UINT, .fn = ain_cap_method_stop}},
};

const class_desc_t av_audioin_capture_class = {
    .name = "capture",
    .members = ain_capture_members,
    .n_members = sizeof(ain_capture_members) / sizeof(ain_capture_members[0]),
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
        s->ain_cap_object = object_new(&av_audioin_capture_class, s, "capture");
        if (s->ain_cap_object) {
            object_set_label(s->ain_cap_object, "Capture");
            object_attach(s->ain_object, s->ain_cap_object);
        }
    }

    LOG(1, "Singer init (%u Hz, audioin %s)", s->open_rate, ain_src_name(s->ain_src));
    return s;
}

void av_singer_delete(av_singer_t *s) {
    if (!s)
        return;
    audio_out_capture_detach();
    if (s->ain_cap_object) {
        object_detach(s->ain_cap_object);
        object_delete(s->ain_cap_object);
    }
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
    free(s->ain_cap);
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
