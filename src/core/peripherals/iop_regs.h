// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iop_regs.h
// Register and command-byte names for the Macintosh IIfx I/O Processor
// PIC (part 343S1021), as seen from the 68030 host.  Names match the
// conventions used by the System ROM's IOP Manager (host-side PIC
// register window, status-bit names, composed command bytes for
// iopStatCtl).  Mailbox geometry is exposed in iop.h alongside the
// public API.
//
// The PIC also has 64 bytes of on-die registers at $F010-$F04F (timer,
// DMA, IRQ mask/status, HostIntReq, front-side device passthrough).
// Those are *invisible to the 68030 host* — they are accessed only by
// the 65C02 firmware running inside the PIC, and our behavioural model
// doesn't emulate that firmware as code.  We model the host-visible
// consequences of what the firmware would do (e.g. raising Int0/Int1
// via iop_raise_int0/int1, scheduling timer ticks via the scheduler).

#ifndef IOP_REGS_H
#define IOP_REGS_H

// ============================================================================
//  Host-side PIC register window
//
// The PIC's 8-bit data port sits on one byte lane of the 32-bit bus; each
// logical register is therefore exposed at every-other-byte offsets.  The
// IIfx wires the bus so that consecutive byte offsets pair onto the same
// register (e.g. $50F12008 and $50F12009 both hit iopRamData).
// ============================================================================

// Bus-byte offsets within the PIC's $2000-byte aperture (the
// "$20 addressing-mode trick" used in the System ROM stripped).
#define iopRamAddrH 0x00 // high byte of the 16-bit shared-RAM pointer
#define iopRamAddr  0x01 // word view of the pointer (hi at $01, lo at $02)
#define iopRamAddrL 0x02 // low byte of the shared-RAM pointer
#define iopStatCtl  0x04 // status/control (R: status, W: command)
#define iopRamData  0x08 // R/W at iopRamAddr; auto-increments if iopIncEnable

// Peripheral-passthrough aperture (bypass-mode only).  Sixteen byte-wide
// slots at +$20..+$3F that bypass the firmware and pass directly through
// to the front-side device (Z8530 SCC / SWIM1).
#define iopBypassBase 0x20
#define iopBypassEnd  0x3F

// ============================================================================
//  iopStatCtl bit shifts
//
// The same byte is read and written, but with different semantics per bit.
// On a write:
//   - iopRun is edge-detected (drives the reset line only on transition).
//   - iopGenInterrupt is not stored — writing 1 generates a host-kick at
//     the 65C02 and clears immediately.
//   - iopInt0Active / iopInt1Active are write-1-to-clear.
//   - iopIncEnable is stored.
//   - The bypass / passthrough bits are read-only on the host side.
// ============================================================================

#define iopInBypassMode 0 // 1 = IOP is in bypass mode (mirrored from on-die $F030 bit 0)
#define iopIncEnable    1 // address-pointer auto-increment for iopRamData
#define iopRun          2 // 0 = hold 65C02 in reset, 1 = run
#define iopGenInterrupt 3 // write-1 generates a host-kick IRQ at the 65C02
#define iopInt0Active   4 // 65C02 → host interrupt source 0 (W1C)
#define iopInt1Active   5 // 65C02 → host interrupt source 1 (W1C)
#define iopBypassIntReq 6 // peripheral /INT (bypass-mode only, read-only)
#define iopSCCWrReq     7 // SCC WREQ / device /REQ live signal (read-only; 0 = active)

// Convenience masks — useful when manipulating iopStatCtl by name.
#define iopInBypassModeBit ((uint8_t)(1u << iopInBypassMode))
#define iopIncEnableBit    ((uint8_t)(1u << iopIncEnable))
#define iopRunBit          ((uint8_t)(1u << iopRun))
#define iopGenInterruptBit ((uint8_t)(1u << iopGenInterrupt))
#define iopInt0ActiveBit   ((uint8_t)(1u << iopInt0Active))
#define iopInt1ActiveBit   ((uint8_t)(1u << iopInt1Active))
#define iopBypassIntReqBit ((uint8_t)(1u << iopBypassIntReq))
#define iopSCCWrReqBit     ((uint8_t)(1u << iopSCCWrReq))

// ============================================================================
//  Named command bytes for iopStatCtl
//
// The IOP Manager writes these canonical values to iopStatCtl at various
// boot / runtime points (firmware load, RUN release, interrupt ack, etc.).
// ============================================================================

// $06 — enable address-pointer auto-increment, keep IOP running
#define setIopIncEnable ((uint8_t)(iopIncEnableBit | iopRunBit))

// $04 — disable auto-increment, keep IOP running
#define clrIopIncEnable ((uint8_t)(iopRunBit))

// $16 — ack iopInt0Active, keep IOP running with auto-increment
#define clrIopInt0 ((uint8_t)(iopInt0ActiveBit | iopRunBit | iopIncEnableBit))

// $26 — ack iopInt1Active, keep IOP running with auto-increment
#define clrIopInt1 ((uint8_t)(iopInt1ActiveBit | iopRunBit | iopIncEnableBit))

// $0E — kick the IOP (host-side host-kick interrupt; bit 3 is edge-only)
#define setIopGenInt ((uint8_t)(iopGenInterruptBit | iopRunBit | iopIncEnableBit))

// $32 — hold IOP in reset, clear pending host ints, auto-increment on
//       (used at the start of firmware load)
#define resetIopRun ((uint8_t)(iopInt0ActiveBit | iopInt1ActiveBit | iopIncEnableBit))

// $36 — release IOP from reset, clear pending host ints, auto-increment on
//       (used at the end of firmware load)
#define setIopRun ((uint8_t)(iopRunBit | iopInt0ActiveBit | iopInt1ActiveBit | iopIncEnableBit))

#endif // IOP_REGS_H
