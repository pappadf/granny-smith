// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_checkpoint.h
// The one config_t-owned checkpoint prefix, shared by every machine family
// whose construction order allows it.
//
// This is `mac030_checkpoint_save_core()` renamed and generalised
// (05-chipsets-irq F-18).  The finding asks for "a single
// system_checkpoint_common"; the helper already served five of the nine
// families, and the `mac030_` prefix -- plus a header comment saying the
// PowerPC families do not use it -- was most of what stopped the other four
// adopting it.  It is a rename and an extension, not a second helper.
//
// Nothing is parameterised, because nothing needed to be: every difference
// between the families falls out of which handles the config actually has.
// A PowerPC machine has cfg->ppc and no cfg->cpu, so it takes the ppc block
// and skips cfg->irq (PDM and TNT keep interrupt state in their own register
// blobs and never read cfg->irq at all).  A machine with one VIA passes
// straight through the second, because via_checkpoint(NULL) writes nothing.
//
// THE ORDER IS THE CONTRACT.  The checkpoint stream is positional and there
// is no version field -- a build mismatch is rejected outright, so layout is
// free to change, but a save and its restore must move together.  The
// restore side is each family's init, where rtc_init, scc_init and
// appletalk_init consume their own block as they construct, so save order
// must mirror construction order.  A swapped pair does not fail at the swap:
// it cross-loads and dies later at whichever block first disagrees on size,
// which is exactly what the IIfx did.
//
// NOT every family: the Lisa's construction order genuinely differs -- it has
// no RTC (the COPS does that job), no AppleTalk at this point, and writes the
// SCC last, after the VIAs and the ProFile.  Forcing it into this order would
// mean reordering lisa_init's construction for no gain.  It keeps its own
// save function and says so there.

#ifndef GS_MACHINES_RUNTIME_MACHINE_CHECKPOINT_H
#define GS_MACHINES_RUNTIME_MACHINE_CHECKPOINT_H

struct config;
struct checkpoint;

// Write the core blocks every adopting family shares, in this order:
//
//   memory map -> cpu | ppc -> scheduler -> [cfg->irq, 68k only] ->
//   RTC -> SCC -> AppleTalk -> VIA1 -> [VIA2]
//
// The family then appends its own devices and its substrate tail, in its
// own construction order.
void machine_checkpoint_save_core(struct config *cfg, struct checkpoint *cp);

#endif // GS_MACHINES_RUNTIME_MACHINE_CHECKPOINT_H
