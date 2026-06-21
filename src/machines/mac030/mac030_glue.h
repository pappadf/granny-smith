// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac030_glue.h
// Shared GLUE-family lifecycle leaves: the IRQ→IPL routing and the teardown
// delete-chain, byte-identical across SE/30, IIcx and IIx (proposal §1.1,
// "teardown chain (~275)").  The I/O dispatcher lives in mac030_glue_io.h.

#ifndef GS_MACHINES_MAC030_GLUE_H
#define GS_MACHINES_MAC030_GLUE_H

#include "checkpoint.h"
#include "system_config.h"

#include <stdbool.h>

struct adb;
struct asc;
struct floppy;
struct mmu_state;

// The II-family construction prefix shared by every GLUE machine: build the
// memory map, the CPU (model read FROM THE PROFILE — closing the §1.3 drift
// where every init hardcoded CPU_MODEL_68030), the scheduler, and its
// frequency/CPI.  The caller continues with any machine-specific scheduler
// event types, IRQ-state restore, and device construction.
void mac030_build_core(config_t *cfg, checkpoint_t *cp);

// Shared GLUE IRQ callbacks: route the SCC / VIA1 / VIA2 interrupt line to
// the CPU IPL via mac030_glue_update_ipl.  Identical across se30/iicx/iix.
void mac030_glue_scc_irq(void *context, bool active);
void mac030_glue_via1_irq(void *context, bool active);
void mac030_glue_via2_irq(void *context, bool active);

// IRQ source bits driven into cfg->irq.  GLUE routes them to fixed IPLs:
// VIA1→1, VIA2→2, SCC→4, NMI→7.  The per-machine SE30_IRQ_* / IICX_IRQ_*
// aliases carry the same values and remain valid `source` arguments.
#define MAC030_GLUE_IRQ_VIA1 (1 << 0)
#define MAC030_GLUE_IRQ_VIA2 (1 << 1)
#define MAC030_GLUE_IRQ_SCC  (1 << 2)
#define MAC030_GLUE_IRQ_NMI  (1 << 3)

// Set/clear an IRQ source bit and re-derive the CPU IPL (highest active wins).
void mac030_glue_update_ipl(config_t *cfg, int source, bool active);

// Family-shared teardown delete-chain: scheduler_stop → mmu → floppy → asc →
// adb → scsi → via2 → via1 → scc → rtc → scheduler → cpu → mem_map → debugger.
// The machine-owned devices (which live in its private state, not config_t)
// are passed in; the caller frees its own state struct afterwards.  Any NULL
// handle is skipped.
void mac030_glue_teardown(config_t *cfg, struct adb *adb, struct asc *asc, struct floppy *floppy,
                          struct mmu_state *mmu);

#endif // GS_MACHINES_MAC030_GLUE_H
