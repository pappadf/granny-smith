// Platform stubs for unit tests
// Provides minimal implementations of platform-specific functions.

#include <stdint.h>
#include <sys/time.h>

// platform_bsr32 / platform_ntz32 come from the shared platform.h now.

void platform_play_8bit_pwm(unsigned char *buf, int n, unsigned int vol) {
    (void)buf;
    (void)n;
    (void)vol;
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
const char *__attribute__((weak)) macos_atrap_name(uint16_t trap) {
    (void)trap;
    return "ATRAP";
}
