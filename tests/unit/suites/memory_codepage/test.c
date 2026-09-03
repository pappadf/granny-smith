// Code-page coherence unit tests (proposal-predecoded-interpreter-cores.md
// Phase A, §3.6).  A predecoded block makes its host page a "code page":
// the page's WRITE SoA entries must vanish for every logical alias, stay
// vanished across every refill site, and every store into the page — guest
// or host-side — must reach the invalidation hook before it lands.

#include "memory.h"
#include "mmu.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Hook capture
// ============================================================================

static const uint8_t *hook_host;
static uint32_t hook_len;
static int hook_calls;

static void capture_hook(const uint8_t *host, uint32_t len) {
    hook_host = host;
    hook_len = len;
    hook_calls++;
}

static void reset_hook(void) {
    hook_host = NULL;
    hook_len = 0;
    hook_calls = 0;
    g_mem_code_written_hook = capture_hook;
}

// Plus layout: 4 MB RAM at 0 (mirrored up to the ROM), 128 KB ROM at $400000.
static memory_map_t *make_plus(void) {
    memory_map_t *mem = memory_map_init(24, 0x400000, 0x020000, NULL);
    memory_populate_pages(mem, 0x400000, 0x580000);
    memory_populate_ram_mirror(mem, 0x400000 - 0x400000 + 0x100000 * 0, 0); // no-op range
    return mem;
}

// Host pointer of guest RAM page `page`.
static uint8_t *ram_page(memory_map_t *mem, uint32_t page) {
    return ram_native_pointer(mem, page << PAGE_SHIFT);
}

// ============================================================================
// Tests
// ============================================================================

// A fresh map registers its image as region 0 and bumps the generation.
TEST(test_region_and_generation) {
    uint32_t gen0 = g_mem_map_generation;
    memory_map_t *mem = make_plus();
    ASSERT_TRUE(g_mem_map_generation != gen0);
    ASSERT_EQ_INT(1, g_mem_code_region_count);
    uint32_t page;
    ASSERT_EQ_INT(0, memory_code_region_of(ram_page(mem, 3), &page));
    ASSERT_EQ_INT(3, (int)page);
    // ROM bytes are in the image too (after RAM).
    ASSERT_EQ_INT(0, memory_code_region_of(memory_rom_bytes(mem), &page));
    ASSERT_EQ_INT(0x400, (int)page);
    // Something outside the image is nobody's code region.
    uint8_t outside[16];
    ASSERT_EQ_INT(-1, memory_code_region_of(outside, &page));
    ASSERT_TRUE(!memory_host_is_code(ram_page(mem, 3)));
    memory_map_delete(mem);
    ASSERT_EQ_INT(0, g_mem_code_region_count);
}

// Marking zeroes the write entries of every alias and leaves reads alone.
TEST(test_mark_suppresses_writes) {
    memory_map_t *mem = make_plus();
    uint8_t *host = ram_page(mem, 5);
    ASSERT_TRUE(g_supervisor_write[5] != 0);
    ASSERT_TRUE(g_user_write[5] != 0);
    memory_code_page_mark(host);
    ASSERT_TRUE(memory_host_is_code(host));
    ASSERT_TRUE(memory_host_is_code(host + 100)); // any pointer into the page
    ASSERT_EQ_INT(0, (int)g_supervisor_write[5]);
    ASSERT_EQ_INT(0, (int)g_user_write[5]);
    ASSERT_TRUE(g_supervisor_read[5] != 0);
    ASSERT_TRUE(g_user_read[5] != 0);
    // Neighbours untouched.
    ASSERT_TRUE(g_supervisor_write[4] != 0);
    ASSERT_TRUE(g_supervisor_write[6] != 0);
    memory_map_delete(mem);
}

// A guest store into a marked page takes the slow path, reaches the hook
// with the exact host bytes, lands, and does not re-plant the write entry.
TEST(test_guest_store_invalidates) {
    memory_map_t *mem = make_plus();
    reset_hook();
    uint8_t *host = ram_page(mem, 5);
    memory_code_page_mark(host);
    g_active_read = g_supervisor_read;
    g_active_write = g_supervisor_write;
    memory_write_uint16(0x5010, 0x4E71);
    ASSERT_EQ_INT(1, hook_calls);
    ASSERT_TRUE(hook_host == host + 0x10);
    ASSERT_EQ_INT(2, (int)hook_len);
    ASSERT_EQ_INT(0x4E71, memory_read_uint16(0x5010)); // the store landed
    ASSERT_EQ_INT(0, (int)g_supervisor_write[5]); // lazy install refused the write entry
    ASSERT_TRUE(g_supervisor_read[5] != 0);
    ASSERT_EQ_INT(1, (int)g_mem_code_write_count);
    // Byte and long stores too.
    memory_write_uint8(0x5FFF, 0xAA);
    ASSERT_EQ_INT(2, hook_calls);
    ASSERT_EQ_INT(1, (int)hook_len);
    memory_write_uint32(0x5100, 0x12345678);
    ASSERT_EQ_INT(3, hook_calls);
    ASSERT_EQ_INT(4, (int)hook_len);
    ASSERT_EQ_INT((int)0x12345678, (int)memory_read_uint32(0x5100));
    // A store to an unmarked page never reaches the hook.
    memory_write_uint32(0x6100, 1);
    ASSERT_EQ_INT(3, hook_calls);
    memory_map_delete(mem);
}

// Unmarking lets the write entry refill lazily on the next store.
TEST(test_unmark_refills) {
    memory_map_t *mem = make_plus();
    reset_hook();
    uint8_t *host = ram_page(mem, 7);
    memory_code_page_mark(host);
    g_active_read = g_supervisor_read;
    g_active_write = g_supervisor_write;
    memory_code_page_unmark(host);
    ASSERT_TRUE(!memory_host_is_code(host));
    ASSERT_EQ_INT(0, (int)g_supervisor_write[7]); // still zero until a store
    memory_write_uint16(0x7000, 1);
    ASSERT_EQ_INT(0, hook_calls); // not a code page any more
    ASSERT_TRUE(g_supervisor_write[7] != 0); // lazily re-planted
    memory_map_delete(mem);
}

// A host-side writer over a marked range reaches the hook once per page,
// with the in-page span; unmarked pages are skipped.
TEST(test_host_written) {
    memory_map_t *mem = make_plus();
    reset_hook();
    uint8_t *p8 = ram_page(mem, 8);
    memory_code_page_mark(p8);
    memory_host_written(p8 + 0xFF0, 0x40); // crosses into page 9 (unmarked)
    ASSERT_EQ_INT(1, hook_calls);
    ASSERT_TRUE(hook_host == p8 + 0xFF0);
    ASSERT_EQ_INT(0x10, (int)hook_len);
    memory_code_page_mark(ram_page(mem, 9));
    memory_host_written(p8 + 0xFF0, 0x40);
    ASSERT_EQ_INT(3, hook_calls);
    ASSERT_TRUE(hook_host == ram_page(mem, 9));
    ASSERT_EQ_INT(0x30, (int)hook_len);
    memory_host_written(ram_page(mem, 20), 4096); // unmarked: silent
    ASSERT_EQ_INT(3, hook_calls);
    memory_map_delete(mem);
}

// The debug poke path (memory.poke) reports through memory_host_written.
TEST(test_debug_write_reports) {
    memory_map_t *mem = make_plus();
    reset_hook();
    memory_code_page_mark(ram_page(mem, 2));
    g_active_write = g_supervisor_write;
    ASSERT_TRUE(memory_debug_write_uint16(0x2004, 0x1234));
    ASSERT_EQ_INT(1, hook_calls);
    ASSERT_TRUE(hook_host == ram_page(mem, 2) + 4);
    memory_map_delete(mem);
}

// Aliases: the Plus mirrors RAM under the ROM; a mark through one alias
// suppresses the write entry of the other, and a store through the alias
// invalidates the same host bytes.
TEST(test_alias_mirror) {
    memory_map_t *mem = memory_map_init(24, 0x100000, 0x020000, NULL); // 1 MB RAM
    memory_populate_pages(mem, 0x400000, 0x580000);
    memory_populate_ram_mirror(mem, 0x100000, 0x400000); // 3 aliases of RAM
    reset_hook();
    uint32_t page = 0x30; // $030000
    uint32_t alias = 0x130; // $130000 → same host bytes
    ASSERT_TRUE(g_page_table[alias].host_base == g_page_table[page].host_base);
    ASSERT_TRUE(g_supervisor_write[alias] != 0);
    memory_code_page_mark(g_page_table[page].host_base);
    ASSERT_EQ_INT(0, (int)g_supervisor_write[page]);
    ASSERT_EQ_INT(0, (int)g_supervisor_write[alias]);
    ASSERT_EQ_INT(0, (int)g_user_write[alias]);
    g_active_read = g_supervisor_read;
    g_active_write = g_supervisor_write;
    memory_write_uint16(0x130020, 0x6000);
    ASSERT_EQ_INT(1, hook_calls);
    ASSERT_TRUE(hook_host == g_page_table[page].host_base + 0x20);
    ASSERT_EQ_INT(0x6000, memory_read_uint16(0x030020)); // visible through the primary
    ASSERT_EQ_INT(0, (int)g_supervisor_write[alias]); // still suppressed
    memory_map_delete(mem);
}

// Refill sites: the 68040/030 shared fill declines a write entry for a
// marked physical page (and plants the read entry), and rebuild after a
// logpoint uninstall declines too.
TEST(test_refill_sites_honour_mark) {
    memory_map_t *mem = memory_map_init(32, 0x400000, 0x080000, NULL);
    memory_populate_pages(mem, 0x40800000, 0x40900000);
    uint8_t *ram = ram_native_pointer(mem, 0);
    mmu_state_t *mmu =
        mmu_init(ram, 0x400000, 0x40000000, (uint8_t *)memory_rom_bytes(mem), 0x080000, 0x40800000, 0x40900000);
    ASSERT_TRUE(mmu != NULL);
    g_mmu = mmu;
    // Logical page $1000 → physical page $3 (a plain RAM page).
    memory_code_page_mark(ram + 3 * MEM_PAGE_SIZE);
    mmu_fill_soa_page(mmu, 0x1000000, 0x3000, true, true, true);
    ASSERT_TRUE(g_supervisor_read[0x1000] != 0);
    ASSERT_EQ_INT(0, (int)g_supervisor_write[0x1000]);
    ASSERT_TRUE(g_user_read[0x1000] != 0);
    ASSERT_EQ_INT(0, (int)g_user_write[0x1000]);
    // An unmarked physical page fills both.
    mmu_fill_soa_page(mmu, 0x1001000, 0x4000, true, true, true);
    ASSERT_TRUE(g_supervisor_write[0x1001] != 0);
    // Unmark → the next fill plants the write entry again.
    memory_code_page_unmark(ram + 3 * MEM_PAGE_SIZE);
    mmu_fill_soa_page(mmu, 0x1000000, 0x3000, true, true, true);
    ASSERT_TRUE(g_supervisor_write[0x1000] != 0);
    // Identity rebuild after a logpoint round trip (MMU off) declines too.
    mmu->enabled = false;
    memory_code_page_mark(ram + 9 * MEM_PAGE_SIZE);
    memory_logpoint_install(9, 9);
    memory_logpoint_uninstall(9, 9); // rebuild_soa_page(9)
    ASSERT_TRUE(g_supervisor_read[9] != 0);
    ASSERT_EQ_INT(0, (int)g_supervisor_write[9]);
    g_mmu = NULL;
    mmu_delete(mmu);
    memory_map_delete(mem);
}

// The MMU-enabled slow write path completes a store to a marked page
// through the physical host pointer (the write entry stays suppressed).
TEST(test_mmu_slow_write_lands) {
    memory_map_t *mem = memory_map_init(32, 0x400000, 0x080000, NULL);
    memory_populate_pages(mem, 0x40800000, 0x40900000);
    uint8_t *ram = ram_native_pointer(mem, 0);
    mmu_state_t *mmu =
        mmu_init(ram, 0x400000, 0x40000000, (uint8_t *)memory_rom_bytes(mem), 0x080000, 0x40800000, 0x40900000);
    g_mmu = mmu;
    // Transparent translation of the whole space (TT0: all addresses, any FC).
    mmu->tt0 = 0x00FF8043u;
    mmu->enabled = true;
    g_active_read = g_supervisor_read;
    g_active_write = g_supervisor_write;
    reset_hook();
    memory_code_page_mark(ram + 0x20 * MEM_PAGE_SIZE);
    memory_write_uint32(0x20010, 0xCAFEBABE);
    ASSERT_EQ_INT(1, hook_calls);
    ASSERT_TRUE(hook_host == ram + 0x20 * MEM_PAGE_SIZE + 0x10);
    ASSERT_EQ_INT((int)0xCAFEBABE, (int)LOAD_BE32(ram + 0x20010));
    ASSERT_EQ_INT(0, (int)g_supervisor_write[0x20]);
    mmu->enabled = false;
    g_mmu = NULL;
    mmu_delete(mmu);
    memory_map_delete(mem);
}

// A second registered region (a unit-test buffer) gets its own marks.
TEST(test_extra_region) {
    memory_map_t *mem = make_plus();
    uint8_t *buf = calloc(8 * MEM_PAGE_SIZE + MEM_PAGE_SIZE, 1);
    uint8_t *base = (uint8_t *)(((uintptr_t)buf + MEM_PAGE_SIZE - 1) & ~(uintptr_t)PAGE_MASK);
    int r = memory_code_region_register(base, 8 * MEM_PAGE_SIZE);
    ASSERT_EQ_INT(1, r);
    uint32_t page;
    ASSERT_EQ_INT(1, memory_code_region_of(base + 2 * MEM_PAGE_SIZE + 5, &page));
    ASSERT_EQ_INT(2, (int)page);
    memory_code_page_mark(base + 2 * MEM_PAGE_SIZE);
    ASSERT_TRUE(memory_host_is_code(base + 2 * MEM_PAGE_SIZE + 7));
    ASSERT_TRUE(!memory_host_is_code(ram_page(mem, 2)));
    memory_map_delete(mem);
    ASSERT_EQ_INT(0, g_mem_code_region_count);
    free(buf);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    RUN(test_region_and_generation);
    RUN(test_mark_suppresses_writes);
    RUN(test_guest_store_invalidates);
    RUN(test_unmark_refills);
    RUN(test_host_written);
    RUN(test_debug_write_reports);
    RUN(test_alias_mirror);
    RUN(test_refill_sites_honour_mark);
    RUN(test_mmu_slow_write_lands);
    RUN(test_extra_region);
    printf("[PASS] All memory_codepage tests passed\n");
    return 0;
}
