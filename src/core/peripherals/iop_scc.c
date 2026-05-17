// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iop_scc.c
// Behavioural model of the SCC IOP's 65C02 firmware.  Replaces the 6502
// core itself with C code that produces the same host-visible effects.
//
// The SCC IOP fronts the Zilog Z8530 dual-channel serial controller
// (modem on channel A, printer on channel B).  The firmware's job is:
//
//   1. At reset, enable bypass mode ($F030 bit 0 = 1).  This tells the
//      host's iopStatCtl.iopInBypassMode that the host may drive the
//      Z8530 directly through the +$20..+$3F passthrough window.  The
//      IOP Manager uses this during early boot before the IOP firmware
//      is ready to handle the SCC autonomously.
//
//   2. Once the host posts an XmtMsg to leave bypass mode, the firmware
//      programs the Z8530 (init table at $078A in the firmware image),
//      enables its two DMA channels (one per Z8530 channel), and routes
//      per-channel TX/RX events back to the host via Int0.
//
// What our behavioural model covers:
//
//   - Boot-time bypass enable, so the host's IOPMgr sees iopInBypassMode
//     on its first iopStatCtl read.
//   - Generic "ack and ignore" host-kick handler that marks any
//     NewMsgSent slot as MsgCompleted (firmware's $0521 fast path).
//
// What it does NOT cover yet:
//
//   - The actual SCC byte-driver (no serial throughput in/out the IOP
//     side; if you need Z8530 emulation, use bypass mode and let the
//     host drive the chip directly).
//   - The Z8530 init table; only the bypass entry/exit messages.
//
// For the IIfx boot stall, only the bypass enable + alive-handshake
// matters — we replicate the firmware's reset-entry behaviour at the
// host-visible level only.

#include "iop_internal.h"

#include "log.h"

LOG_USE_CATEGORY_NAME("iop_scc");

// On RUN-bit 0→1: firmware's $040E reset entry (annotated source).
//
// We replicate the firmware-visible end state:
//   - iopInBypassMode set in iopStatCtl  (host can drive SCC directly)
//   - All mailbox slots idle  (iop.c's iop_init_mailbox already did this)
//   - $031F = $FF                       (alive flag, already set)
//
// The firmware's internal state (zero-page variables, SCC init table,
// timer setup, etc.) is invisible to the host, so we skip it.
static void iop_scc_on_run_start(iop_t *iop) {
    iop->stat_ctl |= iopInBypassModeBit;
    LOG(2, "SCC IOP: firmware started, bypass mode enabled");
}

// On host-kick: walk XmtMsg looking for NewMsgSent slots and mark them
// complete.  This is the firmware's $0521 host-kick handler condensed:
// we don't actually parse the mailbox command (no serial driver), we
// just acknowledge so the host doesn't spin waiting for a response that
// will never come.
//
// If the host needs richer per-command behaviour we'd dispatch here on
// XmtMsg[slot].msg[0] — but accept-and-ack is sufficient for IIfx boot
// through Finder (the host has no SCC traffic during the boot path).
static void iop_scc_on_host_kick(iop_t *iop) {
    for (int slot = 1; slot <= MaxIopMsgNum; slot++) {
        uint8_t state = iop->ram[IOPXmtMsgBase + IOPMsgState(slot)];
        if (state == NewMsgSent) {
            uint32_t base = IOPMsgPayload(IOPXmtMsgBase, slot);
            LOG(3, "SCC IOP: XmtMsg[%d] req=$%02x $%02x $%02x — acking", slot, iop->ram[base], iop->ram[base + 1],
                iop->ram[base + 2]);
            iop_complete_xmt(iop, slot);
        }
    }
}

const iop_behavior_t iop_scc_behavior = {
    .name = "SCC IOP",
    .kind = SccIopNum,
    // FNV-1a32 of iop-scc.bin captured 2026-05-16 from a IIfx ROM boot.
    .expected_fnv1a = 0x752d244au,
    .on_run_start = iop_scc_on_run_start,
    .on_host_kick = iop_scc_on_host_kick,
};
