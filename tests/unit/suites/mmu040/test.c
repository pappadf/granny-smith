// MC68040 MMU unit tests (Quadra proposal Phase B gate).
//
// Verifies the mmu040.c translation front-end dispatched through mmu.c:
// three-level table walk (root/pointer/page), U/M bit update protocol,
// write protection accumulation, supervisor-only pages, indirect page
// descriptors, 8K pages, split URP/SRP roots, transparent-translation
// registers, PTEST MMUSR, and side-effect-free translation.
//
// Guest translation tables are built directly in the physical RAM buffer;
// accesses run through the real memory.c slow path so the SoA fill and
// deferred bus-error latching are exercised end to end.

#include "memory.h"
#include "mmu.h"
#include "mmu040.h"
#include "test_assert.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Table placement inside the 4 MB physical RAM buffer
#define ROOT_TABLE   0x00008000u // 512-byte aligned
#define ROOT_TABLE_S 0x00009000u // separate supervisor root for split tests
#define PTR_TABLE    0x0000A000u
#define PTR_TABLE_S  0x0000B000u
#define PAGE_TABLE   0x0000C000u
#define PAGE_TABLE_S 0x0000D000u

// The logical test address: RI=0, PI=1, PGI(4K)=0
#define TEST_LA 0x00040000u
// Physical page it maps to
#define TEST_PA 0x00100000u
// Supervisor-root alternative physical page
#define TEST_PA_S 0x00180000u

// Per-test context
typedef struct {
    memory_map_t *mem;
    mmu_state_t *bus;
    mmu040_state_t *mmu;
    uint8_t *ram;
} ctx_t;

// Store a 32-bit big-endian value into the physical RAM buffer
static void phys32(ctx_t *c, uint32_t addr, uint32_t val) {
    c->ram[addr] = (uint8_t)(val >> 24);
    c->ram[addr + 1] = (uint8_t)(val >> 16);
    c->ram[addr + 2] = (uint8_t)(val >> 8);
    c->ram[addr + 3] = (uint8_t)val;
}

// Read a 32-bit big-endian value from the physical RAM buffer
static uint32_t phys32_read(ctx_t *c, uint32_t addr) {
    return ((uint32_t)c->ram[addr] << 24) | ((uint32_t)c->ram[addr + 1] << 16) | ((uint32_t)c->ram[addr + 2] << 8) |
           (uint32_t)c->ram[addr + 3];
}

// Create memory map + bus MMU + attached 040 register file
static void ctx_open(ctx_t *c) {
    memset(c, 0, sizeof(*c));
    c->mem = memory_map_init(32, 0x400000, 0x040000, NULL);
    ASSERT_TRUE(c->mem != NULL);
    memory_populate_pages(c->mem, 0x40000000, 0x40080000);
    c->ram = ram_native_pointer(c->mem, 0);
    ASSERT_TRUE(c->ram != NULL);
    c->bus = mmu_init(c->ram, 0x400000, 0x8000000, c->ram + 0x400000, 0x040000, 0x40000000, 0x50000000);
    ASSERT_TRUE(c->bus != NULL);
    c->mmu = mmu040_init();
    ASSERT_TRUE(c->mmu != NULL);
    mmu_attach_mmu040(c->bus, c->mmu);
    g_mmu = c->bus;
    g_bus_error_pending = false;
}

static void ctx_close(ctx_t *c) {
    g_bus_error_pending = false;
    mmu040_delete(c->mmu);
    // mmu_delete clears g_mmu and resets the TLB tracker when it matches —
    // required so the next test's invalidation does a full memset.
    mmu_delete(c->bus);
    memory_map_delete(c->mem);
}

// Build the standard 4K user mapping: TEST_LA → TEST_PA via
// root[0] → PTR_TABLE, ptr[1] → PAGE_TABLE, page[0] → TEST_PA.
// `page_flags` ORs into the page descriptor (beyond PDT=resident).
static void build_tables(ctx_t *c, uint32_t root_flags, uint32_t ptr_flags, uint32_t page_flags) {
    memset(c->ram + ROOT_TABLE, 0, 0x200);
    memset(c->ram + PTR_TABLE, 0, 0x200);
    memset(c->ram + PAGE_TABLE, 0, 0x100);
    phys32(c, ROOT_TABLE + 0 * 4, PTR_TABLE | 2u | root_flags); // UDT resident
    phys32(c, PTR_TABLE + 1 * 4, PAGE_TABLE | 2u | ptr_flags);
    phys32(c, PAGE_TABLE + 0 * 4, TEST_PA | 1u | page_flags); // PDT resident
}

// Enable translation with both roots at ROOT_TABLE (shared, Mac OS style)
static void enable_mmu(ctx_t *c) {
    mmu040_set_root(c->mmu, &c->mmu->urp, ROOT_TABLE);
    mmu040_set_root(c->mmu, &c->mmu->srp, ROOT_TABLE);
    mmu040_set_tc(c->mmu, TC040_E);
    ASSERT_TRUE(c->mmu->enabled);
    ASSERT_TRUE(c->bus->enabled);
}

// ============================================================================
// Basic 4K walk
// ============================================================================

TEST(walk_4k_translates_and_sets_used_bits) {
    ctx_t c;
    ctx_open(&c);
    build_tables(&c, 0, 0, 0);
    c.ram[TEST_PA + 0x123] = 0x5A; // pattern behind the mapping
    enable_mmu(&c);

    // Read through the logical address: slow path → walk → SoA fill
    ASSERT_EQ_INT(0x5A, memory_read_uint8(TEST_LA + 0x123));
    ASSERT_TRUE(!g_bus_error_pending);

    // U bits set on all three descriptors by the walk
    ASSERT_TRUE(phys32_read(&c, ROOT_TABLE) & (1u << 3));
    ASSERT_TRUE(phys32_read(&c, PTR_TABLE + 4) & (1u << 3));
    ASSERT_TRUE(phys32_read(&c, PAGE_TABLE) & (1u << 3));
    // M clear: the access was a read
    ASSERT_TRUE(!(phys32_read(&c, PAGE_TABLE) & (1u << 4)));

    // Fast path now hits: SoA read entry populated
    ASSERT_TRUE(g_supervisor_read[TEST_LA >> PAGE_SHIFT] != 0);
    ctx_close(&c);
}

TEST(walk_invalid_descriptor_faults_with_retry_semantics) {
    ctx_t c;
    ctx_open(&c);
    build_tables(&c, 0, 0, 0);
    phys32(&c, PAGE_TABLE, 0); // PDT invalid
    enable_mmu(&c);

    (void)memory_read_uint8(TEST_LA);
    ASSERT_TRUE(g_bus_error_pending);
    ASSERT_TRUE(g_bus_error_is_pmmu); // nonresident page: retry semantics
    g_bus_error_pending = false;
    ctx_close(&c);
}

// ============================================================================
// Modified-bit protocol
// ============================================================================

TEST(modified_bit_set_on_first_write_only) {
    ctx_t c;
    ctx_open(&c);
    build_tables(&c, 0, 0, 0);
    enable_mmu(&c);

    // Read fault first: fills the read SoA, leaves the write SoA empty
    (void)memory_read_uint8(TEST_LA);
    ASSERT_TRUE(g_supervisor_read[TEST_LA >> PAGE_SHIFT] != 0);
    ASSERT_TRUE(g_supervisor_write[TEST_LA >> PAGE_SHIFT] == 0);
    ASSERT_TRUE(!(phys32_read(&c, PAGE_TABLE) & (1u << 4)));

    // First write re-faults, sets M, and fills the write SoA
    memory_write_uint8(TEST_LA + 5, 0x77);
    ASSERT_TRUE(!g_bus_error_pending);
    ASSERT_TRUE(phys32_read(&c, PAGE_TABLE) & (1u << 4));
    ASSERT_TRUE(g_supervisor_write[TEST_LA >> PAGE_SHIFT] != 0);
    ASSERT_EQ_INT(0x77, (int)c.ram[TEST_PA + 5]); // landed at the physical page
    ctx_close(&c);
}

// ============================================================================
// Protection
// ============================================================================

TEST(write_protect_accumulates_from_pointer_level) {
    ctx_t c;
    ctx_open(&c);
    build_tables(&c, 0, /*ptr W*/ 4u, 0);
    enable_mmu(&c);

    // Reads succeed
    (void)memory_read_uint8(TEST_LA);
    ASSERT_TRUE(!g_bus_error_pending);

    // Writes fault (W accumulated from the pointer descriptor)
    memory_write_uint8(TEST_LA, 0x01);
    ASSERT_TRUE(g_bus_error_pending);
    ASSERT_TRUE(g_bus_error_is_pmmu);
    g_bus_error_pending = false;
    // M must not be set on a protected write
    ASSERT_TRUE(!(phys32_read(&c, PAGE_TABLE) & (1u << 4)));
    ctx_close(&c);
}

TEST(supervisor_only_page_rejects_user_access) {
    ctx_t c;
    ctx_open(&c);
    build_tables(&c, 0, 0, /*S bit*/ (1u << 7));
    enable_mmu(&c);

    // Supervisor read succeeds (active arrays default to supervisor)
    (void)memory_read_uint8(TEST_LA);
    ASSERT_TRUE(!g_bus_error_pending);
    // User SoA must NOT have been filled despite the shared root
    ASSERT_TRUE(g_user_read[TEST_LA >> PAGE_SHIFT] == 0);

    // Simulate user mode: switch the active arrays
    g_active_read = g_user_read;
    g_active_write = g_user_write;
    (void)memory_read_uint8(TEST_LA);
    ASSERT_TRUE(g_bus_error_pending);
    g_bus_error_pending = false;
    g_active_read = g_supervisor_read;
    g_active_write = g_supervisor_write;
    ctx_close(&c);
}

// ============================================================================
// Split roots (URP != SRP)
// ============================================================================

TEST(split_roots_map_user_and_supervisor_differently) {
    ctx_t c;
    ctx_open(&c);
    build_tables(&c, 0, 0, 0); // user root: TEST_LA → TEST_PA
    // Supervisor root: TEST_LA → TEST_PA_S
    memset(c.ram + ROOT_TABLE_S, 0, 0x200);
    memset(c.ram + PTR_TABLE_S, 0, 0x200);
    memset(c.ram + PAGE_TABLE_S, 0, 0x100);
    phys32(&c, ROOT_TABLE_S, PTR_TABLE_S | 2u);
    phys32(&c, PTR_TABLE_S + 4, PAGE_TABLE_S | 2u);
    phys32(&c, PAGE_TABLE_S, TEST_PA_S | 1u);

    c.ram[TEST_PA] = 0x11;
    c.ram[TEST_PA_S] = 0x22;

    mmu040_set_root(c.mmu, &c.mmu->urp, ROOT_TABLE);
    mmu040_set_root(c.mmu, &c.mmu->srp, ROOT_TABLE_S);
    mmu040_set_tc(c.mmu, TC040_E);

    // Supervisor sees the SRP mapping
    ASSERT_EQ_INT(0x22, memory_read_uint8(TEST_LA));
    // User sees the URP mapping
    g_active_read = g_user_read;
    g_active_write = g_user_write;
    ASSERT_EQ_INT(0x11, memory_read_uint8(TEST_LA));
    g_active_read = g_supervisor_read;
    g_active_write = g_supervisor_write;
    ASSERT_TRUE(!g_bus_error_pending);
    ctx_close(&c);
}

// ============================================================================
// Indirect page descriptors
// ============================================================================

TEST(indirect_page_descriptor_resolves) {
    ctx_t c;
    ctx_open(&c);
    build_tables(&c, 0, 0, 0);
    // Replace page[0] with an indirect descriptor pointing at a real one
    uint32_t real_desc_addr = 0x0000E000u;
    phys32(&c, real_desc_addr, TEST_PA | 1u);
    phys32(&c, PAGE_TABLE, real_desc_addr | 2u); // PDT=2: indirect
    c.ram[TEST_PA + 9] = 0x3C;
    enable_mmu(&c);

    ASSERT_EQ_INT(0x3C, memory_read_uint8(TEST_LA + 9));
    ASSERT_TRUE(!g_bus_error_pending);
    // U bit set on the REAL descriptor, not the indirect pointer
    ASSERT_TRUE(phys32_read(&c, real_desc_addr) & (1u << 3));
    ctx_close(&c);
}

// ============================================================================
// 8K pages
// ============================================================================

TEST(walk_8k_pages) {
    ctx_t c;
    ctx_open(&c);
    // 8K: PGI = LA[17:13]; for LA 0x40000, PGI = 0x40000>>13 & 0x1F = 0.
    // Page descriptor PA mask is 0xFFFFE000.
    memset(c.ram + ROOT_TABLE, 0, 0x200);
    memset(c.ram + PTR_TABLE, 0, 0x200);
    memset(c.ram + PAGE_TABLE, 0, 0x80);
    phys32(&c, ROOT_TABLE, PTR_TABLE | 2u);
    phys32(&c, PTR_TABLE + 4, PAGE_TABLE | 2u);
    phys32(&c, PAGE_TABLE, TEST_PA | 1u);
    c.ram[TEST_PA + 0x1F00] = 0x66; // beyond 4K into the 8K page
    mmu040_set_root(c.mmu, &c.mmu->urp, ROOT_TABLE);
    mmu040_set_root(c.mmu, &c.mmu->srp, ROOT_TABLE);
    mmu040_set_tc(c.mmu, TC040_E | TC040_P); // 8K pages

    // Access in the second half of the 8K page (the second 4K SoA page)
    ASSERT_EQ_INT(0x66, memory_read_uint8(TEST_LA + 0x1F00));
    ASSERT_TRUE(!g_bus_error_pending);
    ctx_close(&c);
}

// ============================================================================
// Transparent translation registers
// ============================================================================

TEST(ttr_identity_mapping_and_write_protect) {
    ctx_t c;
    ctx_open(&c);
    // No tables at all: DTT0 covers 0x00xxxxxx (base 0, mask 0), both modes
    mmu040_set_ttr(c.mmu, &c.mmu->dtt0, 0x00000000u | (1u << 15) | (2u << 13));
    mmu040_set_tc(c.mmu, TC040_E);

    c.ram[0x2340] = 0x44;
    ASSERT_EQ_INT(0x44, memory_read_uint8(0x2340));
    memory_write_uint8(0x2344, 0x55);
    ASSERT_TRUE(!g_bus_error_pending);
    ASSERT_EQ_INT(0x55, (int)c.ram[0x2344]);

    // Write-protect the TTR: writes fault, reads keep working
    mmu040_set_ttr(c.mmu, &c.mmu->dtt0, 0x00000000u | (1u << 15) | (2u << 13) | (1u << 2));
    ASSERT_EQ_INT(0x44, memory_read_uint8(0x2340));
    memory_write_uint8(0x2348, 0x66);
    ASSERT_TRUE(g_bus_error_pending);
    g_bus_error_pending = false;
    ctx_close(&c);
}

TEST(ttr_supervisor_only_field) {
    ctx_t c;
    ctx_open(&c);
    build_tables(&c, 0, 0, 0); // fallback mapping for user accesses
    // DTT0: 0x00xxxxxx supervisor-only (S-field = 1)
    mmu040_set_ttr(c.mmu, &c.mmu->dtt0, (1u << 15) | (1u << 13));
    enable_mmu(&c);

    // Supervisor access matches the TTR (identity)
    c.ram[0x3000] = 0x77;
    ASSERT_EQ_INT(0x77, memory_read_uint8(0x3000));
    // A user access to TEST_LA does not match the supervisor-only TTR; it
    // walks the tables instead and reaches TEST_PA
    c.ram[TEST_PA + 1] = 0x88;
    g_active_read = g_user_read;
    g_active_write = g_user_write;
    ASSERT_EQ_INT(0x88, memory_read_uint8(TEST_LA + 1));
    g_active_read = g_supervisor_read;
    g_active_write = g_supervisor_write;
    ASSERT_TRUE(!g_bus_error_pending);
    ctx_close(&c);
}

// ============================================================================
// PTEST / MMUSR
// ============================================================================

TEST(ptest_builds_mmusr) {
    ctx_t c;
    ctx_open(&c);
    build_tables(&c, 0, 0, (1u << 10)); // G bit on the page
    enable_mmu(&c);

    // Resident page: PA, G, R
    mmu040_ptest(c.mmu, TEST_LA + 0x345, false, 5 /* supervisor data */);
    ASSERT_EQ_INT((int)(TEST_PA | MMUSR040_G | MMUSR040_R), (int)c.mmu->mmusr);

    // PTEST for write sets M (the update protocol runs on PTEST too)
    mmu040_ptest(c.mmu, TEST_LA, true, 5);
    ASSERT_TRUE(c.mmu->mmusr & MMUSR040_M);

    // Nonresident: R clear
    phys32(&c, PAGE_TABLE, 0);
    mmu040_invalidate_tlb(c.mmu);
    mmu040_ptest(c.mmu, TEST_LA, false, 5);
    ASSERT_TRUE(!(c.mmu->mmusr & MMUSR040_R));

    // TTR hit: T + R
    mmu040_set_ttr(c.mmu, &c.mmu->dtt0, (1u << 15) | (2u << 13));
    mmu040_ptest(c.mmu, 0x1234, false, 5);
    ASSERT_TRUE(c.mmu->mmusr & MMUSR040_T);
    ASSERT_TRUE(c.mmu->mmusr & MMUSR040_R);
    ctx_close(&c);
}

// ============================================================================
// Side-effect-free translation
// ============================================================================

TEST(translate_checked_is_side_effect_free) {
    ctx_t c;
    ctx_open(&c);
    build_tables(&c, 0, 0, 0);
    enable_mmu(&c);

    uint32_t pa = 0;
    ASSERT_TRUE(mmu_translate_checked(c.bus, TEST_LA + 0x40, true, &pa));
    ASSERT_EQ_INT((int)(TEST_PA + 0x40), (int)pa);
    // No U bits set, no SoA fill
    ASSERT_TRUE(!(phys32_read(&c, ROOT_TABLE) & (1u << 3)));
    ASSERT_TRUE(g_supervisor_read[TEST_LA >> PAGE_SHIFT] == 0);

    // Failed walk reports false
    ASSERT_TRUE(!mmu_translate_checked(c.bus, 0x0FF00000u, true, &pa));
    ctx_close(&c);
}

int main(void) {
    RUN(walk_4k_translates_and_sets_used_bits);
    RUN(walk_invalid_descriptor_faults_with_retry_semantics);
    RUN(modified_bit_set_on_first_write_only);
    RUN(write_protect_accumulates_from_pointer_level);
    RUN(supervisor_only_page_rejects_user_access);
    RUN(split_roots_map_user_and_supervisor_differently);
    RUN(indirect_page_descriptor_resolves);
    RUN(walk_8k_pages);
    RUN(ttr_identity_mapping_and_write_protect);
    RUN(ttr_supervisor_only_field);
    RUN(ptest_builds_mmusr);
    RUN(translate_checked_is_side_effect_free);
    printf("[mmu040] all tests passed\n");
    return 0;
}
