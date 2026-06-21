// Minimal system accessor stubs for the Lisa ROM unit test.
//
// We cannot link the shared support/stub_system.c here because it also stubs
// rom_identify_data(), which collides with the real rom.c under test. These
// tests never create a machine, so NULL returns are sufficient — the ROM
// interleave / identification paths exercised here don't touch the machine.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct memory memory_map_t;
typedef struct cpu cpu_t;
typedef struct rtc rtc_t;

memory_map_t *system_memory(void) {
    return NULL;
}

// memory.c's slow path delegates to the Lisa segment MMU when g_lisa_mmu is
// set.  These tests never install one (g_lisa_mmu stays NULL, so the delegate
// branch is never taken), but the symbols must resolve at link time.
typedef struct lisa_mmu lisa_mmu_t;
lisa_mmu_t *g_lisa_mmu = NULL;

uint8_t lisa_mmu_read8(uint32_t addr, bool supervisor) {
    (void)addr;
    (void)supervisor;
    return 0;
}
uint16_t lisa_mmu_read16(uint32_t addr, bool supervisor) {
    (void)addr;
    (void)supervisor;
    return 0;
}
uint32_t lisa_mmu_read32(uint32_t addr, bool supervisor) {
    (void)addr;
    (void)supervisor;
    return 0;
}
void lisa_mmu_write8(uint32_t addr, bool supervisor, uint8_t value) {
    (void)addr;
    (void)supervisor;
    (void)value;
}
void lisa_mmu_write16(uint32_t addr, bool supervisor, uint16_t value) {
    (void)addr;
    (void)supervisor;
    (void)value;
}
void lisa_mmu_write32(uint32_t addr, bool supervisor, uint32_t value) {
    (void)addr;
    (void)supervisor;
    (void)value;
}
uint32_t lisa_mmu_debug_read(uint32_t addr, unsigned size, bool supervisor) {
    (void)addr;
    (void)size;
    (void)supervisor;
    return 0;
}
bool lisa_mmu_debug_write(uint32_t addr, unsigned size, bool supervisor, uint32_t value) {
    (void)addr;
    (void)size;
    (void)supervisor;
    (void)value;
    return false;
}

cpu_t *system_cpu(void) {
    return NULL;
}

const char *system_machine_model_id(void) {
    return NULL;
}

rtc_t *system_rtc(void) {
    return NULL;
}
