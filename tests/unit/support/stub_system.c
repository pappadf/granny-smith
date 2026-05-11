// System accessor stubs for unit tests
// Routes system_*() calls to the active test context via the harness API.

#include "display.h"
#include "harness.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct scheduler scheduler_t;
typedef struct debug debug_t;
typedef struct rtc rtc_t;
typedef void (*event_callback_t)(void *source, uint64_t data);

// System accessor stubs that use the harness context
memory_map_t *system_memory(void) {
    test_context_t *ctx = test_get_active_context();
    return ctx ? test_get_memory(ctx) : NULL;
}

cpu_t *system_cpu(void) {
    test_context_t *ctx = test_get_active_context();
    return ctx ? test_get_cpu(ctx) : NULL;
}

scheduler_t *system_scheduler(void) {
    // No scheduler in test harness
    return NULL;
}

debug_t *system_debug(void) {
    // No debugger in test harness
    return NULL;
}

rtc_t *system_rtc(void) {
    // No RTC in test harness — cmd_set_time guards on NULL and exits cleanly
    return NULL;
}

uint8_t *system_framebuffer(void) {
    test_context_t *ctx = test_get_active_context();
    return ctx ? test_get_framebuffer(ctx) : NULL;
}

// Stub display descriptor — unit tests don't render but a few code paths
// (debug.c screen.* methods) reach for system_display(); return a 1bpp
// 512x342 descriptor pointing at the harness framebuffer when present.
display_t *system_display(void) {
    static display_t synth = {
        .width = 512,
        .height = 342,
        .stride = 512 / 8,
        .format = PIXEL_1BPP_MSB,
        .bits = NULL,
        .clut = NULL,
        .clut_len = 0,
    };
    test_context_t *ctx = test_get_active_context();
    const uint8_t *bits = ctx ? test_get_framebuffer(ctx) : NULL;
    if (bits != synth.bits) {
        synth.bits = bits;
        synth.fb_dirty = true;
    }
    return bits ? &synth : NULL;
}

bool system_is_initialized(void) {
    // Return true if we have an active context
    return test_get_active_context() != NULL;
}

// Scheduler stubs for mouse automation commands
void scheduler_new_cpu_event(scheduler_t *sched, event_callback_t callback, void *source, uint64_t data,
                             uint64_t cpu_cycles, uint64_t ns_delay) {
    (void)sched;
    (void)callback;
    (void)source;
    (void)data;
    (void)cpu_cycles;
    (void)ns_delay;
}

void scheduler_new_event_type(scheduler_t *sched, const char *module, void *source, const char *name,
                              event_callback_t callback) {
    (void)sched;
    (void)module;
    (void)source;
    (void)name;
    (void)callback;
}

void remove_event(scheduler_t *sched, event_callback_t callback, void *source) {
    (void)sched;
    (void)callback;
    (void)source;
}

void remove_event_by_data(scheduler_t *sched, event_callback_t callback, void *source, uint64_t data) {
    (void)sched;
    (void)callback;
    (void)source;
    (void)data;
}

// Scheduler-tracked instruction counter — referenced by cpu.c's
// `cpu.instr_count` attribute. The unit tests don't drive the
// scheduler so the live counter is irrelevant; return 0.
uint64_t cpu_instr_count(void) {
    return 0;
}

// Keyboard stub for key injection command
typedef enum { key_up, key_down } key_event_t;
void system_keyboard_update(key_event_t event, int key) {
    (void)event;
    (void)key;
}

// Mouse stub for automation command
void system_mouse_update(bool button_down, int dx, int dy) {
    (void)button_down;
    (void)dx;
    (void)dy;
}

// Mouse move stubs for set-mouse debugger command
bool system_mouse_move(int dx, int dy) {
    (void)dx;
    (void)dy;
    return false;
}

bool system_mouse_move_adb(int dx, int dy) {
    (void)dx;
    (void)dy;
    return false;
}

// Machine management stub (used by cmd_rom in memory.c)
int system_ensure_machine(const char *model_id) {
    (void)model_id;
    return -1; // no machine in unit tests
}

// ROM identification stub (used by cmd_rom in memory.c)
typedef struct rom_info rom_info_t;
const rom_info_t *rom_identify_data(const uint8_t *data, size_t size, uint32_t *out_checksum) {
    (void)data;
    (void)size;
    (void)out_checksum;
    return NULL;
}
