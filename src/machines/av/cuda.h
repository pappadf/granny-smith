// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cuda.h
// Behavioral model of the Apple "Cuda" (68HC05, firmware 2.37, part
// 341S0788) system-management MCU on the AV Quadras — Egret's successor,
// owning ADB, PRAM/RTC, the one-second tick, and power control.  Modelled
// functionally (not as an HC05 core) against BOTH sides of the wire:
//   * host side — OS/CudaMgr.a (SendCudaCmd / CudaShiftRegIRQ / CudaInit)
//   * Cuda side — the firmware disassembly distilled in
//     docs/internals/machines/av/cuda.md (handshake pin map,
//     the 37-entry pseudo-command dispatch with its 12 REJECTED commands,
//     PRAM = 256 bytes at $0100-$01FF, the RTC counter)
//
// Transport: VIA1's shift register (Cuda is the external shift clock) plus
// three port-B pins:
//   * vCudaTREQ    (PB3, host input)  — active-LOW: Cuda holds it low while
//     it owns the bus / has bytes to send, raises it with the last byte.
//   * vCudaBYTEACK (PB4, host output) — a LEVEL toggled once per byte
//     ("byte serviced, next please"); also the sync-cycle strobe.
//   * vCudaTIP     (PB5, host output) — active-LOW transaction-in-progress.
//
// Distinct from Egret: the BYTEACK toggle (vs viaFull pulse), the inverted
// TIP polarity (vs sysSes active-high), the sync cycle CudaInit runs
// (ByteAck asserted while TIP negated), and the trailing "idle acknowledge"
// byte Cuda clocks after every terminated response — the host spin-waits
// on it (CudaMgr.a <SM6>).

#ifndef GS_MACHINES_AV_CUDA_H
#define GS_MACHINES_AV_CUDA_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

struct via;
struct rtc;
struct adb;
struct scheduler;
struct av_vdc;

struct av_cuda;
typedef struct av_cuda av_cuda_t;

// === Lifecycle ==============================================================

// Create a Cuda instance.  `via1` carries the SR + PB3/PB4/PB5 transport;
// `rtc` backs the clock + PRAM; `adb` backs ADB packets; `sched` drives the
// 1-second tick, autopoll, and the delayed byte pushes the sync protocol
// needs.  Restores plain-data state from `cp` when non-NULL.
// mode3_clock enables the Mode3Clock RdTime one-second tick (the tick carries
// the RTC). Enable only for machines with a live guest clock that needs a real
// seed (the PDM family); the AV Quadras pass false to keep the bare tick.
av_cuda_t *av_cuda_init(struct via *via1, struct rtc *rtc, struct adb *adb, struct scheduler *sched, checkpoint_t *cp,
                        bool mode3_clock);
void av_cuda_delete(av_cuda_t *cuda);

// The machine was power-cycled.  Cuda itself stays powered (it is what turns
// the machine on, and it keeps the clock and PRAM), but the host side of the
// VIA1 handshake lost power under it: the transport returns to idle with no
// transaction, response or push in flight, and the asynchronous sources go
// off until the next sync enables them -- the state av_cuda_init leaves.
void av_cuda_host_power_cycle(av_cuda_t *cuda);
void av_cuda_checkpoint(av_cuda_t *cuda, checkpoint_t *cp);

// === VIA1 transport hooks ===================================================

// The host shifted a byte OUT to Cuda (VIA SR output mode).  Wired from the
// machine's VIA1 shift-out callback.
// The VIA1 glue is the same on every machine that wires this transport to
// VIA1: port B is the handshake, the shift register is a command byte, and
// only the port test and a NULL check stand between the VIA callback and
// the transport.  Five machines each carried their own copy of that,
// and they had already drifted -- two tested the machine-state pointer for
// NULL and two did not.  Both entry points below are NULL-tolerant, and
// av_cuda_via1_port_output takes the port number, so a machine's callback is one
// line and cannot get the port wrong.
//
// Registering the transport pointer as the VIA callback context, which
// would remove the shim entirely, is NOT available: via_init takes one
// cb_context for all three callbacks, and the IRQ callback needs cfg on
// every one of these machines (av_via1_irq -> av_update_ipl((config_t *)
// context), same shape on PDM and TNT), as does the IIsi's port-A output
// for the ROM overlay.  A vtable returning the pair runs into the same
// wall.  This is the available consolidation, not the preferred one.
void av_cuda_via1_port_output(av_cuda_t *cuda, uint8_t port, uint8_t value);

void av_cuda_via1_shift_input(av_cuda_t *cuda, uint8_t byte);

// The host's VIA1 port-B output changed (TIP/BYTEACK edges).  Wired from
// the machine's VIA1 port-B output callback.
void av_cuda_via1_pb_input(av_cuda_t *cuda, uint8_t port_b);

// === Object-model / test helpers ============================================

// Firmware identity string ("Cuda 2.37").

// Attach the video digitizer's I2C targets (DMSD + VDC) behind
// pseudo-command $22 (wired from av_build_devices once both exist).
void av_cuda_attach_vdc(av_cuda_t *cuda, struct av_vdc *vdc);

// A machine-level I2C WRITE target behind pseudo-command $22, consulted
// before the digitizer bus: `slave` is the wire address (bit 0 clear =
// write), `data` the bytes after it (subaddress first).  Returns true when
// the slave is its own.  The TNT's Control pixel-clock synthesiser (device
// $50) is the one such target; a write nobody claims stays the header-only
// acknowledgement it always was.
typedef bool (*av_cuda_i2c_write_fn)(void *ctx, uint8_t slave, const uint8_t *data, int len);
void av_cuda_attach_i2c_write(av_cuda_t *cuda, av_cuda_i2c_write_fn fn, void *ctx);

// A machine-level I2C BUS behind pseudo-commands $22 and $25: every
// transaction goes to it, and a slave neither hook claims is a NAK (an
// error packet, which is how firmware learns a device is absent).  `rd`
// fills `out` with up to `max` bytes read from `addr8` (bit 0 set; with
// `has_sub`, from subaddress `sub`) and returns the count, or -1 for a
// NAK.  Takes precedence over the digitizer bus and the write hook.
typedef int (*av_cuda_i2c_read_fn)(void *ctx, uint8_t addr8, bool has_sub, uint8_t sub, uint8_t *out, int max);
void av_cuda_attach_i2c_bus(av_cuda_t *cuda, av_cuda_i2c_read_fn rd, av_cuda_i2c_write_fn wr, void *ctx);

// Model the 341S0060 part with firmware 2.40 (the beige G3's): the
// dispatch table extends to $26, adding $25 COMBINED_FORMAT_IIC and the
// undocumented $26.  Wiring, not guest state.
void av_cuda_set_firmware_240(av_cuda_t *cuda);

#endif // GS_MACHINES_AV_CUDA_H
