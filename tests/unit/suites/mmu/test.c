// MMU unit tests (Milestone 7)
// Verifies 68030 PMMU: SoA fast-path arrays, table walk, TLB invalidation,
// transparent translation, PTEST, write protection, and supervisor-only pages.

#include "memory.h"
#include "mmu.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

// Free a memory map and MMU state
static void cleanup(memory_map_t *mem, mmu_state_t *mmu) {
    if (mmu) {
        g_mmu = NULL;
        mmu_delete(mmu);
    }
    if (mem)
        memory_map_delete(mem);
}

// Store a 32-bit big-endian value into a buffer
static void store_be32(uint8_t *p, uint32_t val) {
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)(val);
}

// ============================================================================
// Test: SoA arrays are populated for RAM/ROM (identity mapping, MMU disabled)
// ============================================================================

TEST(test_soa_identity_mapping) {
    // Create 32-bit address space with 4MB RAM, 256KB ROM
    memory_map_t *mem = memory_map_init(32, 0x400000, 0x040000, NULL);
    ASSERT_TRUE(mem != NULL);
    ASSERT_TRUE(g_supervisor_read != NULL);
    ASSERT_TRUE(g_supervisor_write != NULL);
    ASSERT_TRUE(g_user_read != NULL);
    ASSERT_TRUE(g_user_write != NULL);
    ASSERT_TRUE(g_active_read != NULL);
    ASSERT_TRUE(g_active_write != NULL);

    // Populate pages: RAM at 0, ROM at 0x40000000
    memory_populate_pages(mem, 0x40000000, 0x40080000);

    // RAM page 0: should have non-zero entries in all 4 SoA arrays
    ASSERT_TRUE(g_supervisor_read[0] != 0);
    ASSERT_TRUE(g_supervisor_write[0] != 0);
    ASSERT_TRUE(g_user_read[0] != 0);
    ASSERT_TRUE(g_user_write[0] != 0);

    // RAM page 1 (0x1000): should also be populated
    ASSERT_TRUE(g_supervisor_read[1] != 0);

    // ROM page at 0x40000000: read-only (read entries non-zero, write entries zero)
    uint32_t rom_page = 0x40000000 >> PAGE_SHIFT;
    ASSERT_TRUE(g_supervisor_read[rom_page] != 0);
    ASSERT_TRUE(g_supervisor_write[rom_page] == 0); // ROM is read-only
    ASSERT_TRUE(g_user_read[rom_page] != 0);
    ASSERT_TRUE(g_user_write[rom_page] == 0);

    // Unmapped page (e.g., 0x80000000): should have zero entries
    uint32_t unmapped_page = 0x80000000 >> PAGE_SHIFT;
    ASSERT_TRUE(g_supervisor_read[unmapped_page] == 0);
    ASSERT_TRUE(g_supervisor_write[unmapped_page] == 0);

    // Verify read/write through SoA fast path
    // Active pointers should be set to supervisor arrays by default
    ASSERT_TRUE(g_active_read == g_supervisor_read);
    ASSERT_TRUE(g_active_write == g_supervisor_write);

    // Write and read back through the fast path
    memory_write_uint8(0x1000, 0x42);
    ASSERT_EQ_INT(0x42, memory_read_uint8(0x1000));

    memory_write_uint16(0x2000, 0x1234);
    ASSERT_EQ_INT(0x1234, memory_read_uint16(0x2000));

    memory_write_uint32(0x3000, 0xDEADBEEF);
    ASSERT_EQ_INT((int)0xDEADBEEF, (int)memory_read_uint32(0x3000));

    cleanup(mem, NULL);
}

// ============================================================================
// Test: MMU init/delete
// ============================================================================

TEST(test_mmu_init_delete) {
    memory_map_t *mem = memory_map_init(32, 0x400000, 0x040000, NULL);
    uint8_t *ram = ram_native_pointer(mem, 0);

    mmu_state_t *mmu = mmu_init(ram, 0x400000, ram + 0x400000, 0x040000, 0x40000000);
    ASSERT_TRUE(mmu != NULL);
    ASSERT_TRUE(!mmu->enabled);
    ASSERT_EQ_INT(0, (int)mmu->tc);
    ASSERT_EQ_INT(0, (int)mmu->tt0);
    ASSERT_EQ_INT(0, (int)mmu->tt1);

    cleanup(mem, mmu);
}

// ============================================================================
// Test: TLB invalidation zeros all SoA arrays
// ============================================================================

TEST(test_tlb_invalidation) {
    memory_map_t *mem = memory_map_init(32, 0x400000, 0x040000, NULL);
    memory_populate_pages(mem, 0x40000000, 0x40080000);

    // Verify some entries are non-zero before invalidation
    ASSERT_TRUE(g_supervisor_read[0] != 0);
    ASSERT_TRUE(g_supervisor_write[0] != 0);

    mmu_state_t *mmu = mmu_init(ram_native_pointer(mem, 0), 0x400000, NULL, 0, 0);
    mmu_invalidate_tlb(mmu);

    // After invalidation, all entries should be zero
    ASSERT_TRUE(g_supervisor_read[0] == 0);
    ASSERT_TRUE(g_supervisor_write[0] == 0);
    ASSERT_TRUE(g_user_read[0] == 0);
    ASSERT_TRUE(g_user_write[0] == 0);

    cleanup(mem, mmu);
}

// ============================================================================
// Test: Simple two-level translation table walk
// ============================================================================

TEST(test_two_level_translation) {
    // Set up a 32-bit address space with 4MB RAM
    memory_map_t *mem = memory_map_init(32, 0x400000, 0x040000, NULL);
    memory_populate_pages(mem, 0x40000000, 0x40080000);

    uint8_t *ram = ram_native_pointer(mem, 0);
    mmu_state_t *mmu = mmu_init(ram, 0x400000, ram + 0x400000, 0x040000, 0x40000000);

    // Build a simple two-level page table in RAM:
    // TC: IS=0, PS=4 (4KB pages), TIA=8, TIB=12, TIC=0, TID=0
    // This means: 8 bits for level-A index, 12 bits for level-B index, 12 bits page offset
    // Equation: 0 + (4+8) + 8 + 12 + 0 + 0 = 32 ✓
    uint32_t tc = (1u << 31) // E=1 (enable)
                  | (4u << 20) // PS=4 (4KB pages)
                  | (0u << 16) // IS=0
                  | (8u << 12) // TIA=8
                  | (12u << 8) // TIB=12
                  | (0u << 4) // TIC=0
                  | (0u << 0); // TID=0

    // Level-A table at physical 0x10000 (256 entries × 4 bytes = 1KB)
    uint32_t level_a_base = 0x10000;

    // Level-B table at physical 0x11000 (4096 entries × 4 bytes = 16KB)
    uint32_t level_b_base = 0x11000;

    // Set up CRP: DT=2 (short table descriptor), address=level_a_base
    // CRP is 64-bit: upper 32 bits = flags (DT in bits 1:0), lower 32 bits = address
    uint64_t crp = ((uint64_t)DESC_DT_TABLE4 << 32) | level_a_base;

    // Level-A entry 0: points to level-B table (DT=2, address=level_b_base)
    store_be32(ram + level_a_base, level_b_base | DESC_DT_TABLE4);

    // Level-B entry 0: page descriptor mapping logical 0x00000000 → physical 0x00000000
    // DT=1 (page descriptor), address=0x00000000
    store_be32(ram + level_b_base, 0x00000000 | DESC_DT_PAGE);

    // Level-B entry 1: page descriptor mapping logical 0x00001000 → physical 0x00002000
    store_be32(ram + level_b_base + 4, 0x00002000 | DESC_DT_PAGE);

    // Configure the MMU
    mmu->tc = tc;
    mmu->crp = crp;
    mmu->enabled = true;

    // Invalidate TLB to start fresh
    mmu_invalidate_tlb(mmu);

    // Test table walk for logical address 0x00000000 → physical 0x00000000
    // We'll use mmu_handle_fault which does the walk and fills SoA
    bool ok = mmu_handle_fault(mmu, 0x00000000, false, true);
    ASSERT_TRUE(ok);

    // Verify SoA entry was populated for page 0
    ASSERT_TRUE(g_supervisor_read[0] != 0);

    // Test PTEST for logical address 0x00001000
    uint16_t mmusr = mmu_test_address(mmu, 0x00001000, false, true);
    // Should be valid (no I bit)
    ASSERT_TRUE((mmusr & MMUSR_I) == 0);

    // Test table walk for logical address 0x00001042 → physical 0x00002042
    ok = mmu_handle_fault(mmu, 0x00001042, false, true);
    ASSERT_TRUE(ok);
    // SoA entry for page at 0x00001000 should be populated
    ASSERT_TRUE(g_supervisor_read[1] != 0);

    cleanup(mem, mmu);
}

// ============================================================================
// Test: Bus error on invalid descriptor
// ============================================================================

TEST(test_invalid_descriptor_bus_error) {
    memory_map_t *mem = memory_map_init(32, 0x400000, 0x040000, NULL);
    uint8_t *ram = ram_native_pointer(mem, 0);
    mmu_state_t *mmu = mmu_init(ram, 0x400000, NULL, 0, 0);

    // TC: IS=0, PS=4, TIA=8, TIB=12
    uint32_t tc = (1u << 31) | (4u << 20) | (8u << 12) | (12u << 8);

    uint32_t level_a_base = 0x10000;
    uint64_t crp = ((uint64_t)DESC_DT_TABLE4 << 32) | level_a_base;

    // Level-A entry 0: invalid descriptor (DT=0)
    store_be32(ram + level_a_base, 0x00000000); // DT=0 = invalid

    mmu->tc = tc;
    mmu->crp = crp;
    mmu->enabled = true;
    mmu_invalidate_tlb(mmu);

    // Table walk should fail
    bool ok = mmu_handle_fault(mmu, 0x00000000, false, true);
    ASSERT_TRUE(!ok);

    // PTEST should report invalid
    uint16_t mmusr = mmu_test_address(mmu, 0x00000000, false, true);
    ASSERT_TRUE((mmusr & MMUSR_I) != 0);

    cleanup(mem, mmu);
}

// ============================================================================
// Test: Transparent translation (TT0)
// ============================================================================

TEST(test_transparent_translation) {
    memory_map_t *mem = memory_map_init(32, 0x400000, 0x040000, NULL);
    memory_populate_pages(mem, 0x40000000, 0x40080000);

    uint8_t *ram = ram_native_pointer(mem, 0);
    mmu_state_t *mmu = mmu_init(ram, 0x400000, NULL, 0, 0);

    // Enable MMU but don't set up translation tables
    mmu->tc = (1u << 31) | (4u << 20) | (8u << 12) | (12u << 8);
    mmu->enabled = true;

    // TT0: match addresses 0x00xxxxxx (base=0x00, mask=0x00)
    // Enable=1, base=0x00, mask=0x00, FC base=5, FC mask=7 (match all)
    // Format: bits [31:24]=base, [23:16]=mask, [15]=enable, [4:2]=fc_base, [1:0]=fc_mask
    // TT register format:
    //   bits 31:24 = logical address base (0x00)
    //   bits 23:16 = logical address mask (0x00 = exact match on upper byte)
    //   bit 15 = enable (1)
    //   bit 13 = R/W (0=match both)
    //   bits 6:4 = FC base
    //   bits 2:0 = FC mask (7=match all)
    mmu->tt0 = (0x00u << 24) // base = 0x00
               | (0x00u << 16) // mask = 0x00 (exact match)
               | (1u << 15) // enable
               | (0u << 13) // R/W = match both
               | (1u << 4) // FC base = 1
               | (7u << 0); // FC mask = 7 (match all FCs)

    mmu_invalidate_tlb(mmu);

    // Check TT match for address 0x00001000 (should match TT0)
    ASSERT_TRUE(mmu_check_tt(mmu, 0x00001000, false, true));

    // Check TT no-match for address 0x10001000 (different upper byte)
    ASSERT_TRUE(!mmu_check_tt(mmu, 0x10001000, false, true));

    // Handle fault should succeed via TT (identity mapping)
    bool ok = mmu_handle_fault(mmu, 0x00001000, false, true);
    ASSERT_TRUE(ok);
    // SoA entry should be populated
    ASSERT_TRUE(g_supervisor_read[1] != 0);

    // PTEST should report transparent translation
    uint16_t mmusr = mmu_test_address(mmu, 0x00002000, false, true);
    ASSERT_TRUE((mmusr & MMUSR_T) != 0);

    cleanup(mem, mmu);
}

// ============================================================================
// Test: Write protection
// ============================================================================

TEST(test_write_protection) {
    memory_map_t *mem = memory_map_init(32, 0x400000, 0x040000, NULL);
    uint8_t *ram = ram_native_pointer(mem, 0);
    mmu_state_t *mmu = mmu_init(ram, 0x400000, NULL, 0, 0);

    // TC: IS=0, PS=4, TIA=8, TIB=12
    uint32_t tc = (1u << 31) | (4u << 20) | (8u << 12) | (12u << 8);

    uint32_t level_a_base = 0x10000;
    uint32_t level_b_base = 0x11000;
    uint64_t crp = ((uint64_t)DESC_DT_TABLE4 << 32) | level_a_base;

    // Level-A entry 0 → level-B table
    store_be32(ram + level_a_base, level_b_base | DESC_DT_TABLE4);

    // Level-B entry 0: write-protected page (W bit = bit 2 set)
    store_be32(ram + level_b_base, 0x00000000 | DESC_DT_PAGE | (1u << 2));

    mmu->tc = tc;
    mmu->crp = crp;
    mmu->enabled = true;
    mmu_invalidate_tlb(mmu);

    // Read should succeed
    bool ok = mmu_handle_fault(mmu, 0x00000000, false, true);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(g_supervisor_read[0] != 0);

    // Write array should remain 0 (write-protected)
    ASSERT_TRUE(g_supervisor_write[0] == 0);

    // Write fault should fail
    mmu_invalidate_tlb(mmu);
    ok = mmu_handle_fault(mmu, 0x00000000, true, true);
    ASSERT_TRUE(!ok);

    // PTEST should report W bit
    uint16_t mmusr = mmu_test_address(mmu, 0x00000000, false, true);
    ASSERT_TRUE((mmusr & MMUSR_W) != 0);

    cleanup(mem, mmu);
}

// ============================================================================
// Test: Supervisor-only pages
// ============================================================================

TEST(test_supervisor_only_pages) {
    memory_map_t *mem = memory_map_init(32, 0x400000, 0x040000, NULL);
    uint8_t *ram = ram_native_pointer(mem, 0);
    mmu_state_t *mmu = mmu_init(ram, 0x400000, NULL, 0, 0);

    // TC: IS=0, PS=4, TIA=8, TIB=12
    uint32_t tc = (1u << 31) | (4u << 20) | (8u << 12) | (12u << 8);

    uint32_t level_a_base = 0x10000;
    uint32_t level_b_base = 0x11000;
    uint64_t crp = ((uint64_t)DESC_DT_TABLE8 << 32) | level_a_base;

    // Level-A entry 0 (long format, 8 bytes): table descriptor
    store_be32(ram + level_a_base, level_b_base | DESC_DT_TABLE4);
    store_be32(ram + level_a_base + 4, 0); // unused upper word

    // Level-B entry 0 (short format): page with S bit
    // In long-format descriptors, S bit is at bit 8. For short descriptors
    // accessed via a long-format root, the S bit isn't directly in the descriptor.
    // For this test, use long-format table root so the walk uses long_desc=true
    // at the first level, then short at level B.
    // Actually, for the 68030, S bit is only valid in long-format page descriptors.
    // Let's use a simpler approach: the fill function checks supervisor_only flag
    // which comes from the walk result.

    // Level-B entry 0: supervisor-only page (using long format page descriptor)
    // Since root DT=3 (long desc), level-A entries are 8 bytes
    // Level-A entry 0 as long: table pointer with DT=2 at byte 0
    store_be32(ram + level_a_base, level_b_base | DESC_DT_TABLE8);
    store_be32(ram + level_a_base + 4, 0);

    // Level-B entries are now 8-byte (long) format
    // Long page descriptor: S bit at bit 8 of first longword
    store_be32(ram + level_b_base, 0x00000000 | DESC_DT_PAGE | (1u << 8));
    store_be32(ram + level_b_base + 4, 0);

    mmu->tc = tc;
    mmu->crp = crp;
    mmu->enabled = true;
    mmu_invalidate_tlb(mmu);

    // Supervisor access should succeed
    bool ok = mmu_handle_fault(mmu, 0x00000000, false, true);
    ASSERT_TRUE(ok);
    // Supervisor read should be populated
    ASSERT_TRUE(g_supervisor_read[0] != 0);
    // User read should NOT be populated (supervisor-only)
    ASSERT_TRUE(g_user_read[0] == 0);
    ASSERT_TRUE(g_user_write[0] == 0);

    // User access should fail
    mmu_invalidate_tlb(mmu);
    ok = mmu_handle_fault(mmu, 0x00000000, false, false);
    ASSERT_TRUE(!ok);

    cleanup(mem, mmu);
}

// ============================================================================
// Test: 24-bit identity mapping (Macintosh Plus compatibility)
// ============================================================================

TEST(test_24bit_soa_compatibility) {
    // Create 24-bit address space with Plus layout
    memory_map_t *mem = memory_map_init(24, 0x400000, 0x020000, NULL);
    ASSERT_TRUE(mem != NULL);

    memory_populate_pages(mem, 0x400000, 0x580000);

    // RAM page 0: all four SoA arrays populated
    ASSERT_TRUE(g_supervisor_read[0] != 0);
    ASSERT_TRUE(g_supervisor_write[0] != 0);
    ASSERT_TRUE(g_user_read[0] != 0);
    ASSERT_TRUE(g_user_write[0] != 0);

    // Active pointers default to supervisor
    ASSERT_TRUE(g_active_read == g_supervisor_read);

    // Write and read through SoA fast path
    memory_write_uint32(0x000100, 0x12345678);
    ASSERT_EQ_INT((int)0x12345678, (int)memory_read_uint32(0x000100));

    // ROM page: read-only
    uint32_t rom_page_idx = 0x400000 >> PAGE_SHIFT;
    ASSERT_TRUE(g_supervisor_read[rom_page_idx] != 0);
    ASSERT_TRUE(g_supervisor_write[rom_page_idx] == 0);

    cleanup(mem, NULL);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    RUN(test_soa_identity_mapping);
    RUN(test_mmu_init_delete);
    RUN(test_tlb_invalidation);
    RUN(test_two_level_translation);
    RUN(test_invalid_descriptor_bus_error);
    RUN(test_transparent_translation);
    RUN(test_write_protection);
    RUN(test_supervisor_only_pages);
    RUN(test_24bit_soa_compatibility);
    printf("[PASS] All MMU tests passed\n");
    return 0;
}
