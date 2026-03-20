#include "cpu.h"

void cpu_set_an(cpu_t *restrict cpu, int n, uint32_t value) {
    (void)cpu;
    (void)n;
    (void)value;
}

void cpu_set_pc(cpu_t *restrict cpu, uint32_t value) {
    (void)cpu;
    (void)value;
}
