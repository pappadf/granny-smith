// Memory stubs for isolated unit tests
// Provides minimal no-op memory implementations for tests not using real memory.

#include "memory.h"
#include <stdint.h>

// Sound offset symbol referenced by inline functions in sound.h
int snd_offset = 0;

// Static stub page table so inline accessors in memory.h have a valid backing store
static page_entry_t stub_page_table[1 << 16]; // zero-initialized for safe no-op access
page_entry_t *g_page_table = stub_page_table;
uint32_t g_address_mask = 0x00FFFFFFUL;
int g_page_count = 1 << 16;

// Memory read/write stubs (return zeros, ignore writes)
uint32_t memory_read(unsigned int size, uint32_t addr) {
    (void)size;
    (void)addr;
    return 0;
}

void memory_write(unsigned int size, uint32_t addr, uint32_t value) {
    (void)size;
    (void)addr;
    (void)value;
}

uint16_t memory_read_uint16_slow(uint32_t addr) {
    (void)addr;
    return 0;
}

uint32_t memory_read_uint32_slow(uint32_t addr) {
    (void)addr;
    return 0;
}

void memory_write_uint16_slow(uint32_t addr, uint16_t value) {
    (void)addr;
    (void)value;
}

void memory_write_uint32_slow(uint32_t addr, uint32_t value) {
    (void)addr;
    (void)value;
}
