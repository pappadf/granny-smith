// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// awacs.c
// The AWACS codec and the AMIC sound engine's output datapath — the PDM
// sound block at $50F14000.  AMIC owns everything software-visible (the
// byte register file, the double-buffered DMA engine over fixed window
// offsets, the per-half completion flags); the codec behind the command
// port is a dumb ITT ASCO 2300-family converter whose "expanded command
// set" face takes 16-bit `reg<<12|data12` commands through the $40/hi/lo/
// $C0 busy handshake.  Only registers 0/1/2/4 are ever addressed: input
// mux/gain, mutes/loopthru, headphone attenuation, speaker attenuation.
//
// The output engine renders each half-buffer into the shared host audio
// stream at the moment it completes — the same end-of-window convention as
// the AV Singer engine (av/singer.c), whose codec is the non-expanded face
// of the same ASCO spec: the guest stages samples while the half "plays",
// so reading at completion time picks up exactly what a real fetch would
// have clocked out.  The boot chime is pure polling (flags + the 24-bit
// position counter, no interrupts), so both must advance with time — a
// frozen engine hangs the ROM right there.
//
// Register truth: the shipping 1994-03 ROM's boot beep, AWACS sdev, and
// .AppleSoundInput driver, cross-checked against the ITT ASCO 2300 codec
// datasheet and the Power Macintosh Developer Note (1994) pp. 46-48.

#include "pdm.h"

#include "audio_out.h"
#include "awacs.h" // the shared ASCO codec semantics (core/peripherals/)
#include "log.h"
#include "machine_profile.h"
#include "object.h"
#include "scheduler.h"
#include "sound_surface.h"
#include "system.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("awacs");

// The engine's half-buffer regions are 8 KB fixed windows (2048 frames);
// system software always programs 1024-frame halves.
#define AWACS_MAX_FRAMES 2048u

// Output half-buffer offsets inside the 256 KB-aligned DMA window.
#define AWACS_OUT_HALF0 0x10000u
#define AWACS_OUT_HALF1 0x12000u

// Codec sample rate for the whole engine, from the shared rate field in the
// OUTPUT control register (+$10 bits 2:1): the drivers program %10 = 44 100
// and %00 = 22 050, and read it back as `(+$10 & $FE) == $04 ? 44100 :
// 22050` — the model implements exactly that predicate.
static uint32_t awacs_rate(pdm_amic_t *a) {
    return ((a->snd[0x10] >> 1) & 3u) == 2u ? 44100u : 22050u;
}

// Half-buffer length in frames (+$08/+$09; 0 selects the full 2048-frame
// region, and the register can't name more than the region holds).
static uint32_t awacs_frames(pdm_amic_t *a) {
    uint32_t frames = (uint32_t)(((a->snd[0x08] & 0x07u) << 8) | a->snd[0x09]);
    if (frames == 0 || frames > AWACS_MAX_FRAMES)
        frames = AWACS_MAX_FRAMES;
    return frames;
}

static uint64_t awacs_half_ns(pdm_amic_t *a) {
    return (uint64_t)awacs_frames(a) * 1000000000ull / awacs_rate(a);
}

// Physical DMA window base: bytes [31:24]/[23:16] at $50F31000/1, low 18
// bits ignored (256 KB alignment).
static uint32_t awacs_window_base(pdm_amic_t *a) {
    return (((uint32_t)a->dma_base[0] << 24) | ((uint32_t)a->dma_base[1] << 16)) & 0xFFFC0000u;
}

// Physical RAM read through the identity page table — the window sits in
// DRAM wherever the HMC mapped it (the fixed 7100/8100 bank windows are not
// host-identity, so no raw RAM-offset shortcut).  Samples are 4-byte
// aligned inside 8 KB-aligned regions, so a 16-bit read never crosses a
// page.
static uint16_t awacs_phys_read16(uint32_t phys) {
    uint32_t page = phys >> PAGE_SHIFT;
    if (page >= (uint32_t)g_page_count)
        return 0;
    uint8_t *host = g_page_table[page].host_base;
    if (!host)
        return 0;
    host += phys & ((1u << PAGE_SHIFT) - 1u);
    return (uint16_t)((host[0] << 8) | host[1]);
}

// ============================================================
// The output datapath
// ============================================================

// Render one completed half-buffer: 16-bit big-endian interleaved stereo
// from the DMA window, through the speaker path's codec controls (register
// 4 attenuation, register 1 bit 7 mute — the headphone jack is never
// connected, so the speaker is the audible output), into the host stream.
static void awacs_render_half(config_t *cfg, uint32_t half) {
    pdm_state_t *st = pdm_st(cfg);
    pdm_amic_t *a = &st->amic;
    if (!st->snd_stage)
        return;
    uint32_t frames = awacs_frames(a);
    uint32_t base = awacs_window_base(a) + (half ? AWACS_OUT_HALF1 : AWACS_OUT_HALF0);
    // Speaker path (the headphone jack is never connected): register 4
    // attenuation + register 1 mute, via the shared codec law.
    bool mute;
    uint32_t gl, gr;
    awacs_speaker_gains(a->codec, &gl, &gr, &mute);
    for (uint32_t i = 0; i < frames; i++) {
        int16_t l = 0, r = 0;
        if (!mute) {
            l = (int16_t)awacs_phys_read16(base + i * 4);
            r = (int16_t)awacs_phys_read16(base + i * 4 + 2);
            l = (int16_t)(((int32_t)l * (int32_t)gl) >> 16);
            r = (int16_t)(((int32_t)r * (int32_t)gr) >> 16);
        }
        st->snd_stage[i * 2] = l;
        st->snd_stage[i * 2 + 1] = r;
        int32_t al = l < 0 ? -l : l;
        int32_t ar = r < 0 ? -r : r;
        if (al > a->snd_peak)
            a->snd_peak = al;
        if (ar > a->snd_peak)
            a->snd_peak = ar;
    }
    uint32_t rate = awacs_rate(a);
    if (audio_out_rate() != rate)
        audio_out_set_rate(rate);
    audio_out_push(st->snd_stage, (int)frames, 7); // attenuation already applied
    a->snd_halves++;
}

// Sound-out half-buffer completion: render the half that just finished,
// raise its flag (or the underrun flag if software never consumed the
// previous one), flip, re-arm.
static void pdm_awacs_out_event(void *source, uint64_t data) {
    (void)data;
    config_t *cfg = (config_t *)source;
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    if (!(a->snd[0x10] & 0x01u))
        return; // stopped since scheduling
    LOG(2, "sndout half %d complete (snd18=$%02X)", a->snd_out_buf, a->snd[0x18]);
    awacs_render_half(cfg, a->snd_out_buf);
    uint8_t flag = a->snd_out_buf == 0 ? 0x40u : 0x80u; // bit 6 <-> +$10000
    if (a->snd[0x18] & flag) {
        a->snd[0x18] |= 0x20u; // over/underrun: ERR instead of the IF
        a->snd_underruns++; // surfaced as machine.sound.overruns
    } else {
        a->snd[0x18] |= flag;
    }
    a->snd_out_buf ^= 1u;
    a->snd_half_start_ns = scheduler_time_ns(cfg->scheduler);
    pdm_amic_recompute(cfg);
    scheduler_new_cpu_event(cfg->scheduler, pdm_awacs_out_event, cfg, 0, 0, awacs_half_ns(a));
}

// The combinational sound half of the AMIC DMA-flags mirror ($50F2A00A):
// enables in bits 3:1 gate flags in bits 7:5 with a shift of 4.
uint8_t pdm_awacs_irq_summary(pdm_amic_t *a) {
    return (uint8_t)((((a->snd[0x14] >> 4) & (a->snd[0x14] & 0x0Fu)) ? 0x01u : 0) |
                     (((a->snd[0x18] >> 4) & (a->snd[0x18] & 0x0Fu)) ? 0x02u : 0));
}

// ============================================================
// The register file ($50F14000, $20 bytes)
// ============================================================

// Live 24-bit output position (+$0C..+$0E): the current fetch pointer as a
// window offset, 64-byte granularity.  The boot beep drains by polling
// until `(value & $3FFC0) == 0`, i.e. until the engine has stopped and the
// pointer parked back at the window base — so a stopped engine reads 0 and
// a running one walks its half-buffer region in real time.
static uint32_t awacs_out_position(config_t *cfg, pdm_amic_t *a) {
    if (!(a->snd[0x10] & 0x01u))
        return 0;
    double now = scheduler_time_ns(cfg->scheduler);
    double elapsed = now - a->snd_half_start_ns;
    if (elapsed < 0.0)
        elapsed = 0.0;
    uint32_t frames = (uint32_t)(elapsed * (double)awacs_rate(a) / 1e9);
    uint32_t limit = awacs_frames(a);
    if (frames > limit)
        frames = limit;
    uint32_t pos = (a->snd_out_buf ? AWACS_OUT_HALF1 : AWACS_OUT_HALF0) + frames * 4u;
    return pos & 0xFFFFC0u;
}

uint8_t pdm_awacs_read(config_t *cfg, uint32_t off) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    if (off >= 0x20)
        return 0;
    switch (off) {
    case 0x0C:
    case 0x0D:
    case 0x0E: {
        uint32_t pos = awacs_out_position(cfg, a);
        return (uint8_t)(pos >> (8 * (0x0E - off)));
    }
    default:
        return a->snd[off];
    }
}

void pdm_awacs_write(config_t *cfg, uint32_t off, uint8_t value) {
    pdm_amic_t *a = &pdm_st(cfg)->amic;
    if (off >= 0x20)
        return;
    switch (off) {
    case 0x00:
        // Codec command handshake: software loads +$01/+$02, then strobes
        // $C0; hardware clocks the 16-bit command out over the frame's aux
        // bits and clears BUSY (bit 7).  The serial link is fast at
        // emulated-time scale, so BUSY clears within the same access — the
        // ROM's WaitExpandClear spin sees it immediately (it must clear:
        // the ROM spins on it, with a timeout that costs ~170k iterations
        // per command otherwise).  A $40 write is the idle/cancel state.
        if (value & 0x80u) {
            uint32_t reg = (a->snd[0x01] >> 4) & 7u;
            uint16_t data = (uint16_t)(((a->snd[0x01] & 0x0Fu) << 8) | a->snd[0x02]);
            a->codec[reg] = data;
            LOG(2, "codec reg %u = $%03X", reg, data);
        }
        a->snd[0x00] = value & 0x7Fu; // BUSY reads back clear
        break;
    case 0x10: { // control: bit 0 = output RUN, bits 2:1 = codec rate
        bool was_running = (a->snd[0x10] & 0x01u) != 0;
        a->snd[off] = value;
        bool running = (value & 0x01u) != 0;
        if (running && !was_running) {
            a->snd_out_buf = 0; // playback starts with the +$10000 half
            a->snd_half_start_ns = scheduler_time_ns(cfg->scheduler);
            LOG(2, "sndout run: %u frames/half at %u Hz", awacs_frames(a), awacs_rate(a));
            remove_event(cfg->scheduler, pdm_awacs_out_event, cfg);
            scheduler_new_cpu_event(cfg->scheduler, pdm_awacs_out_event, cfg, 0, 0, awacs_half_ns(a));
        } else if (!running && was_running) {
            remove_event(cfg->scheduler, pdm_awacs_out_event, cfg);
            // The engine raises the stopped/underrun flag when it stops
            // (StopHardware acks bit 5 AFTER clearing RUN).
            a->snd[0x18] |= 0x20u;
        }
        break;
    }
    case 0x14: // in/out DMA status: flag bits (high nibble) are W1C, never
    case 0x18: // read-to-clear (the handlers ack by RMW-ing the register)
        a->snd[off] = (uint8_t)((a->snd[off] & 0xF0u & ~(value & 0xF0u)) | (value & 0x0Fu));
        break;
    default:
        a->snd[off] = value;
        break;
    }
    pdm_amic_recompute(cfg);
}

// ============================================================
// machine.sound — the shared surface (sound_surface.h)
// ============================================================
//
// Volume and mute were computed here all along and never surfaced: the render
// path calls awacs_speaker_gains() every half-buffer, applies the ladder
// itself, and pushes a hardcoded 7 to audio_out ("attenuation already
// applied").  So the level existed and simply had nowhere to be read from.
// It is now reported on the 0..7 Sound control panel scale, derived from the
// codec's attenuation index (sound_volume_from_atten).
//
// in_enabled reports false because the AWACS input path is not modelled yet --
// not because the hardware lacks one.  The register map has an input
// sub-frame field, and a Power Mac has a Sound In jack.  When that path lands
// this reads true with no change here.

static pdm_amic_t *snd_amic_ctx(void *ctx) {
    config_t *cfg = (config_t *)ctx;
    return cfg && cfg->machine_context ? &pdm_st(cfg)->amic : NULL;
}

static uint32_t pdm_snd_sample_rate(void *ctx) {
    pdm_amic_t *a = snd_amic_ctx(ctx);
    return a ? awacs_rate(a) : 0;
}

// The codec's left attenuation index (register 4, bits 6-9) is the guest's
// level; left and right move together under the Sound control panel.
static uint32_t pdm_snd_volume(void *ctx) {
    pdm_amic_t *a = snd_amic_ctx(ctx);
    return a ? sound_volume_from_atten((a->codec[4] >> 6) & 15u) : 0;
}

static bool pdm_snd_muted(void *ctx) {
    pdm_amic_t *a = snd_amic_ctx(ctx);
    if (!a)
        return true;
    bool mute;
    uint32_t gl, gr;
    awacs_speaker_gains(a->codec, &gl, &gr, &mute);
    return mute;
}

static bool pdm_snd_out_enabled(void *ctx) {
    pdm_amic_t *a = snd_amic_ctx(ctx);
    return a && (a->snd[0x10] & 0x01u);
}

static bool pdm_snd_in_enabled(void *ctx) {
    (void)ctx;
    return false; // AWACS sound input is not modelled yet
}

static uint64_t pdm_snd_frames(void *ctx) {
    pdm_amic_t *a = snd_amic_ctx(ctx);
    return a ? a->snd_halves : 0;
}

static int32_t pdm_snd_peak(void *ctx) {
    pdm_amic_t *a = snd_amic_ctx(ctx);
    return a ? a->snd_peak : 0;
}

static uint64_t pdm_snd_overruns(void *ctx) {
    pdm_amic_t *a = snd_amic_ctx(ctx);
    return a ? a->snd_underruns : 0;
}

// ============================================================
// Lifecycle
// ============================================================

void pdm_awacs_register_events(config_t *cfg) {
    scheduler_new_event_type(cfg->scheduler, "amic", cfg, "sndout", pdm_awacs_out_event);
}

void pdm_awacs_init(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    st->snd_stage = calloc(AWACS_MAX_FRAMES, 2 * sizeof(int16_t));
    if (!st->snd_stage)
        LOG(0, "Error: out of memory allocating the AWACS staging buffer; this machine plays no sound");

    // The shared host stream, opened at the AWACS master-clock family rate
    // (45.1584 MHz / 1024): everything the boot plays is 44.1 kHz, and
    // opening there keeps a capture started before the chime valid (a
    // mid-capture rate switch invalidates golden matching).
    audio_out_open(44100, 2);

    const sound_surface_t surface = {
        .sample_rate = pdm_snd_sample_rate,
        .volume = pdm_snd_volume,
        .muted = pdm_snd_muted,
        .out_enabled = pdm_snd_out_enabled,
        .in_enabled = pdm_snd_in_enabled,
        .frames = pdm_snd_frames,
        .peak = pdm_snd_peak,
        .overruns = pdm_snd_overruns,
        .ctx = cfg,
    };
    st->snd_object = sound_object_new(&surface);
}

void pdm_awacs_teardown(config_t *cfg) {
    pdm_state_t *st = pdm_st(cfg);
    if (st->snd_object) {
        audio_out_capture_detach();
        object_detach(st->snd_object);
        object_delete(st->snd_object);
        st->snd_object = NULL;
    }
    free(st->snd_stage);
    st->snd_stage = NULL;
}
