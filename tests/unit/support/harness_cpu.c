// CPU harness - initializes real CPU and memory subsystems
// Used for tests that need CPU emulation with memory access (cputest, disasm).

#include "harness.h"
#include "memory.h"
#include "cpu.h"
#include <stdlib.h>

// Initialize a CPU test context with real memory and CPU
test_context_t* test_harness_init(void) {
    test_context_t *ctx = calloc(1, sizeof(test_context_t));
    if (!ctx) return NULL;

    // Initialize memory map (no checkpoint restore)
    ctx->memory = memory_map_init(NULL);
    if (!ctx->memory) {
        free(ctx);
        return NULL;
    }

    // Get the memory interface
    ctx->mem_iface = memory_map_interface(ctx->memory);

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
    if (!ctx) return;

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
