// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// audio_out.c
// Single host-facing audio output stream + deterministic capture sink.
// See audio_out.h for the design contract. Producers push int16 frames at
// guest rate; this module forwards them to the platform sink and, while a
// capture is active, records them pre-volume for golden-WAV test matching.

#include "audio_out.h"
#include "log.h"
#include "object.h"
#include "platform.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

LOG_USE_CATEGORY_NAME("audio");

// Forward declaration — class descriptor is at the bottom of the file.
extern const class_desc_t audio_capture_class;

// ============================================================================
// Module State
// ============================================================================

// Stream + capture state; a process singleton like the screen facade (one
// host audio pipeline exists regardless of which machine is booted).
static struct {
    // Current stream parameters (set by audio_out_open / set_rate)
    uint32_t rate;
    int channels;

    // Capture sink: growable interleaved int16 sample buffer
    bool active; // recording right now
    bool have_capture; // a stopped capture is available for match
    bool rate_changed; // rate switched mid-capture (invalidates the capture)
    uint32_t cap_rate; // rate latched at capture start
    int cap_channels; // channels latched at capture start
    int16_t *samples; // interleaved samples (frames * channels)
    size_t nsamples; // samples recorded so far
    size_t max_samples; // allocated capacity

    struct object *object; // the attached `capture` object node (or NULL)
} s;

// ============================================================================
// Stream API
// ============================================================================

// Opens (or re-parameterizes) the host audio stream
void audio_out_open(uint32_t src_rate_hz, int channels) {
    s.rate = src_rate_hz;
    s.channels = channels;
    platform_audio_open(src_rate_hz, channels);
    LOG(2, "open: rate=%u Hz channels=%d", src_rate_hz, channels);
}

// Changes the source sample rate mid-stream
void audio_out_set_rate(uint32_t src_rate_hz) {
    if (s.rate == src_rate_hz)
        return;
    s.rate = src_rate_hz;
    // A mid-capture rate switch makes the capture ill-defined (a WAV has one
    // rate); latch the fact so match reports it instead of comparing garbage.
    if (s.active)
        s.rate_changed = true;
    platform_audio_set_rate(src_rate_hz);
    LOG(2, "set_rate: %u Hz", src_rate_hz);
}

// Pushes interleaved int16 frames: record into the capture, forward to host
void audio_out_push(const int16_t *frames, int nframes, int vol_0_7) {
    if (!frames || nframes <= 0)
        return;

    if (s.active) {
        size_t add = (size_t)nframes * (size_t)s.cap_channels;
        if (s.nsamples + add > s.max_samples) {
            // Grow geometrically; start at ~1s of stereo audio
            size_t want = s.nsamples + add;
            size_t cap = s.max_samples ? s.max_samples : 65536;
            while (cap < want)
                cap *= 2;
            int16_t *grown = (int16_t *)realloc(s.samples, cap * sizeof(int16_t));
            if (!grown) {
                LOG(0, "capture: out of memory at %zu samples, stopping", s.nsamples);
                s.active = false;
                return;
            }
            s.samples = grown;
            s.max_samples = cap;
        }
        memcpy(s.samples + s.nsamples, frames, add * sizeof(int16_t));
        s.nsamples += add;
    }

    platform_audio_push(frames, nframes, vol_0_7);
}

uint32_t audio_out_rate(void) {
    return s.rate;
}

int audio_out_channels(void) {
    return s.channels;
}

// ============================================================================
// WAV I/O (PCM int16 only)
// ============================================================================

// Appends a 32-bit little-endian value to a byte buffer
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Appends a 16-bit little-endian value to a byte buffer
static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

// Reads a 32-bit little-endian value from a byte buffer
static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Reads a 16-bit little-endian value from a byte buffer
static uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// Writes an interleaved int16 sample buffer as a canonical 44-byte-header
// PCM WAV file. Returns 0 on success, -1 on I/O error.
static int wav_write(const char *path, const int16_t *samples, size_t nsamples, uint32_t rate, int channels) {
    uint32_t data_bytes = (uint32_t)(nsamples * sizeof(int16_t));
    uint8_t hdr[44];

    memcpy(hdr + 0, "RIFF", 4);
    put_le32(hdr + 4, 36 + data_bytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    put_le32(hdr + 16, 16); // PCM fmt chunk size
    put_le16(hdr + 20, 1); // audio format = PCM
    put_le16(hdr + 22, (uint16_t)channels);
    put_le32(hdr + 24, rate);
    put_le32(hdr + 28, rate * (uint32_t)channels * 2); // byte rate
    put_le16(hdr + 32, (uint16_t)(channels * 2)); // block align
    put_le16(hdr + 34, 16); // bits per sample
    memcpy(hdr + 36, "data", 4);
    put_le32(hdr + 40, data_bytes);

    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    // Samples are written per-element via LE conversion so big-endian hosts
    // produce identical files (int16_t in memory is host-endian).
    int ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    for (size_t i = 0; ok && i < nsamples; i++) {
        uint8_t b[2];
        put_le16(b, (uint16_t)samples[i]);
        ok = fwrite(b, 1, 2, f) == 2;
    }
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

// Loads a PCM int16 WAV file: walks RIFF chunks for `fmt ` and `data`.
// Returns 0 on success with malloc'd *samples; -1 on error (message in errbuf).
static int wav_read(const char *path, int16_t **samples, size_t *nsamples, uint32_t *rate, int *channels, char *errbuf,
                    size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(errbuf, errlen, "cannot open '%s'", path);
        return -1;
    }

    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        snprintf(errbuf, errlen, "'%s' is not a RIFF/WAVE file", path);
        fclose(f);
        return -1;
    }

    bool have_fmt = false;
    uint16_t fmt_channels = 0;
    uint32_t fmt_rate = 0;
    uint8_t *data = NULL;
    uint32_t data_bytes = 0;

    // Walk chunks until we have both `fmt ` and `data`
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8)
            break; // end of file
        uint32_t size = get_le32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16)
                break;
            uint16_t format = get_le16(fmt + 0);
            fmt_channels = get_le16(fmt + 2);
            fmt_rate = get_le32(fmt + 4);
            uint16_t bits = get_le16(fmt + 14);
            if (format != 1 || bits != 16) {
                snprintf(errbuf, errlen, "'%s': only PCM int16 WAV supported (format=%u bits=%u)", path, format, bits);
                fclose(f);
                free(data);
                return -1;
            }
            have_fmt = true;
            // Skip any fmt-chunk extension bytes
            if (size > 16 && fseek(f, (long)(size - 16), SEEK_CUR) != 0)
                break;
        } else if (memcmp(ch, "data", 4) == 0) {
            data = (uint8_t *)malloc(size ? size : 1);
            if (!data || fread(data, 1, size, f) != size) {
                snprintf(errbuf, errlen, "'%s': truncated data chunk", path);
                fclose(f);
                free(data);
                return -1;
            }
            data_bytes = size;
        } else {
            // Unknown chunk: skip payload (chunks are word-aligned)
            if (fseek(f, (long)(size + (size & 1)), SEEK_CUR) != 0)
                break;
        }
        if (have_fmt && data)
            break;
    }
    fclose(f);

    if (!have_fmt || !data) {
        snprintf(errbuf, errlen, "'%s': missing %s chunk", path, have_fmt ? "data" : "fmt");
        free(data);
        return -1;
    }

    // Convert little-endian file samples to host int16
    size_t n = data_bytes / 2;
    int16_t *out = (int16_t *)malloc(n ? n * sizeof(int16_t) : 1);
    if (!out) {
        free(data);
        snprintf(errbuf, errlen, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        out[i] = (int16_t)get_le16(data + i * 2);
    free(data);

    *samples = out;
    *nsamples = n;
    *rate = fmt_rate;
    *channels = (int)fmt_channels;
    return 0;
}

// ============================================================================
// Capture API
// ============================================================================

// Starts recording pushed frames at the current stream parameters
bool audio_out_capture_start(void) {
    if (s.active)
        return false;
    s.nsamples = 0;
    s.have_capture = false;
    s.rate_changed = false;
    s.cap_rate = s.rate;
    s.cap_channels = s.channels ? s.channels : 1;
    s.active = true;
    LOG(1, "capture start: rate=%u Hz channels=%d", s.cap_rate, s.cap_channels);
    return true;
}

// Stops recording; optionally writes the capture to a WAV file
int64_t audio_out_capture_stop(const char *wav_path) {
    if (!s.active)
        return -1;
    s.active = false;
    s.have_capture = true;
    LOG(1, "capture stop: %zu frames", s.nsamples / (size_t)s.cap_channels);
    if (wav_path && *wav_path) {
        if (wav_write(wav_path, s.samples, s.nsamples, s.cap_rate, s.cap_channels) < 0)
            return -1;
    }
    return (int64_t)(s.nsamples / (size_t)s.cap_channels);
}

bool audio_out_capture_active(void) {
    return s.active;
}

uint64_t audio_out_capture_frames(void) {
    return s.cap_channels ? (uint64_t)(s.nsamples / (size_t)s.cap_channels) : 0;
}

// Writes the current capture next to the golden as "<golden>.actual.wav"
// so a mismatch can be listened to and diffed (screenshot-failure ergonomics).
static void write_actual(const char *golden) {
    char path[1024];
    snprintf(path, sizeof(path), "%s.actual.wav", golden);
    if (wav_write(path, s.samples, s.nsamples, s.cap_rate, s.cap_channels) == 0)
        printf("MATCH FAILED: Saved actual capture to '%s'.\n", path);
}

// Sample-exact comparison of the stopped capture against a golden WAV
value_t audio_out_match_value(const char *golden_wav) {
    if (s.active)
        return val_err("sound.match: capture still recording — call capture.stop first");
    if (!s.have_capture)
        return val_err("sound.match: no capture available — record one with capture.start/stop");
    if (s.rate_changed) {
        write_actual(golden_wav);
        return val_err("sound.match: sample rate changed mid-capture");
    }

    int16_t *ref = NULL;
    size_t ref_samples = 0;
    uint32_t ref_rate = 0;
    int ref_channels = 0;
    char err[512];
    if (wav_read(golden_wav, &ref, &ref_samples, &ref_rate, &ref_channels, err, sizeof(err)) < 0) {
        printf("MATCH FAILED: Error loading reference WAV: %s\n", err);
        write_actual(golden_wav);
        return val_err("sound.match: cannot load reference '%s'", golden_wav);
    }

    // Stream parameters must match exactly
    if (ref_rate != s.cap_rate || ref_channels != s.cap_channels) {
        free(ref);
        write_actual(golden_wav);
        return val_err("sound.match: format mismatch — golden %u Hz/%dch vs capture %u Hz/%dch", ref_rate, ref_channels,
                       s.cap_rate, s.cap_channels);
    }

    // Find the first divergent sample (a length mismatch diverges at the
    // shorter stream's end)
    size_t n = s.nsamples < ref_samples ? s.nsamples : ref_samples;
    size_t diff = n;
    for (size_t i = 0; i < n; i++) {
        if (s.samples[i] != ref[i]) {
            diff = i;
            break;
        }
    }
    free(ref);

    if (diff == n && s.nsamples == ref_samples) {
        printf("MATCH OK: Capture matches '%s' (%zu samples).\n", golden_wav, s.nsamples);
        return val_bool(true);
    }

    // Report frame index + timestamp of the first divergence
    size_t frame = diff / (size_t)s.cap_channels;
    double t = s.cap_rate ? (double)frame / (double)s.cap_rate : 0.0;
    printf("MATCH FAILED: Capture does not match '%s'.\n", golden_wav);
    write_actual(golden_wav);
    if (s.nsamples != ref_samples && diff == n)
        return val_err("sound.match: length mismatch — golden %zu vs capture %zu samples (diverge at %.3fs)",
                       ref_samples, s.nsamples, t);
    return val_err("sound.match: first divergent sample at index %zu (frame %zu, %.3fs)", diff, frame, t);
}

// ============================================================================
// Object-model surface: the `capture` node
// ============================================================================

static value_t capture_method_start(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    if (!audio_out_capture_start())
        return val_err("sound.capture.start: capture already active");
    return val_none();
}

static value_t capture_method_stop(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = (argc >= 1 && argv[0].s && *argv[0].s) ? argv[0].s : NULL;
    if (path) {
        size_t n = strlen(path);
        if (n < 4 || strcasecmp(path + n - 4, ".wav") != 0)
            return val_err("sound.capture.stop: path must end in .wav (got '%s')", path);
    }
    if (!audio_out_capture_active())
        return val_err("sound.capture.stop: no capture active");
    int64_t frames = audio_out_capture_stop(path);
    if (frames < 0)
        return val_err("sound.capture.stop: failed to write '%s'", path ? path : "");
    return val_uint(4, (uint64_t)frames);
}

static value_t capture_attr_active(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_bool(audio_out_capture_active());
}

static value_t capture_attr_frames(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, audio_out_capture_frames());
}

// Peak |sample| of the current/last capture, DC-compensated against the
// stream's first sample (the ASC's offset-binary silence sits at a constant
// non-zero level; peak-vs-first isolates actual signal).  Diagnostic: lets a
// live session answer "is there audio IN the stream?" without exporting a WAV.
static value_t capture_attr_peak(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    if (!s.samples || s.nsamples == 0)
        return val_uint(4, 0);
    int16_t base = s.samples[0];
    int32_t peak = 0;
    for (size_t i = 0; i < s.nsamples; i++) {
        int32_t d = (int32_t)s.samples[i] - base;
        if (d < 0)
            d = -d;
        if (d > peak)
            peak = d;
    }
    return val_uint(4, (uint64_t)peak);
}

static const arg_decl_t capture_stop_args[] = {
    {.name = "path",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Optional WAV path to write the capture to (golden regeneration)"},
};

static const member_t capture_members[] = {
    {.kind = M_ATTR,
     .name = "active",
     .flags = VAL_RO,
     .doc = "True while a capture is recording",
     .attr = {.type = V_BOOL, .get = capture_attr_active, .set = NULL}},
    {.kind = M_ATTR,
     .name = "frames",
     .flags = VAL_RO,
     .doc = "Frames accumulated in the current or last capture",
     .attr = {.type = V_UINT, .get = capture_attr_frames, .set = NULL}},
    {.kind = M_ATTR,
     .name = "peak",
     .flags = VAL_RO,
     .doc = "Peak |sample - first sample| of the capture (signal presence check)",
     .attr = {.type = V_UINT, .get = capture_attr_peak, .set = NULL}},
    {.kind = M_METHOD,
     .name = "start",
     .doc = "Start recording producer audio at guest rate (deterministic)",
     .method = {.args = NULL, .nargs = 0, .result = V_NONE, .fn = capture_method_start}},
    {.kind = M_METHOD,
     .name = "stop",
     .doc = "Stop recording; optionally write the capture as a PCM int16 WAV",
     .method = {.args = capture_stop_args, .nargs = 1, .result = V_UINT, .fn = capture_method_stop}},
};

const class_desc_t audio_capture_class = {
    .name = "capture",
    .members = capture_members,
    .n_members = sizeof(capture_members) / sizeof(capture_members[0]),
};

// Attaches the singleton `capture` node under a machine's sound object
struct object *audio_out_capture_attach(struct object *parent) {
    if (s.object || !parent)
        return s.object;
    s.object = object_new(&audio_capture_class, NULL, "capture");
    if (s.object) {
        object_set_label(s.object, "Capture");
        object_attach(parent, s.object);
    }
    return s.object;
}

// Detaches and deletes the capture node (machine teardown)
void audio_out_capture_detach(void) {
    if (!s.object)
        return;
    object_detach(s.object);
    object_delete(s.object);
    s.object = NULL;
}
