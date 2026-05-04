// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu.c
// CPU lifecycle, public API, and runtime dispatch for Motorola 68000.

#include "cpu_internal.h"

#include "fpu.h"
#include "log.h"
#include "memory.h"
#include "object.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

// Forward declarations — class descriptors are at the bottom of the file but
// cpu_init / cpu_delete reference them.
extern const class_desc_t cpu_class;
extern const class_desc_t fpu_class;
LOG_USE_CATEGORY_NAME("cpu");

// Declare decoder functions (defined in cpu_68000.c and cpu_68030.c)
void cpu_run_68000(cpu_t *restrict cpu, uint32_t *instructions);
void cpu_run_68030(cpu_t *restrict cpu, uint32_t *instructions);

// === Public Accessors ===

// Get the value of address register An (n=0-7)
uint32_t cpu_get_an(cpu_t *restrict cpu, int n) {
    assert(n >= 0 && n < 8);
    return cpu->a[n];
}

// Get the value of data register Dn (n=0-7)
uint32_t cpu_get_dn(cpu_t *restrict cpu, int n) {
    assert(n >= 0 && n < 8);
    return cpu->d[n];
}

// Get the program counter value
uint32_t cpu_get_pc(cpu_t *restrict cpu) {
    return cpu->pc;
}

// Set the value of address register An (n=0-7)
void cpu_set_an(cpu_t *restrict cpu, int n, uint32_t value) {
    assert(n >= 0 && n < 8);
    cpu->a[n] = value;
}

// Set the value of data register Dn (n=0-7)
void cpu_set_dn(cpu_t *restrict cpu, int n, uint32_t value) {
    assert(n >= 0 && n < 8);
    cpu->d[n] = value;
}

// Set the program counter value
void cpu_set_pc(cpu_t *restrict cpu, uint32_t value) {
    cpu->pc = value;
}

// Get the supervisor stack pointer value
uint32_t cpu_get_ssp(cpu_t *restrict cpu) {
    return cpu->ssp;
}

// Set the supervisor stack pointer value
void cpu_set_ssp(cpu_t *restrict cpu, uint32_t value) {
    cpu->ssp = value;
}

// Get the master stack pointer value (68030)
uint32_t cpu_get_msp(cpu_t *restrict cpu) {
    return cpu->msp;
}

// Set the master stack pointer value (68030)
void cpu_set_msp(cpu_t *restrict cpu, uint32_t value) {
    cpu->msp = value;
}

// Get the user stack pointer value
uint32_t cpu_get_usp(cpu_t *restrict cpu) {
    return cpu->usp;
}

// Set the user stack pointer value
void cpu_set_usp(cpu_t *restrict cpu, uint32_t value) {
    cpu->usp = value;
}

// Get the interrupt priority level
uint32_t cpu_get_ipl(cpu_t *restrict cpu) {
    return cpu->ipl;
}

// Set the interrupt priority level
void cpu_set_ipl(cpu_t *restrict cpu, uint32_t value) {
    cpu->ipl = value;
}

// Get the vector base register (68010+)
uint32_t cpu_get_vbr(cpu_t *restrict cpu) {
    return cpu->vbr;
}

// Set the vector base register (68010+)
void cpu_set_vbr(cpu_t *restrict cpu, uint32_t value) {
    cpu->vbr = value;
}

// Get the complete status register (includes CCR and system byte).
// On 68030, includes M bit (bit 12) and T0 bit (bit 14).
uint16_t cpu_get_sr(cpu_t *restrict cpu) {
    uint16_t sr = read_ccr(cpu);

    if (cpu->cpu_model == CPU_MODEL_68030) {
        sr |= (cpu->trace >> 1 & 1) << 15; // T1
        sr |= (cpu->trace & 1) << 14; // T0
        if (cpu->m)
            sr |= 1 << 12;
    } else {
        if (cpu->trace)
            sr |= 1 << 15; // T1 only
    }
    if (cpu->supervisor)
        sr |= 1 << 13;
    sr |= cpu->interrupt_mask << 8;

    return sr;
}

// Set the complete status register
void cpu_set_sr(cpu_t *restrict cpu, uint16_t sr) {
    write_sr(cpu, sr);
}

// Whether the CPU is currently in supervisor mode.
bool cpu_is_supervisor(cpu_t *restrict cpu) {
    return cpu->supervisor != 0;
}

// === Lifecycle ===

// Create and initialize a CPU instance for the specified model.
extern cpu_t *cpu_init(int cpu_model, checkpoint_t *checkpoint) {

    cpu_t *cpu = (cpu_t *)malloc(sizeof(cpu_t));
    if (!cpu)
        return NULL;

    memset(cpu, 0, sizeof(cpu_t));

    // Load from checkpoint if provided
    if (checkpoint) {
        // Read contiguous plain-data portion of cpu_t (everything before the first pointer)
        system_read_checkpoint_data(checkpoint, cpu, sizeof(cpu_t));
    } else {
        cpu->cpu_model = cpu_model;
        // Initial PC and SSP will be loaded from reset vectors after ROM is loaded
        cpu->pc = 0;
        cpu->a[7] = 0;
        cpu->supervisor = 1;
        cpu->interrupt_mask = 7;
        // 68030-specific registers default to zero (VBR=0, CACR=0, etc.)
    }

    // Allocate FPU state for 68030 model
    if (cpu->cpu_model == CPU_MODEL_68030) {
        cpu->fpu = fpu_init();
    }

    // Object-tree binding — instance_data on the cpu node is the cpu_t
    // itself, on the fpu node it's the fpu_state_t* directly.
    cpu->cpu_object = object_new(&cpu_class, cpu, "cpu");
    if (cpu->cpu_object) {
        object_attach(object_root(), cpu->cpu_object);
        if (cpu->fpu) {
            cpu->fpu_object = object_new(&fpu_class, cpu->fpu, "fpu");
            if (cpu->fpu_object)
                object_attach(cpu->cpu_object, cpu->fpu_object);
        }
    }

    return cpu;
}

// Free resources associated with a CPU instance
void cpu_delete(cpu_t *cpu) {
    if (!cpu)
        return;
    if (cpu->fpu_object) {
        object_detach(cpu->fpu_object);
        object_delete(cpu->fpu_object);
        cpu->fpu_object = NULL;
    }
    if (cpu->cpu_object) {
        object_detach(cpu->cpu_object);
        object_delete(cpu->cpu_object);
        cpu->cpu_object = NULL;
    }
    if (cpu->fpu) {
        fpu_free((fpu_state_t *)cpu->fpu);
        cpu->fpu = NULL;
    }
    free(cpu);
}

// Save CPU state to a checkpoint
void cpu_checkpoint(cpu_t *restrict cpu, checkpoint_t *checkpoint) {
    if (!cpu || !checkpoint)
        return;
    // Write contiguous plain-data portion of cpu_t in one operation
    system_write_checkpoint_data(checkpoint, cpu, sizeof(cpu_t));
}

// === Runtime Dispatch ===

// Run the appropriate decoder for the CPU model
void cpu_run_sprint(cpu_t *restrict cpu, uint32_t *instructions) {
    if (cpu->cpu_model == CPU_MODEL_68030)
        cpu_run_68030(cpu, instructions);
    else
        cpu_run_68000(cpu, instructions);
}

// === Object-model class descriptors =========================================
//
// instance_data on the cpu node is the cpu_t* itself; lifetime is tied
// to cpu_init / cpu_delete.

static cpu_t *cpu_from(struct object *self) {
    return (cpu_t *)object_data(self);
}

// === CPU class ==============================================================

static value_t attr_cpu_pc(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_pc(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_sr(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(2, cpu_get_sr(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_ccr(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(1, cpu_get_sr(cpu) & 0xFFu);
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_ssp(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_ssp(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_usp(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_usp(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_msp(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_msp(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_vbr(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_vbr(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_cpu_sp(struct object *self, const member_t *m) {
    (void)m;
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_an(cpu, 7));
    v.flags |= VAL_HEX;
    return v;
}

#define CPU_DREG_GETTER(N)                                                                                             \
    static value_t attr_cpu_d##N(struct object *self, const member_t *m) {                                             \
        (void)m;                                                                                                       \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        value_t v = val_uint(4, cpu_get_dn(cpu, N));                                                                   \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }
#define CPU_AREG_GETTER(N)                                                                                             \
    static value_t attr_cpu_a##N(struct object *self, const member_t *m) {                                             \
        (void)m;                                                                                                       \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        value_t v = val_uint(4, cpu_get_an(cpu, N));                                                                   \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }

// clang-format off — these macros expand to function definitions
// without a trailing `;`, which clang-format mis-parses as expressions.
CPU_DREG_GETTER(0)
CPU_DREG_GETTER(1)
CPU_DREG_GETTER(2)
CPU_DREG_GETTER(3)
CPU_DREG_GETTER(4)
CPU_DREG_GETTER(5)
CPU_DREG_GETTER(6)
CPU_DREG_GETTER(7)
CPU_AREG_GETTER(0)
CPU_AREG_GETTER(1)
CPU_AREG_GETTER(2)
CPU_AREG_GETTER(3)
CPU_AREG_GETTER(4)
CPU_AREG_GETTER(5)
CPU_AREG_GETTER(6)
CPU_AREG_GETTER(7)
// clang-format on

#define ATTR_RO_HEX(name_, get_)                                                                                       \
    {                                                                                                                  \
        .kind = M_ATTR, .name = name_, .flags = VAL_RO | VAL_HEX, .attr = {.type = V_UINT, .get = get_, .set = NULL }  \
    }

static const member_t cpu_members[] = {
    ATTR_RO_HEX("pc", attr_cpu_pc),   ATTR_RO_HEX("sr", attr_cpu_sr),   ATTR_RO_HEX("ccr", attr_cpu_ccr),
    ATTR_RO_HEX("ssp", attr_cpu_ssp), ATTR_RO_HEX("usp", attr_cpu_usp), ATTR_RO_HEX("msp", attr_cpu_msp),
    ATTR_RO_HEX("vbr", attr_cpu_vbr), ATTR_RO_HEX("sp", attr_cpu_sp),   ATTR_RO_HEX("d0", attr_cpu_d0),
    ATTR_RO_HEX("d1", attr_cpu_d1),   ATTR_RO_HEX("d2", attr_cpu_d2),   ATTR_RO_HEX("d3", attr_cpu_d3),
    ATTR_RO_HEX("d4", attr_cpu_d4),   ATTR_RO_HEX("d5", attr_cpu_d5),   ATTR_RO_HEX("d6", attr_cpu_d6),
    ATTR_RO_HEX("d7", attr_cpu_d7),   ATTR_RO_HEX("a0", attr_cpu_a0),   ATTR_RO_HEX("a1", attr_cpu_a1),
    ATTR_RO_HEX("a2", attr_cpu_a2),   ATTR_RO_HEX("a3", attr_cpu_a3),   ATTR_RO_HEX("a4", attr_cpu_a4),
    ATTR_RO_HEX("a5", attr_cpu_a5),   ATTR_RO_HEX("a6", attr_cpu_a6),   ATTR_RO_HEX("a7", attr_cpu_a7),
};

const class_desc_t cpu_class = {
    .name = "cpu",
    .members = cpu_members,
    .n_members = sizeof(cpu_members) / sizeof(cpu_members[0]),
};

// === CPU.fpu child class ====================================================
//
// instance_data on the fpu node is the fpu_state_t* itself; lifetime
// is tied to cpu_init / cpu_delete (the parent cpu owns the fpu).

static fpu_state_t *fpu_from(struct object *self) {
    return (fpu_state_t *)object_data(self);
}

static value_t attr_fpu_fpcr(struct object *self, const member_t *m) {
    (void)m;
    fpu_state_t *fpu = fpu_from(self);
    if (!fpu)
        return val_err("fpu not present");
    value_t v = val_uint(4, fpu->fpcr);
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_fpu_fpsr(struct object *self, const member_t *m) {
    (void)m;
    fpu_state_t *fpu = fpu_from(self);
    if (!fpu)
        return val_err("fpu not present");
    value_t v = val_uint(4, fpu->fpsr);
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_fpu_fpiar(struct object *self, const member_t *m) {
    (void)m;
    fpu_state_t *fpu = fpu_from(self);
    if (!fpu)
        return val_err("fpu not present");
    value_t v = val_uint(4, fpu->fpiar);
    v.flags |= VAL_HEX;
    return v;
}

// FP0..FP7: 80-bit extended precision. Expose the raw register bytes
// as V_BYTES (10 bytes) so the formatter can hex-dump it and tests
// can compare bit-for-bit. Conversion to a host double is lossy and
// belongs in a future helper; M3 keeps the raw payload visible.
static value_t attr_fpu_fpN(struct object *self, const member_t *m) {
    fpu_state_t *fpu = fpu_from(self);
    if (!fpu)
        return val_err("fpu not present");
    int n = (int)(uintptr_t)m->attr.user_data;
    if (n < 0 || n > 7)
        return val_err("invalid fp register index %d", n);
    return val_bytes(&fpu->fp[n], sizeof(fpu->fp[n]));
}

#define FP_REG(idx)                                                                                                    \
    {                                                                                                                  \
        .kind = M_ATTR, .name = "fp" #idx, .flags = VAL_RO, .attr = {                                                  \
            .type = V_BYTES,                                                                                           \
            .get = attr_fpu_fpN,                                                                                       \
            .set = NULL,                                                                                               \
            .user_data = (const void *)(uintptr_t)idx                                                                  \
        }                                                                                                              \
    }

static const member_t fpu_members[] = {
    FP_REG(0),
    FP_REG(1),
    FP_REG(2),
    FP_REG(3),
    FP_REG(4),
    FP_REG(5),
    FP_REG(6),
    FP_REG(7),
    {.kind = M_ATTR,
         .name = "fpcr",
         .flags = VAL_RO | VAL_HEX,
         .attr = {.type = V_UINT, .get = attr_fpu_fpcr, .set = NULL} },
    {.kind = M_ATTR,
         .name = "fpsr",
         .flags = VAL_RO | VAL_HEX,
         .attr = {.type = V_UINT, .get = attr_fpu_fpsr, .set = NULL} },
    {.kind = M_ATTR,
         .name = "fpiar",
         .flags = VAL_RO | VAL_HEX,
         .attr = {.type = V_UINT, .get = attr_fpu_fpiar, .set = NULL}},
};

const class_desc_t fpu_class = {
    .name = "fpu",
    .members = fpu_members,
    .n_members = sizeof(fpu_members) / sizeof(fpu_members[0]),
};
