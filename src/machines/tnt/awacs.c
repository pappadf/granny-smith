// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// awacs.c
// The TNT AWACS sound face: Grand Central's DAVbus cell at island
// +$14000 (the shared model in core/peripherals/davbus.c, AWACS codec
// variant) with its output on DBDMA channel 8.  This file is the family
// wiring; the register file, codec shadows and channel pacing live in the
// cell.

#include "tnt.h"

#include "davbus.h"

// Bind the cell to this machine: its checkpointed register block, the
// scheduler, the DBDMA engine and the CPU clock the pacing is exact
// against.
static davbus_host_t *awacs_host(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    davbus_host_t *h = &st->awacs_host;
    h->regs = &st->awacs;
    h->sched = cfg->scheduler;
    h->dbdma = st->dbdma;
    h->out_chan = 8;
    h->cpu_hz = cfg->machine->freq;
    h->screamer = false;
    return h;
}

void tnt_awacs_register_events(config_t *cfg) {
    davbus_register_events(awacs_host(cfg));
}

void tnt_awacs_init(config_t *cfg) {
    davbus_init(awacs_host(cfg));
}

void tnt_awacs_reset(config_t *cfg) {
    davbus_reset(&tnt_st(cfg)->awacs_host);
}

void tnt_awacs_teardown(config_t *cfg) {
    davbus_teardown(&tnt_st(cfg)->awacs_host);
}

uint32_t tnt_awacs_read32(config_t *cfg, uint32_t offset) {
    return davbus_read32(&tnt_st(cfg)->awacs_host, offset);
}

void tnt_awacs_write32(config_t *cfg, uint32_t offset, uint32_t value) {
    davbus_write32(&tnt_st(cfg)->awacs_host, offset, value);
}
