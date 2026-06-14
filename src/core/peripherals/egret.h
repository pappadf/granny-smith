// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// egret.h
// Public interface for the Apple "Egret" companion chip used by the Macintosh
// IIsi (and the LC family).  Egret is a 65C02-based microcontroller that owns
// ADB, the real-time clock, parameter RAM, the 1-second tick, soft power-off,
// and the audio DFAC gain.  We model it functionally — not the 65C02 core —
// reproducing only the host-visible wire protocol.
//
// Egret talks to the host over VIA1's 8-bit shift register, clocked by Egret,
// plus three port-B handshake pins (Apple names, verbatim from EgretEqu.h):
//   * xcvrSes (PB3, host input)  — Egret session line, active-LOW: Egret pulls
//                                  it low while it owns the bus / has bytes to
//                                  send, raises it high on its last byte.
//   * viaFull (PB4, host output) — host "byte serviced, send the next".
//   * sysSes  (PB5, host output) — host "I am running a transaction".
// vACR bit 4 (SRdir) selects shift-in (0, receiving from Egret) vs shift-out
// (1, transmitting to Egret); vIFR bit 2 (ifSR) fires once per byte.
//
// The machine wires Egret in by: routing VIA1's shift-out callback to
// egret_via1_shift_input(), routing VIA1's port-B output callback to
// egret_via1_pb_input(), and letting Egret drive PB3 (via_input) and push
// bytes back (via_input_sr) on the supplied via1 handle.  ADB transactions are
// delegated to adb.c (adb_iop_transact), and clock/PRAM to rtc.c.
//
// Protocol reference: OS/EgretMgr.a (EgretInit / SendEgretCmd / EgretDispatch /
// ShiftRegIRQ) and OS/I2C/EgretEqu.h in the local Apple source mirror.

#ifndef EGRET_H
#define EGRET_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

struct via;
struct rtc;
struct adb;
struct scheduler;

struct egret;
typedef struct egret egret_t;

// === Lifecycle ==============================================================

// Create an Egret instance.  `via1` is the host VIA whose shift register and
// PB3/PB4/PB5 carry the Egret transport; `rtc` backs the clock + PRAM
// pseudo-commands; `adb` backs ADB packet transactions; `sched` drives the
// autonomous 1-second tick and ADB autopoll timers.  Restores plain-data
// state from `cp` when non-NULL.
egret_t *egret_init(struct via *via1, struct rtc *rtc, struct adb *adb, struct scheduler *sched, checkpoint_t *cp);
void egret_delete(egret_t *eg);
void egret_checkpoint(egret_t *eg, checkpoint_t *cp);

// === VIA1 transport hooks ===================================================

// The host shifted a byte OUT to Egret (VIA SR in output mode, mode 7).  Wired
// from the machine's VIA1 shift-out callback.
void egret_via1_shift_input(egret_t *eg, uint8_t byte);

// The host's VIA1 port-B output changed.  `port_b` is the VIA's masked port-B
// output (only output pins are meaningful).  Egret tracks PB4/PB5 edges to pace
// the transfer.  Wired from the machine's VIA1 port-B output callback.
void egret_via1_pb_input(egret_t *eg, uint8_t port_b);

// === Configuration ==========================================================

// Soft-power-off callback — fires on the Egret PwrDown ($0A) pseudo-command.
void egret_set_power_off_callback(egret_t *eg, void (*cb)(void *ctx), void *ctx);

// === Object-model / test helpers ============================================

// Firmware identity string ("Egret8").
const char *egret_firmware(const egret_t *eg);

// Force a 1-second tick packet now (test helper; normally autonomous).
void egret_force_tick(egret_t *eg);

#endif // EGRET_H
