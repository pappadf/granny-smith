// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// adb_mcu_link.h
// The ADB half Egret and Cuda share: the two Apple system-controller MCUs run
// the same host protocol (EgretMgr.a drives both), so they build the same
// response header, answer an ADB command packet the same way, and run the
// same auto-poll -- a Talk R0 on the bus once a device has data, the reply
// waiting as an unsolicited adbPkt until the link to the host is free.
//
// What stays per chip (egret.c, cuda.c): the port-B handshake pins, the
// transport state machine and its timing, the pseudo-command tables, the
// reply to an unknown packet type, and what happens to a parked auto-poll
// packet when the link is busy (Egret retries, Cuda drops a due
// re-presentation).  Those are firmware differences, not copies.

#ifndef ADB_MCU_LINK_H
#define ADB_MCU_LINK_H

#include "adb.h"
#include "scheduler.h"

#include <stdbool.h>
#include <stdint.h>

// Packet types (EgretEqu.h / egretequ.a; the same values on both parts).
#define ADB_MCU_PKT_ADB    0x00 // adbPkt
#define ADB_MCU_PKT_PSEUDO 0x01 // pseudoPkt
#define ADB_MCU_PKT_ERROR  0x02 // errorPkt
#define ADB_MCU_PKT_TICK   0x03 // tickPkt — 1-second tick

// ADB response status flags.
#define ADB_MCU_FLAG_TIMEOUT  (1u << 1) // the addressed device had no data
#define ADB_MCU_FLAG_AUTOPOLL (1u << 6) // the data came from an auto-poll

// The auto-poll packet: the 4-byte response header plus at most 8 data bytes.
#define ADB_MCU_AP_MAX 16

// Auto-poll state.  Plain data, so it can sit inside a chip's checkpointed
// prefix.  Whether auto-poll is on and the host's poll period stay the
// chip's (its pseudo-commands set and read them).
typedef struct adb_mcu_autopoll {
    uint8_t pkt[ADB_MCU_AP_MAX]; // the packet waiting for the link to the host
    int len; // its length; 0 = none waiting
    bool talk_pending; // a Talk is running on the bus
    double talk_start_ns; // when the last one started
} adb_mcu_autopoll_t;

// Lay down the 4-byte response header [attn, pktType, flags, cmd]; returns 4.
int adb_mcu_put_header(uint8_t *tx, uint8_t pkt_type, uint8_t flags, uint8_t cmd);

// Answer a completed ADB command packet (rx = [adbPkt, adbCmd, listen...])
// against the device state machine: the reply packet goes into `tx` (at most
// `tx_max` bytes) and its length is returned.  `*replied` says whether a
// device answered and `*out_len` how many data bytes it gave.
int adb_mcu_adb_reply(adb_t *adb, const uint8_t *rx, int rx_len, uint8_t *tx, int tx_max, bool *replied, int *out_len);

// Start the next auto-poll Talk if auto-poll is `enabled`, no Talk is
// running and a device has data: once the host's poll period (`period_ms`,
// 0 = the ADB default) is up since the last one, plus the Talk's own bus
// time.  `talk_done` (registered by the chip on `source`) fires when it
// completes.  The caller has already handled a packet still waiting.
void adb_mcu_autopoll_arm(adb_mcu_autopoll_t *ap, bool enabled, adb_t *adb, struct scheduler *sched, uint8_t period_ms,
                          event_callback_t talk_done, void *source);

// The Talk has completed (call from `talk_done`): build the unsolicited
// adbPkt from the device's reply.  False when auto-poll was turned off
// meanwhile or a packet is already waiting -- the data stays in the device
// and the chip has nothing to service; true otherwise (even when no device
// answered), and the chip services its link.
bool adb_mcu_autopoll_collect(adb_mcu_autopoll_t *ap, bool enabled, adb_t *adb);

#endif // ADB_MCU_LINK_H
