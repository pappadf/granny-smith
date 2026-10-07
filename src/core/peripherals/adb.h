// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// adb.h
// Public interface for Apple Desktop Bus (ADB) transceiver emulation (SE/30).

#ifndef ADB_H
#define ADB_H

// === Includes ===

#include "common.h"
#include "keyboard.h"
#include "scheduler.h"
#include "via.h"

#include <stdbool.h>
#include <stdint.h>

// === Forward Declarations ===

struct adb;
struct object;

// The `machine.adb` bus container node (see docs/internals/core/object/object-model.md).
// Lazily created under machine_object(); keyboard and mouse attach to it as
// named children. Process-singleton, shared by adb.c (keyboard) and mouse.c.
struct object *adb_bus_object(void);

// === Type Definitions ===

// Opaque handle for the ADB transceiver + device state machine
typedef struct adb adb_t;

// === Lifecycle (Constructor / Destructor / Checkpoint) ===

// Creates and initialises an ADB controller instance, wiring it to VIA1
adb_t *adb_init(via_t *via, struct scheduler *scheduler, checkpoint_t *checkpoint);

// Frees all resources associated with an ADB controller instance
void adb_delete(adb_t *adb);

// Power cycle: the ADB bus is powered by the machine, so every device on it
// comes back at its default address and handler with its buffers empty --
// what an ADB SendReset does -- and the transceiver goes idle.  Keys still
// physically held, a latched Caps Lock among them, stay held.
void adb_power_on(adb_t *adb);

// Saves ADB controller state to a checkpoint
void adb_checkpoint(adb_t *restrict adb, checkpoint_t *checkpoint);

// === VIA Callback Hooks ===

// Called by the machine's VIA port-B output callback when ST0/ST1 state lines change
void adb_port_b_output(adb_t *adb, uint8_t value);

// === Operations ===

// Enqueues a host keyboard event in ADB Register 0 format
void adb_keyboard_event(adb_t *adb, key_event_t event, int key);

// The current bus addresses of the keyboard and mouse.  ADB devices MOVE:
// an OS's init may re-address them off their defaults (2 / 3) via Listen R3
// and leave them there — classic Mac OS moves them back, Copland does not —
// so an auto-polling transport must ask the model where its devices are
// now, never assume the power-on addresses.
uint8_t adb_keyboard_address(adb_t *adb);
uint8_t adb_mouse_address(adb_t *adb);

// Updates mouse movement and button state; accumulates deltas until next Talk R0
void adb_mouse_event(adb_t *adb, bool button, int dx, int dy);

// Injects mouse movement deltas without changing the current button state
void adb_mouse_move(adb_t *adb, int dx, int dy);
void adb_mouse_pending(const adb_t *adb, int *dx, int *dy);

// === Auto-poll, as the host sees it =======================================
//
// Every ADB transceiver -- the VIA-side one, Egret, Cuda, the IIfx's SWIM IOP
// -- auto-polls: while the host lets it, it repeats Talk R0 once a poll
// period and passes on the answer when a device has data.  What the host can
// see of that is two things, and the model keeps exactly those:
//
//   - data arriving, no earlier than one Talk after the device had it.  A
//     poll that finds nothing is invisible, and so is the poll's phase, so a
//     transceiver runs no poll clock: it starts a Talk when a device HAS data
//     (adb_set_data_hook) and the data arrives when that Talk would have
//     finished on the bus (adb_talk_ns).
//   - the poll period, as the most often a busy device can report.  While a
//     device keeps having data, a report reaches the host once a period, and
//     the host's cursor code depends on it: the older Mac OS cursor task
//     takes one report per VBL and loses motion when several land in one
//     frame.  So a Talk starts no sooner than a period after the last one
//     (adb_poll_wait_ns).

// The auto-poll period: the VIA transceiver repeats its Talk "every ~11 ms";
// the IIfx's IOP firmware times its polls to ~10 ms; Egret and Cuda take the
// period from the host (SetAutoPollRate, in milliseconds -- Apple's own Cuda
// driver converts microseconds to it, MkLinux asks for 11) and default to it.
#define ADB_POLL_PERIOD_NS 11000000ULL

// How long before a transceiver may start its next auto-poll Talk: none if
// the last one started a period ago or more, else the rest of the period.
static inline uint64_t adb_poll_wait_ns(double now_ns, double last_start_ns, uint64_t period_ns) {
    double due = last_start_ns + (double)period_ns;
    return due <= now_ns ? 0 : (uint64_t)(due - now_ns);
}

// One Talk on the bus with `data_bytes` of reply, from the ADB timing
// specification (Guide to the Macintosh Family Hardware 2e, Table 8-14):
// Attention 800 us, Sync 65 us, eight 100 us command bit cells and a 70 us
// stop bit; the device's 200 us stop-to-start time, then its start bit, its
// data bit cells and its stop bit.  ~3.7 ms for a 2-byte reply.
#define ADB_BIT_NS           100000ULL
#define ADB_ATTENTION_NS     800000ULL
#define ADB_SYNC_NS          65000ULL
#define ADB_STOP_BIT_NS      70000ULL
#define ADB_STOP_TO_START_NS 200000ULL
static inline uint64_t adb_talk_ns(int data_bytes) {
    uint64_t command = ADB_ATTENTION_NS + ADB_SYNC_NS + 8 * ADB_BIT_NS + ADB_STOP_BIT_NS;
    if (data_bytes <= 0)
        return command + ADB_STOP_TO_START_NS; // no device answered
    return command + ADB_STOP_TO_START_NS + ADB_BIT_NS + (uint64_t)data_bytes * 8 * ADB_BIT_NS + ADB_STOP_BIT_NS;
}

// Called whenever a device gets new data (a key transition, mouse motion or a
// button change).  A transceiver outside this file (Egret, Cuda, the IOP)
// starts its Talk from here.  One hook; NULL removes it.
void adb_set_data_hook(adb_t *adb, void (*hook)(void *ctx), void *ctx);

// Whether any device has data a Talk R0 would return -- what an auto-poll
// would find (adb_autopoll_next answers exactly when this is true).
bool adb_has_data(const adb_t *adb);

// How long the Talk that adb_autopoll_next would run now takes on the bus
// (its reply length through adb_talk_ns), or 0 if no device would answer.
// A transceiver schedules the end of its Talk with this.
uint64_t adb_autopoll_talk_ns(const adb_t *adb, uint16_t enable_mask);

// === IOP-based ADB transaction (Macintosh IIfx and friends) ================
//
// On VIA-shift machines (SE/30, IIcx, IIx) the host writes the ADB command
// byte into VIA1's shift register and clocks bytes back via SR interrupts
// (handled by adb_port_b_output above).  On the Macintosh
// IIfx, the SWIM IOP firmware bit-bangs the ADB bus itself — the host
// just posts an ADBMsg on XmtMsg[3] and reads the reply from RcvMsg[3].
//
// adb_iop_transact bridges that protocol to this module's existing device
// state machine: given an ADB command byte plus any Listen-side payload,
// it runs the same dispatch (Talk / Listen / Reset / Flush) and returns
// the Talk reply (if any).
//
// Output buffer must hold up to 8 bytes.  That is the HARDWARE's contract:
// the IOP ADB Driver ERS puts the ADB data field at "zero, or in the range 2
// to 8 bytes", and all callers size `out[8]` accordingly.  The extended
// mouse's Register 0 is 4 bytes and its Register 1 is 8.
//
// Returns true if a device responded with `*out_data_len` reply bytes;
// false for "no device at this address" (= NoReply, the firmware sets
// ADBMSG_FLAG_NOREPLY on the reply).
bool adb_iop_transact(adb_t *adb, uint8_t cmd, const uint8_t *in_data, int in_data_len, uint8_t *out_data,
                      int *out_data_len);

// Bit per ADB address (bit N = address N) of the devices the model actually
// has, at wherever Listen R3 has most recently moved them.  This is the shape
// every transport's device bitmap takes: the IOP ADB Driver ERS's SetPollEnables
// DevMap ("The most significant bit corresponds to device address 15, and the
// least significant bit corresponds to device address 0"), and Cuda's
// RdDevList, which built the same value by hand.
uint16_t adb_device_mask(const adb_t *adb);

// One autonomous auto-poll step, shared by every transport that polls the bus
// on its own: Egret, Cuda and the IIfx's SWIM IOP, which between them used to
// carry four copies of this loop that disagreed four ways.  (The VIA
// transceiver in this file is deliberately NOT one of them -- it repeats the
// last active device and leaves the SRQ scan to the 68k ADB Manager, which is
// what Guide 2e :7798-7808 describes and what the hardware does.)
//
// `enable_mask` is the host's polling-enable bitmap in adb_device_mask's
// layout; 0 means the host has installed none, so every address is eligible.
// On a reply the answering address becomes the most-recently-used one.
//
// Returns true and fills *cmd_out (the Talk R0 that was issued, so the
// transport can tell the host which device answered), out_data and *len_out.
// out_data must hold 8 bytes, the same contract as adb_iop_transact's.
bool adb_autopoll_next(adb_t *adb, uint16_t enable_mask, uint8_t *cmd_out, uint8_t *out_data, int *len_out);

#endif // ADB_H
