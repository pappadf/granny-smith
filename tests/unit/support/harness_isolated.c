// Isolated harness - no emulator subsystems, only stubs
// Used for pure unit tests that don't need CPU, memory, or other subsystems.

#include "harness.h"
#include <stdlib.h>

// Initialize a minimal isolated test context
test_context_t* test_harness_init(void) {
    test_context_t *ctx = calloc(1, sizeof(test_context_t));
    if (!ctx) return NULL;

    // In isolated mode, all subsystem pointers remain NULL.
    // Tests using this harness rely entirely on stub implementations.
    ctx->memory = NULL;
    ctx->mem_iface = NULL;
    ctx->cpu = NULL;
    ctx->framebuffer = NULL;

    // Set as active context for system_*() stub redirects
    test_set_active_context(ctx);

    return ctx;
}

// Destroy the isolated test context
void test_harness_destroy(test_context_t *ctx) {
    if (!ctx) return;

    // Clear active context if it points to us
    if (test_get_active_context() == ctx) {
        test_set_active_context(NULL);
    }

    free(ctx);
}
