// Minimal system accessor stubs for the Lisa ROM unit test.
//
// We cannot link the shared support/stub_system.c here because it also stubs
// rom_identify_data(), which collides with the real rom.c under test. These
// tests never create a machine, so NULL returns are sufficient — the ROM
// interleave / identification paths exercised here don't touch the machine.

#include <stddef.h>
#include <stdint.h>

typedef struct memory memory_map_t;
typedef struct cpu cpu_t;
typedef struct rtc rtc_t;

memory_map_t *system_memory(void) {
    return NULL;
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
