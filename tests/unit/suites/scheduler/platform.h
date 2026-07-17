#ifndef PLATFORM_H
#define PLATFORM_H
// Platform override for the scheduler unit suite. Identical in spirit to
// ../../support/platform.h, except host_time() is an *extern* function the
// test implements over a controllable fake clock — the pacing estimators
// (host_secs_per_vbl / host_secs_per_loop) are meaningless with the support
// header's constant-zero inline.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct platform platform_t; // opaque

// Activity enums (subset) to satisfy references if any
enum activity { activity_off = 0, activity_idle = 1, activity_read = 2, activity_write = 3 };
enum devices { device_floppy_0 = 0, device_floppy_1 = 1 };

static inline void platform_refresh_screen(platform_t *p, unsigned char *buf) {
    (void)p;
    (void)buf;
}
static inline void platform_verify(platform_t *p) {
    (void)p;
}
static inline void js_activity_indicator(int device, int activity) {
    (void)device;
    (void)activity;
}
static inline void js_console_log(const char *msg) {
    (void)msg;
}
static inline void js_play_audio(uint8_t *samples, int num_samples, unsigned int volume) {
    (void)samples;
    (void)num_samples;
    (void)volume;
}
static inline void activity_indicator(int device, int activity) {
    (void)device;
    (void)activity;
}

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

static inline uint64_t platform_ticks(void) {
    return 0;
}

// Controllable fake host clock, implemented in test.c
double host_time(void);

// Controllable fake audio-ring fill signal (governor tests), implemented in
// test.c; < 0 = no signal, matching the real platform contract.
double platform_audio_ring_fill(void);

#endif // PLATFORM_H
