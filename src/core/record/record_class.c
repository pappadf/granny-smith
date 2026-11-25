// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// record_class.c
// The `recorder` object: start and stop, the settings a recording starts
// with, its state, and -- for tests and for looking at a recording without
// another tool -- a reader that summarises a file, decodes one of its
// frames to a PNG or its sound to a WAV, and a snapshot of the screen as a
// recording shows it (the response tables applied).  A root object beside
// `checkpoint`: a recording spans machine swaps and covers screen and
// sound, so it belongs to neither machine.screen nor machine.sound.

#include "record.h"

#include "record_pixels.h"

#include "audio_out.h"
#include "codec.h"
#include "codec_flac.h"
#include "codec_zmbv.h"
#include "debug.h"
#include "display.h"
#include "mkv_read.h"
#include "object.h"
#include "object_order.h"
#include "storage_util.h"
#include "system.h"
#include "value.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Settings
// ============================================================================

static const char *const k_rate_names[] = {"full", "half", "quarter", NULL};
static const char *const k_format_names[] = {"auto", "rgb", NULL};

// The settings are fixed while a recording runs.
static value_t settings_locked(const char *attr) {
    return val_err("recorder.%s: fixed while recording (stop first)", attr);
}

// `rate`: full, half or quarter.
static DEF_GETTER(rec_attr_rate) {
    return val_enum((int)recorder_settings()->rate, k_rate_names, 3);
}

// Set `rate` (refused while recording).
static DEF_SETTER(rec_attr_rate_set) {
    if (recorder_active()) {
        value_free(&in);
        return settings_locked("rate");
    }
    if (in.kind != VK_ENUM || in.enm.idx < 0 || in.enm.idx > 2) {
        value_free(&in);
        return val_err("recorder.rate: expected full, half or quarter");
    }
    recorder_settings()->rate = (rec_rate_t)in.enm.idx;
    value_free(&in);
    return val_none();
}

// `format`: auto or rgb.
static DEF_GETTER(rec_attr_format) {
    return val_enum(recorder_settings()->rgb ? 1 : 0, k_format_names, 2);
}

// Set `format` (refused while recording).
static DEF_SETTER(rec_attr_format_set) {
    if (recorder_active()) {
        value_free(&in);
        return settings_locked("format");
    }
    if (in.kind != VK_ENUM || in.enm.idx < 0 || in.enm.idx > 1) {
        value_free(&in);
        return val_err("recorder.format: expected auto or rgb");
    }
    recorder_settings()->rgb = in.enm.idx == 1;
    value_free(&in);
    return val_none();
}

// `audio`: whether the sound is recorded.
static DEF_GETTER(rec_attr_audio) {
    return val_bool(recorder_settings()->audio);
}

// Set `audio` (refused while recording).
static DEF_SETTER(rec_attr_audio_set) {
    if (recorder_active())
        return settings_locked("audio");
    recorder_settings()->audio = in.b;
    return val_none();
}

// `block`: wait for the recorder instead of dropping frames.
static DEF_GETTER(rec_attr_block) {
    return val_bool(recorder_settings()->block);
}

// The one setting that may change mid-recording: it is a policy, not a format.
static DEF_SETTER(rec_attr_block_set) {
    recorder_settings()->block = in.b;
    recorder_set_block_live(in.b);
    return val_none();
}

// `threaded`: code on the recorder thread.
static DEF_GETTER(rec_attr_threaded) {
    return val_bool(recorder_settings()->threaded);
}

// Set `threaded` (refused while recording).
static DEF_SETTER(rec_attr_threaded_set) {
    if (recorder_active())
        return settings_locked("threaded");
    recorder_settings()->threaded = in.b;
    return val_none();
}

// `canvas`: "auto" or "WxH".
static DEF_GETTER(rec_attr_canvas) {
    const recorder_settings_t *s = recorder_settings();
    if (!s->canvas_w || !s->canvas_h)
        return val_str("auto");
    char buf[32];
    snprintf(buf, sizeof buf, "%ux%u", (unsigned)s->canvas_w, (unsigned)s->canvas_h);
    return val_str(buf);
}

// Set `canvas` from "auto" or "WxH" (refused while recording).
static DEF_SETTER(rec_attr_canvas_set) {
    if (recorder_active()) {
        value_free(&in);
        return settings_locked("canvas");
    }
    const char *t = in.kind == VK_STRING && in.s ? in.s : "";
    unsigned w = 0, h = 0;
    char tail = 0;
    if (strcmp(t, "auto") == 0) {
        recorder_settings()->canvas_w = recorder_settings()->canvas_h = 0;
    } else if (sscanf(t, "%ux%u%c", &w, &h, &tail) == 2 && w >= 1 && h >= 1 && w <= 8192 && h <= 8192) {
        recorder_settings()->canvas_w = w;
        recorder_settings()->canvas_h = h;
    } else {
        value_t e = val_err("recorder.canvas: expected \"auto\" or \"WxH\" (got '%s')", t);
        value_free(&in);
        return e;
    }
    value_free(&in);
    return val_none();
}

// ============================================================================
// State
// ============================================================================

// `active`: a recording runs.
static DEF_GETTER(rec_attr_active) {
    return val_bool(recorder_active());
}

// `path`: the file being written, or the last one.
static DEF_GETTER(rec_attr_path) {
    recorder_state_t st;
    recorder_get_state(&st);
    return val_str(st.path);
}

// `frames`: output frames, every segment.
static DEF_GETTER(rec_attr_frames) {
    recorder_state_t st;
    recorder_get_state(&st);
    return val_uint(8, st.frames);
}

// `units`: frame-units completed since start.
static DEF_GETTER(rec_attr_units) {
    recorder_state_t st;
    recorder_get_state(&st);
    return val_uint(8, st.units);
}

// `seconds`: the emulated time the frames cover.
static DEF_GETTER(rec_attr_seconds) {
    recorder_state_t st;
    recorder_get_state(&st);
    return val_float(st.seconds);
}

// `bytes`: file bytes so far.
static DEF_GETTER(rec_attr_bytes) {
    recorder_state_t st;
    recorder_get_state(&st);
    return val_uint(8, st.bytes);
}

// `dropped`: frames recorded as repeats because the recorder was behind.
static DEF_GETTER(rec_attr_dropped) {
    recorder_state_t st;
    recorder_get_state(&st);
    return val_uint(8, st.dropped);
}

// `segments`: files written so far.
static DEF_GETTER(rec_attr_segments) {
    recorder_state_t st;
    recorder_get_state(&st);
    return val_uint(4, st.segments);
}

// ============================================================================
// Start and stop
// ============================================================================

// `recorder.start([path])`: begin; the path being written.
static DEF_METHOD(rec_method_start) {
    const char *path = (argc >= 1 && argv[0].kind == VK_STRING && argv[0].s) ? argv[0].s : "";
    char err[300];
    if (recorder_start(path, err, sizeof err) < 0)
        return val_err("recorder.start: %s", err);
    recorder_state_t st;
    recorder_get_state(&st);
    return val_str(st.path);
}

// `recorder.stop()`: finalise; the paths written.
static DEF_METHOD(rec_method_stop) {
    char **paths = NULL;
    size_t n = 0;
    char err[200];
    if (recorder_stop(&paths, &n, err, sizeof err) < 0)
        return val_err("recorder.stop: %s", err);
    value_t *items = NULL;
    size_t len = 0, cap = 0;
    for (size_t i = 0; i < n; i++) {
        val_list_push(&items, &len, &cap, val_str(paths[i]));
        free(paths[i]);
    }
    free(paths);
    return val_list(items, len);
}

// ============================================================================
// Reading a recording back
// ============================================================================

// A recording opened for reading: its tracks found.
typedef struct {
    mkv_reader_t *r;
    const mkv_track_t *video, *audio;
} rec_file_t;

// Open `path` and find its ZMBV and FLAC tracks; NULL with `err` set.
static bool file_open(rec_file_t *f, const char *path, char *err, size_t cap) {
    memset(f, 0, sizeof *f);
    int e = 0;
    f->r = mkv_read_open(path, &e);
    if (!f->r) {
        snprintf(err, cap, "cannot read '%s': %s", path, strerror(e < 0 ? -e : EIO));
        return false;
    }
    for (int i = 0; i < mkv_read_track_count(f->r); i++) {
        const mkv_track_t *t = mkv_read_track(f->r, i);
        if (t->type == MKV_TRACK_VIDEO && !f->video)
            f->video = t;
        else if (t->type == MKV_TRACK_AUDIO && !f->audio)
            f->audio = t;
    }
    if (!f->video || !f->video->pixel_width || !f->video->pixel_height) {
        snprintf(err, cap, "'%s' has no video track", path);
        mkv_read_close(f->r);
        return false;
    }
    return true;
}

// The CRC-32 and size of a whole file; false when it cannot be read.
static bool file_crc(const char *path, uint32_t *crc, uint64_t *size) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;
    uint8_t buf[65536];
    uint32_t c = 0;
    uint64_t n = 0;
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, fp)) > 0) {
        c = codec_crc32(c, buf, got);
        n += got;
    }
    fclose(fp);
    *crc = c;
    *size = n;
    return true;
}

// `recorder.inspect(path)`: what a recording holds, decoded end to end.
static DEF_METHOD(rec_method_inspect) {
    const char *path = argv[0].s;
    char err[300];
    rec_file_t f;
    if (!file_open(&f, path, err, sizeof err))
        return val_err("recorder.inspect: %s", err);
    int e = 0;
    codec_zmbv_dec_t *zd = codec_zmbv_dec_new((int)f.video->pixel_width, (int)f.video->pixel_height, &e);
    codec_flac_dec_t *fd = f.audio ? codec_flac_dec_new(&e) : NULL;
    int16_t *pcm = malloc(CODEC_FLAC_BLOCK * 2 * sizeof(int16_t));
    uint64_t frames = 0, keys = 0, vbytes = 0, abytes = 0, asamples = 0, apackets = 0, bad = 0;
    int64_t last_ts = 0;
    mkv_packet_t p;
    int rc;
    while (zd && pcm && (rc = mkv_read_packet(f.r, &p)) == 1) {
        if (p.track == f.video->number) {
            frames++;
            keys += p.keyframe;
            vbytes += p.len;
            last_ts = p.timestamp;
            if (codec_zmbv_dec_frame(zd, p.data, p.len) != 0)
                bad++;
        } else if (f.audio && p.track == f.audio->number) {
            apackets++;
            abytes += p.len;
            int64_t n = fd ? codec_flac_dec_frame(fd, p.data, p.len, pcm, CODEC_FLAC_BLOCK, NULL) : -1;
            if (n < 0)
                bad++;
            else
                asamples += (uint64_t)n;
        }
    }
    uint32_t crc = 0;
    uint64_t size = 0;
    file_crc(path, &crc, &size);
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "frames", val_uint(8, frames));
    val_map_put(b, "keyframes", val_uint(8, keys));
    val_map_put(b, "width", val_uint(4, f.video->pixel_width));
    val_map_put(b, "height", val_uint(4, f.video->pixel_height));
    val_map_put(b, "bits", val_uint(4, zd ? (uint64_t)codec_zmbv_dec_bytes_per_pixel(zd) * 8 : 0));
    val_map_put(b, "frame_period_ns", val_uint(8, f.video->default_duration_ns));
    val_map_put(b, "last_ms", val_int(last_ts));
    val_map_put(b, "duration_ms", val_float(mkv_read_duration(f.r)));
    val_map_put(b, "video_bytes", val_uint(8, vbytes));
    val_map_put(b, "audio", val_bool(f.audio != NULL));
    val_map_put(b, "audio_rate", val_uint(4, f.audio ? (uint64_t)f.audio->sampling_frequency : 0));
    val_map_put(b, "audio_channels", val_uint(4, f.audio ? f.audio->channels : 0));
    val_map_put(b, "audio_packets", val_uint(8, apackets));
    val_map_put(b, "audio_frames", val_uint(8, asamples));
    val_map_put(b, "audio_bytes", val_uint(8, abytes));
    val_map_put(b, "decode_errors", val_uint(8, bad));
    val_map_put(b, "truncated", val_bool(mkv_read_truncated(f.r)));
    val_map_put(b, "size", val_uint(8, size));
    val_map_put(b, "crc32", val_uint(4, crc));
    free(pcm);
    codec_flac_dec_free(fd);
    codec_zmbv_dec_free(zd);
    mkv_read_close(f.r);
    return val_map_finish(b);
}

// Write a w x h RGB24 picture as a PNG (through the screen's PNG writer).
static int write_rgb_png(const uint8_t *rgb, uint32_t w, uint32_t h, const char *out) {
    uint8_t *xrgb = malloc((size_t)w * h * 4);
    if (!xrgb)
        return -1;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        xrgb[i * 4] = 0;
        xrgb[i * 4 + 1] = rgb[i * 3];
        xrgb[i * 4 + 2] = rgb[i * 3 + 1];
        xrgb[i * 4 + 3] = rgb[i * 3 + 2];
    }
    // A 32 bpp descriptor with no tables: the PNG is the pixels as they are
    display_t d = {.width = w, .height = h, .stride = w * 4, .format = PIXEL_32BPP_XRGB, .bits = xrgb};
    int rc = save_framebuffer_as_png(&d, out);
    free(xrgb);
    return rc;
}

// Decode frame `index` (negative: from the end, -1 the last) to a PNG.
static value_t extract_frame(const char *path, const char *out, int64_t index) {
    char err[300];
    rec_file_t f;
    if (!file_open(&f, path, err, sizeof err))
        return val_err("recorder.extract: %s", err);
    if (index < 0) {
        // Count the frames first, to resolve a position from the end
        uint64_t n = 0;
        mkv_packet_t p;
        while (mkv_read_packet(f.r, &p) == 1)
            n += p.track == f.video->number;
        mkv_read_close(f.r);
        if ((uint64_t)(-index) > n)
            return val_err("recorder.extract: '%s' has %llu frames", path, (unsigned long long)n);
        index = (int64_t)n + index;
        if (!file_open(&f, path, err, sizeof err))
            return val_err("recorder.extract: %s", err);
    }
    uint32_t w = (uint32_t)f.video->pixel_width, h = (uint32_t)f.video->pixel_height;
    int e = 0;
    codec_zmbv_dec_t *zd = codec_zmbv_dec_new((int)w, (int)h, &e);
    uint8_t *rgb = malloc((size_t)w * h * 3);
    int64_t k = -1;
    mkv_packet_t p;
    value_t result = val_none();
    while (zd && rgb && k < index && mkv_read_packet(f.r, &p) == 1) {
        if (p.track != f.video->number)
            continue;
        k++;
        if (codec_zmbv_dec_frame(zd, p.data, p.len) != 0) {
            result = val_err("recorder.extract: frame %lld: %s", (long long)k, codec_zmbv_dec_message(zd));
            break;
        }
    }
    if (result.kind == VK_NONE) {
        if (!zd || !rgb)
            result = val_err("recorder.extract: out of memory");
        else if (k < index)
            result = val_err("recorder.extract: '%s' has %lld frames", path, (long long)(k + 1));
        else if (codec_zmbv_dec_rgb24(zd, rgb) != 0 || write_rgb_png(rgb, w, h, out) < 0)
            result = val_err("recorder.extract: cannot write '%s'", out);
    }
    free(rgb);
    codec_zmbv_dec_free(zd);
    mkv_read_close(f.r);
    return result;
}

// The whole sound of a recording, decoded: malloc'd interleaved samples in
// *out (*frames sample frames of *channels), its rate.  NULL error message
// on success, else why (written to err).
static const char *decode_audio(const char *path, int16_t **out, size_t *frames, int *channels, uint32_t *rate,
                                char *err, size_t cap) {
    *out = NULL;
    *frames = 0;
    rec_file_t f;
    if (!file_open(&f, path, err, cap))
        return err;
    if (!f.audio) {
        mkv_read_close(f.r);
        snprintf(err, cap, "'%s' has no audio track", path);
        return err;
    }
    int ch = (int)(f.audio->channels ? f.audio->channels : 1);
    *channels = ch;
    *rate = (uint32_t)f.audio->sampling_frequency;
    int e = 0;
    codec_flac_dec_t *fd = codec_flac_dec_new(&e);
    int16_t *pcm = malloc(CODEC_FLAC_BLOCK * 2 * sizeof(int16_t));
    int16_t *all = NULL;
    size_t n = 0, cap_s = 0;
    const char *why = (!fd || !pcm) ? "out of memory" : NULL;
    mkv_packet_t p;
    while (!why && mkv_read_packet(f.r, &p) == 1) {
        if (p.track != f.audio->number)
            continue;
        int64_t got = codec_flac_dec_frame(fd, p.data, p.len, pcm, CODEC_FLAC_BLOCK, NULL);
        if (got < 0) {
            snprintf(err, cap, "audio: %s", codec_flac_dec_message(fd));
            why = err;
            break;
        }
        size_t add = (size_t)got * (size_t)ch;
        if (n + add > cap_s) {
            size_t nc = cap_s ? cap_s * 2 : 1u << 20;
            while (nc < n + add)
                nc *= 2;
            int16_t *g = realloc(all, nc * sizeof(int16_t));
            if (!g) {
                why = "out of memory";
                break;
            }
            all = g;
            cap_s = nc;
        }
        memcpy(all + n, pcm, add * sizeof(int16_t));
        n += add;
    }
    free(pcm);
    codec_flac_dec_free(fd);
    mkv_read_close(f.r);
    if (why) {
        free(all);
        if (why != err)
            snprintf(err, cap, "%s", why);
        return err;
    }
    *out = all;
    *frames = n / (size_t)ch;
    return NULL;
}

// Decode the sound from sample frame `from`, `count` of them (0: to the end), to a WAV.
static value_t extract_audio(const char *path, const char *out, int64_t from, int64_t count) {
    char err[300];
    int16_t *all;
    size_t total;
    int ch;
    uint32_t rate;
    if (decode_audio(path, &all, &total, &ch, &rate, err, sizeof err))
        return val_err("recorder.extract: %s", err);
    value_t result;
    if (from < 0 || (uint64_t)from > total) {
        result = val_err("recorder.extract: sample %lld is past the end (%zu)", (long long)from, total);
    } else {
        size_t len = count > 0 ? (size_t)count : total - (size_t)from;
        if ((size_t)from + len > total)
            result =
                val_err("recorder.extract: %zu samples from %lld run past the end (%zu)", len, (long long)from, total);
        else if (audio_wav_write(out, all ? all + (size_t)from * (size_t)ch : NULL, len * (size_t)ch, rate, ch) < 0)
            result = val_err("recorder.extract: cannot write '%s'", out);
        else
            result = val_uint(8, len);
    }
    free(all);
    return result;
}

// A sample through the guest's volume as the recording applies it: v / 7,
// rounded half away from zero (record_audio.c).
static int16_t at_volume(int16_t s, int v) {
    int32_t p = (int32_t)s * v * 2;
    return (int16_t)((p + (p >= 0 ? 7 : -7)) / 14);
}

// `recorder.find(path, wav, [volume])`: the sample frame at which a WAV's
// samples appear, exactly, in a recording's sound -- where a captured sound
// landed on the recording's timeline -- or -1.  A capture is taken before
// the guest's volume: `volume` (0..7) scales it as the recording did.
static DEF_METHOD(rec_method_find) {
    char err[300];
    int16_t *all;
    size_t total;
    int ch;
    uint32_t rate;
    if (decode_audio(argv[0].s, &all, &total, &ch, &rate, err, sizeof err))
        return val_err("recorder.find: %s", err);
    int16_t *ref = NULL;
    size_t nref = 0;
    uint32_t ref_rate = 0;
    int ref_ch = 0;
    if (audio_wav_read(argv[1].s, &ref, &nref, &ref_rate, &ref_ch, err, sizeof err) < 0) {
        free(all);
        return val_err("recorder.find: %s", err);
    }
    if (ref_rate != rate || ref_ch != ch) {
        free(all);
        free(ref);
        return val_err("recorder.find: the WAV is %u Hz x %d, the recording %u Hz x %d", ref_rate, ref_ch, rate, ch);
    }
    int vol = argc >= 3 && argv[2].kind == VK_INT ? (int)argv[2].i : 7;
    if (vol < 0 || vol > 7) {
        free(all);
        free(ref);
        return val_err("recorder.find: volume %d is not 0..7", vol);
    }
    if (vol != 7)
        for (size_t i = 0; i < nref; i++)
            ref[i] = at_volume(ref[i], vol);
    size_t want = nref / (size_t)ch;
    int64_t at = -1;
    // The first offset where every sample agrees
    for (size_t i = 0; want && i + want <= total && at < 0; i++) {
        if (memcmp(all + i * (size_t)ch, ref, want * (size_t)ch * sizeof(int16_t)) == 0)
            at = (int64_t)i;
    }
    free(all);
    free(ref);
    return val_int(at);
}

// `recorder.extract(path, out, [index], [count])`: a frame to a PNG, or the
// sound to a WAV, by the output's extension.
static DEF_METHOD(rec_method_extract) {
    const char *path = argv[0].s, *out = argv[1].s;
    bool has_index = argc >= 3 && argv[2].kind == VK_INT;
    int64_t index = has_index ? argv[2].i : 0;
    int64_t count = argc >= 4 && argv[3].kind == VK_INT ? argv[3].i : 0;
    if (path_has_ext(out, ".png")) {
        value_t r = extract_frame(path, out, has_index ? index : -1);
        return r.kind == VK_NONE ? val_bool(true) : r;
    }
    if (path_has_ext(out, ".wav"))
        return extract_audio(path, out, index, count);
    return val_err("recorder.extract: the output must end in .png (a frame) or .wav (the sound)");
}

// `recorder.snapshot(out)`: the screen now as a recording shows it --
// machine.screen.save's pixels with the monitor's response curve applied.
static DEF_METHOD(rec_method_snapshot) {
    const char *out = argv[0].s;
    if (!path_has_ext(out, ".png"))
        return val_err("recorder.snapshot: path must end in .png (got '%s')", out);
    const display_t *d = system_display_synced();
    if (!d || !d->bits || !d->width || !d->height)
        return val_err("recorder.snapshot: framebuffer not available");
    uint8_t *rgb = malloc((size_t)d->width * d->height * 3);
    if (!rgb)
        return val_err("recorder.snapshot: out of memory");
    for (uint32_t y = 0; y < d->height; y++) {
        const uint8_t *row = d->bits + (size_t)y * d->stride;
        for (uint32_t x = 0; x < d->width; x++) {
            // The bus's colour (CLUT and DAC table), then the monitor's curve
            uint8_t *px = rgb + ((size_t)y * d->width + x) * 3;
            display_pixel_rgb(d, row, x, px);
            if (d->crt_response)
                for (int c = 0; c < 3; c++)
                    px[c] = d->crt_response[c][px[c]];
        }
    }
    int rc = write_rgb_png(rgb, d->width, d->height, out);
    free(rgb);
    if (rc < 0)
        return val_err("recorder.snapshot: cannot write '%s'", out);
    return val_none();
}

// ============================================================================
// Class descriptor
// ============================================================================

static const arg_decl_t rec_start_args[] = {
    {.name = "path",
     .kind = VK_STRING,
     .presentation_flags = VFLAG_PATH,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "The .mkv to write",
     .default_doc = "the platform's default name (the browser's /opfs/recordings/<machine>-<time>.mkv)"},
};

static const arg_decl_t rec_inspect_args[] = {
    ARG_PATH("path", "A recording (.mkv)"),
};

static const arg_decl_t rec_extract_args[] = {
    ARG_PATH("path", "A recording (.mkv)"),
    ARG_PATH("out", "A .png for one frame, or a .wav for the sound"),
    {.name = "index",
                                                              .kind = VK_INT,
                                                              .validation_flags = OBJ_ARG_OPTIONAL,
                                                              .doc = "PNG: the frame (negative counts from the end); WAV: the first sample frame",
                                                              .default_doc = "the last frame; the first sample"},
    {.name = "count",
                                                              .kind = VK_INT,
                                                              .validation_flags = OBJ_ARG_OPTIONAL,
                                                              .doc = "WAV: sample frames to write",
                                                              .default_doc = "to the end"                      },
};

static const arg_decl_t rec_find_args[] = {
    ARG_PATH("path", "A recording (.mkv)"),
    ARG_PATH("wav", "A PCM WAV at the recording's rate and channel count"),
    {.name = "volume",
                                                                    .kind = VK_INT,
                                                                    .validation_flags = OBJ_ARG_OPTIONAL,
                                                                    .doc = "The guest's volume (0..7) the recording applied to the sound the WAV captured",
                                                                    .default_doc = "7, the samples as they are"},
};

static const arg_decl_t rec_snapshot_args[] = {
    ARG_PATH("path", "Output PNG path"),
};

static const member_t recorder_members[] = {
    {.kind = MK_METHOD,
     .name = "start",
     .examples = EXAMPLES("recorder.start \"/opfs/recordings/session.mkv\"", "recorder.start"),
     .doc = "Begin recording the screen and sound to a Matroska file (ZMBV + FLAC)",
     .method = {.result_doc = "the path being written",
                METHOD_ARGS(rec_start_args),
                .result = VK_STRING,
                .ui_flags = MM_MUTATE,
                .fn = rec_method_start}},
    {.kind = MK_METHOD,
     .name = "stop",
     .examples = EXAMPLES("recorder.stop"),
     .doc = "Finalise the recording; returns when the file is closed",
     .method = {.result_doc = "the paths written (one per segment)",
                .args = NULL,
                .nargs = 0,
                .result = VK_LIST,
                .ui_flags = MM_MUTATE,
                .fn = rec_method_stop}},
    {.kind = MK_ATTR,
     .name = "rate",
     .doc = "Output frames per guest frame: full (60.15 fps), half or quarter; fixed while recording",
     .attr = {.type = VK_ENUM, .enum_values = k_rate_names, .get = rec_attr_rate, .set = rec_attr_rate_set}},
    {.kind = MK_ATTR,
     .name = "audio",
     .doc = "Record the sound too (when the machine has a stream); fixed while recording",
     .attr = {.type = VK_BOOL, .get = rec_attr_audio, .set = rec_attr_audio_set}},
    {.kind = MK_ATTR,
     .name = "format",
     .doc = "auto (indexed if the screen is, else direct colour) or rgb (32 bpp throughout); fixed while recording",
     .attr = {.type = VK_ENUM, .enum_values = k_format_names, .get = rec_attr_format, .set = rec_attr_format_set}},
    {.kind = MK_ATTR,
     .name = "canvas",
     .doc = "The stored frame size: auto (the screen's at start) or \"WxH\"; a smaller mode is centred on black",
     .attr = {.type = VK_STRING, .get = rec_attr_canvas, .set = rec_attr_canvas_set}},
    {.kind = MK_ATTR,
     .name = "block",
     .doc = "Wait for the recorder instead of dropping frames when it falls behind (headless default true)",
     .attr = {.type = VK_BOOL, .get = rec_attr_block, .set = rec_attr_block_set}},
    {.kind = MK_ATTR,
     .name = "threaded",
     .flags = M_CAT_ADVANCED,
     .doc = "Code on a recorder thread (false: on the emulator thread, as headless --recorder=inline)",
     .attr = {.type = VK_BOOL, .get = rec_attr_threaded, .set = rec_attr_threaded_set}},
    {.kind = MK_ATTR,
     .name = "active",
     .doc = "True while recording",
     .attr = {.type = VK_BOOL, .get = rec_attr_active}},
    {.kind = MK_ATTR,
     .name = "path",
     .doc = "The file being written (or the last one written)",
     .attr = {.type = VK_STRING, .presentation_flags = VFLAG_PATH, .get = rec_attr_path}},
    {.kind = MK_ATTR,
     .name = "frames",
     .doc = "Output frames recorded, every segment",
     .attr = {.type = VK_UINT, .presentation_flags = VFLAG_VOLATILE, .get = rec_attr_frames}},
    {.kind = MK_ATTR,
     .name = "seconds",
     .doc = "Emulated seconds the recorded frames cover",
     .attr = {.type = VK_FLOAT, .presentation_flags = VFLAG_VOLATILE, .get = rec_attr_seconds}},
    {.kind = MK_ATTR,
     .name = "bytes",
     .doc = "File size so far, every segment",
     .attr = {.type = VK_UINT, .presentation_flags = VFLAG_VOLATILE, .get = rec_attr_bytes}},
    {.kind = MK_ATTR,
     .name = "dropped",
     .doc = "Frames recorded as a repeat because the recorder was behind",
     .attr = {.type = VK_UINT, .presentation_flags = VFLAG_VOLATILE, .get = rec_attr_dropped}},
    {.kind = MK_ATTR,
     .name = "units",
     .flags = M_CAT_ADVANCED,
     .doc = "Frame-units completed since the recording started (frames = units / rate's divisor)",
     .attr = {.type = VK_UINT, .presentation_flags = VFLAG_VOLATILE, .get = rec_attr_units}},
    {.kind = MK_ATTR,
     .name = "segments",
     .flags = M_CAT_ADVANCED,
     .doc = "Files the recording has written so far (a mode that does not fit starts the next)",
     .attr = {.type = VK_UINT, .presentation_flags = VFLAG_VOLATILE, .get = rec_attr_segments}},
    {.kind = MK_METHOD,
     .name = "inspect",
     .flags = M_CAT_ADVANCED,
     .examples = EXAMPLES("recorder.inspect \"/opfs/recordings/session.mkv\""),
     .doc = "Read a recording end to end: frames, keyframes, sizes, sound, the file's CRC-32",
     .method = {.result_doc = "a map of the recording's figures",
                METHOD_ARGS(rec_inspect_args),
                .result = VK_MAP,
                .fn = rec_method_inspect}},
    {.kind = MK_METHOD,
     .name = "extract",
     .flags = M_CAT_ADVANCED,
     .examples = EXAMPLES("recorder.extract \"/opfs/recordings/session.mkv\" \"/tmp/last.png\"",
     "recorder.extract \"/opfs/recordings/session.mkv\" \"/tmp/sound.wav\""),
     .doc = "Decode one frame of a recording to a PNG, or its sound (or a window of it) to a WAV",
     .method = {.result_doc = "true for a frame; the sample frames written for the sound",
                METHOD_ARGS(rec_extract_args),
                .result = VK_ANY,
                .fn = rec_method_extract}},
    {.kind = MK_METHOD,
     .name = "find",
     .flags = M_CAT_ADVANCED,
     .examples = EXAMPLES("recorder.find \"/opfs/recordings/session.mkv\" \"/tmp/capture.wav\""),
     .doc = "Where a WAV's samples appear, exactly, in a recording's sound",
     .method = {.result_doc = "the first sample frame they start at, or -1",
                METHOD_ARGS(rec_find_args),
                .result = VK_INT,
                .fn = rec_method_find}},
    {.kind = MK_METHOD,
     .name = "snapshot",
     .flags = M_CAT_ADVANCED,
     .examples = EXAMPLES("recorder.snapshot \"/tmp/monitor.png\""),
     .doc = "Save the screen as a recording shows it: screen.save's pixels through the monitor's response curve",
     .method = {METHOD_ARGS(rec_snapshot_args), .result = VK_NONE, .fn = rec_method_snapshot}},
};

static const class_desc_t recorder_class = {
    .name = "recorder",
    .members = recorder_members,
    .n_members = sizeof(recorder_members) / sizeof(recorder_members[0]),
    .doc = "Records the screen and sound to a lossless video file on the guest's clock",
};

// ============================================================================
// Lifecycle (process singleton)
// ============================================================================

static struct object *s_recorder_object = NULL;

// Register the `recorder` root object.
void recorder_init(void) {
    if (s_recorder_object)
        return;
    s_recorder_object = object_new(&recorder_class, NULL, "recorder");
    if (s_recorder_object) {
        object_set_order(s_recorder_object, OBJ_ORDER_RECORDER);
        object_attach(object_root(), s_recorder_object);
    }
}
