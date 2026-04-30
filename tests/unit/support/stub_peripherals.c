// Peripheral stubs for unit tests
// Provides no-op implementations for floppy and network functions.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct floppy floppy_t;
typedef struct image image_t;
typedef struct memory_map memory_map_t;
typedef struct rtc rtc_t;

// Floppy subsystem stubs
floppy_t *floppy_new(memory_map_t *map) {
    (void)map;
    return NULL;
}

int floppy_insert(floppy_t *floppy, int drive, image_t *disk) {
    (void)floppy;
    (void)drive;
    (void)disk;
    return 0;
}

bool floppy_is_inserted(floppy_t *floppy, int drive) {
    (void)floppy;
    (void)drive;
    return false;
}

void floppy_set_sel_signal(floppy_t *floppy, bool sel) {
    (void)floppy;
    (void)sel;
}

// Network packet processing stub
void process_packet(uint8_t *buf, size_t size) {
    (void)buf;
    (void)size;
}

// RTC stub for cmd_set_time (debug_mac.c) — no-op since system_rtc() returns NULL
void rtc_set_seconds(rtc_t *rtc, uint32_t mac_seconds) {
    (void)rtc;
    (void)mac_seconds;
}
