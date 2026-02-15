#ifndef PLATFORM_H
#define PLATFORM_H
// Lightweight platform.h override for native unit tests (non-Emscripten)
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct platform platform_t; // opaque

// Activity enums (subset) to satisfy references if any
enum activity { activity_off = 0, activity_idle = 1, activity_read = 2, activity_write = 3 };
enum devices { device_floppy_0 = 0, device_floppy_1 = 1 };

// Stubs (no-op implementations used by code under test)
static inline void platform_refresh_screen(platform_t* p, unsigned char* buf) { (void)p; (void)buf; }
static inline void platform_verify(platform_t* p) { (void)p; }

static inline void js_activity_indicator(int device, int activity) { (void)device; (void)activity; }
static inline void js_console_log(const char* msg) { (void)msg; }
static inline void js_play_audio(uint8_t* samples, int num_samples, unsigned int volume) { (void)samples; (void)num_samples; (void)volume; }
static inline void activity_indicator(int device, int activity) { (void)device; (void)activity; }

// Bit helpers (reuse minimal needed)
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

// Byte-swap helpers to match originals (used by some codepaths)
#ifndef BE16
#define BE16(x) ((((x) & 0xff00) >> 8) | (((x) & 0x00ff) << 8))
#endif
#ifndef BE32
#define BE32(x) ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >> 8) | \
				 (((x) & 0x0000ff00) << 8) | (((x) & 0x000000ff) << 24))
#endif


static inline uint16_t reverse16(uint16_t x)
{
    x = (x & 0x5555) << 1 | (x & 0xAAAA) >> 1;
    x = (x & 0x3333) << 2 | (x & 0xCCCC) >> 2;
    x = (x & 0x0F0F) << 4 | (x & 0xF0F0) >> 4;
    x = (x & 0x00FF) << 8 | (x & 0xFF00) >> 8;
    return x;
}

static inline uint64_t platform_ticks(void) { return 0; }
static inline double host_time(void) { return 0.0; }

#endif // PLATFORM_H
