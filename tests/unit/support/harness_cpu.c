// CPU harness - initializes real CPU and memory subsystems
// Used for tests that need CPU emulation with memory access (disasm).

#include "cpu.h"
#include "harness.h"
#include "memory.h"
#include <stdlib.h>

// Initialize a CPU test context with real memory and CPU
test_context_t *test_harness_init(void) {
    test_context_t *ctx = calloc(1, sizeof(test_context_t));
    if (!ctx)
        return NULL;

    // Initialize memory map: 24-bit address space, 4 MB RAM, 128 KB ROM, no checkpoint restore
    ctx->memory = memory_map_init(24, 0x400000, 0x020000, NULL);
    if (!ctx->memory) {
        free(ctx);
        return NULL;
    }

    // Populate Plus memory layout (RAM/ROM pages + Phase Read) for CPU tests
    // The CPU test harness emulates the Plus, so we use Plus-specific layout constants.
    extern void memory_populate_pages(memory_map_t * mem, uint32_t rom_start, uint32_t rom_end);
    memory_populate_pages(ctx->memory, 0x400000, 0x580000);

    // Set as active context BEFORE cpu_init (CPU may call system_memory())
    test_set_active_context(ctx);

    // Initialize CPU (no checkpoint restore)
    ctx->cpu = cpu_init(NULL);
    if (!ctx->cpu) {
        memory_map_delete(ctx->memory);
        test_set_active_context(NULL);
        free(ctx);
        return NULL;
    }

    // No framebuffer in CPU-only mode
    ctx->framebuffer = NULL;

    return ctx;
}

// Destroy the CPU test context
void test_harness_destroy(test_context_t *ctx) {
    if (!ctx)
        return;

    // Clear active context if it points to us
    if (test_get_active_context() == ctx) {
        test_set_active_context(NULL);
    }

    // Destroy in reverse order of creation
    if (ctx->cpu) {
        cpu_delete(ctx->cpu);
    }
    if (ctx->memory) {
        memory_map_delete(ctx->memory);
    }

    free(ctx);
}
