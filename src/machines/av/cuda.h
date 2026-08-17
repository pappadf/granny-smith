// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cuda.h
// Behavioral model of the Apple "Cuda" (68HC05, firmware 2.37, part
// 341S0788) system-management MCU on the AV Quadras — Egret's successor,
// owning ADB, PRAM/RTC, the one-second tick, and power control.  Modelled
// functionally (not as an HC05 core) against BOTH sides of the wire:
//   * host side — OS/CudaMgr.a (SendCudaCmd / CudaShiftRegIRQ / CudaInit)
//   * Cuda side — the firmware disassembly distilled in
//     docs/machines/av/cuda.md §3c (handshake pin map,
//     the 37-entry pseudo-command dispatch with its 12 REJECTED commands,
//     PRAM = 256 bytes at $0100-$01FF, the RTC counter)
//
// Transport: VIA1's shift register (Cuda is the external shift clock) plus
// three port-B pins (via1-cuda.md §2):
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
av_cuda_t *av_cuda_init(struct via *via1, struct rtc *rtc, struct adb *adb, struct scheduler *sched, checkpoint_t *cp);
void av_cuda_delete(av_cuda_t *cuda);
void av_cuda_checkpoint(av_cuda_t *cuda, checkpoint_t *cp);

// === VIA1 transport hooks ===================================================

// The host shifted a byte OUT to Cuda (VIA SR output mode).  Wired from the
// machine's VIA1 shift-out callback.
void av_cuda_via1_shift_input(av_cuda_t *cuda, uint8_t byte);

// The host's VIA1 port-B output changed (TIP/BYTEACK edges).  Wired from
// the machine's VIA1 port-B output callback.
void av_cuda_via1_pb_input(av_cuda_t *cuda, uint8_t port_b);

// === Object-model / test helpers ============================================

// Firmware identity string ("Cuda 2.37").
const char *av_cuda_firmware(const av_cuda_t *cuda);

// Attach the video digitizer's I2C targets (DMSD + VDC) behind
// pseudo-command $22 (wired from av_build_devices once both exist).
void av_cuda_attach_vdc(av_cuda_t *cuda, struct av_vdc *vdc);

#endif // GS_MACHINES_AV_CUDA_H
