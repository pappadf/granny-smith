// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// test.c — m68k-test conformance vectors against the 68000/68030/68040 cores.
//
// The PowerPC core replays powerpc-test (suites/ppc_vectors/); this is the
// 68K equivalent over the sister project.  third-party/m68k-test ships
// per-mnemonic vector files for five family members, generated from the
// m68k-sail formal model, plus a reference runner that IS the executable
// definition of the replay contract (sparse inputs, the three output
// categories, the randomization envelope).  The Makefile compiles that
// runner from the submodule; this file supplies the one piece it asks an
// emulator author to write — the backend `step` below — and an entry point
// that points it at the in-tree smoke tier for the members this emulator has
// a core for (68000, 68030, 68040; the 68010 and 68020 tiers are not run).
//
// What a pass means: agreement with the model, NOT with silicon (m68k-test
// INTEGRATING.md).  What the suite tests independently of that: the read set
// (unlisted state is randomized every replay, so an instruction that
// consults a register it should not fails almost surely) and the write set
// (anything randomized and unlisted must survive the step untouched) — the
// exact failure mode of the predecoded executor's operand pre-extraction,
// which is why CI replays this suite through both executors
// (CPU_TEST_PREDECODE=1 / 0, read by the cpu harness).
//
// A disagreement is a bug report either way — ours or the model's.

#include "runner.h"

#include "cpu.h"
#include "cpu_internal.h"
#include "fpu.h"
#include "harness.h"
#include "memory.h"
#include "mmu040.h"
#include "predecode.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// The runner's CLI, renamed by the Makefile so this file can own main().
int m68k_runner_main(int argc, char **argv);

// The runner's --fault plumbing lives in the dry-run backend we replace;
// main.c still references it (the option is meaningless here and unused).
int dryrun_fault_index = -1;
evalue dryrun_fault_value;

// The sail-harness backend is not linked into this suite.
backend *backend_harness(const char *binary, char *err, size_t errlen) {
    (void)binary;
    snprintf(err, errlen, "the m68k-sail harness backend is not part of this suite");
    return NULL;
}

// Full 24-bit address space, as suites/cpu/ maps it: the vectors' 64 KiB
// window sits at zero and every address they use is inside it.
#define TEST_MEM_SIZE (16 * 1024 * 1024)

static test_context_t *CTX;
static uint8_t *g_mem; // the flat host buffer behind every page

// Executor differential (M68K_VECTORS_DIFF, default on): every replay also
// runs through the executor CPU_TEST_PREDECODE did not select, from the
// same established state, and the two results must agree element for
// element and byte for byte — the proposal's "same guest timeline" oracle
// at instruction granularity, with randomized state, and independent of
// whether either agrees with the model.
static bool g_diff = true;
static bool g_verbose; // M68K_VECTORS_VERBOSE=1: every differing element, and the vector a stray PC came from
static state_t g_other; // the other executor's result (window allocated once)
static uint32_t g_diverged; // replays on which the executors disagreed

// RSTO pulses since the vector's step began: the RESET opcode calls
// system_reset_devices() (the weak no-op in stub_system.c, overridden here).
static uint32_t g_rsto_pulses;
void system_reset_devices(void) {
    g_rsto_pulses++;
}

// === Element indices (resolved once from the runner's canonical table) ===

static int ei_pc, ei_sr_t, ei_sr_s, ei_sr_m, ei_sr_i, ei_sr_x, ei_sr_n, ei_sr_z, ei_sr_v, ei_sr_c;
static int ei_d[8], ei_a[7], ei_usp, ei_isp, ei_msp, ei_vbr, ei_cacr, ei_caar, ei_sfc, ei_dfc;
static int ei_fp[8], ei_fpcr, ei_fpsr, ei_fpiar;
static int ei_tc040, ei_urp, ei_srp, ei_itt0, ei_itt1, ei_dtt0, ei_dtt1, ei_mmusr040;
static int ei_pstate, ei_rsto;

// Resolve one flat element name, aborting on a vocabulary mismatch.
static int ei(const char *name) {
    int i = element_index(name);
    if (i < 0) {
        fprintf(stderr, "[m68k_vectors] runner vocabulary lacks '%s'\n", name);
        exit(2);
    }
    return i;
}

static void resolve_elements(void) {
    ei_pc = ei("pc");
    ei_sr_t = ei("sr.t");
    ei_sr_s = ei("sr.s");
    ei_sr_m = ei("sr.m");
    ei_sr_i = ei("sr.i");
    ei_sr_x = ei("sr.x");
    ei_sr_n = ei("sr.n");
    ei_sr_z = ei("sr.z");
    ei_sr_v = ei("sr.v");
    ei_sr_c = ei("sr.c");
    char name[8];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof name, "d%d", i);
        ei_d[i] = ei(name);
        snprintf(name, sizeof name, "fp%d", i);
        ei_fp[i] = ei(name);
    }
    for (int i = 0; i < 7; i++) {
        snprintf(name, sizeof name, "a%d", i);
        ei_a[i] = ei(name);
    }
    ei_usp = ei("usp");
    ei_isp = ei("isp");
    ei_msp = ei("msp");
    ei_vbr = ei("vbr");
    ei_cacr = ei("cacr");
    ei_caar = ei("caar");
    ei_sfc = ei("sfc");
    ei_dfc = ei("dfc");
    ei_fpcr = ei("fpcr");
    ei_fpsr = ei("fpsr");
    ei_fpiar = ei("fpiar");
    ei_tc040 = ei("tc040");
    ei_urp = ei("urp");
    ei_srp = ei("srp");
    ei_itt0 = ei("itt0");
    ei_itt1 = ei("itt1");
    ei_dtt0 = ei("dtt0");
    ei_dtt1 = ei("dtt1");
    ei_mmusr040 = ei("mmusr040");
    ei_pstate = ei("pstate");
    ei_rsto = ei("rsto");
}

// Element accessors (every element the machine lacks reads as 0 and is
// never compared, so writing it back is harmless).
static uint32_t get32(const state_t *st, int i) {
    return (uint32_t)st->v[i].lo;
}
static void set32(state_t *st, int i, uint32_t v) {
    st->v[i].lo = v;
    st->v[i].hi = 0;
}

// === Memory ================================================================

// Point every page of the harness map at one flat buffer (suites/cpu/ does
// the same): the window is 64 KiB at zero, but the 24-bit space stays
// addressable so a stray access faults nowhere the vector did not expect.
static bool init_test_memory(void) {
    g_mem = calloc(TEST_MEM_SIZE, 1);
    if (!g_mem)
        return false;

    // The buffer replaces the map's image as the code region the predecoded
    // executor may cache (CPU_TEST_PREDECODE=1).
    memory_code_region_register(g_mem, TEST_MEM_SIZE);

    for (int p = 0; p < (TEST_MEM_SIZE >> PAGE_SHIFT); p++) {
        g_page_table[p].host_base = g_mem + (p << PAGE_SHIFT);
        g_page_table[p].dev = NULL;
        g_page_table[p].dev_context = NULL;
        g_page_table[p].writable = true;
        // Write entries through memory_write_fill: the code-page marks must
        // be able to find and suppress them.
        uintptr_t adjusted = (uintptr_t)(g_mem + (p << PAGE_SHIFT)) - ((uint32_t)p << PAGE_SHIFT);
        uintptr_t wadj = memory_write_fill((uint32_t)p, g_mem + (p << PAGE_SHIFT), adjusted);
        g_supervisor_read[p] = adjusted;
        g_supervisor_write[p] = wadj;
        g_user_read[p] = adjusted;
        g_user_write[p] = wadj;
    }
    return true;
}

// === Backend ===============================================================

// Select the core for the file's family member; the FPU pairing is fixed
// per model here (none / 68882 / on-chip), so a file naming another
// pairing is unsupported rather than approximately run.
static bool custom_configure(backend *b, const char *cpu_name, const char *fpu_name, char *err, size_t errlen) {
    (void)b;
    int model;
    const char *fpu_expected;
    if (!strcmp(cpu_name, "68000")) {
        model = CPU_MODEL_68000;
        fpu_expected = "none";
    } else if (!strcmp(cpu_name, "68030")) {
        model = CPU_MODEL_68030;
        fpu_expected = "68882";
    } else if (!strcmp(cpu_name, "68040")) {
        model = CPU_MODEL_68040;
        fpu_expected = "68040";
    } else {
        snprintf(err, errlen, "no %s core in this emulator", cpu_name);
        return false;
    }
    if (strcmp(fpu_name, fpu_expected) != 0) {
        snprintf(err, errlen, "the %s core pairs with fpu %s, not %s", cpu_name, fpu_expected, fpu_name);
        return false;
    }

    // One CPU instance per model; rebuilt only when the file changes model.
    if (CTX->cpu && CTX->cpu->cpu_model == model)
        return true;
    if (CTX->cpu)
        cpu_delete(CTX->cpu);
    CTX->cpu = cpu_init(model, NULL);
    if (!CTX->cpu) {
        snprintf(err, errlen, "cpu_init(%s) failed", cpu_name);
        return false;
    }
    return true;
}

// --- 1. load: establish the vector's input state in the core ---------------
static void load_state(const state_t *st) {
    cpu_t *cpu = CTX->cpu;
    bool has_m = cpu->cpu_model >= CPU_MODEL_68030;

    for (int i = 0; i < 8; i++)
        cpu->d[i] = get32(st, ei_d[i]);
    for (int i = 0; i < 7; i++)
        cpu->a[i] = get32(st, ei_a[i]);

    // SR by field (FORMAT.md §3.2); the M bit exists from the 68020 on.
    uint32_t s = get32(st, ei_sr_s) & 1, m = has_m ? (get32(st, ei_sr_m) & 1) : 0;
    uint16_t sr =
        (uint16_t)(((get32(st, ei_sr_t) & 3) << 14) | (s << 13) | (m << 12) | ((get32(st, ei_sr_i) & 7) << 8) |
                   ((get32(st, ei_sr_x) & 1) << 4) | ((get32(st, ei_sr_n) & 1) << 3) | ((get32(st, ei_sr_z) & 1) << 2) |
                   ((get32(st, ei_sr_v) & 1) << 1) | (get32(st, ei_sr_c) & 1));
    // From a known mode so write_sr's stack swap and SoA repoint are
    // well-defined, then the three stack pointers and the A7 encoding they
    // select (FORMAT.md §3.3) are planted directly.
    cpu->supervisor = 1;
    cpu->m = 0;
    cpu_set_sr(cpu, sr);
    cpu->usp = get32(st, ei_usp);
    cpu->ssp = get32(st, ei_isp);
    cpu->msp = get32(st, ei_msp);
    cpu->a[7] = s ? (m ? cpu->msp : cpu->ssp) : cpu->usp;

    cpu->pc = get32(st, ei_pc);
    cpu->instruction_pc = cpu->pc;
    cpu->vbr = get32(st, ei_vbr);
    cpu->cacr = get32(st, ei_cacr);
    cpu->caar = get32(st, ei_caar);
    cpu->sfc = get32(st, ei_sfc) & 7;
    cpu->dfc = get32(st, ei_dfc) & 7;

    // Execution state: nothing pending, no interrupt request (RUNNING.md §2).
    cpu->ipl = 0;
    cpu->stopped = 0;
    cpu->halted = 0;
    cpu->last_bus_error_pc = 0;
    cpu->bus_error_pending = 0;
    g_rsto_pulses = 0;

    if (cpu->fpu) {
        fpu_state_t *f = (fpu_state_t *)cpu->fpu;
        for (int i = 0; i < 8; i++) {
            // 80-bit image: sign + exponent in the top 16 bits, then the mantissa.
            f->fp[i].exponent = (uint16_t)st->v[ei_fp[i]].hi;
            f->fp[i].mantissa = st->v[ei_fp[i]].lo;
        }
        f->fpcr = get32(st, ei_fpcr);
        f->fpsr = get32(st, ei_fpsr);
        f->fpiar = get32(st, ei_fpiar);
        // No exception is pending from a previous instruction: the vector
        // has no element for that state, so an enabled status bit it
        // randomized into FPSR must not fire as a pre-instruction exception.
        // The core keeps "pending" as FPSR & FPCR minus this mask.
        f->pre_exc_mask = f->fpsr & f->fpcr & 0xFF00u;
        // Likewise no operation in flight: FSAVE writes a NULL frame and
        // FRESTORE of one keeps the programmer model, as from reset.
        f->initialized = false;
        f->exceptional_operand = FP80_ZERO;
    }

    if (cpu->cpu_model == CPU_MODEL_68040 && cpu->mmu) {
        mmu040_state_t *mm = (mmu040_state_t *)cpu->mmu;
        mm->tc = get32(st, ei_tc040);
        mm->enabled = (mm->tc >> 15) & 1; // held at 0 by the core family
        mm->urp = get32(st, ei_urp);
        mm->srp = get32(st, ei_srp);
        mm->itt0 = get32(st, ei_itt0);
        mm->itt1 = get32(st, ei_itt1);
        mm->dtt0 = get32(st, ei_dtt0);
        mm->dtt1 = get32(st, ei_dtt1);
        mm->mmusr = get32(st, ei_mmusr040);
    }

    // The window, instruction already placed by the runner, straight into
    // the host buffer — then the host-write hook, so a block the predecoded
    // executor cached over these bytes last replay is dropped.
    memcpy(g_mem + st->window_base, st->window, st->window_size);
    memory_host_written(g_mem + st->window_base, st->window_size);
}

// --- 2. step: exactly one instruction ---------------------------------------
static void run_one(void) {
    // A budget of 1 retires exactly one instruction; a delivered exception
    // ends the sprint at the vector without running the handler.
    uint32_t budget = 1;
    cpu_run_sprint(CTX->cpu, &budget);
}

// --- 3. store: the core's state back into `st` ------------------------------
static void store_state(state_t *st) {
    cpu_t *cpu = CTX->cpu;
    bool has_m = cpu->cpu_model >= CPU_MODEL_68030;

    for (int i = 0; i < 8; i++)
        set32(st, ei_d[i], cpu->d[i]);
    for (int i = 0; i < 7; i++)
        set32(st, ei_a[i], cpu->a[i]);

    uint16_t sr_out = cpu_get_sr(cpu);
    set32(st, ei_sr_t, (sr_out >> 14) & 3);
    set32(st, ei_sr_s, (sr_out >> 13) & 1);
    set32(st, ei_sr_m, (sr_out >> 12) & 1);
    set32(st, ei_sr_i, (sr_out >> 8) & 7);
    set32(st, ei_sr_x, (sr_out >> 4) & 1);
    set32(st, ei_sr_n, (sr_out >> 3) & 1);
    set32(st, ei_sr_z, (sr_out >> 2) & 1);
    set32(st, ei_sr_v, (sr_out >> 1) & 1);
    set32(st, ei_sr_c, sr_out & 1);

    // A7 is whichever stack pointer the final S/M select; the others sit in
    // their saved slots.
    uint32_t usp = cpu->usp, isp = cpu->ssp, msp = cpu->msp;
    if (cpu->supervisor) {
        if (has_m && cpu->m)
            msp = cpu->a[7];
        else
            isp = cpu->a[7];
    } else {
        usp = cpu->a[7];
    }
    set32(st, ei_usp, usp);
    set32(st, ei_isp, isp);
    set32(st, ei_msp, msp);

    set32(st, ei_pc, cpu->pc);
    set32(st, ei_vbr, cpu->vbr);
    set32(st, ei_cacr, cpu->cacr);
    set32(st, ei_caar, cpu->caar);
    set32(st, ei_sfc, cpu->sfc);
    set32(st, ei_dfc, cpu->dfc);
    set32(st, ei_pstate, cpu->halted ? 2 : cpu->stopped ? 1 : 0);
    set32(st, ei_rsto, g_rsto_pulses);

    if (cpu->fpu) {
        fpu_state_t *f = (fpu_state_t *)cpu->fpu;
        for (int i = 0; i < 8; i++) {
            st->v[ei_fp[i]].hi = f->fp[i].exponent;
            st->v[ei_fp[i]].lo = f->fp[i].mantissa;
        }
        set32(st, ei_fpcr, f->fpcr);
        set32(st, ei_fpsr, f->fpsr);
        set32(st, ei_fpiar, f->fpiar);
    }

    if (cpu->cpu_model == CPU_MODEL_68040 && cpu->mmu) {
        mmu040_state_t *mm = (mmu040_state_t *)cpu->mmu;
        set32(st, ei_tc040, mm->tc);
        set32(st, ei_urp, mm->urp);
        set32(st, ei_srp, mm->srp);
        set32(st, ei_itt0, mm->itt0);
        set32(st, ei_itt1, mm->itt1);
        set32(st, ei_dtt0, mm->dtt0);
        set32(st, ei_dtt1, mm->dtt1);
        set32(st, ei_mmusr040, mm->mmusr);
    }

    memcpy(st->window, g_mem + st->window_base, st->window_size);
}

// Which exception vector (if any) holds `pc` in the window's vector table —
// the tell-tale of an exception the model did not take, since the table's
// other entries are randomized (RUNNING.md §3.3).
static int vector_holding(const state_t *st, uint32_t pc) {
    if (st->window_base != 0 || st->window_size < 0x400)
        return -1;
    for (int v = 0; v < 256; v++) {
        const uint8_t *e = st->window + v * 4;
        uint32_t entry = ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) | ((uint32_t)e[2] << 8) | e[3];
        if (entry == pc)
            return v;
    }
    return -1;
}

// Report the first element or window byte on which the two executors'
// results differ (all of them when verbose); returns false when they agree.
static bool report_divergence(const state_t *a, const state_t *b, const vector_t *vec, const char *name_a,
                              const char *name_b) {
    int n = 0;
    for (size_t i = 0; i < N_ELEMENTS; i++) {
        if (!a->have[i])
            continue;
        if (a->v[i].lo == b->v[i].lo && a->v[i].hi == b->v[i].hi)
            continue;
        char va[40], vb[40];
        format_value(va, sizeof va, ELEMENTS[i].bits, a->v[i]);
        format_value(vb, sizeof vb, ELEMENTS[i].bits, b->v[i]);
        printf("DIVERGE [%s] %s: %s %s=%s %s=%s\n", vec->name, vec->asm_text, ELEMENTS[i].name, name_a, va, name_b, vb);
        if (++n && !g_verbose)
            return true;
    }
    for (uint32_t off = 0; off < a->window_size; off++) {
        if (a->window[off] == b->window[off])
            continue;
        printf("DIVERGE [%s] %s: mem:0x%08X+1 %s=0x%02X %s=0x%02X\n", vec->name, vec->asm_text, a->window_base + off,
               name_a, a->window[off], name_b, b->window[off]);
        if (++n && !g_verbose)
            return true;
    }
    return n > 0;
}

// Verbose: after the step, say where a PC outside the window came from.
static void note_stray_pc(const state_t *st, const vector_t *vec) {
    uint32_t pc = get32(st, ei_pc);
    if (pc - st->window_base < st->window_size)
        return;
    int v = vector_holding(st, pc);
    if (v >= 0)
        printf("NOTE [%s] %s: final pc 0x%08X is exception vector %d's entry\n", vec->name, vec->asm_text, pc, v);
    else
        printf("NOTE [%s] %s: final pc 0x%08X is outside the window\n", vec->name, vec->asm_text, pc);
}

// The one function the runner asks for: load `st`, execute one instruction,
// store the result back into `st` — through the selected executor, and
// (differential mode) through the other one first, from the same state.
static bool custom_step(backend *b, state_t *st, const vector_t *vec, char *err, size_t errlen) {
    (void)b;
    (void)err;
    (void)errlen;
    bool primary = predecode_enabled();

    if (g_diff) {
        if (!g_other.window)
            state_init(&g_other, st->window_base, st->window_size);
        state_copy(&g_other, st);
        predecode_set_enabled(!primary);
        load_state(&g_other);
        run_one();
        store_state(&g_other);
        predecode_set_enabled(primary);
    }

    load_state(st);
    run_one();
    store_state(st);
    if (g_verbose)
        note_stray_pc(st, vec);

    if (g_diff) {
        const char *pd = "predecoded", *sw = "switch";
        if (report_divergence(st, &g_other, vec, primary ? pd : sw, primary ? sw : pd))
            g_diverged++;
    }
    return true;
}

static void custom_close(backend *b) {
    free(b);
}

// Built in the runner's dry-run slot (see the Makefile): the backend it
// constructs by default is this emulator.
backend *m68k_backend_custom(char *err, size_t errlen) {
    (void)err;
    (void)errlen;
    backend *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->name = "granny-smith";
    b->configure = custom_configure;
    b->step = custom_step;
    b->close = custom_close;
    return b;
}

// === Entry point ============================================================

// Where the tier lives: $M68K_VECTORS_DIR (the Makefile's `run` points it at
// the filtered tier, exclusions applied), then the submodule's raw smoke
// tier from wherever make happens to have chdir'd (the same ladder
// suites/cpu/ climbs) — every vector, the excluded ones included.
static const char *find_vectors_dir(void) {
    static char resolved[4096];
    struct stat st;

    const char *env_dir = getenv("M68K_VECTORS_DIR");
    if (env_dir && env_dir[0]) {
        if (stat(env_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(resolved, sizeof resolved, "%s", env_dir);
            return resolved;
        }
        fprintf(stderr, "[m68k_vectors] M68K_VECTORS_DIR=%s is not a directory (run `make run` to build it)\n",
                env_dir);
        return NULL;
    }
    fprintf(stderr,
            "[m68k_vectors] M68K_VECTORS_DIR unset: replaying the raw smoke tier, exclusions.txt not applied\n");

    static const char *const candidates[] = {
        "../../../../third-party/m68k-test/vectors/smoke",
        "../../../third-party/m68k-test/vectors/smoke",
        "../../third-party/m68k-test/vectors/smoke",
        "../third-party/m68k-test/vectors/smoke",
        "third-party/m68k-test/vectors/smoke",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++)
        if (realpath(candidates[i], resolved) && stat(resolved, &st) == 0 && S_ISDIR(st.st_mode))
            return resolved;

    return NULL;
}

// The family members this emulator has a core for, in tier-directory names.
static const char *const members[] = {"68000", "68030", "68040"};

int main(int argc, char **argv) {
    resolve_elements();
    {
        const char *diff_env = getenv("M68K_VECTORS_DIFF");
        g_diff = !(diff_env && diff_env[0] == '0');
        const char *verbose_env = getenv("M68K_VECTORS_VERBOSE");
        g_verbose = verbose_env && verbose_env[0] == '1';
    }

    const char *dir = find_vectors_dir();
    if (!dir) {
        fprintf(stderr, "[m68k_vectors] cannot find the vector tier\n");
        fprintf(stderr, "[m68k_vectors] set M68K_VECTORS_DIR, or run:"
                        " git submodule update --init third-party/m68k-test\n");
        return 1;
    }

    // The cpu harness: the 24-bit map, and CPU_TEST_PREDECODE / CPU_TEST_ELIDE
    // applied to the executors.  Its 68000 is replaced per file by configure().
    CTX = test_harness_init();
    if (!CTX) {
        fprintf(stderr, "[m68k_vectors] test_harness_init failed\n");
        return 1;
    }
    if (!init_test_memory()) {
        fprintf(stderr, "[m68k_vectors] out of memory\n");
        return 1;
    }

    // One runner pass over the three member directories (hand-driven
    // invocations can still pass runner flags: tests/unit/build/m68k_vectors
    // --replays 8 --seed 7 -v).
    size_t nmembers = sizeof members / sizeof members[0];
    char **av = calloc((size_t)argc + nmembers + 1, sizeof *av);
    char **paths = calloc(nmembers, sizeof *paths);
    int n = 0;
    av[n++] = argv[0];
    for (int i = 1; i < argc; i++)
        av[n++] = argv[i];
    for (size_t i = 0; i < nmembers; i++) {
        paths[i] = malloc(strlen(dir) + strlen(members[i]) + 2);
        sprintf(paths[i], "%s/%s", dir, members[i]);
        av[n++] = paths[i];
    }
    av[n] = NULL;

    printf("[m68k_vectors] executor: %s%s\n", predecode_enabled() ? "predecoded" : "switch",
           g_diff ? " (differential against the other executor)" : "");
    int rc = m68k_runner_main(n, av);
    if (g_diff) {
        printf("[m68k_vectors] executor divergences: %u\n", g_diverged);
        if (g_diverged)
            rc = 1;
        state_free(&g_other);
    }

    for (size_t i = 0; i < nmembers; i++)
        free(paths[i]);
    free(paths);
    free(av);
    test_harness_destroy(CTX);
    free(g_mem);
    return rc;
}
