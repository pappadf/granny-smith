// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// ppc.c
// PPC (MPC601) core: lifecycle, exception machinery, SPR file, scheduler and
// debugger adapters, object class.  The interpreter lives in ppc_run.c.

#include "ppc_internal.h"

#include "alias.h"
#include "log.h"
#include "machine_profile.h"
#include "object.h"
#include "ppc_disasm.h"
#include "system.h"
#include "value.h"

LOG_USE_CATEGORY_NAME("ppc");

// Forward declaration — class descriptor is at the bottom of the file.
extern const class_desc_t ppc_cpu_class;

// === Exception machinery ====================================================

// Raise an exception (601UM §5.4, per-exception Register Settings tables):
// SRR0 = resume_pc; SRR1 = exception-specific high bits | MSR[16-31]; MSR
// clears EE/PR/FP/FE0/SE/FE1/IT/DT and keeps ME/EP; PC = vector, prefixed
// $FFF00000 when MSR[EP] is set.
void ppc_exception(ppc_t *p, uint32_t vector, uint32_t srr1_hi, uint32_t resume_pc) {
    p->srr0 = resume_pc;
    p->srr1 = (srr1_hi & 0xFFFF0000u) | (p->msr & 0x0000FFFFu);
    p->msr &= PPC_MSR_ME | PPC_MSR_EP;
    ppc_update_active_maps(p);
    p->pc = ((p->msr & PPC_MSR_EP) ? 0xFFF00000u : 0u) + vector;
    // Record in the shared exception trace ring (§3.9c field mapping:
    // vbr slot = MSR, format_frame = vector offset, fault_addr = DAR).
    exc_trace_record(vector, resume_pc, p->srr0, p->dar, 0, p->msr, 0, (uint16_t)vector, 0);
}

// Take a pending external/decrementer interrupt when MSR[EE] allows.
// Level-sensitive: called before each instruction and from the sched-if
// poll hook (the just-re-enabled case after rfi/mtmsr, proposal §4.6).
void ppc_poll_interrupt(ppc_t *p) {
    if (!(p->msr & PPC_MSR_EE))
        return;
    if (p->ext_irq) {
        ppc_exception(p, PPC_VEC_EXTERNAL, 0, p->pc);
    } else if (p->dec_pending) {
        p->dec_pending = 0;
        ppc_exception(p, PPC_VEC_DEC, 0, p->pc);
    }
}

void ppc_set_ext_irq(ppc_t *p, bool level) {
    p->ext_irq = level ? 1u : 0u;
}

// === SPR file (mfspr/mtspr Tables 10-4/10-5) ================================

// Common privilege gate: a supervisor-level SPR touched from user mode takes
// the privileged-instruction program exception.
static bool spr_priv_fault(ppc_t *p, uint32_t iw) {
    if (p->msr & PPC_MSR_PR) {
        ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_PRIV, p->instruction_pc);
        return true;
    }
    (void)iw;
    return false;
}

// SPR number: the two 5-bit instruction halves are swapped (601UM mfspr page)
static inline uint32_t spr_number(uint32_t iw) {
    return (((iw) >> 16) & 0x1Fu) | ((((iw) >> 11) & 0x1Fu) << 5);
}

bool ppc_mfspr(ppc_t *p, uint32_t iw) {
    uint32_t n = spr_number(iw);
    uint32_t d = PPC_RT(iw);
    uint32_t v;
    switch (n) {
    case 0:
        v = p->mq;
        break;
    case 1:
        v = p->xer;
        break;
    case 4:
        v = p->rtcu;
        break; // RTC reads use SPR 4/5 in EVERY mode (601 asymmetry)
    case 5:
        v = p->rtcl;
        break;
    case 6: // POWER user-level DEC read, 601-supported (601UM Table 10-4 note 3)
        v = p->dec;
        break;
    case 8:
        v = p->lr;
        break;
    case 9:
        v = p->ctr;
        break;
    case 18:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->dsisr;
        break;
    case 19:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->dar;
        break;
    case 22:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->dec;
        break;
    case 25:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->sdr1;
        break;
    case 26:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->srr0;
        break;
    case 27:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->srr1;
        break;
    case 272:
    case 273:
    case 274:
    case 275:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->sprg[n - 272];
        break;
    case 282:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->ear;
        break;
    case 287:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->pvr;
        break;
    case 528:
    case 529:
    case 530:
    case 531:
    case 532:
    case 533:
    case 534:
    case 535: {
        if (spr_priv_fault(p, iw))
            return false;
        uint32_t pair = (n - 528) >> 1;
        v = (n & 1) ? p->batl[pair] : p->batu[pair];
        break;
    }
    case 1008:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->hid0;
        break;
    case 1009:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->hid1;
        break;
    case 1010:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->iabr;
        break;
    case 1013:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->dabr;
        break;
    case 1023:
        if (spr_priv_fault(p, iw))
            return false;
        v = p->pir;
        break;
    default:
        // Invalid SPR: no-op, unless SPR[0]=1 in user mode → privileged
        // program exception (601UM mfspr page).  SPR[0] is the high bit of
        // the low half of the swapped number, i.e. bit 4 of `n`.
        if ((n & 0x10u) && (p->msr & PPC_MSR_PR)) {
            ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_PRIV, p->instruction_pc);
            return false;
        }
        LOG(5, "mfspr r%u,%u: unimplemented SPR (no-op)", d, n);
        return true;
    }
    p->gpr[d] = v;
    return true;
}

bool ppc_mtspr(ppc_t *p, uint32_t iw) {
    uint32_t n = spr_number(iw);
    uint32_t v = p->gpr[PPC_RT(iw)];
    switch (n) {
    case 0:
        p->mq = v;
        break;
    case 1:
        p->xer = v;
        break;
    case 8:
        p->lr = v;
        break;
    case 9:
        p->ctr = v;
        break;
    case 18:
        if (spr_priv_fault(p, iw))
            return false;
        p->dsisr = v;
        break;
    case 19:
        if (spr_priv_fault(p, iw))
            return false;
        p->dar = v;
        break;
    case 20: // RTC writes use SPR 20/21 (supervisor); reads use 4/5
        if (spr_priv_fault(p, iw))
            return false;
        p->rtcu = v;
        break;
    case 21:
        if (spr_priv_fault(p, iw))
            return false;
        p->rtcl = v & 0x3FFFFF80u; // RTCL: bits 25-31 and 0-1 read as zero
        break;
    case 22:
        if (spr_priv_fault(p, iw))
            return false;
        p->dec = v;
        p->dec_pending = 0; // re-arming clears the latched expiry
        break;
    case 25:
        if (spr_priv_fault(p, iw))
            return false;
        p->sdr1 = v;
        break;
    case 26:
        if (spr_priv_fault(p, iw))
            return false;
        p->srr0 = v;
        break;
    case 27:
        if (spr_priv_fault(p, iw))
            return false;
        p->srr1 = v;
        break;
    case 272:
    case 273:
    case 274:
    case 275:
        if (spr_priv_fault(p, iw))
            return false;
        p->sprg[n - 272] = v;
        break;
    case 282:
        if (spr_priv_fault(p, iw))
            return false;
        p->ear = v;
        break;
    case 528:
    case 529:
    case 530:
    case 531:
    case 532:
    case 533:
    case 534:
    case 535: {
        if (spr_priv_fault(p, iw))
            return false;
        uint32_t pair = (n - 528) >> 1;
        if (n & 1)
            p->batl[pair] = v;
        else
            p->batu[pair] = v;
        // BAT writes invalidate cached translations (Phase D wires this
        // into the shared TLB-shootdown entry points).
        break;
    }
    case 1008:
        if (spr_priv_fault(p, iw))
            return false;
        p->hid0 = v;
        break;
    case 1009:
        if (spr_priv_fault(p, iw))
            return false;
        p->hid1 = v;
        break;
    case 1010:
        if (spr_priv_fault(p, iw))
            return false;
        p->iabr = v;
        break;
    case 1013:
        if (spr_priv_fault(p, iw))
            return false;
        p->dabr = v;
        break;
    case 1023:
        if (spr_priv_fault(p, iw))
            return false;
        p->pir = v;
        break;
    default:
        if ((n & 0x10u) && (p->msr & PPC_MSR_PR)) {
            ppc_exception(p, PPC_VEC_PROGRAM, PPC_SRR1_PROG_PRIV, p->instruction_pc);
            return false;
        }
        LOG(5, "mtspr %u,$%08X: unimplemented SPR (no-op)", n, v);
        return true;
    }
    return true;
}

// === Public register accessors ==============================================

uint32_t ppc_get_pc(ppc_t *restrict p) {
    return p->pc;
}

void ppc_set_pc(ppc_t *restrict p, uint32_t pc) {
    p->pc = pc;
}

uint32_t ppc_get_gpr(ppc_t *restrict p, int n) {
    assert(n >= 0 && n < 32);
    return p->gpr[n];
}

void ppc_set_gpr(ppc_t *restrict p, int n, uint32_t value) {
    assert(n >= 0 && n < 32);
    p->gpr[n] = value;
}

uint32_t ppc_get_msr(ppc_t *restrict p) {
    return p->msr;
}

void ppc_set_msr(ppc_t *restrict p, uint32_t value) {
    p->msr = value & PPC_MSR_MASK;
    ppc_update_active_maps(p);
}

bool ppc_is_supervisor(ppc_t *restrict p) {
    return !(p->msr & PPC_MSR_PR);
}

// === Lifecycle ==============================================================

// Hard-reset register state (601UM Table 5-8)
void ppc_reset(ppc_t *p) {
    struct object *keep_cpu = p->cpu_object;
    struct object *keep_fpu = p->fpu_object;
    struct object *keep_mmu = p->mmu_object;
    memset(p, 0, sizeof(*p));
    p->cpu_object = keep_cpu;
    p->fpu_object = keep_fpu;
    p->mmu_object = keep_mmu;
    p->cpu_model = CPU_MODEL_PPC601;
    p->msr = PPC_MSR_ME | PPC_MSR_EP; // $00001040
    p->pvr = 0x00010001u;
    p->hid0 = 0x80010080u;
    p->pc = 0xFFF00100u; // reset vector, MSR[EP]=1
    p->instruction_pc = p->pc;
}

// === `$reg` aliases (main-CPU privilege per cores.md) =======================

static void register_alias_or_warn(const char *name, const char *path) {
    char err[160];
    if (alias_register_builtin(name, path, err, sizeof(err)) < 0)
        LOG(0, "ppc: built-in alias '$%s' → '%s' rejected: %s", name, path, err);
}

static void register_ppc_aliases(void) {
    register_alias_or_warn("pc", "machine.cpu.pc");
    register_alias_or_warn("lr", "machine.cpu.lr");
    register_alias_or_warn("ctr", "machine.cpu.ctr");
    register_alias_or_warn("cr", "machine.cpu.cr");
    register_alias_or_warn("msr", "machine.cpu.msr");
    register_alias_or_warn("xer", "machine.cpu.xer");
    for (int i = 0; i < 32; i++) {
        char name[8], path[24];
        snprintf(name, sizeof(name), "r%d", i);
        snprintf(path, sizeof(path), "machine.cpu.r%d", i);
        register_alias_or_warn(name, path);
    }
}

ppc_t *ppc_init(checkpoint_t *checkpoint) {
    ppc_t *p = (ppc_t *)malloc(sizeof(ppc_t));
    if (!p)
        return NULL;

    if (checkpoint) {
        // The stream carries the whole struct including save-time pointers;
        // null them so the bindings below are rebuilt for THIS machine
        // (the cpu.c same-process-restore double-free precedent).
        system_read_checkpoint_data(checkpoint, p, sizeof(ppc_t));
        p->cpu_object = NULL;
        p->fpu_object = NULL;
        p->mmu_object = NULL;
        ppc_update_active_maps(p);
    } else {
        memset(p, 0, sizeof(ppc_t));
        ppc_reset(p);
    }

    // Object-tree binding: the main CPU owns `machine.cpu`.
    p->cpu_object = object_new(&ppc_cpu_class, p, "cpu");
    if (p->cpu_object) {
        object_set_label(p->cpu_object, "CPU");
        object_set_order(p->cpu_object, 10);
        object_attach(machine_object(), p->cpu_object);
    }

    // `$pc`, `$r0`... — the 68K `$d0`-style aliases simply don't exist on a
    // PPC machine (§3.9d); registration is idempotent.
    register_ppc_aliases();

    return p;
}

void ppc_delete(ppc_t *p) {
    if (!p)
        return;
    if (p->cpu_object) {
        object_detach(p->cpu_object);
        object_delete(p->cpu_object);
        p->cpu_object = NULL;
    }
    free(p);
}

void ppc_checkpoint(ppc_t *restrict p, checkpoint_t *checkpoint) {
    if (!p || !checkpoint)
        return;
    // One POD blob; pointers are nulled on restore (§3.9f).
    system_write_checkpoint_data(checkpoint, p, sizeof(ppc_t));
}

// === Scheduler adapter (the main-CPU seam) ==================================

static void ppc_if_run_sprint(void *ctx, uint32_t *instructions) {
    ppc_run((ppc_t *)ctx, instructions);
}

// The 601 never parks: PowerPC has no STOP-equivalent the Mac uses — the
// guest idles in loops, exactly as the real machine burns its CPU (§3.7).
static bool ppc_if_is_stopped(void *ctx) {
    (void)ctx;
    return false;
}

static void ppc_if_poll_interrupt(void *ctx) {
    ppc_poll_interrupt((ppc_t *)ctx);
}

sched_cpu_if_t ppc_sched_if(ppc_t *p) {
    sched_cpu_if_t cif = {p, ppc_if_run_sprint, ppc_if_is_stopped, ppc_if_poll_interrupt};
    return cif;
}

// === Debugger adapter (§3.9b) ===============================================

static uint32_t ppc_dbgif_get_pc(void *ctx) {
    return ((ppc_t *)ctx)->pc;
}

static void ppc_dbgif_set_pc(void *ctx, uint32_t pc) {
    ((ppc_t *)ctx)->pc = pc;
}

// One instruction at pc through the debug memory view; always 4 bytes.
static int ppc_dbgif_disasm(void *ctx, uint32_t pc, char *buf) {
    (void)ctx;
    ppc_insn ins;
    ppc_disassemble(memory_debug_read_uint32(pc), pc, &ins);
    // debug.c splits on '\t'; ppc_disasm emits "mnemonic\toperands" already.
    snprintf(buf, 100, "%s", ins.text);
    return 4;
}

// Translation is identity until the MMU front end lands (Phase D); report
// validity through the shared debug path so callers stay honest.
static uint32_t ppc_dbgif_translate(void *ctx, uint32_t logical, bool *ok) {
    (void)ctx;
    if (ok)
        *ok = true;
    return logical;
}

cpu_debug_if_t ppc_debug_if(ppc_t *p) {
    cpu_debug_if_t dif = {p, ppc_dbgif_get_pc, ppc_dbgif_set_pc, ppc_dbgif_disasm, ppc_dbgif_translate};
    return dif;
}

// === Object-model class (§3.9d) =============================================

static ppc_t *ppc_from(struct object *self) {
    return (ppc_t *)object_data(self);
}

// Uniform hex attribute plumbing: user_data selects the field.
enum ppc_attr_id {
    PA_PC = 0,
    PA_MSR,
    PA_CR,
    PA_XER,
    PA_LR,
    PA_CTR,
    PA_MQ,
    PA_SRR0,
    PA_SRR1,
    PA_DEC,
    PA_RTCU,
    PA_RTCL,
    PA_SDR1,
    PA_FPSCR,
    PA_GPR0 = 0x100, // ..0x11F
    PA_SR0 = 0x200, // ..0x20F
    PA_BAT0U = 0x300, // U/L interleaved ..0x307
};

static uint32_t *ppc_attr_slot(ppc_t *p, int id) {
    if (id >= PA_GPR0 && id < PA_GPR0 + 32)
        return &p->gpr[id - PA_GPR0];
    if (id >= PA_SR0 && id < PA_SR0 + 16)
        return &p->sr[id - PA_SR0];
    if (id >= PA_BAT0U && id < PA_BAT0U + 8) {
        int i = id - PA_BAT0U;
        return (i & 1) ? &p->batl[i >> 1] : &p->batu[i >> 1];
    }
    switch (id) {
    case PA_PC:
        return &p->pc;
    case PA_MSR:
        return &p->msr;
    case PA_CR:
        return &p->cr;
    case PA_XER:
        return &p->xer;
    case PA_LR:
        return &p->lr;
    case PA_CTR:
        return &p->ctr;
    case PA_MQ:
        return &p->mq;
    case PA_SRR0:
        return &p->srr0;
    case PA_SRR1:
        return &p->srr1;
    case PA_DEC:
        return &p->dec;
    case PA_RTCU:
        return &p->rtcu;
    case PA_RTCL:
        return &p->rtcl;
    case PA_SDR1:
        return &p->sdr1;
    case PA_FPSCR:
        return &p->fpscr;
    }
    return NULL;
}

static value_t attr_ppc_get(struct object *self, const member_t *m) {
    ppc_t *p = ppc_from(self);
    if (!p)
        return val_err("cpu not initialised");
    uint32_t *slot = ppc_attr_slot(p, (int)(uintptr_t)m->attr.user_data);
    if (!slot)
        return val_err("bad register id");
    value_t v = val_uint(4, *slot);
    v.flags |= VAL_HEX;
    return v;
}

static value_t attr_ppc_set(struct object *self, const member_t *m, value_t in) {
    ppc_t *p = ppc_from(self);
    if (!p)
        return val_err("cpu not initialised");
    int id = (int)(uintptr_t)m->attr.user_data;
    uint32_t *slot = ppc_attr_slot(p, id);
    if (!slot)
        return val_err("bad register id");
    *slot = (uint32_t)in.u;
    // An MSR poke must keep the SoA maps coherent (the §3.5 discipline).
    if (id == PA_MSR) {
        p->msr &= PPC_MSR_MASK;
        ppc_update_active_maps(p);
    }
    return val_none();
}

#define PPC_ATTR(name_, id_)                                                                                           \
    {                                                                                                                  \
        .kind = M_ATTR, .name = name_, .attr = {                                                                       \
            .type = V_UINT,                                                                                            \
            .presentation_flags = VAL_HEX,                                                                             \
            .get = attr_ppc_get,                                                                                       \
            .set = attr_ppc_set,                                                                                       \
            .user_data = (const void *)(uintptr_t)(id_)                                                                \
        }                                                                                                              \
    }

// clang-format off
static const member_t ppc_members[] = {
    PPC_ATTR("pc", PA_PC),       PPC_ATTR("msr", PA_MSR),   PPC_ATTR("cr", PA_CR),     PPC_ATTR("xer", PA_XER),
    PPC_ATTR("lr", PA_LR),       PPC_ATTR("ctr", PA_CTR),   PPC_ATTR("mq", PA_MQ),     PPC_ATTR("srr0", PA_SRR0),
    PPC_ATTR("srr1", PA_SRR1),   PPC_ATTR("dec", PA_DEC),   PPC_ATTR("rtcu", PA_RTCU), PPC_ATTR("rtcl", PA_RTCL),
    PPC_ATTR("sdr1", PA_SDR1),   PPC_ATTR("fpscr", PA_FPSCR),
    PPC_ATTR("r0", PA_GPR0 + 0),   PPC_ATTR("r1", PA_GPR0 + 1),   PPC_ATTR("r2", PA_GPR0 + 2),
    PPC_ATTR("r3", PA_GPR0 + 3),   PPC_ATTR("r4", PA_GPR0 + 4),   PPC_ATTR("r5", PA_GPR0 + 5),
    PPC_ATTR("r6", PA_GPR0 + 6),   PPC_ATTR("r7", PA_GPR0 + 7),   PPC_ATTR("r8", PA_GPR0 + 8),
    PPC_ATTR("r9", PA_GPR0 + 9),   PPC_ATTR("r10", PA_GPR0 + 10), PPC_ATTR("r11", PA_GPR0 + 11),
    PPC_ATTR("r12", PA_GPR0 + 12), PPC_ATTR("r13", PA_GPR0 + 13), PPC_ATTR("r14", PA_GPR0 + 14),
    PPC_ATTR("r15", PA_GPR0 + 15), PPC_ATTR("r16", PA_GPR0 + 16), PPC_ATTR("r17", PA_GPR0 + 17),
    PPC_ATTR("r18", PA_GPR0 + 18), PPC_ATTR("r19", PA_GPR0 + 19), PPC_ATTR("r20", PA_GPR0 + 20),
    PPC_ATTR("r21", PA_GPR0 + 21), PPC_ATTR("r22", PA_GPR0 + 22), PPC_ATTR("r23", PA_GPR0 + 23),
    PPC_ATTR("r24", PA_GPR0 + 24), PPC_ATTR("r25", PA_GPR0 + 25), PPC_ATTR("r26", PA_GPR0 + 26),
    PPC_ATTR("r27", PA_GPR0 + 27), PPC_ATTR("r28", PA_GPR0 + 28), PPC_ATTR("r29", PA_GPR0 + 29),
    PPC_ATTR("r30", PA_GPR0 + 30), PPC_ATTR("r31", PA_GPR0 + 31),
    PPC_ATTR("sr0", PA_SR0 + 0),   PPC_ATTR("sr1", PA_SR0 + 1),   PPC_ATTR("sr2", PA_SR0 + 2),
    PPC_ATTR("sr3", PA_SR0 + 3),   PPC_ATTR("sr4", PA_SR0 + 4),   PPC_ATTR("sr5", PA_SR0 + 5),
    PPC_ATTR("sr6", PA_SR0 + 6),   PPC_ATTR("sr7", PA_SR0 + 7),   PPC_ATTR("sr8", PA_SR0 + 8),
    PPC_ATTR("sr9", PA_SR0 + 9),   PPC_ATTR("sr10", PA_SR0 + 10), PPC_ATTR("sr11", PA_SR0 + 11),
    PPC_ATTR("sr12", PA_SR0 + 12), PPC_ATTR("sr13", PA_SR0 + 13), PPC_ATTR("sr14", PA_SR0 + 14),
    PPC_ATTR("sr15", PA_SR0 + 15),
    PPC_ATTR("bat0u", PA_BAT0U + 0), PPC_ATTR("bat0l", PA_BAT0U + 1), PPC_ATTR("bat1u", PA_BAT0U + 2),
    PPC_ATTR("bat1l", PA_BAT0U + 3), PPC_ATTR("bat2u", PA_BAT0U + 4), PPC_ATTR("bat2l", PA_BAT0U + 5),
    PPC_ATTR("bat3u", PA_BAT0U + 6), PPC_ATTR("bat3l", PA_BAT0U + 7),
};
// clang-format on

const class_desc_t ppc_cpu_class = {
    .name = "ppc",
    .members = ppc_members,
    .n_members = sizeof(ppc_members) / sizeof(ppc_members[0]),
};
