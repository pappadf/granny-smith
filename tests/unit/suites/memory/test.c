// Memory subsystem unit tests (M2)
// Verifies parameterised memory_map_init() for 24-bit and 32-bit address spaces,
// correct page table allocation, and page population for RAM and ROM regions.

#include "memory.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// ============================================================================
// Helpers
// ============================================================================

// Free a memory map and clear global state
static void cleanup(memory_map_t *mem) {
    if (mem)
        memory_map_delete(mem);
}

// ============================================================================
// Tests
// ============================================================================

// Verify 24-bit init produces correct address mask and page count
TEST(test_24bit_init) {
    memory_map_t *mem = memory_map_init(24, 0x400000, 0x020000, NULL);
    ASSERT_TRUE(mem != NULL);
    ASSERT_EQ_INT((int)0x00FFFFFFUL, (int)g_address_mask);
    ASSERT_EQ_INT(1 << (24 - 12), g_page_count); // 4096 pages
    ASSERT_TRUE(g_page_table != NULL);
    cleanup(mem);
}

// Verify 32-bit init produces correct address mask and page count
TEST(test_32bit_init) {
    memory_map_t *mem = memory_map_init(32, 0x400000, 0x020000, NULL);
    ASSERT_TRUE(mem != NULL);
    ASSERT_EQ_INT((int)0xFFFFFFFFUL, (int)g_address_mask);
    // 1M pages for 32-bit space (2^20)
    ASSERT_EQ_INT(1 << (32 - 12), g_page_count);
    ASSERT_TRUE(g_page_table != NULL);
    cleanup(mem);
}

// Verify RAM pages are writable and point into the flat buffer
TEST(test_24bit_ram_pages) {
    memory_map_t *mem = memory_map_init(24, 0x400000, 0x020000, NULL);
    ASSERT_TRUE(mem != NULL);

    // Populate the Plus memory layout
    memory_populate_pages(mem, 0x400000, 0x580000);

    // First RAM page (addr 0x000000)
    page_entry_t *p0 = &g_page_table[0x000000 >> 12];
    ASSERT_TRUE(p0->host_base != NULL);
    ASSERT_TRUE(p0->writable);
    ASSERT_TRUE(p0->dev == NULL);

    // Last RAM page (addr 0x3FF000)
    page_entry_t *p_last_ram = &g_page_table[0x3FF000 >> 12];
    ASSERT_TRUE(p_last_ram->host_base != NULL);
    ASSERT_TRUE(p_last_ram->writable);

    // RAM page at 0x001000 should follow the first by one page worth of bytes
    page_entry_t *p1 = &g_page_table[0x001000 >> 12];
    ASSERT_EQ_INT((int)(p1->host_base - p0->host_base), 0x1000);

    cleanup(mem);
}

// Verify ROM pages are read-only and correctly mirrored in the page table
TEST(test_24bit_rom_pages) {
    memory_map_t *mem = memory_map_init(24, 0x400000, 0x020000, NULL);
    ASSERT_TRUE(mem != NULL);

    memory_populate_pages(mem, 0x400000, 0x580000);

    // Primary ROM page (addr 0x400000)
    page_entry_t *p_rom0 = &g_page_table[0x400000 >> 12];
    ASSERT_TRUE(p_rom0->host_base != NULL);
    ASSERT_TRUE(!p_rom0->writable);
    ASSERT_TRUE(p_rom0->dev == NULL);

    // ROM mirror at 0x440000 should map to the same host data as 0x400000
    page_entry_t *p_mirror = &g_page_table[0x440000 >> 12];
    ASSERT_TRUE(p_mirror->host_base != NULL);
    ASSERT_TRUE(!p_mirror->writable);
    ASSERT_EQ_INT((int)(p_mirror->host_base - p_rom0->host_base), 0);

    // ROM page inside primary region: 0x401000 → one page into ROM
    page_entry_t *p_rom1 = &g_page_table[0x401000 >> 12];
    ASSERT_EQ_INT((int)(p_rom1->host_base - p_rom0->host_base), 0x1000);

    // Interleaved I/O range (A17=1 relative to ROM base) must be unmapped, not ROM
    page_entry_t *p_io = &g_page_table[0x420000 >> 12];
    ASSERT_TRUE(p_io->host_base == NULL);
    ASSERT_TRUE(p_io->dev == NULL);

    cleanup(mem);
}

// Verify delete clears globals
TEST(test_delete_clears_globals) {
    memory_map_t *mem = memory_map_init(24, 0x400000, 0x020000, NULL);
    ASSERT_TRUE(mem != NULL);
    ASSERT_TRUE(g_page_table != NULL);

    memory_map_delete(mem);

    ASSERT_TRUE(g_page_table == NULL);
    ASSERT_EQ_INT(0, g_page_count);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    RUN(test_24bit_init);
    RUN(test_32bit_init);
    RUN(test_24bit_ram_pages);
    RUN(test_24bit_rom_pages);
    RUN(test_delete_clears_globals);
    printf("[PASS] All memory tests passed\n");
    return 0;
}
