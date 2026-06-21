// Lisa segment-MMU link stubs for the `cpu` unit-test harness.
//
// The cpu harness links the real src/core/memory/memory.c, whose slow path
// delegates to the Lisa segment MMU when g_lisa_mmu != NULL (see lisa_mmu.h).
// These tests never install a Lisa MMU (g_lisa_mmu stays NULL, so the delegate
// branch is never taken), but the symbols must resolve at link time.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
