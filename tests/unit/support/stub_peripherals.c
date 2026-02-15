// Peripheral stubs for unit tests
// Provides no-op implementations for floppy and network functions.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Forward declarations
typedef struct floppy floppy_t;
typedef struct image image_t;
typedef struct memory_map memory_map_t;

// Floppy subsystem stubs
floppy_t* floppy_new(memory_map_t *map) {
    (void)map;
    return NULL;
}

int floppy_insert(floppy_t *floppy, int drive, image_t *disk) {
    (void)floppy; (void)drive; (void)disk;
    return 0;
}

bool floppy_is_inserted(floppy_t *floppy, int drive) {
    (void)floppy; (void)drive;
    return false;
}

void floppy_set_sel_signal(floppy_t *floppy, bool sel) {
    (void)floppy; (void)sel;
}

// Network packet processing stub
void process_packet(uint8_t *buf, size_t size) {
    (void)buf; (void)size;
}
