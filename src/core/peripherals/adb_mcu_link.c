// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// adb_mcu_link.c
// The ADB half Egret and Cuda share.  See adb_mcu_link.h.

#include "adb_mcu_link.h"

int adb_mcu_put_header(uint8_t *tx, uint8_t pkt_type, uint8_t flags, uint8_t cmd) {
    tx[0] = 0x00; // attention byte (discarded by the host)
    tx[1] = pkt_type;
    tx[2] = flags;
    tx[3] = cmd;
    return 4;
}

int adb_mcu_adb_reply(adb_t *adb, const uint8_t *rx, int rx_len, uint8_t *tx, int tx_max, bool *replied, int *out_len) {
    uint8_t cmd = (rx_len >= 2) ? rx[1] : 0;
    uint8_t out[8];
    int n_out = 0;
    bool ok = false;
    // Clamp here, the way both chips' pseudo-command paths do: a truncated
    // packet (rx_len 0 or 1) otherwise passes -2 or -1 as the length.
    // adb_iop_transact absorbs it, but the guard belongs at the call site.
    int data_len = rx_len - 2;
    if (data_len < 0)
        data_len = 0;
    if (adb)
        ok = adb_iop_transact(adb, cmd, &rx[2], data_len, out, &n_out);

    int n = adb_mcu_put_header(tx, ADB_MCU_PKT_ADB, ok ? 0 : ADB_MCU_FLAG_TIMEOUT, cmd);
    for (int i = 0; i < n_out && n < tx_max; i++)
        tx[n++] = out[i];
    *replied = ok;
    *out_len = n_out;
    return n;
}

void adb_mcu_autopoll_arm(adb_mcu_autopoll_t *ap, bool enabled, adb_t *adb, struct scheduler *sched, uint8_t period_ms,
                          event_callback_t talk_done, void *source) {
    if (!enabled || !adb || ap->talk_pending)
        return;
    // The device-selection rules live in adb.c, shared with the SWIM IOP too.
    // Neither chip models a host-set polling mask (Egret has no WrDevList;
    // Cuda's host can read the device list but not set one), so 0 means
    // every address is eligible, and the scan finds the devices wherever
    // Listen R3 has most recently moved them.
    uint64_t talk_ns = adb_autopoll_talk_ns(adb, 0);
    if (!talk_ns)
        return; // no device has data
    // The Talk starts once the host's poll period is up since the last one.
    uint64_t period = period_ms ? (uint64_t)period_ms * 1000000ULL : ADB_POLL_PERIOD_NS;
    double now = scheduler_time_ns(sched);
    uint64_t wait = adb_poll_wait_ns(now, ap->talk_start_ns, period);
    ap->talk_start_ns = now + (double)wait;
    ap->talk_pending = true;
    scheduler_new_cpu_event(sched, talk_done, source, 0, 0, wait + talk_ns);
}

bool adb_mcu_autopoll_collect(adb_mcu_autopoll_t *ap, bool enabled, adb_t *adb) {
    ap->talk_pending = false;
    if (!enabled || !adb || ap->len)
        return false; // turned off meanwhile: the data stays in the device
    uint8_t cmd;
    uint8_t out[8];
    int out_len = 0;
    if (adb_autopoll_next(adb, 0, &cmd, out, &out_len)) {
        int n = adb_mcu_put_header(ap->pkt, ADB_MCU_PKT_ADB, ADB_MCU_FLAG_AUTOPOLL, cmd);
        for (int i = 0; i < out_len && n < ADB_MCU_AP_MAX; i++)
            ap->pkt[n++] = out[i];
        ap->len = n;
    }
    return true;
}
