// Test harness API for unit tests
// Provides a clean abstraction over test context management, replacing
// direct use of global_emulator in tests.

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations (matching actual type names from memory.h and cpu.h)
typedef struct memory memory_map_t;
typedef struct cpu cpu_t;

// Test context - replaces global_emulator for tests
typedef struct test_context {
    memory_map_t *memory;
    cpu_t *cpu;
    uint8_t *framebuffer;
    // Additional fields can be added as needed
} test_context_t;

// Lifecycle functions

// Initialize a new test context with the default harness configuration.
// The specific initialization depends on which harness_*.c is linked.
// Returns NULL on failure.
test_context_t *test_harness_init(void);

// Destroy a test context and free all associated resources.
void test_harness_destroy(test_context_t *ctx);

// Accessors - tests use these instead of global_emulator

// Get the memory map from the test context. May return NULL for isolated tests.
memory_map_t *test_get_memory(test_context_t *ctx);

// Get the CPU from the test context. May return NULL for isolated tests.
cpu_t *test_get_cpu(test_context_t *ctx);

// Get the framebuffer from the test context. May return NULL for isolated tests.
uint8_t *test_get_framebuffer(test_context_t *ctx);

// For code that calls system_*() accessors - redirect to test context
// This sets the active context that system_*() stubs will reference.
void test_set_active_context(test_context_t *ctx);

// Get the currently active test context (for use by system_*() stubs)
test_context_t *test_get_active_context(void);

#endif // TEST_HARNESS_H
