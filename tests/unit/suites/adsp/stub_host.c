// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Host stubs for the ADSP protocol suite.
//
// appletalk_adsp.c carries both the protocol engine and the production
// instance that binds it to DDP and to the scheduler.  The suite drives the
// engine directly through adsp_stack_new(), so the production side only has
// to link: nothing here is ever called.

#include "appletalk_internal.h"

#include <stdint.h>

double scheduler_time_ns(void *scheduler) {
    (void)scheduler;
    return 0.0;
}

void atalk_timer_init(atalk_timer_t *t, const char *source_name, const char *event_name, atalk_timer_fn cb) {
    (void)t, (void)source_name, (void)event_name, (void)cb;
}
void atalk_timer_arm(atalk_timer_t *t, uint64_t data, uint64_t delay_ns) {
    (void)t, (void)data, (void)delay_ns;
}
void atalk_timer_cancel(atalk_timer_t *t, uint64_t data) {
    (void)t, (void)data;
}
void atalk_timer_cancel_all(atalk_timer_t *t) {
    (void)t;
}

int atalk_ddp_send_to(const atalk_socket_addr_t *dest, uint8_t src_socket, uint8_t ddp_type, const uint8_t *data,
                      int len) {
    (void)dest;
    (void)src_socket;
    (void)ddp_type;
    (void)data;
    (void)len;
    return -1;
}
