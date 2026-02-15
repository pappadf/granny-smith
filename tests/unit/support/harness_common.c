// Common utilities shared by all harness implementations

#include "harness.h"
#include <stdlib.h>

// Global active context pointer (used by system_*() stubs)
static test_context_t *g_active_context = NULL;

// Set the active test context for system_*() stubs
void test_set_active_context(test_context_t *ctx) {
    g_active_context = ctx;
}

// Get the currently active test context
test_context_t* test_get_active_context(void) {
    return g_active_context;
}

// Accessor implementations

memory_map_t* test_get_memory(test_context_t *ctx) {
    return ctx ? ctx->memory : NULL;
}

memory_interface_t* test_get_mem_iface(test_context_t *ctx) {
    return ctx ? ctx->mem_iface : NULL;
}

cpu_t* test_get_cpu(test_context_t *ctx) {
    return ctx ? ctx->cpu : NULL;
}

uint8_t* test_get_framebuffer(test_context_t *ctx) {
    return ctx ? ctx->framebuffer : NULL;
}
