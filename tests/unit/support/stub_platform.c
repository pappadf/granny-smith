// Platform stubs for unit tests
// Provides minimal implementations of platform-specific functions.

#include <stdint.h>
#include <sys/time.h>

// Platform helpers referenced by some subsystems.
int platform_bsr32(uint32_t value) {
    if (value == 0) return -1;
    int pos = 0;
    if (value & 0xFFFF0000) { pos += 16; value >>= 16; }
    if (value & 0x0000FF00) { pos += 8; value >>= 8; }
    if (value & 0x000000F0) { pos += 4; value >>= 4; }
    if (value & 0x0000000C) { pos += 2; value >>= 2; }
    if (value & 0x00000002) { pos += 1; }
    return pos;
}

int platform_ntz32(uint32_t mask) {
    if (mask == 0) return 32;
    int n = 0;
    if ((mask & 0x0000FFFF) == 0) { n += 16; mask >>= 16; }
    if ((mask & 0x000000FF) == 0) { n += 8; mask >>= 8; }
    if ((mask & 0x0000000F) == 0) { n += 4; mask >>= 4; }
    if ((mask & 0x00000003) == 0) { n += 2; mask >>= 2; }
    if ((mask & 0x00000001) == 0) { n += 1; }
    return n;
}

void platform_play_8bit_pwm(unsigned char *buf, int n, unsigned int vol) {
    (void)buf; (void)n; (void)vol;
}

void platform_init_sound(void) {}

// Emscripten timing stub
double emscripten_get_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

void emscripten_force_exit(int code) {
    (void)code;
}

// A-Trap name lookup stub (weak symbol so real implementation can override)
const char* __attribute__((weak)) macos_atrap_name(uint16_t trap) {
    (void)trap;
    return "ATRAP";
}
