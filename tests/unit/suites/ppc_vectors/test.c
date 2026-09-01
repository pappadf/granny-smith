// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// test.c — powerpc-test conformance vectors against the PPC (MPC601) core.
//
// The 68k core replays SingleStepTests (suites/cpu/); this is the PowerPC
// equivalent.  third-party/powerpc-test ships per-mnemonic vector files
// generated from the powerpc-sail formal 601 model, plus a reference runner
// that IS the executable definition of the replay contract (sparse inputs,
// the three output categories, the randomization envelope).  The Makefile
// compiles that runner from the submodule; this file supplies the one piece
// it asks an emulator author to write — the `custom` backend below — and an
// entry point that points it at the in-tree smoke tier, because the unit-test
// orchestrator runs suite binaries with no arguments.
//
// What a pass means: agreement with the model, NOT with silicon.  The model
// is not hardware-validated (powerpc-test INTEGRATING.md §1), and both it and
// this core were derived from the same 601 manual, so a shared misreading
// would agree here.  What the suite does test independently of that: the
// read set (unlisted state is randomized every replay, so an instruction that
// consults a register it should not fails almost surely) and the write set
// (anything randomized and unlisted must survive the step untouched).
//
// A disagreement is a bug report either way — ours or the model's.

// The runner's state.h and our ppc_internal.h both define PPC_MSR_*: bit
// POSITIONS there, bit MASKS here.  The runner's are documented as
// diagnostics-only and nothing below uses them, so they are dropped before
// the core's header lands.
#include "runner.h"

#undef PPC_MSR_EE
#undef PPC_MSR_PR
#undef PPC_MSR_FP
#undef PPC_MSR_ME
#undef PPC_MSR_FE0
#undef PPC_MSR_SE
#undef PPC_MSR_FE1
#undef PPC_MSR_EP
#undef PPC_MSR_IT
#undef PPC_MSR_DT

#include "ppc_internal.h"

#include "harness.h"
#include "memory.h"
#include "predecode.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// The runner's CLI, renamed by the Makefile so this file can own main().
int ppc_runner_main(int argc, char **argv);

// Which model the custom backend builds (TNT proposal §4.5): the corpus is
// generated from the sail 601 model, and main() below replays it twice —
// once as the 601 (every file), once as the 604 with the 601-divergent
// mnemonics filtered out.
static int g_backend_model = CPU_MODEL_PPC601;

// 8 MB of RAM at 0: the vectors' 64 KiB test window ($00100000, recorded in
// each file's provenance) has to be ordinary read/write memory.  ROM sits
// where the ppc suite puts it; no vector reaches it.
#define TEST_RAM_SIZE 0x800000u
#define TEST_ROM_SIZE 0x20000u

static test_context_t *CTX;
static ppc_t *P;

// === Backend lifecycle ======================================================

static int custom_open(ppc_backend *self, char *err, size_t errlen) {
    (void)self;

    // Built by hand, not via test_harness_init(): that harness is Plus-shaped
    // (24-bit), and this needs the 32-bit map (as suites/ppc does).
    CTX = calloc(1, sizeof *CTX);
    if (!CTX) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    CTX->memory = memory_map_init(32, TEST_RAM_SIZE, TEST_ROM_SIZE, NULL);
    if (!CTX->memory) {
        snprintf(err, errlen, "memory_map_init failed");
        return -1;
    }
    memory_populate_pages(CTX->memory, 0x40800000u, 0x40820000u); // plants the RAM/ROM page tables
    test_set_active_context(CTX);

    // CPU_TEST_PREDECODE=1 routes the sprints through the predecoded loop.
    {
        const char *pd_env = getenv("CPU_TEST_PREDECODE");
        predecode_set_enabled(pd_env && pd_env[0] == '1');
    }
    P = ppc_init(NULL, g_backend_model);
    if (!P) {
        snprintf(err, errlen, "ppc_init failed");
        return -1;
    }
    return 0;
}

static void custom_close(ppc_backend *self) {
    (void)self;
    if (P)
        ppc_delete(P);
    if (CTX) {
        test_set_active_context(NULL);
        if (CTX->memory)
            memory_map_delete(CTX->memory);
        free(CTX);
    }
    P = NULL;
    CTX = NULL;
}

// === Memory =================================================================

// The runner hands us sparse 4 KiB pages (the opcode already among them, at
// cia).  Copy them whole in both directions: only changed bytes matter, but
// copying all of them is always correct.
static int load_pages(const ppc_mem *m, char *err, size_t errlen) {
    uint32_t *bases = NULL;
    size_t n = ppc_mem_page_union(m, NULL, &bases);

    for (size_t k = 0; k < n; k++) {
        const ppc_page *pg = ppc_mem_find_page(m, bases[k]);
        if (!pg)
            continue;
        if (pg->base > TEST_RAM_SIZE - PPC_PAGE_SIZE) {
            snprintf(err, errlen, "vector page $%08X is outside the %u MB test RAM", pg->base,
                     (unsigned)(TEST_RAM_SIZE >> 20));
            free(bases);
            return -1;
        }
        for (uint32_t i = 0; i < PPC_PAGE_SIZE; i++)
            memory_write_uint8(pg->base + i, pg->b[i]);
    }
    free(bases);
    return 0;
}

static void store_pages(const ppc_mem *src, ppc_mem *dst) {
    uint32_t *bases = NULL;
    size_t n = ppc_mem_page_union(src, NULL, &bases);

    for (size_t k = 0; k < n; k++) {
        unsigned char page[PPC_PAGE_SIZE];
        for (uint32_t i = 0; i < PPC_PAGE_SIZE; i++)
            page[i] = memory_read_uint8(bases[k] + i);
        ppc_mem_write(dst, bases[k], page, PPC_PAGE_SIZE);
    }
    free(bases);
}

// === The seam ===============================================================

static int custom_step(ppc_backend *self, const ppc_vector *vec, const ppc_state *in, ppc_state *out, char *err,
                       size_t errlen) {
    (void)self;
    (void)vec; // a real emulator never looks at the vector

    // The 601's reservation carries an address; the model's is a valid flag
    // only (DECISIONS.md §1).  A set reservation is therefore not
    // representable here without inventing the address the instruction is
    // about to compute — report it rather than guessing.  No vector in the
    // smoke tier sets one (the envelope holds it false), so this is a guard
    // against a future tier, not a live skip.
    if (ppc_state_get(in, PPC_E_RESERVATION) != 0) {
        snprintf(err, errlen,
                 "initial reservation is set: this core models a reserved ADDRESS, "
                 "which the vector format does not carry");
        return PPC_STEP_UNSUPPORTED;
    }

    // --- 1. load ------------------------------------------------------------

    for (int i = 0; i < 32; i++) {
        P->gpr[i] = (uint32_t)ppc_state_get(in, PPC_E_GPR(i));
        P->fpr[i] = ppc_state_get(in, PPC_E_FPR(i));
    }
    P->cr = ppc_cr_pack(in);
    P->xer = ppc_xer_pack(in);
    P->fpscr = ppc_fpscr_pack(in);

    P->lr = (uint32_t)ppc_state_get(in, PPC_E_LR);
    P->ctr = (uint32_t)ppc_state_get(in, PPC_E_CTR);
    P->mq = (uint32_t)ppc_state_get(in, PPC_E_MQ);
    P->srr0 = (uint32_t)ppc_state_get(in, PPC_E_SRR0);
    P->srr1 = (uint32_t)ppc_state_get(in, PPC_E_SRR1);
    P->dar = (uint32_t)ppc_state_get(in, PPC_E_DAR);
    P->dsisr = (uint32_t)ppc_state_get(in, PPC_E_DSISR);
    for (int i = 0; i < 4; i++)
        P->sprg[i] = (uint32_t)ppc_state_get(in, PPC_E_SPRG(i));

    // Held-constant state; loaded anyway, because the core may read it.
    P->dec = (uint32_t)ppc_state_get(in, PPC_E_DEC);
    P->rtcu = (uint32_t)ppc_state_get(in, PPC_E_RTCU);
    P->rtcl = (uint32_t)ppc_state_get(in, PPC_E_RTCL);
    P->sdr1 = (uint32_t)ppc_state_get(in, PPC_E_SDR1);
    P->ear = (uint32_t)ppc_state_get(in, PPC_E_EAR);
    P->hid0 = (uint32_t)ppc_state_get(in, PPC_E_HID0);
    P->hid1 = (uint32_t)ppc_state_get(in, PPC_E_HID1);
    P->iabr = (uint32_t)ppc_state_get(in, PPC_E_IABR);
    P->dabr = (uint32_t)ppc_state_get(in, PPC_E_DABR);
    P->pir = (uint32_t)ppc_state_get(in, PPC_E_PIR);
    for (int i = 0; i < 4; i++) {
        P->batu[i] = (uint32_t)ppc_state_get(in, PPC_E_BATU(i));
        P->batl[i] = (uint32_t)ppc_state_get(in, PPC_E_BATL(i));
    }
    for (int i = 0; i < 16; i++)
        ppc_set_sr(P, (uint32_t)i, (uint32_t)ppc_state_get(in, PPC_E_SR(i))); // maintains sr_t_mask

    // MSR through the setter: it repoints the supervisor/user SoA maps.
    ppc_set_msr(P, (uint32_t)ppc_state_get(in, PPC_E_MSR));

    P->reserve = 0;
    P->reserve_addr = 0;
    P->ext_irq = 0; // no interrupt source in a vector replay
    P->dec_pending = 0;

    if (load_pages(&in->mem, err, errlen) != 0)
        return PPC_STEP_UNSUPPORTED;

    // Guest memory changed underneath the core, and the BAT/SR/SDR1 writes
    // above bypassed the paths that normally invalidate: drop the fetch
    // window and every cached translation before executing.
    ppc_mmu_invalidate_all(P);

    // --- 2. step ------------------------------------------------------------

    // p->pc is the address of the NEXT instruction to fetch, so it starts at
    // cia and ends at nia.  A budget of 1 retires exactly one instruction:
    // branch folding is excluded on the last budget slot (ppc_run.c), and a
    // delivered exception ends the sprint without running the handler.
    P->pc = (uint32_t)ppc_state_get(in, PPC_E_CIA);
    uint32_t budget = 1;
    ppc_run(P, &budget);

    // --- 3. store -----------------------------------------------------------

    for (int i = 0; i < 32; i++) {
        ppc_state_set(out, PPC_E_GPR(i), P->gpr[i]);
        ppc_state_set(out, PPC_E_FPR(i), P->fpr[i]);
    }
    ppc_cr_unpack(out, P->cr);
    ppc_xer_unpack(out, P->xer);
    ppc_fpscr_unpack(out, P->fpscr);

    ppc_state_set(out, PPC_E_CIA, ppc_state_get(in, PPC_E_CIA)); // unchanged by contract
    ppc_state_set(out, PPC_E_NIA, P->pc);
    ppc_state_set(out, PPC_E_LR, P->lr);
    ppc_state_set(out, PPC_E_CTR, P->ctr);
    ppc_state_set(out, PPC_E_MSR, P->msr);
    ppc_state_set(out, PPC_E_SRR0, P->srr0);
    ppc_state_set(out, PPC_E_SRR1, P->srr1);
    ppc_state_set(out, PPC_E_DAR, P->dar);
    ppc_state_set(out, PPC_E_DSISR, P->dsisr);
    ppc_state_set(out, PPC_E_MQ, P->mq);
    for (int i = 0; i < 4; i++)
        ppc_state_set(out, PPC_E_SPRG(i), P->sprg[i]);
    for (int i = 0; i < 16; i++)
        ppc_state_set(out, PPC_E_SR(i), P->sr[i]);
    for (int i = 0; i < 4; i++) {
        ppc_state_set(out, PPC_E_BATU(i), P->batu[i]);
        ppc_state_set(out, PPC_E_BATL(i), P->batl[i]);
    }
    ppc_state_set(out, PPC_E_SDR1, P->sdr1);
    ppc_state_set(out, PPC_E_EAR, P->ear);
    ppc_state_set(out, PPC_E_HID0, P->hid0);
    ppc_state_set(out, PPC_E_HID1, P->hid1);
    ppc_state_set(out, PPC_E_IABR, P->iabr);
    ppc_state_set(out, PPC_E_DABR, P->dabr);
    ppc_state_set(out, PPC_E_PIR, P->pir);
    ppc_state_set(out, PPC_E_DEC, P->dec);
    ppc_state_set(out, PPC_E_RTCU, P->rtcu);
    ppc_state_set(out, PPC_E_RTCL, P->rtcl);
    ppc_state_set(out, PPC_E_RESERVATION, P->reserve ? 1 : 0);

    store_pages(&in->mem, &out->mem);

    return PPC_STEP_OK;
}

ppc_backend ppc_backend_custom = {"custom", "the Granny Smith MPC601 core", custom_open, custom_step, custom_close,
                                  NULL};

// === Entry point ============================================================

// Where the tier lives, tried in the same order as suites/cpu/ does for
// SingleStepTests: $PPC_VECTORS_DIR, then the submodule from wherever make
// happens to have chdir'd.
static const char *find_vectors_dir(void) {
    static char resolved[4096];
    struct stat st;

    const char *env_dir = getenv("PPC_VECTORS_DIR");
    if (env_dir && env_dir[0]) {
        if (stat(env_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(resolved, sizeof resolved, "%s", env_dir);
            return resolved;
        }
    }

    static const char *const candidates[] = {
        "../../../../third-party/powerpc-test/vectors/smoke",
        "../../../third-party/powerpc-test/vectors/smoke",
        "../../third-party/powerpc-test/vectors/smoke",
        "../third-party/powerpc-test/vectors/smoke",
        "third-party/powerpc-test/vectors/smoke",
    };
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++)
        if (realpath(candidates[i], resolved) && stat(resolved, &st) == 0 && S_ISDIR(st.st_mode))
            return resolved;

    return NULL;
}

// Mnemonic files whose replay legitimately diverges under the 604 model
// (TNT proposal §4.5: "the 601-only encodings masked out"): the POWER
// holdovers and MQ/RTC SPR moves trap; mtmsr/rfi mask differently; the
// word-alignment classes and dcbz's cache-disabled rule fault where the
// sail 601 model completes.  Everything else must replay bit-identically.
static const char *const skip_604[] = {
    // POWER holdovers (program exception on the 604)
    "abs", "clcs", "div", "divs", "doz", "dozi", "lscbx", "maskg", "maskir", "mul", "nabs", "rlmi", "rrib", "sle",
    "sleq", "sliq", "slliq", "sllq", "slq", "sraiq", "sraq", "sre", "srea", "sreq", "sriq", "srliq", "srlq", "srq",
    // per-model SPR map / MSR mask
    "mfspr", "mtspr", "mtmsr", "rfi",
    // 604 word-alignment classes (604UM §4.5.6)
    "lfd", "lfdu", "lfdux", "lfdx", "lfs", "lfsu", "lfsux", "lfsx", "stfd", "stfdu", "stfdux", "stfdx", "stfs", "stfsu",
    "stfsux", "stfsx", "lmw", "stmw", "lwarx", "stwcx_dot", "eciwx", "ecowx",
    // dcbz gates on HID0[DCE] (cleared in the vectors' fixed HID0)
    "dcbz"};

static int skip_under_604(const char *name) {
    for (size_t i = 0; i < sizeof skip_604 / sizeof skip_604[0]; i++)
        if (strcmp(name, skip_604[i]) == 0)
            return 1;
    return 0;
}

// Run one pass of the runner over `files` (count `nfiles`), passing the
// caller's extra argv through.
static int run_pass(char *argv0, int argc, char **argv, char **files, int nfiles) {
    char **av = calloc((size_t)argc + (size_t)nfiles + 4, sizeof *av);
    int n = 0;
    av[n++] = argv0;
    av[n++] = (char *)"--backend";
    av[n++] = (char *)"custom";
    for (int i = 1; i < argc; i++)
        av[n++] = argv[i];
    for (int i = 0; i < nfiles; i++)
        av[n++] = files[i];
    av[n] = NULL;
    int rc = ppc_runner_main(n, av);
    free(av);
    return rc;
}

int main(int argc, char **argv) {
    const char *dir = find_vectors_dir();
    if (!dir) {
        fprintf(stderr, "[ppc_vectors] cannot find the vector tier\n");
        fprintf(stderr, "[ppc_vectors] set PPC_VECTORS_DIR, or run:"
                        " git submodule update --init third-party/powerpc-test\n");
        return 1;
    }

    // Pass 1 — the 601 over the whole tier (hand-driven invocations can
    // still pass runner flags: tests/unit/build/ppc_vectors --verbose
    // --filter add).
    g_backend_model = CPU_MODEL_PPC601;
    char *tier[1] = {(char *)dir};
    printf("[ppc_vectors] 601 pass\n");
    int rc = run_pass(argv[0], argc, argv, tier, 1);
    if (rc != 0)
        return rc;

    // Pass 2 — the 604 over the model-shared subset.
    DIR *dp = opendir(dir);
    if (!dp) {
        fprintf(stderr, "[ppc_vectors] cannot enumerate %s\n", dir);
        return 1;
    }
    char **files = NULL;
    int nfiles = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        const char *suffix = strstr(de->d_name, ".json");
        if (!suffix)
            continue;
        char base[128];
        snprintf(base, sizeof base, "%.*s", (int)(suffix - de->d_name), de->d_name);
        if (skip_under_604(base))
            continue;
        if (nfiles == cap) {
            cap = cap ? cap * 2 : 64;
            files = realloc(files, (size_t)cap * sizeof *files);
        }
        char *path = malloc(strlen(dir) + strlen(de->d_name) + 2);
        sprintf(path, "%s/%s", dir, de->d_name);
        files[nfiles++] = path;
    }
    closedir(dp);

    g_backend_model = CPU_MODEL_PPC604;
    printf("[ppc_vectors] 604 pass (%d files; %d model-divergent mnemonics skipped)\n", nfiles,
           (int)(sizeof skip_604 / sizeof skip_604[0]));
    rc = run_pass(argv[0], argc, argv, files, nfiles);
    for (int i = 0; i < nfiles; i++)
        free(files[i]);
    free(files);
    return rc;
}
