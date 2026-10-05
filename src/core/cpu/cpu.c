// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cpu.c
// CPU lifecycle, public API, and runtime dispatch for Motorola 68000.

#include "cpu_internal.h"

#include "alias.h"
#include "cpu_pd_ids.h"
#include "debug.h"
#include "debug_mmu.h"
#include "fpu.h"
#include "log.h"
#include "memory.h"
#include "mmu040.h"
#include "object.h"
#include "predecode.h"
#include "scheduler.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

// Forward declarations — class descriptors are at the bottom of the file but
// cpu_init / cpu_delete reference them.
static const class_desc_t cpu_class;
static const class_desc_t fpu_class;
static const class_desc_t mmu_class;
static const class_desc_t mmu040_class;
LOG_USE_CATEGORY_NAME("cpu");

// Declare decoder functions (defined in cpu_68000.c, cpu_68030.c, cpu_68040.c)
void cpu_run_68000(cpu_t *restrict cpu, uint32_t *instructions);
void cpu_run_68030(cpu_t *restrict cpu, uint32_t *instructions);
void cpu_run_68040(cpu_t *restrict cpu, uint32_t *instructions);

// === Predecode id properties (cpu_pd_ids.h) =================================
//
// The flag-liveness pass reads three static facts per id:
// whether it overwrites all of NZVC without reading them, whether it can
// fault or trap before writing them, and whether it has a no-flags twin.
// The bits are derived from the family list so a family added to the id
// space without a rule here defaults to the conservative T1 bits.

uint8_t g_cpu_pd_prop[PD_ID_COUNT];

// One row per family: its id range and its name (classified below).
typedef struct pd_family_desc {
    uint16_t first, last;
    const char *name;
} pd_family_desc_t;

static const pd_family_desc_t pd_families[] = {
#define X(name, slots) {PDF_##name, PDF_##name##_END, #name},
    PD_FAMILIES(X)
#undef X
};

static bool pd_name_starts(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool pd_name_ends(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

// True for a destination-shape suffix that reaches memory (MOVE families).
static bool pd_dst_shape_mem(const char *name) {
    return pd_name_ends(name, "_IND") || pd_name_ends(name, "_INC") || pd_name_ends(name, "_DEC") ||
           pd_name_ends(name, "_D16") || pd_name_ends(name, "_ABS");
}

// Fill a paired family: slot 2k = full (elidable), 2k+1 = its twin.
// mem_shape(k) says whether shape k touches memory.
static void pd_fill_pairs(const pd_family_desc_t *f, uint8_t base, bool (*mem_shape)(int), bool all_mem) {
    int slots = f->last - f->first + 1;
    for (int k = 0; k < slots / 2; k++) {
        uint8_t p = base | PD_P_ELIDABLE;
        if (all_mem || (mem_shape && mem_shape(k)))
            p |= PD_P_CANFAULT | PD_P_MEMDEF;
        g_cpu_pd_prop[f->first + 2 * k] = p;
        g_cpu_pd_prop[f->first + 2 * k + 1] = (uint8_t)((p & ~PD_P_ELIDABLE) | PD_P_TWIN);
    }
}

// Seven source shapes (D, IND, INC, DEC, D16, ABS, IMM): memory for 1..5.
static bool pd_s7_mem(int k) {
    return k >= 1 && k <= 5;
}

// Six destination shapes (D, IND, INC, DEC, D16, ABS): memory for 1..5.
static bool pd_d6_mem(int k) {
    return k >= 1 && k <= 5;
}

#include "cpu_pd_t1_names.h" // generated: leaf names by T1 id

// Handler name for the decode histogram: control, T1 leaf, or T0 family
// (+ shape slot) — a reviewer's view of which shapes the guest executes.
static const char *cpu_pd_id_name(uint16_t id) {
    static char buf[64];
    if (id == PD_UNDECODED)
        return "undecoded";
    if (id == PD_CROSS)
        return "cross";
    if (id == PD_GENERIC)
        return "generic";
    if (id >= T1_FIRST && id < T1_END)
        return cpu_pd_t1_names[id - T1_FIRST];
    for (size_t i = 0; i < sizeof(pd_families) / sizeof(pd_families[0]); i++) {
        const pd_family_desc_t *f = &pd_families[i];
        if (id >= f->first && id <= f->last) {
            int slot = id - f->first;
            snprintf(buf, sizeof(buf), "%s+%d%s", f->name, slot, (g_cpu_pd_prop[id] & PD_P_TWIN) ? " (nf)" : "");
            return buf;
        }
    }
    return "?";
}

void cpu_pd_prop_init(void) {
    static bool done = false;
    if (done)
        return;
    done = true;
    g_pd_id_name[PD_ARCH_68K] = cpu_pd_id_name;
    // Control and T1 ids: never overwriters, always able to fault.
    for (uint32_t i = 0; i < PD_ID_COUNT; i++)
        g_cpu_pd_prop[i] = PD_P_CANFAULT;
    for (size_t i = 0; i < sizeof(pd_families) / sizeof(pd_families[0]); i++) {
        const pd_family_desc_t *f = &pd_families[i];
        const char *n = f->name;
        int slots = f->last - f->first + 1;
        if (pd_name_starts(n, "MOVE_")) {
            pd_fill_pairs(f, PD_P_WNZVC, pd_s7_mem, pd_dst_shape_mem(n));
        } else if (pd_name_starts(n, "MOVEA_") || pd_name_starts(n, "ADDA_") || pd_name_starts(n, "SUBA_")) {
            for (int k = 0; k < slots; k++)
                g_cpu_pd_prop[f->first + k] = pd_s7_mem(k) ? PD_P_CANFAULT : 0;
        } else if (pd_name_starts(n, "DIVU_") || pd_name_starts(n, "DIVS_")) {
            for (int k = 0; k < slots; k++)
                g_cpu_pd_prop[f->first + k] = PD_P_CANFAULT; // the divide can raise
        } else if (pd_name_ends(n, "_EA_DN") || pd_name_starts(n, "TST_") || pd_name_starts(n, "CMPA_") ||
                   pd_name_starts(n, "MULU_") || pd_name_starts(n, "MULS_")) {
            pd_fill_pairs(f, PD_P_WNZVC, pd_s7_mem, false);
        } else if (pd_name_ends(n, "_DN_EA") || pd_name_starts(n, "ADDI_") || pd_name_starts(n, "SUBI_") ||
                   pd_name_starts(n, "ANDI_") || pd_name_starts(n, "ORI_") || pd_name_starts(n, "EORI_") ||
                   pd_name_starts(n, "CMPI_") || pd_name_starts(n, "CLR_") ||
                   ((pd_name_starts(n, "ADDQ_") || pd_name_starts(n, "SUBQ_")) && slots == PD_D6P_SLOTS)) {
            pd_fill_pairs(f, PD_P_WNZVC, pd_d6_mem, false);
        } else if (pd_name_starts(n, "CMPM_")) {
            pd_fill_pairs(f, PD_P_WNZVC, NULL, true);
        } else if (pd_name_starts(n, "BTST_") || pd_name_starts(n, "BCHG_") || pd_name_starts(n, "BCLR_") ||
                   pd_name_starts(n, "BSET_")) {
            pd_fill_pairs(f, 0, NULL, false); // Z only: elidable, never an overwriter
        } else if (slots == 2) {
            pd_fill_pairs(f, PD_P_WNZVC, NULL, false); // NEG/NOT/EXT/SWAP/MOVEQ/shifts
        } else if (pd_name_starts(n, "BSR_") || pd_name_starts(n, "JSR_") || pd_name_starts(n, "RTS") ||
                   pd_name_starts(n, "RTD") || pd_name_starts(n, "UNLK") || pd_name_starts(n, "LINK_") ||
                   pd_name_starts(n, "PEA_") || pd_name_starts(n, "MOVEM_") || pd_name_starts(n, "ATRAP") ||
                   pd_name_starts(n, "TRAP")) {
            for (int k = 0; k < slots; k++)
                g_cpu_pd_prop[f->first + k] = PD_P_CANFAULT;
        } else {
            // ADDQ_AN/SUBQ_AN, EXG, ABCD/SBCD, Bcc/DBcc, JMP, NOP, LEA, Scc:
            // no flag result to elide, never an overwriter, cannot fault.
            for (int k = 0; k < slots; k++)
                g_cpu_pd_prop[f->first + k] = 0;
        }
    }
}

// === Public Accessors ===

// FPU presence per 68K model (see cpu.h).  An explicit switch so a non-68K
// cpu_model can never read as "has a 68881/68882" — only the models this
// module implements answer true.
bool cpu_has_fpu(int cpu_model) {
    switch (cpu_model) {
    case CPU_MODEL_68030: // paired 68882
    case CPU_MODEL_68040: // on-chip FPU
    case CPU_MODEL_PPC601: // on-chip FPU
    case CPU_MODEL_PPC604: // on-chip FPU
    case CPU_MODEL_PPC750: // on-chip FPU
        return true;
    default: // 68000 compacts
        return false;
    }
}

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

// Set the interrupt priority level.
//
// THIS IS A BARE STORE, AND THAT IS THE CONTRACT.  Raising the IPL here does
// not itself wake a STOP-halted CPU; delivery happens when the scheduler's run
// loop next polls, which it does at the top of every iteration and again after
// each event batch (scheduler.c, the `is_stopped` branch).  That is sufficient
// because every caller raises the IPL from one of exactly two places:
//
//   1. inside a scheduler event callback -- every *_update_ipl in
//      mac030_glue.c, av.c, iifx.c, lisa.c and plus.c reaches here that way;
//   2. before the run loop starts (the cold-boot `cpu_set_ipl(cpu, 0)` in
//      mac030_glue_finish and its siblings).
//
// In both, the scheduler polls before the CPU next executes anything.
//
// The five *_update_ipl implementations pair this call with cpu_reschedule(),
// which only reconciles sprint counters -- it is NOT the wake mechanism, and
// reading it as one is the mistake this comment exists to prevent.
//
// DO NOT "fix" this by calling cpu_check_interrupt() from here, even guarded on
// the CPU being stopped.  The guard is sound as far as it goes -- a stopped CPU
// is at a clean instruction boundary.  But the common caller is a device
// callback running inside process_event_queue(), so delivering there would push
// an exception frame and move the PC part-way through an event batch that then
// continues.  That reorders exception delivery against the remaining events for
// no present benefit: no caller today violates the invariant above.
//
// A caller that genuinely cannot satisfy it -- a host-input hook, or the
// `machine.irq.inject` method the interrupt-controller object nodes would add
// -- must drive delivery through the scheduler rather than widen this
// function.  cpu_poll_interrupt() below is the entry point for that.
void cpu_set_ipl(cpu_t *restrict cpu, uint32_t value) {
    cpu->ipl = value;
}

// True if the CPU is halted by a STOP instruction (awaiting an interrupt).
bool cpu_is_stopped(cpu_t *restrict cpu) {
    return cpu->stopped != 0;
}

// Service a pending interrupt if one is now eligible.  Used by the scheduler to
// wake a STOP-halted CPU after an event raises the IPL (the check normally runs
// only at instruction boundaries, which a stopped CPU never reaches).
void cpu_poll_interrupt(cpu_t *restrict cpu) {
    cpu_check_interrupt(cpu);
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
// On 68030/68040, includes M bit (bit 12) and T0 bit (bit 14).
uint16_t cpu_get_sr(cpu_t *restrict cpu) {
    uint16_t sr = read_ccr(cpu);

    sr |= (cpu->trace >> 1 & 1) << 15; // T1 -- bit 1 of cpu->trace on every model
    if (cpu->cpu_model >= CPU_MODEL_68030) {
        sr |= (cpu->trace & 1) << 14; // T0 (does not exist below the 030)
        if (cpu->m)
            sr |= 1 << 12;
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

// === Built-in alias registration =============================================
//
// `$pc`, `$d0`, `$fpcr`, etc. are convenience names that resolve through
// the alias table to their typed cpu / cpu.fpu paths. Registration is
// idempotent (alias.c returns 0 on a re-registration with the same
// target), so calling these from every cpu_init is safe.

static void register_alias_or_warn(const char *name, const char *path) {
    char err[160];
    if (alias_register_builtin(name, path, err, sizeof(err)) < 0)
        LOG(0, "cpu: built-in alias '$%s' → '%s' rejected: %s", name, path, err);
}

static void register_cpu_aliases(void) {
    register_alias_or_warn("pc", "machine.cpu.pc");
    register_alias_or_warn("sr", "machine.cpu.sr");
    register_alias_or_warn("ccr", "machine.cpu.ccr");
    register_alias_or_warn("ssp", "machine.cpu.ssp");
    register_alias_or_warn("usp", "machine.cpu.usp");
    register_alias_or_warn("msp", "machine.cpu.msp");
    register_alias_or_warn("vbr", "machine.cpu.vbr");
    register_alias_or_warn("sp", "machine.cpu.sp");
    static const char *const dnames[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
    static const char *const anames[] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
    for (int i = 0; i < 8; i++) {
        char path[24];
        snprintf(path, sizeof(path), "machine.cpu.%s", dnames[i]);
        register_alias_or_warn(dnames[i], path);
        snprintf(path, sizeof(path), "machine.cpu.%s", anames[i]);
        register_alias_or_warn(anames[i], path);
    }
}

static void register_fpu_aliases(void) {
    register_alias_or_warn("fpcr", "machine.cpu.fpu.fpcr");
    register_alias_or_warn("fpsr", "machine.cpu.fpu.fpsr");
    register_alias_or_warn("fpiar", "machine.cpu.fpu.fpiar");
    static const char *const fpnames[] = {"fp0", "fp1", "fp2", "fp3", "fp4", "fp5", "fp6", "fp7"};
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof(path), "machine.cpu.fpu.%s", fpnames[i]);
        register_alias_or_warn(fpnames[i], path);
    }
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
        // The stream carries the whole struct, including the SAVE-TIME
        // pointer fields. NULL them all so the bindings below (and
        // cpu_attach_mmu, whose idempotence guard tests mmu_object) are
        // rebuilt for THIS machine — a stale non-NULL pointer here made
        // teardown free another machine's objects after a same-process
        // restore (double free) and left machine.cpu.mmu unbound.
        system_read_checkpoint_data(checkpoint, cpu, offsetof(struct cpu, mmu));
        cpu->mmu = NULL;
        cpu->fpu = NULL;
        cpu->cpu_object = NULL;
        cpu->fpu_object = NULL;
        cpu->mmu_object = NULL;
    } else {
        cpu->cpu_model = cpu_model;
        // Initial PC and SSP will be loaded from reset vectors after ROM is loaded
        cpu->pc = 0;
        cpu->a[7] = 0;
        cpu->supervisor = 1;
        cpu->interrupt_mask = 7;
        // 68030-specific registers default to zero (VBR=0, CACR=0, etc.)
    }

    // The predecoded executors' id property table (once per process).
    cpu_pd_prop_init();

    // Allocate FPU state for models that carry one (68030 paired 68882,
    // 68040 on-chip FPU — the same datapath serves both, see fpu.c).
    if (cpu_has_fpu(cpu->cpu_model)) {
        cpu->fpu = fpu_init();
    }

    // The 68040 MMU is on-chip: the CPU owns its state (created here, freed
    // in cpu_delete), unlike the 030 PMMU which machine code constructs and
    // attaches via cpu_attach_mmu.  MOVEC in cpu_68040.c reaches it through
    // cpu->mmu.  On checkpoint restore the register file follows the cpu_t
    // blob in the stream (see cpu_checkpoint).
    if (cpu->cpu_model == CPU_MODEL_68040) {
        cpu->mmu = mmu040_init();
        if (checkpoint && cpu->mmu) {
            system_read_checkpoint_data(checkpoint, cpu->mmu, sizeof(mmu040_state_t));
            // The bus backlink is a save-time pointer; machine init
            // re-establishes it via mmu_attach_mmu040 after restore.
            ((mmu040_state_t *)cpu->mmu)->bus = NULL;
        }
    }

    // FP register file, in the same order cpu_checkpoint wrote it (after the
    // 040 MMU blob).  cpu->fpu was allocated above, so this lands in place.
    if (checkpoint && cpu->fpu)
        system_read_checkpoint_data(checkpoint, cpu->fpu, sizeof(fpu_state_t));

    // Object-tree binding — instance_data on the cpu node is the cpu_t
    // itself, on the fpu node it's the fpu_state_t* directly.
    cpu->cpu_object = object_new(&cpu_class, cpu, "cpu");
    if (cpu->cpu_object) {
        object_set_label(cpu->cpu_object, "CPU");
        object_set_order(cpu->cpu_object, 10);
        object_attach(machine_object(), cpu->cpu_object);
        if (cpu->fpu) {
            cpu->fpu_object = object_new(&fpu_class, cpu->fpu, "fpu");
            if (cpu->fpu_object)
                object_attach(cpu->cpu_object, cpu->fpu_object);
        }
        // The 040's on-chip MMU binds its inspector here (the 030 PMMU is
        // machine-constructed and binds later via cpu_attach_mmu).
        if (cpu->cpu_model == CPU_MODEL_68040 && cpu->mmu) {
            cpu->mmu_object = object_new(&mmu040_class, cpu->mmu, "mmu");
            if (cpu->mmu_object)
                object_attach(cpu->cpu_object, cpu->mmu_object);
        }
    }

    // Built-in `$reg` aliases. CPU set is always registered; FPU set
    // only when the model has one. Idempotent, so safe under repeated
    // cpu_init / cpu_delete cycles (e.g. machine.boot).
    register_cpu_aliases();
    if (cpu->fpu)
        register_fpu_aliases();

    return cpu;
}

// Attach an MMU instance to the CPU and bind a `cpu.mmu` child object.
// Called by machine setup after the MMU is constructed.  Idempotent —
// safe to call again after a checkpoint restore that replants cpu->mmu.
void cpu_attach_mmu(cpu_t *cpu, void *mmu) {
    if (!cpu)
        return;
    cpu->mmu = mmu;
    if (!cpu->cpu_object || !mmu)
        return;
    if (cpu->mmu_object)
        return; // already attached (idempotent)
    cpu->mmu_object = object_new(&mmu_class, mmu, "mmu");
    if (cpu->mmu_object)
        object_attach(cpu->cpu_object, cpu->mmu_object);
}

// Bind a `cpu.mmu` child of an MMU kind this file does not model (the Lisa's
// segment MMU), so every MMU is reached at the same path.  Idempotent.
void cpu_attach_mmu_node(cpu_t *cpu, const class_desc_t *cls, void *data) {
    if (!cpu || !cpu->cpu_object || !cls || cpu->mmu_object)
        return;
    cpu->mmu_object = object_new(cls, data, "mmu");
    if (cpu->mmu_object)
        object_attach(cpu->cpu_object, cpu->mmu_object);
}

// Free resources associated with a CPU instance
void cpu_delete(cpu_t *cpu) {
    if (!cpu)
        return;
    if (cpu->mmu_object) {
        object_detach(cpu->mmu_object);
        object_delete(cpu->mmu_object);
        cpu->mmu_object = NULL;
    }
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
    // The 040 MMU is CPU-owned (see cpu_init); the 030 PMMU belongs to the
    // machine and must not be freed here.
    if (cpu->cpu_model == CPU_MODEL_68040 && cpu->mmu) {
        mmu040_delete((mmu040_state_t *)cpu->mmu);
        cpu->mmu = NULL;
    }
    free(cpu);
}

// Save CPU state to a checkpoint
void cpu_checkpoint(cpu_t *restrict cpu, checkpoint_t *checkpoint) {
    if (!cpu || !checkpoint)
        return;
    // Write the plain-data PREFIX of cpu_t.  sizeof(cpu_t) reached past the
    // last guest-state member and shipped five host pointers -- void *mmu,
    // void *fpu and the three object bindings -- whose bytes are meaningless
    // in a stream and differ run to run, so they also made checkpoints
    // non-deterministic.  offsetof ends the blob at the first of them.
    system_write_checkpoint_data(checkpoint, cpu, offsetof(struct cpu, mmu));
    // 68040: the on-chip MMU register file follows the cpu_t blob (the 030
    // PMMU is machine-owned and checkpointed by the machine instead).
    if (cpu->cpu_model == CPU_MODEL_68040 && cpu->mmu)
        system_write_checkpoint_data(checkpoint, cpu->mmu, sizeof(mmu040_state_t));
    // The FP register file is guest state and was never saved at all: every
    // restore silently resumed against a zeroed FPU, so guest arithmetic came
    // back wrong rather than merely non-deterministic.  fpu_state_t is pure
    // POD (eight 80-bit registers plus four words and a flag), so it rides as
    // one blob like the 040 MMU above.
    if (cpu->fpu)
        system_write_checkpoint_data(checkpoint, cpu->fpu, sizeof(fpu_state_t));
}

// === Runtime Dispatch ===

// 68K adapter for the scheduler's main-CPU seam:
// one indirect call per sprint, nothing per instruction.
static void cpu_if_run_sprint(void *ctx, uint32_t *instructions) {
    cpu_run_sprint((cpu_t *)ctx, instructions);
}

static bool cpu_if_is_stopped(void *ctx) {
    return cpu_is_stopped((cpu_t *)ctx);
}

static void cpu_if_poll_interrupt(void *ctx) {
    cpu_poll_interrupt((cpu_t *)ctx);
}

sched_cpu_if_t cpu_sched_if(cpu_t *cpu) {
    sched_cpu_if_t cif = {cpu, cpu_if_run_sprint, cpu_if_is_stopped, cpu_if_poll_interrupt};
    return cif;
}

// === Main-CPU debug interface adapter ===
// Debugger paths (breakpoints, disasm, shell prompt) reach the main CPU
// through this vtable so debug.c stays architecture-neutral.

static uint32_t cpu_dbgif_get_pc(void *ctx) {
    return cpu_get_pc((cpu_t *)ctx);
}

static void cpu_dbgif_set_pc(void *ctx, uint32_t pc) {
    cpu_set_pc((cpu_t *)ctx, pc);
}

// Disassemble one 68K instruction at pc, reading the instruction stream
// through the debug memory view (side-effect-free).  Returns bytes consumed.
static int cpu_dbgif_disasm(void *ctx, uint32_t pc, char *buf, size_t buflen) {
    (void)ctx;
    (void)buflen; // cpu_disasm's worst case is 76 bytes; see debug.h
    uint16_t words[16]; // longest 68K instruction is 10 words; decoder may peek further
    for (int i = 0; i < 16; i++)
        words[i] = memory_debug_read_uint16(pc + (uint32_t)(i * 2));
    return cpu_disasm(words, buf) * 2;
}

// Logical→physical through the current MMU context (identity when off).
static uint32_t cpu_dbgif_translate(void *ctx, uint32_t logical, bool *ok) {
    (void)ctx;
    bool valid = true;
    uint32_t phys = debug_translate_address(logical, NULL, NULL, &valid);
    if (ok)
        *ok = valid;
    return phys;
}

// The 68K register file for debug.frame: D0-D7, A0-A7, PC, SR, USP, SSP.
static void cpu_dbgif_regs(void *ctx, struct value_map_builder *regs) {
    cpu_t *cpu = (cpu_t *)ctx;
    char rname[4];
    for (int i = 0; i < 8; i++) {
        snprintf(rname, sizeof(rname), "d%d", i);
        val_map_put(regs, rname, val_int((int64_t)cpu_get_dn(cpu, i)));
    }
    for (int i = 0; i < 8; i++) {
        snprintf(rname, sizeof(rname), "a%d", i);
        val_map_put(regs, rname, val_int((int64_t)cpu_get_an(cpu, i)));
    }
    val_map_put(regs, "pc", val_int((int64_t)cpu_get_pc(cpu)));
    val_map_put(regs, "sr", val_int((int64_t)cpu_get_sr(cpu)));
    val_map_put(regs, "usp", val_int((int64_t)cpu_get_usp(cpu)));
    val_map_put(regs, "ssp", val_int((int64_t)cpu_get_ssp(cpu)));
}

// Side-effect-free conversion of an 80-bit extended-precision register
// to a host double for display. The FPU's own fpu_to_double helper sets
// inexact / SNaN bits in fpsr — we don't want that for an observer that
// just reads register state. Precision loss in normal range is fine for
// human-readable display; tests/tools that need bit-exact bytes can
// consume the hex form instead.
static double fp80_to_display_double(float80_reg_t f) {
    int sign = FP80_SIGN(f);
    uint16_t exp = FP80_EXP(f);
    if (exp == 0 && f.mantissa == 0)
        return sign ? -0.0 : 0.0;
    if (exp == 0x7FFF) {
        if (f.mantissa == 0 || (f.mantissa & ~(1ULL << 63)) == 0)
            return sign ? -((double)1.0 / 0.0) : ((double)1.0 / 0.0);
        return (double)0.0 / 0.0;
    }
    int32_t true_exp = (int32_t)exp - 16383;
    if (true_exp > 1023)
        return sign ? -((double)1.0 / 0.0) : ((double)1.0 / 0.0);
    if (true_exp < -1074)
        return sign ? -0.0 : 0.0;
    uint64_t mant52;
    int double_exp;
    if (true_exp >= -1022) {
        mant52 = (f.mantissa >> 11) & 0x000FFFFFFFFFFFFFULL;
        double_exp = true_exp + 1023;
    } else {
        // Subnormal in double precision.
        int shift = -1022 - true_exp;
        if (shift >= 53)
            return sign ? -0.0 : 0.0;
        mant52 = (f.mantissa >> (11 + shift)) & 0x000FFFFFFFFFFFFFULL;
        double_exp = 0;
    }
    uint64_t bits = ((uint64_t)sign << 63) | ((uint64_t)double_exp << 52) | mant52;
    double result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

// The 68881/68882/68040 register file for debug.frame: fp0-fp7 as the raw
// 80-bit value in hex plus a decimal rendering, and FPCR/FPSR/FPIAR.
static bool cpu_dbgif_fpu(void *ctx, struct value_map_builder *fb) {
    fpu_state_t *fpu = (fpu_state_t *)((cpu_t *)ctx)->fpu;
    if (!fpu)
        return false;
    value_t *fps = NULL;
    size_t n_fps = 0, cap_fps = 0;
    char hexbuf[24];
    char valbuf[40];
    for (int i = 0; i < 8; i++) {
        value_map_builder_t *fpb = val_map_new();
        // 4 hex digits of exponent (with sign bit) + underscore + 16 hex
        // digits of mantissa. Underscore makes scanning easier.
        snprintf(hexbuf, sizeof(hexbuf), "%04X_%016llX", fpu->fp[i].exponent, (unsigned long long)fpu->fp[i].mantissa);
        val_map_put(fpb, "hex", val_str(hexbuf));
        // Decimal display — handle special values explicitly and keep the
        // value a string (Inf/NaN aren't legal JSON numbers).
        uint16_t e = FP80_EXP(fpu->fp[i]);
        int sign = FP80_SIGN(fpu->fp[i]);
        if (e == 0 && fpu->fp[i].mantissa == 0)
            snprintf(valbuf, sizeof(valbuf), sign ? "-0" : "0");
        else if (e == 0x7FFF)
            snprintf(valbuf, sizeof(valbuf), "%s",
                     (fpu->fp[i].mantissa == 0 || (fpu->fp[i].mantissa & ~(1ULL << 63)) == 0) ? (sign ? "-Inf" : "Inf")
                                                                                              : "NaN");
        else
            snprintf(valbuf, sizeof(valbuf), "%.17g", fp80_to_display_double(fpu->fp[i]));
        val_map_put(fpb, "val", val_str(valbuf));
        val_list_push(&fps, &n_fps, &cap_fps, val_map_finish(fpb));
    }
    val_map_put(fb, "fp", val_list(fps, n_fps));
    val_map_put(fb, "fpcr", val_int((int64_t)fpu->fpcr));
    val_map_put(fb, "fpsr", val_int((int64_t)fpu->fpsr));
    val_map_put(fb, "fpiar", val_int((int64_t)fpu->fpiar));
    return true;
}

// Supervisor state, from SR.S.
static bool cpu_dbgif_is_supervisor(void *ctx) {
    return cpu_is_supervisor((cpu_t *)ctx);
}

cpu_debug_if_t cpu_debug_if(cpu_t *cpu) {
    // translate_mac is NULL: on 68K machines the mac world is the core's
    // own space (memory_debug_read already applies the 68k MMU).
    // translate_code is NULL: the debug translation serves both sides.
    cpu_debug_if_t dif = {.ctx = cpu,
                          .get_pc = cpu_dbgif_get_pc,
                          .set_pc = cpu_dbgif_set_pc,
                          .disasm = cpu_dbgif_disasm,
                          .translate = cpu_dbgif_translate,
                          .translate_mac = NULL,
                          .arch = "m68k",
                          .regs = cpu_dbgif_regs,
                          .fpu = cpu_dbgif_fpu,
                          .translate_code = NULL,
                          .is_supervisor = cpu_dbgif_is_supervisor};
    return dif;
}

// Run the appropriate decoder for the CPU model
void cpu_run_sprint(cpu_t *restrict cpu, uint32_t *instructions) {
    if (cpu->cpu_model == CPU_MODEL_68040)
        cpu_run_68040(cpu, instructions);
    else if (cpu->cpu_model == CPU_MODEL_68030)
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

static DEF_GETTER(attr_cpu_pc) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_pc(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_cpu_sr) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(2, cpu_get_sr(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_cpu_ccr) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(1, cpu_get_sr(cpu) & 0xFFu);
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_cpu_ssp) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_ssp(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_cpu_usp) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_usp(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_cpu_msp) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_msp(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_cpu_vbr) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_vbr(cpu));
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_cpu_sp) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    value_t v = val_uint(4, cpu_get_an(cpu, 7));
    v.flags |= VAL_HEX;
    return v;
}

#define CPU_DREG_RW(N)                                                                                                 \
    static value_t attr_cpu_d##N(struct object *self, const member_t *m) {                                             \
        (void)m;                                                                                                       \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        value_t v = val_uint(4, cpu_get_dn(cpu, N));                                                                   \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }                                                                                                                  \
    static value_t set_cpu_d##N(struct object *self, const member_t *m, value_t in) {                                  \
        (void)m;                                                                                                       \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        cpu_set_dn(cpu, N, (uint32_t)in.u);                                                                            \
        return val_none();                                                                                             \
    }
#define CPU_AREG_RW(N)                                                                                                 \
    static value_t attr_cpu_a##N(struct object *self, const member_t *m) {                                             \
        (void)m;                                                                                                       \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        value_t v = val_uint(4, cpu_get_an(cpu, N));                                                                   \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }                                                                                                                  \
    static value_t set_cpu_a##N(struct object *self, const member_t *m, value_t in) {                                  \
        (void)m;                                                                                                       \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        cpu_set_an(cpu, N, (uint32_t)in.u);                                                                            \
        return val_none();                                                                                             \
    }

// clang-format off — these macros expand to function definitions
// without a trailing `;`, which clang-format mis-parses as expressions.
CPU_DREG_RW(0)
CPU_DREG_RW(1)
CPU_DREG_RW(2)
CPU_DREG_RW(3)
CPU_DREG_RW(4)
CPU_DREG_RW(5)
CPU_DREG_RW(6)
CPU_DREG_RW(7)
CPU_AREG_RW(0)
CPU_AREG_RW(1)
CPU_AREG_RW(2)
CPU_AREG_RW(3)
CPU_AREG_RW(4)
CPU_AREG_RW(5)
CPU_AREG_RW(6)
CPU_AREG_RW(7)
// clang-format on

// === Setters for the named registers and CCR-bit attributes ===

static DEF_SETTER(set_cpu_pc) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    cpu_set_pc(cpu, (uint32_t)in.u);
    return val_none();
}

static DEF_SETTER(set_cpu_sr) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    cpu_set_sr(cpu, (uint16_t)in.u);
    return val_none();
}

static DEF_SETTER(set_cpu_ccr) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    uint16_t sr = cpu_get_sr(cpu);
    sr = (sr & (uint16_t)~cpu_ccr_mask) | (uint16_t)(in.u & cpu_ccr_mask);
    cpu_set_sr(cpu, sr);
    return val_none();
}

static DEF_SETTER(set_cpu_ssp) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    cpu_set_ssp(cpu, (uint32_t)in.u);
    return val_none();
}

static DEF_SETTER(set_cpu_usp) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    cpu_set_usp(cpu, (uint32_t)in.u);
    return val_none();
}

static DEF_SETTER(set_cpu_msp) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    cpu_set_msp(cpu, (uint32_t)in.u);
    return val_none();
}

static DEF_SETTER(set_cpu_vbr) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    cpu_set_vbr(cpu, (uint32_t)in.u);
    return val_none();
}

// `cpu.sp` aliases A7 (the active stack pointer).
static DEF_SETTER(set_cpu_sp) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    cpu_set_an(cpu, 7, (uint32_t)in.u);
    return val_none();
}

// `cpu.instr_count` — total instructions retired since reset. Read-only;
// the scheduler owns the counter. Mirrors `print instr` from the legacy
// shell.
static DEF_GETTER(attr_cpu_instr_count) {
    return val_uint(8, cpu_instr_count());
}

// `cpu.predecode` — 1 when the predecoded executor runs this CPU, 0 for the
// switch core.  Mirrors `predecode.enabled` (the pool's own node) so a
// reviewer can A/B any row from the shell without rebuilding.
static value_t attr_cpu_predecode(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(1, predecode_enabled() ? 1u : 0u);
}

static value_t set_cpu_predecode(struct object *self, const member_t *m, value_t in) {
    (void)self;
    (void)m;
    predecode_set_enabled((in.u & 1u) != 0);
    return val_none();
}

// `machine.cpu.frame([addr], [count], [before])` -- this CPU's debug frame,
// the contract every CPU-like object shares (debug_frame_build; debug.frame
// is the same call).
static DEF_METHOD(cpu_method_frame) {
    cpu_t *cpu = cpu_from(self);
    if (!cpu)
        return val_err("cpu not initialised");
    cpu_debug_if_t dif = cpu_debug_if(cpu);
    return debug_frame_build(&dif, "machine.cpu.frame", argc, argv);
}

// CCR-bit attributes (cpu.c / cpu.v / cpu.z / cpu.n / cpu.x). 1-bit reads
// and writes that round-trip through SR — the legacy `set z 1` interface
// in typed form.
#define CPU_CCR_BIT_RW(letter, mask_const)                                                                             \
    static value_t attr_cpu_cc_##letter(struct object *self, const member_t *m) {                                      \
        (void)m;                                                                                                       \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        return val_uint(1, (cpu_get_sr(cpu) & (mask_const)) ? 1u : 0u);                                                \
    }                                                                                                                  \
    static value_t set_cpu_cc_##letter(struct object *self, const member_t *m, value_t in) {                           \
        (void)m;                                                                                                       \
        cpu_t *cpu = cpu_from(self);                                                                                   \
        if (!cpu)                                                                                                      \
            return val_err("cpu not initialised");                                                                     \
        uint16_t sr = cpu_get_sr(cpu);                                                                                 \
        if (in.u & 1u)                                                                                                 \
            sr |= (mask_const);                                                                                        \
        else                                                                                                           \
            sr &= (uint16_t) ~(mask_const);                                                                            \
        cpu_set_sr(cpu, sr);                                                                                           \
        return val_none();                                                                                             \
    }

// clang-format off
CPU_CCR_BIT_RW(c, cpu_ccr_c)
CPU_CCR_BIT_RW(v, cpu_ccr_v)
CPU_CCR_BIT_RW(z, cpu_ccr_z)
CPU_CCR_BIT_RW(n, cpu_ccr_n)
CPU_CCR_BIT_RW(x, cpu_ccr_x)
// clang-format on

#define ATTR_RW_HEX_F(name_, get_, set_, doc_, flags_)                                                                 \
    {                                                                                                                  \
        .kind = M_ATTR, .name = name_, .doc = doc_, .flags = flags_, .attr = {                                         \
            .type = V_UINT,                                                                                            \
            .presentation_flags = VAL_HEX,                                                                             \
            .get = get_,                                                                                               \
            .set = set_                                                                                                \
        }                                                                                                              \
    }
#define ATTR_RW_HEX(name_, get_, set_, doc_) ATTR_RW_HEX_F(name_, get_, set_, doc_, 0)
// The same, shown only under the Advanced toggle.
#define ATTR_RW_HEX_ADV(name_, get_, set_, doc_) ATTR_RW_HEX_F(name_, get_, set_, doc_, M_CAT_ADVANCED)
// A read-only counter, shown only under the Advanced toggle.
#define ATTR_RO_ADV(name_, get_, doc_)                                                                                 \
    {                                                                                                                  \
        .kind = M_ATTR, .name = name_, .flags = M_CAT_ADVANCED, .doc = doc_, .attr = {                                 \
            .type = V_UINT,                                                                                            \
            .get = get_,                                                                                               \
            .set = NULL                                                                                                \
        }                                                                                                              \
    }
// A condition-code bit: one of the five CCR flags, readable and writable as 0/1
// (Advanced: SR and CCR already show them).
#define ATTR_RW_BIT(name_, get_, set_, doc_)                                                                           \
    {                                                                                                                  \
        .kind = M_ATTR, .name = name_, .doc = doc_, .flags = M_CAT_ADVANCED, .attr = {                                 \
            .type = V_UINT,                                                                                            \
            .get = get_,                                                                                               \
            .set = set_                                                                                                \
        }                                                                                                              \
    }
// D0..D7 and A0..A7 differ only by number, so generate their doc text too --
// sixteen hand-written strings saying "data register 3" would be sixteen
// chances to write "register 2".
#define CPU_DREG_MEMBER(N) ATTR_RW_HEX("d" #N, attr_cpu_d##N, set_cpu_d##N, "Data register D" #N " (32-bit)")
#define CPU_AREG_MEMBER(N) ATTR_RW_HEX("a" #N, attr_cpu_a##N, set_cpu_a##N, "Address register A" #N " (32-bit)")

// clang-format off
static const member_t cpu_members[] = {
    ATTR_RW_HEX("pc",  attr_cpu_pc,  set_cpu_pc,  "Program counter — address of the next instruction to execute"),
    ATTR_RW_HEX("sr",  attr_cpu_sr,  set_cpu_sr,  "Status register: the CCR in the low byte, plus the supervisor/trace bits and interrupt mask"),
    ATTR_RW_HEX_ADV("ccr", attr_cpu_ccr, set_cpu_ccr, "Condition code register — the low byte of SR (X, N, Z, V, C)"),
    ATTR_RW_HEX_ADV("ssp", attr_cpu_ssp, set_cpu_ssp, "Supervisor stack pointer, the A7 seen in supervisor mode"),
    ATTR_RW_HEX_ADV("usp", attr_cpu_usp, set_cpu_usp, "User stack pointer, the A7 seen in user mode"),
    ATTR_RW_HEX_ADV("msp", attr_cpu_msp, set_cpu_msp, "Master stack pointer (68020+); used instead of SSP when SR's M bit is set"),
    ATTR_RW_HEX_ADV("vbr", attr_cpu_vbr, set_cpu_vbr, "Vector base register (68010+) — where the exception vector table starts"),
    ATTR_RW_HEX("sp",  attr_cpu_sp,  set_cpu_sp,  "Whichever stack pointer A7 currently selects, following the SR's S and M bits"),
    CPU_DREG_MEMBER(0), CPU_DREG_MEMBER(1), CPU_DREG_MEMBER(2), CPU_DREG_MEMBER(3),
    CPU_DREG_MEMBER(4), CPU_DREG_MEMBER(5), CPU_DREG_MEMBER(6), CPU_DREG_MEMBER(7),
    CPU_AREG_MEMBER(0), CPU_AREG_MEMBER(1), CPU_AREG_MEMBER(2), CPU_AREG_MEMBER(3),
    CPU_AREG_MEMBER(4), CPU_AREG_MEMBER(5), CPU_AREG_MEMBER(6), CPU_AREG_MEMBER(7),
    ATTR_RW_BIT("c", attr_cpu_cc_c, set_cpu_cc_c, "Carry flag"),
    ATTR_RW_BIT("v", attr_cpu_cc_v, set_cpu_cc_v, "Overflow flag"),
    ATTR_RW_BIT("z", attr_cpu_cc_z, set_cpu_cc_z, "Zero flag"),
    ATTR_RW_BIT("n", attr_cpu_cc_n, set_cpu_cc_n, "Negative flag"),
    ATTR_RW_BIT("x", attr_cpu_cc_x, set_cpu_cc_x, "Extend flag — the carry out that multi-precision arithmetic carries in"),
    ATTR_RO_ADV("instr_count", attr_cpu_instr_count, "Instructions retired since the machine was created"),
    ATTR_RW_BIT("predecode", attr_cpu_predecode, set_cpu_predecode, "1 when the predecoded executor runs this CPU, 0 for the switch core (mirrors predecode.enabled)"),
    {.kind = M_METHOD, .name = "frame", .examples = EXAMPLES("machine.cpu.frame", "machine.cpu.frame 0x40800000 16"),
     .doc = "The CPU's debug frame: registers, a disassembly window and per-row translation",
     .method = {.result_doc = "{arch, pc, regs, rows, fpu?}", .args = debug_frame_args, .nargs = DEBUG_FRAME_NARGS, .result = V_MAP, .fn = cpu_method_frame}},
};
// clang-format on

static const class_desc_t cpu_class = {
    .name = "cpu",
    .members = cpu_members,
    .n_members = sizeof(cpu_members) / sizeof(cpu_members[0]),
    .doc = "The main CPU: registers and execution state",
};

// === CPU.fpu child class ====================================================
//
// instance_data on the fpu node is the fpu_state_t* itself; lifetime
// is tied to cpu_init / cpu_delete (the parent cpu owns the fpu).

static fpu_state_t *fpu_from(struct object *self) {
    return (fpu_state_t *)object_data(self);
}

static DEF_GETTER(attr_fpu_fpcr) {
    fpu_state_t *fpu = fpu_from(self);
    if (!fpu)
        return val_err("fpu not present");
    value_t v = val_uint(4, fpu->fpcr);
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_fpu_fpsr) {
    fpu_state_t *fpu = fpu_from(self);
    if (!fpu)
        return val_err("fpu not present");
    value_t v = val_uint(4, fpu->fpsr);
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_fpu_fpiar) {
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
// belongs in a future helper; this keeps the raw payload visible.
static DEF_GETTER(attr_fpu_fpN) {
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
        .kind = M_ATTR, .name = "fp" #idx,                                                                             \
        .doc = "Floating-point data register FP" #idx " — 80-bit extended precision, as raw bytes", .attr = {          \
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
         .flags = M_CAT_ADVANCED,
         .doc = "Floating-point control register: rounding mode, rounding precision, and the exception enables",
         .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_fpu_fpcr, .set = NULL}                                                                                                                   },
    {.kind = M_ATTR,
         .name = "fpsr",
         .flags = M_CAT_ADVANCED,
         .doc = "Floating-point status register: condition codes, quotient byte, and the accrued/current exception bytes",
         .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_fpu_fpsr, .set = NULL}                                                                                                                   },
    {.kind = M_ATTR,
         .name = "fpiar",
         .flags = M_CAT_ADVANCED,
         .doc =
         "Address of the last floating-point instruction that could take an exception — where a trap handler resumes",   .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_fpu_fpiar, .set = NULL}},
};

static const class_desc_t fpu_class = {
    .name = "fpu",
    .doc = "The 68881/68882 floating-point unit registers",
    .members = fpu_members,
    .n_members = sizeof(fpu_members) / sizeof(fpu_members[0]),
};

// === CPU.mmu child class ====================================================
//
// On M68030, the PMMU is integrated into the CPU.  Expose the MMU registers
// (TC, CRP, SRP, TT0, TT1, MMUSR) as cpu.mmu.* attributes so debug probes
// can compare translation state across machines without modifying the C
// code.  instance_data on the mmu node is the mmu_state_t* directly.

static mmu_state_t *mmu_from(struct object *self) {
    return (mmu_state_t *)object_data(self);
}

static DEF_GETTER(attr_mmu_tc) {
    mmu_state_t *mmu = mmu_from(self);
    if (!mmu)
        return val_err("mmu not present");
    value_t v = val_uint(4, mmu->tc);
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_mmu_crp_hi) {
    mmu_state_t *mmu = mmu_from(self);
    if (!mmu)
        return val_err("mmu not present");
    value_t v = val_uint(4, (uint32_t)(mmu->crp >> 32));
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_mmu_crp_lo) {
    mmu_state_t *mmu = mmu_from(self);
    if (!mmu)
        return val_err("mmu not present");
    value_t v = val_uint(4, (uint32_t)(mmu->crp & 0xFFFFFFFF));
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_mmu_srp_hi) {
    mmu_state_t *mmu = mmu_from(self);
    if (!mmu)
        return val_err("mmu not present");
    value_t v = val_uint(4, (uint32_t)(mmu->srp >> 32));
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_mmu_srp_lo) {
    mmu_state_t *mmu = mmu_from(self);
    if (!mmu)
        return val_err("mmu not present");
    value_t v = val_uint(4, (uint32_t)(mmu->srp & 0xFFFFFFFF));
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_mmu_tt0) {
    mmu_state_t *mmu = mmu_from(self);
    if (!mmu)
        return val_err("mmu not present");
    value_t v = val_uint(4, mmu->tt0);
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_mmu_tt1) {
    mmu_state_t *mmu = mmu_from(self);
    if (!mmu)
        return val_err("mmu not present");
    value_t v = val_uint(4, mmu->tt1);
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_mmu_mmusr) {
    mmu_state_t *mmu = mmu_from(self);
    if (!mmu)
        return val_err("mmu not present");
    value_t v = val_uint(4, mmu->mmusr);
    v.flags |= VAL_HEX;
    return v;
}

static DEF_GETTER(attr_mmu_enabled) {
    mmu_state_t *mmu = mmu_from(self);
    if (!mmu)
        return val_err("mmu not present");
    return val_uint(1, mmu->enabled ? 1 : 0);
}

// === Inspection methods, shared by the 030 and 040 nodes ===================
//
// translate, walk, map, descriptor and peek exist on every MMU kind (68030,
// 68040, PowerPC, the Lisa's segment MMU) with the same result shapes
// (debug_mmu.h), so a debugger needs no per-kind code to label an address,
// trace a translation, list the mappings or read memory.  The translation
// itself is mmu_debug_translate, which dispatches to the 68040 walker.

// The 68K MMUs' debugger translation (debug_mmu_xlate_fn).
static void mmu68k_xlate(void *ctx, uint32_t addr, bool supervisor, bool fetch, mmu_xlate_t *out, mmu_trace_t *trace) {
    (void)ctx;
    mmu_debug_translate(g_mmu, addr, supervisor, fetch, out, trace);
}

// translate(addr, [supervisor], [fetch]) -> {phys, valid, via, access}.
// Omitted `supervisor` means the CPU's current state.  `fetch` selects the
// 040's instruction TT registers; the 030 PMMU's TT match does not distinguish.
static DEF_METHOD(mmu68k_method_translate) {
    return debug_mmu_translate(mmu68k_xlate, NULL, debug_cpu_is_supervisor(), argc, argv);
}

// walk(addr, [supervisor], [fetch]) -> translate's map plus the steps: the
// TT check, the root pointer, and every table level the walk read.
static DEF_METHOD(mmu68k_method_walk) {
    return debug_mmu_walk(mmu68k_xlate, NULL, debug_cpu_is_supervisor(), argc, argv);
}

// map([start], [end], [supervisor], [fetch], [limit]) -> the mapped runs.
static DEF_METHOD(mmu68k_method_map) {
    return debug_mmu_map(mmu68k_xlate, NULL, debug_cpu_is_supervisor(), 1ull << 32, 12, argc, argv);
}

// descriptor formats: the 030's two descriptor sizes; the 040's three table
// levels (the level decides how a 4-byte descriptor reads).
static const char *const mmu030_desc_formats[] = {"short", "long", NULL};
static const char *const mmu040_desc_formats[] = {"root", "pointer", "page", NULL};

// Index of the `format` argument (argv[2]) in its enum table; `dflt` when omitted.
static int desc_format(int argc, const value_t *argv, const char *const *table, int dflt) {
    if (argc <= 2)
        return dflt;
    if (argv[2].kind == V_ENUM)
        return argv[2].enm.idx;
    if (argv[2].kind == V_STRING && argv[2].s)
        for (int i = 0; table[i]; i++)
            if (strcmp(argv[2].s, table[i]) == 0)
                return i;
    return dflt;
}

// descriptor(addr, [count], [format]) on the 68030: decode short (4-byte) or
// long (8-byte) descriptors at a physical address.  A descriptor's own type
// field says page or table; at the last level a table type means indirect.
static DEF_METHOD(mmu030_method_descriptor) {
    if (!g_mmu)
        return val_err("mmu not present");
    uint32_t addr = (uint32_t)argv[0].u;
    uint32_t count = debug_mmu_desc_count(argc, argv);
    bool is_long = desc_format(argc, argv, mmu030_desc_formats, 0) == 1;
    uint32_t ps_mask = (1u << TC_PS(g_mmu->tc)) - 1; // page-address bits below the page size are unused
    value_t *items = NULL;
    size_t len = 0, cap = 0;
    for (uint32_t i = 0; i < count; i++, addr += is_long ? 8 : 4) {
        uint32_t hi = mmu_read_physical_uint32(g_mmu, addr);
        uint32_t lo = is_long ? mmu_read_physical_uint32(g_mmu, addr + 4) : hi;
        uint32_t dt = hi & 3;
        value_map_builder_t *b = val_map_new();
        debug_mmu_put_hex(b, "addr", addr);
        debug_mmu_put_hex(b, "desc", hi);
        if (is_long)
            debug_mmu_put_hex(b, "desc_lo", lo);
        val_map_put(b, "dt", val_uint(4, dt));
        val_map_put(b, "type", val_str(dt == DESC_DT_INVALID ? "invalid" : dt == DESC_DT_PAGE ? "page" : "table"));
        if (dt != DESC_DT_INVALID) {
            debug_mmu_put_bool(b, "wp", (hi >> 2) & 1);
            debug_mmu_put_bool(b, "u", (hi >> 3) & 1);
            if (is_long)
                debug_mmu_put_bool(b, "s", (hi >> 8) & 1);
        }
        if (dt == DESC_DT_PAGE) {
            debug_mmu_put_bool(b, "m", (hi >> 4) & 1);
            debug_mmu_put_bool(b, "ci", (hi >> 6) & 1);
            debug_mmu_put_hex(b, "phys", lo & ~ps_mask & 0xFFFFFFFCu);
        } else if (dt != DESC_DT_INVALID) {
            debug_mmu_put_hex(b, "next", lo & 0xFFFFFFF0u);
            val_map_put(b, "next_format", val_str(dt == DESC_DT_TABLE8 ? "long" : "short"));
            if (is_long) {
                val_map_put(b, "limit", val_uint(4, (hi >> 16) & 0x7FFF));
                debug_mmu_put_bool(b, "lower_limit", (hi >> 31) & 1);
            }
        }
        val_list_push(&items, &len, &cap, val_map_finish(b));
    }
    return val_list(items, len);
}

// descriptor(addr, [count], [format]) on the 68040: decode 4-byte root,
// pointer or page descriptors at a physical address.
static DEF_METHOD(mmu040_method_descriptor) {
    if (!g_mmu || !g_mmu->m040)
        return val_err("mmu not present");
    uint32_t addr = (uint32_t)argv[0].u;
    uint32_t count = debug_mmu_desc_count(argc, argv);
    int fmt = desc_format(argc, argv, mmu040_desc_formats, 2);
    bool page8k = (g_mmu->m040->tc & TC040_P) != 0;
    value_t *items = NULL;
    size_t len = 0, cap = 0;
    for (uint32_t i = 0; i < count; i++, addr += 4) {
        uint32_t d = mmu_read_physical_uint32(g_mmu, addr);
        uint32_t dt = d & 3;
        value_map_builder_t *b = val_map_new();
        debug_mmu_put_hex(b, "addr", addr);
        debug_mmu_put_hex(b, "desc", d);
        val_map_put(b, "dt", val_uint(4, dt));
        if (fmt != 2) {
            // Root / pointer level: UDT 2 or 3 is resident, pointing at the next table.
            bool resident = dt >= 2;
            val_map_put(b, "type", val_str(resident ? "table" : "invalid"));
            if (resident) {
                debug_mmu_put_bool(b, "wp", (d >> 2) & 1);
                debug_mmu_put_bool(b, "u", (d >> 3) & 1);
                uint32_t mask = fmt == 0 ? 0xFFFFFE00u : page8k ? 0xFFFFFF80u : 0xFFFFFF00u;
                debug_mmu_put_hex(b, "next", d & mask);
            }
        } else if (dt == 2) {
            // Page level, PDT 2: an indirect pointer to the real page descriptor.
            val_map_put(b, "type", val_str("indirect"));
            debug_mmu_put_hex(b, "next", d & 0xFFFFFFFCu);
        } else {
            val_map_put(b, "type", val_str(dt == 0 ? "invalid" : "page"));
            if (dt != 0) {
                debug_mmu_put_bool(b, "wp", (d >> 2) & 1);
                debug_mmu_put_bool(b, "u", (d >> 3) & 1);
                debug_mmu_put_bool(b, "m", (d >> 4) & 1);
                debug_mmu_put_bool(b, "s", (d >> 7) & 1);
                debug_mmu_put_bool(b, "g", (d >> 10) & 1);
                val_map_put(b, "cm", val_uint(4, (d >> 5) & 3));
                debug_mmu_put_hex(b, "phys", d & (page8k ? 0xFFFFE000u : 0xFFFFF000u));
            }
        }
        val_list_push(&items, &len, &cap, val_map_finish(b));
    }
    return val_list(items, len);
}

// peek(addr, [size], [space]) -> the value, big-endian.  "logical" (default)
// reads through the MMU in the CPU's current state; "physical" reads the
// physical address directly.
static DEF_METHOD(mmu68k_method_peek) {
    uint32_t addr = (uint32_t)argv[0].u;
    unsigned size = (argc >= 2 && argv[1].kind == V_UINT) ? (unsigned)argv[1].u : 4;
    bool physical;
    if (!debug_parse_space(argc, argv, 2, &physical))
        return val_err("peek: space must be \"logical\" or \"physical\"");
    if (size != 1 && size != 2 && size != 4)
        return val_err("peek: size must be 1, 2 or 4");
    if (physical) {
        bool ok;
        uint32_t v = memory_debug_read_phys(addr, size, &ok);
        return ok ? val_uint((uint8_t)size, v) : val_err("peek: nothing at physical $%08X", addr);
    }
    uint32_t v = size == 1   ? memory_debug_read_uint8(addr)
                 : size == 2 ? memory_debug_read_uint16(addr)
                             : memory_debug_read_uint32(addr);
    return val_uint((uint8_t)size, v);
}

// peek's default size.
static const value_t k_peek_size4 = {.kind = V_UINT, .u = 4};

// descriptor's arguments, one table per descriptor-format vocabulary.
static const arg_decl_t mmu030_desc_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "physical address of the first descriptor"},
    {.name = "count",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &debug_mmu_desc_count_default,
     .doc = "how many consecutive descriptors (at most 256)"},
    {.name = "format",
     .kind = V_ENUM,
     .enum_values = mmu030_desc_formats,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "\"short\" (4-byte) or \"long\" (8-byte) descriptors",
     .default_doc = "short"},
};
static const arg_decl_t mmu040_desc_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "physical address of the first descriptor"},
    {.name = "count",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &debug_mmu_desc_count_default,
     .doc = "how many consecutive descriptors (at most 256)"},
    {.name = "format",
     .kind = V_ENUM,
     .enum_values = mmu040_desc_formats,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "the table level the descriptors sit at: \"root\", \"pointer\" or \"page\"",
     .default_doc = "page"},
};
static const arg_decl_t mmu68k_peek_args[] = {
    {.name = "addr", .kind = V_UINT, .presentation_flags = VAL_HEX, .doc = "address"},
    {.name = "size",
     .kind = V_UINT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &k_peek_size4,
     .doc = "1, 2 or 4 bytes"},
    {.name = "space",
     .kind = V_ENUM,
     .enum_values = debug_space_values,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "\"logical\" or \"physical\"",
     .default_doc = "logical"},
};

// The methods both 68K mmu member tables share (descriptor differs: each
// table adds its own after this).
// clang-format off
#define MMU68K_METHODS                                                                                                 \
    {.kind = M_METHOD,                                                                                                 \
     .name = "translate",                                                                                              \
     .examples = EXAMPLES("machine.cpu.mmu.translate 0x40800000", "machine.cpu.mmu.translate 0x2000 supervisor=false"),                     \
     .doc = "Translate an address, side-effect-free (same shape on every MMU kind)",                                   \
     .method = {.result_doc = "{phys, valid, via, access}",                                                            \
                .args = debug_mmu_xlate_args, .nargs = DEBUG_MMU_XLATE_NARGS, .result = V_MAP,                        \
                .fn = mmu68k_method_translate}},                                                                       \
    {.kind = M_METHOD,                                                                                                 \
     .name = "walk",                                                                                                   \
     .examples = EXAMPLES("machine.cpu.mmu.walk 0x40800000", "machine.cpu.mmu.walk 0x2000 supervisor=false"),             \
     .doc = "Translate an address and show every step: TT registers, root pointer, each table level read",           \
     .method = {.result_doc = "{phys, valid, via, access, steps: [{step, outcome, ...}]}",                              \
                .args = debug_mmu_xlate_args, .nargs = DEBUG_MMU_XLATE_NARGS, .result = V_MAP,                        \
                .fn = mmu68k_method_walk}},                                                                            \
    {.kind = M_METHOD,                                                                                                 \
     .name = "map",                                                                                                    \
     .examples = EXAMPLES("machine.cpu.mmu.map", "machine.cpu.mmu.map 0 0x10000000 supervisor=false"),                 \
     .doc = "List the mapped address ranges: runs that translate linearly with the same via and access",            \
     .method = {.result_doc = "[{start, size, phys, via, access}]",                                             \
                .args = debug_mmu_map_args, .nargs = DEBUG_MMU_MAP_NARGS, .result = V_LIST,                           \
                .fn = mmu68k_method_map}},                                                                             \
    {.kind = M_METHOD,                                                                                                 \
     .name = "peek",                                                                                                   \
     .examples = EXAMPLES("machine.cpu.mmu.peek 0x40800000", "machine.cpu.mmu.peek 0x400 2 physical"),                                                                          \
     .doc = "Read memory, logical (through the MMU) or physical; side-effect-free",                                    \
     .method = {.args = mmu68k_peek_args, .nargs = 3, .result = V_UINT, .fn = mmu68k_method_peek}}
// clang-format on

static const member_t mmu_members[] = {
    {.kind = M_ATTR,
     .name = "tc",
     .flags = M_CAT_ADVANCED,
     .doc = "Translation control: the enable bit, page size, and the initial-shift/table-index split",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu_tc, .set = NULL}},
    {.kind = M_ATTR,
     .name = "crp_hi",
     .flags = M_CAT_ADVANCED,
     .doc = "CPU root pointer, high longword — descriptor type and limit for the user-space table",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu_crp_hi, .set = NULL}},
    {.kind = M_ATTR,
     .name = "crp_lo",
     .flags = M_CAT_ADVANCED,
     .doc = "CPU root pointer, low longword — physical address of the user-space root table",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu_crp_lo, .set = NULL}},
    {.kind = M_ATTR,
     .name = "srp_hi",
     .flags = M_CAT_ADVANCED,
     .doc = "Supervisor root pointer, high longword; used only when TC selects a separate supervisor tree",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu_srp_hi, .set = NULL}},
    {.kind = M_ATTR,
     .name = "srp_lo",
     .flags = M_CAT_ADVANCED,
     .doc = "Supervisor root pointer, low longword — physical address of the supervisor root table",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu_srp_lo, .set = NULL}},
    {.kind = M_ATTR,
     .name = "tt0",
     .flags = M_CAT_ADVANCED,
     .doc = "Transparent translation register 0 — an address range that bypasses the page tables entirely",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu_tt0, .set = NULL}},
    {.kind = M_ATTR,
     .name = "tt1",
     .flags = M_CAT_ADVANCED,
     .doc = "Transparent translation register 1 — the second such range",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu_tt1, .set = NULL}},
    {.kind = M_ATTR,
     .name = "mmusr",
     .flags = M_CAT_ADVANCED,
     .doc = "Status of the last PTEST: bus error, resident, write-protected, and the level reached",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu_mmusr, .set = NULL}},
    {.kind = M_ATTR,
     .name = "enabled",
     .doc = "Nonzero when TC's enable bit is set and translation is actually in effect",
     .attr = {.type = V_UINT, .get = attr_mmu_enabled, .set = NULL}},
    MMU68K_METHODS,
    {.kind = M_METHOD,
     .name = "descriptor",
     .examples = EXAMPLES("machine.cpu.mmu.descriptor 0x3000 4", "machine.cpu.mmu.descriptor 0x3000 2 long"),
     .doc = "Decode raw PMMU descriptors at a physical address",
     .method = {.result_doc = "[{addr, desc, desc_lo?, dt, type, ...}]",
                .args = mmu030_desc_args,
                .nargs = 3,
                .result = V_LIST,
                .fn = mmu030_method_descriptor}},
};

static const class_desc_t mmu_class = {
    .name = "mmu",
    .doc = "The 68030 PMMU: translation registers; translate, walk, map, descriptor and peek",
    .members = mmu_members,
    .n_members = sizeof(mmu_members) / sizeof(mmu_members[0]),
};

// === CPU.mmu child class (68040) ============================================
//
// The 040 MMU is on-chip (register file in mmu040_state_t, owned by the CPU;
// registers reached via MOVEC, no PMOVE).  Expose TC, the four transparent-
// translation registers, both root pointers, and MMUSR as cpu.mmu.*
// attributes — the PMMU-shaped mmu_class above would misread this state.
// instance_data on the node is the mmu040_state_t* directly.

static mmu040_state_t *mmu040_from(struct object *self) {
    return (mmu040_state_t *)object_data(self);
}

// One read-only hex attribute getter per 32-bit register field.
#define MMU040_HEX_ATTR(field)                                                                                         \
    static value_t attr_mmu040_##field(struct object *self, const member_t *m) {                                       \
        (void)m;                                                                                                       \
        mmu040_state_t *mmu = mmu040_from(self);                                                                       \
        if (!mmu)                                                                                                      \
            return val_err("mmu not present");                                                                         \
        value_t v = val_uint(4, mmu->field);                                                                           \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }

MMU040_HEX_ATTR(tc)
MMU040_HEX_ATTR(itt0)
MMU040_HEX_ATTR(itt1)
MMU040_HEX_ATTR(dtt0)
MMU040_HEX_ATTR(dtt1)
MMU040_HEX_ATTR(urp)
MMU040_HEX_ATTR(srp)
MMU040_HEX_ATTR(mmusr)

static DEF_GETTER(attr_mmu040_enabled) {
    mmu040_state_t *mmu = mmu040_from(self);
    if (!mmu)
        return val_err("mmu not present");
    return val_uint(1, mmu->enabled ? 1 : 0);
}

static const member_t mmu040_members[] = {
    {.kind = M_ATTR,
     .name = "tc",
     .flags = M_CAT_ADVANCED,
     .doc = "Translation control: enable bit and page size (4K or 8K); the 68040 has no configurable table split",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu040_tc, .set = NULL}},
    {.kind = M_ATTR,
     .name = "itt0",
     .flags = M_CAT_ADVANCED,
     .doc = "Instruction transparent translation register 0 — an instruction-fetch range that bypasses the tables",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu040_itt0, .set = NULL}},
    {.kind = M_ATTR,
     .name = "itt1",
     .flags = M_CAT_ADVANCED,
     .doc = "Instruction transparent translation register 1",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu040_itt1, .set = NULL}},
    {.kind = M_ATTR,
     .name = "dtt0",
     .flags = M_CAT_ADVANCED,
     .doc = "Data transparent translation register 0 — a data-access range that bypasses the tables",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu040_dtt0, .set = NULL}},
    {.kind = M_ATTR,
     .name = "dtt1",
     .flags = M_CAT_ADVANCED,
     .doc = "Data transparent translation register 1",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu040_dtt1, .set = NULL}},
    {.kind = M_ATTR,
     .name = "urp",
     .flags = M_CAT_ADVANCED,
     .doc = "User root pointer — physical address of the root table used in user mode",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu040_urp, .set = NULL}},
    {.kind = M_ATTR,
     .name = "srp",
     .flags = M_CAT_ADVANCED,
     .doc = "Supervisor root pointer — physical address of the root table used in supervisor mode",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu040_srp, .set = NULL}},
    {.kind = M_ATTR,
     .name = "mmusr",
     .flags = M_CAT_ADVANCED,
     .doc = "Status of the last PTEST: physical address plus the resident, write-protected and transparent bits",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = attr_mmu040_mmusr, .set = NULL}},
    {.kind = M_ATTR,
     .name = "enabled",
     .doc = "Nonzero when TC's enable bit is set and translation is actually in effect",
     .attr = {.type = V_UINT, .get = attr_mmu040_enabled, .set = NULL}},
    MMU68K_METHODS,
    {.kind = M_METHOD,
     .name = "descriptor",
     .examples = EXAMPLES("machine.cpu.mmu.descriptor 0x3000 4 root", "machine.cpu.mmu.descriptor 0x3400 8"),
     .doc = "Decode raw 68040 table descriptors at a physical address",
     .method = {.result_doc = "[{addr, desc, dt, type, ...}]",
                .args = mmu040_desc_args,
                .nargs = 3,
                .result = V_LIST,
                .fn = mmu040_method_descriptor}},
};

static const class_desc_t mmu040_class = {
    .name = "mmu040",
    .doc = "The 68040 MMU: translation registers; translate, walk, map, descriptor and peek",
    .members = mmu040_members,
    .n_members = sizeof(mmu040_members) / sizeof(mmu040_members[0]),
};
