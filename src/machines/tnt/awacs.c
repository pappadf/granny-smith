// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// awacs.c
// The TNT AWACS sound face (proposal §5.8, ladder rung T10): five 32-bit
// little-endian registers on $10 centres at island +$14000, the shared
// ASCO codec shadows (core/peripherals/awacs.c) behind the NEWECMD
// command port, and the output datapath — DBDMA channel 8 pulling a
// descriptor program into the shared host audio stream at the selected
// sample rate.  Channel 9 (input) stays unattached until an input phase.
//
// Pacing: the DBDMA engine transfers whatever the device port accepts,
// so the port is a FRAME-CREDIT gate — a periodic tick converts elapsed
// scheduler cycles into frames exact-rationally (frames = cycles * rate
// / freq with a running remainder, the house rule) and kicks the
// channel; the engine then drains exactly the granted frames.  The tick
// arms itself only while the guest is actually playing (first stalled
// port call) and disarms when the channel goes idle, so an idle machine
// schedules nothing.  Everything the guest observes — completion
// interrupts, resCount, ring progress — advances in emulated time at
// the codec rate, which is what the ROM's completion-polled beep and
// interrupt-driven chime both time themselves against.
//
// Register truth: the Grand Central register map from the OS driver
// corpus for these machines (Linux sound/ppc awacs, NetBSD awacs.c,
// which agree exactly), the shipping ROM's own use during boot (Open
// Firmware plays its beep through channel 8 — the first exerciser of
// this datapath), and the ITT ASCO 2300 codec datasheet via the shared
// core.  The byte-swap register (+$40) applies to SAMPLE data only.

#include "tnt.h"

#include "audio_out.h"
#include "dbdma.h"
#include "log.h"
#include "object.h"
#include "scheduler.h"
#include "sound_surface.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("awacs");

// Register offsets inside the +$14000 block ($10 centres).
#define AWACS_SOUND_CTRL 0x00u
#define AWACS_CODEC_CTRL 0x10u
#define AWACS_CODEC_STAT 0x20u
#define AWACS_CLIP_COUNT 0x30u
#define AWACS_BYTE_SWAP  0x40u

// Codec-control fields.
#define AWACS_NEWECMD 0x01000000u // lock: hardware busy clocking the command

// The eight sound-control rate codes (bits 10:8), 44.1 kHz family.
static const uint32_t awacs_rates[8] = {44100, 29400, 22050, 17640, 14700, 11025, 8820, 7350};

// Pacing grant quantum: the tick fires roughly every this many frames.
#define AWACS_GRANT_FRAMES 256u

// Credit cap: bounds the burst after a long descriptor-side stall
// (~100 ms of audio), so a WAITing program can't bank unlimited credit.
#define AWACS_CREDIT_CAP(rate) ((rate) / 10u)

static uint32_t awacs_rate(tnt_awacs_t *w) {
    return awacs_rates[(w->sound_ctrl >> 8) & 7u];
}

// ============================================================
// The output datapath: DBDMA channel-8 device port
// ============================================================

// Render whole frames from the port byte stream into the host stream:
// 16-bit interleaved stereo, big-endian unless the byte-swap register
// says little, through the speaker path's codec gains.
static void awacs_render(config_t *cfg, const uint8_t *bytes, uint32_t nframes) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_awacs_t *w = &st->awacs;
    if (!st->snd_stage)
        return; // no staging buffer: stay silent rather than write through NULL
    bool le = (w->byte_swap & 1u) != 0;
    bool mute;
    uint32_t gl, gr;
    awacs_speaker_gains(w->codec, &gl, &gr, &mute);
    for (uint32_t i = 0; i < nframes; i++) {
        const uint8_t *f = bytes + i * 4;
        int16_t l = 0, r = 0;
        if (!mute) {
            l = (int16_t)(le ? (f[1] << 8) | f[0] : (f[0] << 8) | f[1]);
            r = (int16_t)(le ? (f[3] << 8) | f[2] : (f[2] << 8) | f[3]);
            l = (int16_t)(((int32_t)l * (int32_t)gl) >> 16);
            r = (int16_t)(((int32_t)r * (int32_t)gr) >> 16);
        }
        st->snd_stage[i * 2] = l;
        st->snd_stage[i * 2 + 1] = r;
        int32_t al = l < 0 ? -l : l;
        int32_t ar = r < 0 ? -r : r;
        if (al > w->peak)
            w->peak = al;
        if (ar > w->peak)
            w->peak = ar;
    }
    uint32_t rate = awacs_rate(w);
    if (audio_out_rate() != rate)
        audio_out_set_rate(rate);
    audio_out_push(st->snd_stage, (int)nframes, 7); // attenuation already applied
    w->frames_pushed += nframes;
}

static void awacs_tick_event(void *source, uint64_t data);

// Arm the pacing tick for one grant quantum at the current rate.
static void awacs_arm(config_t *cfg) {
    tnt_awacs_t *w = &tnt_st(cfg)->awacs;
    if (w->tick_armed)
        return;
    w->tick_armed = 1;
    uint64_t ns = (uint64_t)AWACS_GRANT_FRAMES * 1000000000ull / awacs_rate(w);
    scheduler_new_cpu_event(cfg->scheduler, awacs_tick_event, cfg, 0, 0, ns);
}

// The channel-8 port: accept up to the granted frame credit.  A short
// acceptance stalls the engine; the tick re-grants and kicks.
static int awacs_port_out(void *ctx, const uint8_t *buf, int len) {
    config_t *cfg = (config_t *)ctx;
    tnt_awacs_t *w = &tnt_st(cfg)->awacs;
    int taken = 0;
    // Finish a partial frame carried across port calls first.
    while (w->partial_len != 0 && taken < len && w->credit != 0) {
        w->partial[w->partial_len++] = buf[taken++];
        if (w->partial_len == 4) {
            awacs_render(cfg, w->partial, 1);
            w->partial_len = 0;
            w->credit--;
        }
    }
    // Whole frames within the credit.
    uint32_t frames = (uint32_t)(len - taken) / 4u;
    if (frames > w->credit)
        frames = w->credit;
    if (frames != 0) {
        awacs_render(cfg, buf + taken, frames);
        w->credit -= frames;
        taken += (int)(frames * 4u);
    }
    // Trailing sub-frame bytes: stage them (they cost the next credit).
    if (w->credit != 0) {
        while (taken < len && w->partial_len < 4)
            w->partial[w->partial_len++] = buf[taken++];
    }
    if (taken < len)
        awacs_arm(cfg); // stalled on credit: the tick resumes the engine
    return taken;
}

// The pacing tick: convert elapsed cycles into frame credit (exact
// rational, running remainder), kick the channel, re-arm while playing.
static void awacs_tick_event(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    tnt_state_t *st = tnt_st(cfg);
    tnt_awacs_t *w = &st->awacs;
    w->tick_armed = 0;
    uint64_t now = scheduler_cpu_cycles(cfg->scheduler);
    uint32_t rate = awacs_rate(w);
    uint64_t num = (now - w->tick_cycles) * rate + w->tick_frac;
    uint64_t freq = cfg->machine->freq;
    w->credit += (uint32_t)(num / freq);
    w->tick_frac = num % freq;
    w->tick_cycles = now;
    uint32_t cap = AWACS_CREDIT_CAP(rate);
    if (w->credit > cap)
        w->credit = cap;
    tnt_dbdma_kick(st->dbdma, 8);
    // Keep ticking while the program runs; an idle channel forfeits its
    // remaining credit (playback restarts from a clean gate).
    if (tnt_dbdma_active(st->dbdma, 8))
        awacs_arm(cfg);
    else
        w->credit = 0;
}

// ============================================================
// The register file (+$14000, LE domain)
// ============================================================

uint32_t tnt_awacs_read32(config_t *cfg, uint32_t offset) {
    tnt_awacs_t *w = &tnt_st(cfg)->awacs;
    switch (offset & 0xFF0u) {
    case AWACS_SOUND_CTRL:
        return w->sound_ctrl;
    case AWACS_CODEC_CTRL:
        // The serial command link is fast at emulated-time scale: the
        // NEWECMD lock reads back clear (the guest polls it before every
        // command and spins forever otherwise).
        return w->codec_ctrl & ~AWACS_NEWECMD;
    case AWACS_CODEC_STAT:
        // Valid data, no pending inputs; revision/part zero (plain
        // AWACS — whether TNT carries a Screamer is an open dossier
        // question; nothing in the boot path discriminates).
        return 0x00400000u;
    case AWACS_CLIP_COUNT:
        return 0;
    case AWACS_BYTE_SWAP:
        return w->byte_swap;
    default:
        LOG(2, "read of unwired sound register +$%03X", offset);
        return 0;
    }
}

void tnt_awacs_write32(config_t *cfg, uint32_t offset, uint32_t value) {
    tnt_awacs_t *w = &tnt_st(cfg)->awacs;
    switch (offset & 0xFF0u) {
    case AWACS_SOUND_CTRL:
        LOG(2, "sound control = $%08X (rate %u Hz)", value, awacs_rates[(value >> 8) & 7u]);
        w->sound_ctrl = value;
        break;
    case AWACS_CODEC_CTRL: {
        // An expanded-mode command: 10-bit address, 12-bit data, clocked
        // to the codec immediately (NEWECMD never reads back set).
        w->codec_ctrl = value;
        uint32_t reg = (value >> 12) & 0x7FFu;
        uint16_t data = (uint16_t)(value & 0xFFFu);
        if (reg < AWACS_CODEC_REGS) {
            w->codec[reg] = data;
            LOG(2, "codec reg %u = $%03X", reg, data);
        } else {
            LOG(1, "codec command to out-of-range address $%03X ignored", reg);
        }
        break;
    }
    case AWACS_BYTE_SWAP:
        w->byte_swap = value;
        break;
    case AWACS_CODEC_STAT:
    case AWACS_CLIP_COUNT:
        LOG(2, "write to read-only sound register +$%03X = $%08X", offset, value);
        break;
    default:
        LOG(2, "write of unwired sound register +$%03X = $%08X", offset, value);
        break;
    }
}

// ============================================================
// machine.sound — the object node (the PDM surface, TNT plumbing)
// ============================================================
// machine.sound — the shared surface (sound_surface.h)
// ============================================================
//
// As on the PDM: the render path already calls awacs_speaker_gains() and
// applies the ladder before pushing a hardcoded 7 to audio_out, so volume and
// mute existed here and had nowhere to be read from.  Both are now reported.
//
// in_enabled is false because the input path is not modelled.  The hardware
// has one -- the TNT sound-control register's bits 0-3 are the Input SubFrame
// Select field (awacs-sound.md §2.1), and an 8500 has a Sound In jack -- so
// this is a gap to close, not a property of the machine.

static tnt_awacs_t *snd_awacs_ctx(void *ctx) {
    config_t *cfg = (config_t *)ctx;
    return cfg && cfg->machine_context ? &tnt_st(cfg)->awacs : NULL;
}

static uint32_t tnt_snd_sample_rate(void *ctx) {
    tnt_awacs_t *w = snd_awacs_ctx(ctx);
    return w ? awacs_rate(w) : 0;
}

static uint32_t tnt_snd_volume(void *ctx) {
    tnt_awacs_t *w = snd_awacs_ctx(ctx);
    return w ? sound_volume_from_atten((w->codec[4] >> 6) & 15u) : 0;
}

static bool tnt_snd_muted(void *ctx) {
    tnt_awacs_t *w = snd_awacs_ctx(ctx);
    if (!w)
        return true;
    bool mute;
    uint32_t gl, gr;
    awacs_speaker_gains(w->codec, &gl, &gr, &mute);
    return mute;
}

static bool tnt_snd_out_enabled(void *ctx) {
    config_t *cfg = (config_t *)ctx;
    tnt_state_t *st = cfg && cfg->machine_context ? tnt_st(cfg) : NULL;
    return st && st->dbdma && tnt_dbdma_active(st->dbdma, 8);
}

static bool tnt_snd_in_enabled(void *ctx) {
    (void)ctx;
    return false; // AWACS sound input is not modelled yet
}

static uint64_t tnt_snd_frames(void *ctx) {
    tnt_awacs_t *w = snd_awacs_ctx(ctx);
    return w ? w->frames_pushed : 0;
}

static int32_t tnt_snd_peak(void *ctx) {
    tnt_awacs_t *w = snd_awacs_ctx(ctx);
    return w ? w->peak : 0;
}

// The DBDMA path has no underrun detection yet: the engine renders whatever
// the channel's program points at, and a starved program simply stops rather
// than raising a flag.  The PDM's AMIC does detect it (an unconsumed half sets
// the ERR bit), so this reads 0 where that one reads a real count -- another
// gap the uniform surface makes visible instead of hiding.
static uint64_t tnt_snd_overruns(void *ctx) {
    (void)ctx;
    return 0;
}

// ============================================================
// Lifecycle
// ============================================================

void tnt_awacs_register_events(config_t *cfg) {
    scheduler_new_event_type(cfg->scheduler, "awacs", cfg, "tick", awacs_tick_event);
}

void tnt_awacs_reset(config_t *cfg) {
    tnt_awacs_t *w = &tnt_st(cfg)->awacs;
    // Power-on register state; pacing restarts from a clean gate.  The
    // armed flag mirrors the scheduler's pending event, which machine
    // reset does not cancel — a stale tick on a reset machine grants to
    // an idle channel and disarms itself.
    w->sound_ctrl = 0;
    w->codec_ctrl = 0;
    w->byte_swap = 0;
    memset(w->codec, 0, sizeof(w->codec));
    w->credit = 0;
    w->partial_len = 0;
    w->tick_frac = 0;
    w->tick_cycles = scheduler_cpu_cycles(cfg->scheduler);
}

void tnt_awacs_init(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    st->snd_stage = calloc(AWACS_CREDIT_CAP(44100) + 1, 2 * sizeof(int16_t));
    if (!st->snd_stage)
        LOG(0, "Error: out of memory allocating the AWACS staging buffer; this machine plays no sound");

    // The shared host stream, opened at the rate the boot actually uses:
    // Open Firmware programs 22 050 Hz (sound-control rate code 2) for
    // its beep — observed, and stable for the whole parked boot — so
    // opening there keeps a boot-long capture free of the mid-capture
    // rate switch that invalidates golden matching.  Revisit when the
    // 68k chime first plays (it did not by the Phase-D wall).
    audio_out_open(22050, 2);

    // The channel-8 device port (replaces nothing: attached at build).
    tnt_dbdma_port_t port = {.out = awacs_port_out, .ctx = cfg};
    tnt_dbdma_set_port(st->dbdma, 8, &port);

    const sound_surface_t surface = {
        .sample_rate = tnt_snd_sample_rate,
        .volume = tnt_snd_volume,
        .muted = tnt_snd_muted,
        .out_enabled = tnt_snd_out_enabled,
        .in_enabled = tnt_snd_in_enabled,
        .frames = tnt_snd_frames,
        .peak = tnt_snd_peak,
        .overruns = tnt_snd_overruns,
        .ctx = cfg,
    };
    st->snd_object = sound_object_new(&surface);
}

void tnt_awacs_teardown(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (st->snd_object) {
        audio_out_capture_detach();
        object_detach(st->snd_object);
        object_delete(st->snd_object);
        st->snd_object = NULL;
    }
    free(st->snd_stage);
    st->snd_stage = NULL;
}
