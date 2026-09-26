// The unit tier's clock (see platform.h beside this file): always zero.
#ifndef PLATFORM_CLOCK_H
#define PLATFORM_CLOCK_H
static inline double host_time(void) {
    return 0.0;
}
static inline double host_time_ms(void) {
    return 0.0;
}
#endif // PLATFORM_CLOCK_H
