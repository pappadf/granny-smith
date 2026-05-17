// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iop.c
// Macintosh IIfx I/O Processor (Apple 343S1021 PIC) — host-visible
// register and mailbox model.  This file is the GENERIC ASIC behaviour:
// host register decode, iopStatCtl semantics, iopRamAddr/iopRamData
// access, the IRQ wiring out to OSS.
//
// The 65C02 firmware that would normally run inside the PIC is replaced
// by per-kind behavioural models — iop_scc.c for the SCC IOP, iop_swim.c
// for the SWIM IOP.  This file selects the appropriate model and invokes
// its callbacks at the right moments (RUN-bit 0→1, host-kick, etc.).
//
// Register and command-byte names are exposed via iop_regs.h, mailbox
// geometry via iop.h; both match the System ROM's IOP Manager
// conventions.

#include "iop_internal.h"

#include "log.h"
#include "system.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("iop");

// ============================================================================
//  IRQ plumbing
// ============================================================================

// Drives the IOP `hint` line based on whether any host-visible interrupt
// bits are asserted in iopStatCtl.
static void iop_update_host_irq(iop_t *iop) {
    bool active = (iop->stat_ctl & (iopInt0ActiveBit | iopInt1ActiveBit | iopBypassIntReqBit)) != 0;
    if (active == iop->host_irq)
        return;
    iop->host_irq = active;
    if (iop->irq_cb)
        iop->irq_cb(iop->cb_context, active);
}

void iop_raise_int0(iop_t *iop) {
    iop->stat_ctl |= iopInt0ActiveBit;
    iop_update_host_irq(iop);
}

void iop_raise_int1(iop_t *iop) {
    iop->stat_ctl |= iopInt1ActiveBit;
    iop_update_host_irq(iop);
}

// ============================================================================
//  Mailbox helpers
// ============================================================================

// The IOP Manager seeds each mailbox bank's [0].state byte to MaxIopMsgNum
// (= $07) at install time; the firmware uses that value as the loop bound
// when scanning slots in its host-kick handler (see SWIM $5070, SCC $0521).
static void iop_init_mailbox(iop_t *iop) {
    iop->ram[IOPXmtMsgBase] = MaxIopMsgNum;
    iop->ram[IOPRcvMsgBase] = MaxIopMsgNum;
    for (int i = 1; i <= MaxIopMsgNum; i++) {
        iop->ram[IOPXmtMsgBase + i] = MsgIdle;
        iop->ram[IOPRcvMsgBase + i] = MsgIdle;
    }
    // The IOP Manager polls $031F for $FF after writing setIopRun.  The
    // firmware later writes 'X' ($58) here on every main-loop iteration,
    // but $FF on entry is enough to release the IOP Manager's alive-wait
    // loop.
    iop->ram[IOPAliveAddr] = 0xff;
}

bool iop_post_reply(iop_t *iop, int slot, const uint8_t *payload, unsigned len, bool use_int1) {
    if (slot < 1 || slot > MaxIopMsgNum)
        return false;
    if (len > MaxIopMsgLen)
        len = MaxIopMsgLen;
    uint32_t base = IOPMsgPayload(IOPRcvMsgBase, slot);
    if (payload && len)
        memcpy(&iop->ram[base], payload, len);
    iop->ram[IOPRcvMsgBase + IOPMsgState(slot)] = NewMsgSent;
    if (use_int1)
        iop_raise_int1(iop);
    else
        iop_raise_int0(iop);
    return true;
}

void iop_complete_xmt(iop_t *iop, int slot) {
    if (slot < 1 || slot > MaxIopMsgNum)
        return;
    iop->ram[IOPXmtMsgBase + IOPMsgState(slot)] = MsgCompleted;
    iop_raise_int0(iop);
}

// ============================================================================
//  Firmware-image checksum (verification only — never blocks the boot)
//
// FNV-1a 32-bit hash of the 64 KB image after RUN-bit 0→1.  Compared
// against the behaviour table's expected_fnv1a; mismatches are logged.
// Used to detect "wrong ROM" or "we accidentally model an image that
// doesn't match what the host is loading" situations early.
// ============================================================================

static uint32_t iop_fnv1a32(const uint8_t *data, size_t n) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

// Verify IOP RAM contents after a RUN-bit 0→1 transition matches the
// behavioural model's expected firmware image hash.
//
// IOP firmware loading is multi-stage on real hardware:
//   1. Host writes a small bypass-mode bootstrap into the IOP's vector
//      area (~18 bytes at $7FEE-$7FFF: enable bypass via $F030, halt).
//      All three 65C02 vectors point at the bootstrap.  Host sets RUN.
//   2. Host enters bypass mode, drives the front-side device directly.
//      May re-flip bypass settings (a single byte at $7FF4 changes
//      between successive runs).
//   3. Eventually the host loads the FULL firmware image ($0400-$1020+
//      for SCC, $5000-$7506 for SWIM) and sets RUN again.  This is the
//      hash we capture in the behaviour table.
//
// So mismatches on the early stages are EXPECTED — they reflect the
// intermediate bypass-bootstrap images, not the full firmware.  Only
// the FINAL stage should match.  We log mismatches at level 2 (off by
// default) and the success at level 2; if the boot lands somewhere
// broken the user can enable iop logging to inspect the stage trail.
static void iop_verify_firmware(iop_t *iop) {
    uint32_t got = iop_fnv1a32(iop->ram, IOP_RAM_SIZE);
    if (iop->behavior->expected_fnv1a == 0) {
        LOG(1, "%s firmware loaded (fnv1a=$%08x — no expected hash configured)", iop->behavior->name, got);
        return;
    }
    if (got == iop->behavior->expected_fnv1a) {
        LOG(2, "%s firmware verified ($%08x)", iop->behavior->name, got);
        return;
    }
    LOG(2, "%s firmware stage hash $%08x (expected final $%08x) — intermediate load stage", iop->behavior->name, got,
        iop->behavior->expected_fnv1a);
}

// ============================================================================
//  Bypass-mode passthrough
//
// When the host has set the bypass-mode bit (which on real silicon would
// suspend the 65C02 and route the +$20..+$3F window to the front-side
// device), we forward reads/writes to the device's memory_interface_t.
// ============================================================================

// Converts SCC IOP bypass offsets to the SCC chip's 4-register address
// space.  The SCC's A/B × D/C universal bus is decoded from bits 0,1 of
// the offset; the IIfx wires the SCC at /CS such that only offsets
// $20,$22,$24,$26 are meaningful (rest are aliases — see IIFX_IOPS.md
// §5.1).
static int scc_bypass_addr(uint32_t offset) {
    switch (offset & 0x3e) {
    case 0x20:
        return 0;
    case 0x22:
        return 2;
    case 0x24:
        return 4;
    case 0x26:
        return 6;
    default:
        return -1;
    }
}

static uint8_t iop_bypass_read(iop_t *iop, uint32_t offset) {
    if (!iop->bypass_iface || !iop->bypass_device)
        return 0xff;
    if (iop->behavior->kind == SccIopNum) {
        int addr = scc_bypass_addr(offset);
        if (addr >= 0)
            return iop->bypass_iface->read_uint8(iop->bypass_device, (uint32_t)addr);
        return 0xff;
    }
    return iop->bypass_iface->read_uint8(iop->bypass_device, offset - iopBypassBase);
}

static void iop_bypass_write(iop_t *iop, uint32_t offset, uint8_t value) {
    if (!iop->bypass_iface || !iop->bypass_device)
        return;
    if (iop->behavior->kind == SccIopNum) {
        int addr = scc_bypass_addr(offset);
        if (addr >= 0)
            iop->bypass_iface->write_uint8(iop->bypass_device, (uint32_t)addr, value);
        return;
    }
    iop->bypass_iface->write_uint8(iop->bypass_device, offset - iopBypassBase, value);
}

// ============================================================================
//  Host-side register decode
//
// The IIfx wires the PIC's 8-bit register bank onto every other byte of
// the 32-bit bus, with each register also aliased to its "+1" partner.
// The decoder below maps the $1FFF-masked offset to the register class
// reached by that byte, matching the canonical host-side layout:
//   $00 → iopRamAddrH      (aliased at $01 as the high half of word view)
//   $02 → iopRamAddrL      (low byte / low half of word view)
//   $04 → iopStatCtl
//   $08 → iopRamData       (auto-incrementing if iopIncEnable set)
//   $20+ → iopBypassBase   (passthrough to front-side device)
// ============================================================================

typedef enum iop_reg_class {
    IOP_REG_ADDR_HI,
    IOP_REG_ADDR_LO,
    IOP_REG_STATUS,
    IOP_REG_RAM_DATA,
    IOP_REG_BYPASS,
    IOP_REG_NONE,
} iop_reg_class_t;

static iop_reg_class_t iop_decode_offset(uint32_t offset) {
    uint32_t r = (offset & 0x1fffu) >> 1;
    if (r & 0x10u)
        return IOP_REG_BYPASS;
    if (r & 0x04u)
        return IOP_REG_RAM_DATA;
    if (r & 0x02u)
        return IOP_REG_STATUS;
    if (r & 0x01u)
        return IOP_REG_ADDR_LO;
    return IOP_REG_ADDR_HI;
}

// ============================================================================
//  Host-side reads
// ============================================================================

static uint8_t iop_read_uint8(void *device, uint32_t addr) {
    iop_t *iop = (iop_t *)device;
    uint32_t offset = addr & 0x1fff;
    switch (iop_decode_offset(offset)) {
    case IOP_REG_ADDR_HI:
        return (uint8_t)(iop->ram_addr >> 8);
    case IOP_REG_ADDR_LO:
        return (uint8_t)iop->ram_addr;
    case IOP_REG_STATUS:
        // iopSCCWrReq (bit 7) is a read-only live signal reflecting the
        // front-side device's /REQ line.  It reads as 1 (inactive) when
        // no SCC TX/RX request is pending, which is the steady state
        // outside an active bypass transaction.  IOPMgr's status reads
        // depend on this bit being 1 in the absence of front-side
        // activity, so we OR it in unconditionally.
        return (uint8_t)(iop->stat_ctl | iopSCCWrReqBit);
    case IOP_REG_RAM_DATA: {
        uint8_t value = iop->ram[iop->ram_addr];
        if (iop->stat_ctl & iopIncEnableBit)
            iop->ram_addr++;
        return value;
    }
    case IOP_REG_BYPASS:
        return iop_bypass_read(iop, offset);
    case IOP_REG_NONE:
    default:
        return 0xff;
    }
}

static uint16_t iop_read_uint16(void *device, uint32_t addr) {
    uint16_t hi = iop_read_uint8(device, addr);
    uint16_t lo = iop_read_uint8(device, addr + 1);
    return (uint16_t)((hi << 8) | lo);
}

static uint32_t iop_read_uint32(void *device, uint32_t addr) {
    uint32_t hi = iop_read_uint16(device, addr);
    uint32_t lo = iop_read_uint16(device, addr + 2);
    return (hi << 16) | lo;
}

// ============================================================================
//  Host writes to iopStatCtl
//
// Per IIFX_IOPS.md §3.1, the write semantics are layered:
//   1. iopRun edge-detected — only re-drives reset line on transition.
//   2. iopGenInterrupt — write-1 kicks the 65C02 (firmware host-kick IRQ).
//   3. iopInt0Active / iopInt1Active — write-1-to-clear.
//   4. Composed: iopIncEnable + iopRun stored; bits 4-7 preserved unless
//      explicitly cleared above.
// ============================================================================

static void iop_write_stat_ctl(iop_t *iop, uint8_t value) {
    uint8_t old = iop->stat_ctl;

    // 1. Layer the write through the W1C semantics for INT0/INT1 first:
    //    preserve pending bits but clear any that were set in `value`.
    uint8_t pending = iop->stat_ctl & (iopInt0ActiveBit | iopInt1ActiveBit | iopBypassIntReqBit);
    pending &= (uint8_t) ~(value & (iopInt0ActiveBit | iopInt1ActiveBit));

    // 2. Compose: store iopInBypassMode/iopIncEnable/iopRun + carry pending.
    iop->stat_ctl = (uint8_t)((value & (iopInBypassModeBit | iopIncEnableBit | iopRunBit)) | pending);

    // 3. RUN-bit 0→1 transition: the host has just released the 65C02
    //    from reset.  Seed the canonical mailbox state, verify firmware
    //    image, then hand off to the per-IOP behavioural model.
    if ((old & iopRunBit) == 0 && (iop->stat_ctl & iopRunBit)) {
        iop_verify_firmware(iop);
        iop_init_mailbox(iop);
        if (iop->behavior && iop->behavior->on_run_start)
            iop->behavior->on_run_start(iop);
    }

    // 4. RUN cleared: host-side interrupt bits go away.
    if ((iop->stat_ctl & iopRunBit) == 0)
        iop->stat_ctl &= (uint8_t) ~(iopInt0ActiveBit | iopInt1ActiveBit | iopBypassIntReqBit);

    // 5. iopGenInterrupt is edge-only: every write of `1` kicks the
    //    firmware.  Hand off to the behavioural model.  (Bit not stored.)
    if (value & iopGenInterruptBit) {
        if (iop->behavior && iop->behavior->on_host_kick)
            iop->behavior->on_host_kick(iop);
    }

    iop_update_host_irq(iop);
}

// ============================================================================
//  Host-side writes
// ============================================================================

static void iop_write_uint8(void *device, uint32_t addr, uint8_t value) {
    iop_t *iop = (iop_t *)device;
    uint32_t offset = addr & 0x1fff;
    switch (iop_decode_offset(offset)) {
    case IOP_REG_ADDR_HI:
        iop->ram_addr = (uint16_t)((iop->ram_addr & 0x00ffu) | ((uint16_t)value << 8));
        return;
    case IOP_REG_ADDR_LO:
        iop->ram_addr = (uint16_t)((iop->ram_addr & 0xff00u) | value);
        return;
    case IOP_REG_STATUS:
        iop_write_stat_ctl(iop, value);
        return;
    case IOP_REG_RAM_DATA:
        // Trace writes to the mailbox state bytes (host-side _IOPMsgRequest
        // and IOPInterrupt manipulate these via the iopRamAddr/Data window).
        // Useful for protocol-level debugging when wiring up new behaviour.
        if ((iop->ram_addr >= IOPXmtMsgBase && iop->ram_addr <= IOPXmtMsgBase + MaxIopMsgNum) ||
            (iop->ram_addr >= IOPRcvMsgBase && iop->ram_addr <= IOPRcvMsgBase + MaxIopMsgNum)) {
            LOG(4, "%s ram[$%04x] := $%02x (mailbox state)", iop->behavior->name, iop->ram_addr, value);
        }
        iop->ram[iop->ram_addr] = value;
        if (iop->stat_ctl & iopIncEnableBit)
            iop->ram_addr++;
        return;
    case IOP_REG_BYPASS:
        iop_bypass_write(iop, offset, value);
        return;
    case IOP_REG_NONE:
    default:
        return;
    }
}

static void iop_write_uint16(void *device, uint32_t addr, uint16_t value) {
    iop_write_uint8(device, addr, (uint8_t)(value >> 8));
    iop_write_uint8(device, addr + 1, (uint8_t)value);
}

static void iop_write_uint32(void *device, uint32_t addr, uint32_t value) {
    iop_write_uint16(device, addr, (uint16_t)(value >> 16));
    iop_write_uint16(device, addr + 2, (uint16_t)value);
}

// ============================================================================
//  Lifecycle
// ============================================================================

iop_t *iop_init(iop_kind_t kind, const memory_interface_t *bypass_iface, void *bypass_device, iop_irq_fn irq_cb,
                void *context, struct scheduler *scheduler, checkpoint_t *checkpoint) {
    iop_t *iop = calloc(1, sizeof(*iop));
    if (!iop)
        return NULL;

    iop->bypass_iface = bypass_iface;
    iop->bypass_device = bypass_device;
    iop->irq_cb = irq_cb;
    iop->cb_context = context;
    iop->scheduler = scheduler;
    iop->behavior = (kind == SccIopNum) ? &iop_scc_behavior : &iop_swim_behavior;
    iop->stat_ctl = 0; // 65C02 held in reset (iopRun = 0)
    iop_init_mailbox(iop);

    iop->memory_interface = (memory_interface_t){
        .read_uint8 = iop_read_uint8,
        .read_uint16 = iop_read_uint16,
        .read_uint32 = iop_read_uint32,
        .write_uint8 = iop_write_uint8,
        .write_uint16 = iop_write_uint16,
        .write_uint32 = iop_write_uint32,
    };

    if (checkpoint) {
        system_read_checkpoint_data(checkpoint, iop->ram, sizeof(iop->ram));
        system_read_checkpoint_data(checkpoint, &iop->ram_addr, sizeof(iop->ram_addr));
        system_read_checkpoint_data(checkpoint, &iop->stat_ctl, sizeof(iop->stat_ctl));
    }

    return iop;
}

void iop_delete(iop_t *iop) {
    free(iop);
}

void iop_checkpoint(iop_t *iop, checkpoint_t *checkpoint) {
    if (!iop || !checkpoint)
        return;
    system_write_checkpoint_data(checkpoint, iop->ram, sizeof(iop->ram));
    system_write_checkpoint_data(checkpoint, &iop->ram_addr, sizeof(iop->ram_addr));
    system_write_checkpoint_data(checkpoint, &iop->stat_ctl, sizeof(iop->stat_ctl));
}

const memory_interface_t *iop_get_memory_interface(iop_t *iop) {
    return iop ? &iop->memory_interface : NULL;
}
