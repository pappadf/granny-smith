// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// Apple Sound Chip unit test (proposal-sound-support-all-models §5/§8).
//
// Links the real asc.c against recording stubs and pins three layers:
//
//  1. The ROM POST register vectors — Apple's USTNonCritTsts.a `sndrnold` /
//     `sndrdold` tables (standard old-silicon ASC, the SE/30/IIx/IIcx part):
//     every (register, data, writable-mask) tuple must read back what POST
//     expects, including the ascFifoInt write-through diagnostic case and
//     the old/new-silicon volume probe. Plus the POST SRAM pattern test in
//     wavetable mode.
//  2. The FIFO IRQ state machine — half-empty latch-on-transition (fires on
//     the downward crossing only), overflow latch on push, read-clears with
//     CB1 release, the FIFO-clear strobe, and hold-last-byte on underflow.
//  3. The sample-rate producer — int16 frame conversion, per-board speaker
//     mix (SE/30 sum vs IIx/IIcx channel A), wavetable free-run voice sum,
//     push batching, flush on mode-off, and ascClockRate rate switching.

#include "asc.h"
#include "audio_out.h"
#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <stdint.h>
#include <string.h>

// Register offsets from the ASC base (mirrors asc.c / SoundPrivate.a)
#define R_VERSION 0x800
#define R_MODE    0x801
#define R_CONTROL 0x802
#define R_FIFOCTL 0x803
#define R_FIFOINT 0x804
#define R_ONESHOT 0x805
#define R_VOLUME  0x806
#define R_CLOCK   0x807
#define R_WAVE    0x810

// ============================================================================
// Recording stubs
// ============================================================================

// --- scheduler: capture the producer event so tests can tick it manually ---
typedef void (*event_callback_t_local)(void *, uint64_t);
static event_callback_t_local s_cb;
static void *s_cb_src;
static uint64_t s_period_ns;
static int s_cancels;

event_t *scheduler_new_cpu_event(scheduler_t *sch, event_callback_t callback, void *source, uint64_t data,
                                 uint64_t cycles, uint64_t ns) {
    (void)sch;
    (void)data;
    (void)cycles;
    s_cb = (event_callback_t_local)callback;
    s_cb_src = source;
    s_period_ns = ns;
    return NULL;
}

void remove_event(scheduler_t *sch, event_callback_t callback, void *source) {
    (void)sch;
    (void)callback;
    (void)source;
    s_cb = NULL;
    s_cancels++;
}

void scheduler_new_event_type(scheduler_t *sch, const char *source_name, void *source, const char *event_name,
                              event_callback_t callback) {
    (void)sch;
    (void)source_name;
    (void)source;
    (void)event_name;
    (void)callback;
}

// Advance the producer by n sample ticks (the drain re-schedules itself)
static void tick(int n) {
    for (int i = 0; i < n; i++) {
        ASSERT_TRUE(s_cb != NULL);
        s_cb(s_cb_src, 0);
    }
}

// --- VIA: record the CB1 interrupt line (active-low) ---
static bool s_cb1 = false;
static int s_cb1_falls;

void via_input_c(via_t *via, int port, int c, bool value) {
    (void)via;
    ASSERT_EQ_INT(port, 1);
    ASSERT_EQ_INT(c, 0);
    if (s_cb1 && !value)
        s_cb1_falls++;
    s_cb1 = value;
}

// --- audio_out: record everything the producer pushes ---
static int16_t s_frames[65536];
static size_t s_nframes;
static int s_last_push_len;
static int s_vol;
static uint32_t s_open_rate;
static int s_open_channels;
static uint32_t s_set_rate;

void audio_out_open(uint32_t src_rate_hz, int channels) {
    s_open_rate = src_rate_hz;
    s_open_channels = channels;
}

void audio_out_push(const int16_t *frames, int nframes, int vol_0_7) {
    memcpy(&s_frames[s_nframes], frames, (size_t)nframes * sizeof(int16_t));
    s_nframes += (size_t)nframes;
    s_last_push_len = nframes;
    s_vol = vol_0_7;
}

void audio_out_set_rate(uint32_t src_rate_hz) {
    s_set_rate = src_rate_hz;
}

struct object *audio_out_capture_attach(struct object *parent) {
    (void)parent;
    return NULL;
}

void audio_out_capture_detach(void) {}

value_t audio_out_match_value(const char *golden_wav) {
    (void)golden_wav;
    value_t v;
    memset(&v, 0, sizeof(v));
    return v;
}

// --- object model / values: inert (object_new returns NULL → no attach) ---
struct object *object_new(const class_desc_t *cls, void *instance_data, const char *name) {
    (void)cls;
    (void)instance_data;
    (void)name;
    return NULL;
}
void object_delete(struct object *o) {
    (void)o;
}
void object_attach(struct object *parent, struct object *child) {
    (void)parent;
    (void)child;
}
void object_detach(struct object *child) {
    (void)child;
}
void *object_data(struct object *o) {
    (void)o;
    return NULL;
}
void object_set_label(struct object *o, const char *label) {
    (void)o;
    (void)label;
}
void object_set_order(struct object *o, int order) {
    (void)o;
    (void)order;
}
struct object *machine_object(void) {
    return NULL;
}
value_t val_uint(uint8_t width, uint64_t u) {
    (void)width;
    (void)u;
    value_t v;
    memset(&v, 0, sizeof(v));
    return v;
}

// --- memory map / checkpoint: unused paths (asc_init gets map == NULL) ---
void memory_map_add(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                    void *device) {
    (void)mem;
    (void)addr;
    (void)size;
    (void)name;
    (void)iface;
    (void)device;
}
void memory_map_remove(memory_map_t *mem, uint32_t addr, uint32_t size, const char *name, memory_interface_t *iface,
                       void *device) {
    (void)mem;
    (void)addr;
    (void)size;
    (void)name;
    (void)iface;
    (void)device;
}
// ============================================================================
// Harness helpers
// ============================================================================

static asc_t *g_asc;
static const memory_interface_t *g_if;

static uint8_t rd(uint32_t addr) {
    return g_if->read_uint8(g_asc, addr);
}
static void wr(uint32_t addr, uint8_t v) {
    g_if->write_uint8(g_asc, addr, v);
}

// Fresh chip wired to the stub scheduler + VIA, all recorders reset
static void fresh(void) {
    if (g_asc)
        asc_delete(g_asc);
    s_cb = NULL;
    s_cb_src = NULL;
    s_period_ns = 0;
    s_cancels = 0;
    s_cb1 = false;
    s_cb1_falls = 0;
    s_nframes = 0;
    s_last_push_len = 0;
    s_vol = -1;
    s_open_rate = 0;
    s_open_channels = 0;
    s_set_rate = 0;
    g_asc = asc_init(NULL, (scheduler_t *)0x1, NULL);
    ASSERT_TRUE(g_asc != NULL);
    asc_set_via(g_asc, (via_t *)0x1); // drives CB1 to its idle-HIGH state
    g_if = asc_get_memory_interface(g_asc);
    ASSERT_TRUE(g_if != NULL);
}

// ============================================================================
// 1. ROM POST vectors (USTNonCritTsts.a, standard ASC old silicon)
// ============================================================================

TEST(test_post_register_vectors) {
    // sndrnold / sndrdold: (register, writable-mask, data values). POST
    // writes each datum, delays, reads back through the mask, and requires
    // an exact match.
    static const uint8_t d_mode[] = {0, 1, 2, 0};
    static const uint8_t d_ctl[] = {0, 1, 2, 3, 0};
    static const uint8_t d_fifoctl[] = {0, 1, 2, 0x80, 3, 0x81, 0x82, 0x83, 0};
    static const uint8_t d_fifoint[] = {0, 1, 2, 4, 8, 0};
    static const uint8_t d_oneshot[] = {0, 0x80, 1, 2, 4, 8, 7, 0xb, 0xd, 0xe, 0xf, 0};
    static const uint8_t d_clock[] = {0, 2, 3, 0};
    static const struct {
        uint32_t reg;
        uint8_t mask;
        const uint8_t *data;
        int count;
    } vec[] = {
        {R_MODE,    0xFF, d_mode,    sizeof(d_mode)   },
        {R_CONTROL, 0x7F, d_ctl,     sizeof(d_ctl)    },
        {R_FIFOCTL, 0xFF, d_fifoctl, sizeof(d_fifoctl)},
        {R_FIFOINT, 0xFF, d_fifoint, sizeof(d_fifoint)},
        {R_ONESHOT, 0xFF, d_oneshot, sizeof(d_oneshot)},
        {R_CLOCK,   0xFF, d_clock,   sizeof(d_clock)  },
    };

    fresh();

    // POST's old/new-silicon probe: on the old part, volume bits 2-4 read
    // back as written (the discrete 344S0053 the SE/30/IIx/IIcx carry).
    wr(R_VOLUME, 0x1c);
    ASSERT_EQ_INT(rd(R_VOLUME) & 0x1c, 0x1c);

    // The version register identifies the standard ASC and is read-only
    ASSERT_EQ_INT(rd(R_VERSION), 0x00);
    wr(R_VERSION, 0x5A);
    ASSERT_EQ_INT(rd(R_VERSION), 0x00);

    for (size_t r = 0; r < sizeof(vec) / sizeof(vec[0]); r++) {
        for (int i = 0; i < vec[r].count; i++) {
            wr(vec[r].reg, vec[r].data[i]);
            uint8_t got = rd(vec[r].reg) & vec[r].mask;
            ASSERT_EQ_INT(got, vec[r].data[i]);
        }
    }
}

TEST(test_post_sram_patterns) {
    fresh();

    // POST FIFO-RAM test: mode 2 (wavetable), play mode, volume 0, then two
    // XOR patterns across the full 2 KB — SRAM must read back exactly.
    wr(R_MODE, 2);
    wr(0x80A, 0);
    wr(R_VOLUME, 0);
    static const uint8_t seeds[2] = {0x00, 0xFF};
    for (int p = 0; p < 2; p++) {
        for (int d4 = 0x7ff; d4 >= 0; d4--) {
            uint32_t a = (uint32_t)(0x7ff - d4);
            wr(a, (uint8_t)(seeds[p] ^ (d4 & 0xFF)));
        }
        for (int d4 = 0x7ff; d4 >= 0; d4--) {
            uint32_t a = (uint32_t)(0x7ff - d4);
            ASSERT_EQ_INT(rd(a), (uint8_t)(seeds[p] ^ (d4 & 0xFF)));
        }
    }
    wr(R_MODE, 0);
}

// ============================================================================
// 2. FIFO IRQ state machine
// ============================================================================

TEST(test_fifo_half_empty_latch_on_transition) {
    fresh();
    wr(R_MODE, 1); // FIFO mode: producer event scheduled
    ASSERT_TRUE(s_cb != NULL);
    ASSERT_TRUE(s_cb1); // CB1 idles high

    // Fill channel A to exactly the half threshold — no IRQ on the way up
    for (int i = 0; i < 512; i++)
        wr(0x000, (uint8_t)i);
    ASSERT_EQ_INT(s_cb1_falls, 0);
    ASSERT_EQ_INT(rd(R_FIFOINT), 0);

    // First drain tick crosses below 512 → half-empty latches, CB1 falls
    tick(1);
    ASSERT_EQ_INT(s_cb1_falls, 1);
    ASSERT_EQ_INT(rd(R_FIFOINT), 0x01); // A half-empty; read-clears
    ASSERT_TRUE(s_cb1); // read released CB1
    ASSERT_EQ_INT(rd(R_FIFOINT), 0); // cleared by the previous read

    // Draining the rest does not re-latch (fires on the crossing only)
    tick(511);
    ASSERT_EQ_INT(s_cb1_falls, 1);
    ASSERT_EQ_INT(rd(R_FIFOINT), 0);

    // Refill past the threshold and drain below again → a second latch
    for (int i = 0; i < 512; i++)
        wr(0x000, (uint8_t)i);
    tick(1);
    ASSERT_EQ_INT(s_cb1_falls, 2);
    ASSERT_EQ_INT(rd(R_FIFOINT), 0x01);
}

TEST(test_fifo_overflow_latch_and_write_through) {
    fresh();
    wr(R_MODE, 1);

    // Overfill channel B: byte 1025 overflows → full/overflow bit latches
    for (int i = 0; i < 1024; i++)
        wr(0x400, (uint8_t)i);
    ASSERT_EQ_INT(s_cb1_falls, 0);
    wr(0x400, 0xAA);
    ASSERT_EQ_INT(s_cb1_falls, 1);
    ASSERT_EQ_INT(rd(R_FIFOINT), 0x08); // B full; read-clears + releases CB1
    ASSERT_TRUE(s_cb1);

    // MacTest's diagnostic write-through: a write to ascFifoInt is readable
    // and drives the interrupt line ($0104 case)
    wr(R_FIFOINT, 0x04);
    ASSERT_EQ_INT(s_cb1_falls, 2);
    ASSERT_EQ_INT(rd(R_FIFOINT), 0x04);
    ASSERT_TRUE(s_cb1);
}

TEST(test_fifo_clear_strobe_and_hold_last_byte) {
    fresh();
    wr(R_MODE, 1);

    // Push a byte and drain it: the DAC holds the last byte on underflow
    wr(R_VOLUME, 0xE0); // volume 7
    wr(0x000, 0xC0);
    tick(2); // 1st consumes 0xC0, 2nd underflows → repeats it
    wr(R_MODE, 0); // flush the partial batch
    ASSERT_EQ_INT((int)s_nframes, 2);
    ASSERT_EQ_INT(s_frames[0], (0xC0 - 128) << 8);
    ASSERT_EQ_INT(s_frames[1], (0xC0 - 128) << 8);
    ASSERT_EQ_INT(s_vol, 7);

    // FIFO-clear strobe (bit 7 rising edge) resets fill counts: fill 600,
    // clear, fill 512 — the first tick after must cross the half threshold
    wr(R_MODE, 1);
    for (int i = 0; i < 600; i++)
        wr(0x000, 0x80);
    wr(R_FIFOCTL, 0x80);
    wr(R_FIFOCTL, 0x00);
    s_cb1_falls = 0;
    for (int i = 0; i < 512; i++)
        wr(0x000, 0x80);
    tick(1);
    ASSERT_EQ_INT(s_cb1_falls, 1);
    ASSERT_EQ_INT(rd(R_FIFOINT), 0x01);
}

// ============================================================================
// 3. The sample-rate producer
// ============================================================================

TEST(test_producer_open_batch_and_conversion) {
    fresh();

    // asc_init opened the shared stream: mono at the default 22,257 Hz
    ASSERT_EQ_INT((int)s_open_rate, 22257);
    ASSERT_EQ_INT(s_open_channels, 1);

    wr(R_MODE, 1);
    ASSERT_EQ_INT((int)s_period_ns, 44929);
    wr(R_VOLUME, 0x80); // volume 4

    // Fill A with a known ramp; mono routing (control bit 1 = 0) takes A
    wr(0x000, 0x00);
    wr(0x000, 0x80);
    wr(0x000, 0xFF);
    for (int i = 0; i < 61; i++)
        wr(0x000, 0x80);

    // 63 ticks: still below the batch size — nothing pushed yet
    tick(63);
    ASSERT_EQ_INT((int)s_nframes, 0);

    // 64th tick completes the batch → one push of 64 frames
    tick(1);
    ASSERT_EQ_INT((int)s_nframes, 64);
    ASSERT_EQ_INT(s_last_push_len, 64);
    ASSERT_EQ_INT(s_frames[0], -32768); // 0x00 → full negative
    ASSERT_EQ_INT(s_frames[1], 0); // 0x80 → offset-binary silence
    ASSERT_EQ_INT(s_frames[2], 32512); // 0xFF → (255-128)<<8
    ASSERT_EQ_INT(s_vol, 4);
}

TEST(test_producer_stereo_mix) {
    // FIFO stereo (control bit 1): A drives left, B right. The board mix
    // folds to the speaker: channel A on IIx/IIcx, the averaged analog
    // L+R mix on the SE/30.
    fresh();
    wr(R_MODE, 1);
    wr(R_CONTROL, 0x02);
    asc_set_mix(g_asc, ASC_MIX_CH_A);
    wr(0x000, 0x80); // A: silence
    wr(0x400, 0xFF); // B: full positive
    tick(1);
    wr(R_MODE, 0); // flush
    ASSERT_EQ_INT((int)s_nframes, 1);
    ASSERT_EQ_INT(s_frames[0], 0); // channel A only

    fresh();
    wr(R_MODE, 1);
    wr(R_CONTROL, 0x02);
    asc_set_mix(g_asc, ASC_MIX_SUM);
    wr(0x000, 0x80);
    wr(0x400, 0xFF);
    tick(1);
    wr(R_MODE, 0);
    ASSERT_EQ_INT((int)s_nframes, 1);
    ASSERT_EQ_INT(s_frames[0], 16256); // (0 + right) / 2
}

TEST(test_producer_wavetable_free_run) {
    // Wavetable voices free-run in mode 2 (no enable gate — the boot chime
    // never touches $805). Voices 0+1 sum to the left channel, 2+3 to the
    // right; each voice scales to half full-scale. Table bytes are
    // offset-binary (0x80 = zero), the same DAC convention as FIFO mode.
    fresh();
    wr(0x000, 0xC0); // voice 0 table[0] = +64
    wr(0x200, 0x80); // voice 1 table[0] = 0 (silence)
    wr(0x400, 0xFF); // voice 2 table[0] = +127
    wr(0x600, 0x80); // voice 3 table[0] = 0 (silence)
    // Zero increments keep every voice parked on table[0]

    asc_set_mix(g_asc, ASC_MIX_CH_A);
    wr(R_MODE, 2);
    tick(1);
    wr(R_MODE, 0);
    ASSERT_EQ_INT((int)s_nframes, 1);
    ASSERT_EQ_INT(s_frames[0], 64 << 7); // left pair only

    fresh();
    wr(0x000, 0xC0);
    wr(0x200, 0x80);
    wr(0x400, 0xFF);
    wr(0x600, 0x80);
    asc_set_mix(g_asc, ASC_MIX_SUM);
    wr(R_MODE, 2);
    tick(1);
    wr(R_MODE, 0);
    ASSERT_EQ_INT((int)s_nframes, 1);
    ASSERT_EQ_INT(s_frames[0], ((64 << 7) + (127 << 7)) / 2); // (L + R) / 2

    // Phase advance: increment of 1.0 (integer step) walks the table
    fresh();
    wr(0x000, 0x80 + 10);
    wr(0x001, 0x80 + 20);
    wr(0x002, 0x80 + 30);
    wr(0x200, 0x80); // voice 1 parked on silence
    // Voice 0 increment = 0x008000 (1.0 in 9.15 fixed point)
    wr(R_WAVE + 4, 0x00);
    wr(R_WAVE + 5, 0x00);
    wr(R_WAVE + 6, 0x80);
    wr(R_WAVE + 7, 0x00);
    asc_set_mix(g_asc, ASC_MIX_CH_A);
    wr(R_MODE, 2);
    tick(3);
    wr(R_MODE, 0);
    ASSERT_EQ_INT((int)s_nframes, 3);
    ASSERT_EQ_INT(s_frames[0], 10 << 7);
    ASSERT_EQ_INT(s_frames[1], 20 << 7);
    ASSERT_EQ_INT(s_frames[2], 30 << 7);
}

TEST(test_producer_rate_switch) {
    fresh();
    wr(R_MODE, 1);
    ASSERT_EQ_INT((int)s_period_ns, 44929);

    // ascClockRate = 3 → 44,100 Hz: host stream restarted, event retimed
    wr(R_CLOCK, 3);
    ASSERT_EQ_INT((int)s_set_rate, 44100);
    ASSERT_EQ_INT((int)s_period_ns, 22676);

    // = 2 → 22,050 Hz
    wr(R_CLOCK, 2);
    ASSERT_EQ_INT((int)s_set_rate, 22050);
    ASSERT_EQ_INT((int)s_period_ns, 45351);

    // back to the default
    wr(R_CLOCK, 0);
    ASSERT_EQ_INT((int)s_set_rate, 22257);
    ASSERT_EQ_INT((int)s_period_ns, 44929);
}

TEST(test_producer_stops_when_off) {
    fresh();
    wr(R_MODE, 1);
    ASSERT_TRUE(s_cb != NULL);
    tick(10);
    wr(R_MODE, 0); // cancels the event and flushes the partial batch
    ASSERT_TRUE(s_cb == NULL);
    ASSERT_EQ_INT((int)s_nframes, 10);
    ASSERT_TRUE(s_cancels >= 1);
}

// ============================================================================

int main(void) {
    RUN(test_post_register_vectors);
    RUN(test_post_sram_patterns);
    RUN(test_fifo_half_empty_latch_on_transition);
    RUN(test_fifo_overflow_latch_and_write_through);
    RUN(test_fifo_clear_strobe_and_hold_last_byte);
    RUN(test_producer_open_batch_and_conversion);
    RUN(test_producer_stereo_mix);
    RUN(test_producer_wavetable_free_run);
    RUN(test_producer_rate_switch);
    RUN(test_producer_stops_when_off);
    fprintf(stderr, "asc: all tests passed\n");
    return 0;
}
