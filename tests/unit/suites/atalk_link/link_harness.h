// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// link_harness.h
// The wire and the clock around a real appletalk.c, for the atalk_link suite.
//
// The stack is entered exactly where the SCC enters it -- the frame sink
// appletalk_init installs with scc_set_frame_sink -- and every frame it puts
// on the wire is captured at scc_sdlc_send.  The scheduler is a flat list the
// test owns, so time only moves when a test moves it, and it records which
// event types were registered so a test can ask.

#ifndef LINK_HARNESS_H
#define LINK_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The stack's fixed LLAP node (appletalk.c LLAP_HOST_NODE) and the node the
// tests play the guest from.
#define HOST_NODE  33
#define GUEST_NODE 5

// LLAP types (Inside AppleTalk ch. 1)
#define LLAP_TYPE_DDP_SHORT 0x01
#define LLAP_TYPE_ENQ       0x81
#define LLAP_TYPE_ACK       0x82
#define LLAP_TYPE_RTS       0x84
#define LLAP_TYPE_CTS       0x85

// --- lifecycle ---------------------------------------------------------------

// A fresh stack on a fresh wire and clock: clears the capture, the event list
// and the registration record, then appletalk_init.
void link_boot(void);
// appletalk_delete, then check nothing of the stack's is left queued.
void link_delete(void);

// --- the guest's side of the wire -------------------------------------------

// Hand one raw frame to the stack, as the SCC would.  Fails the test if no
// sink is installed.
void guest_frame(const uint8_t *frame, size_t len);
// One LLAP short-DDP frame from `src_node` to the host.
void guest_ddp(uint8_t src_node, uint8_t dst_sock, uint8_t src_sock, uint8_t ddp_type, const uint8_t *payload,
               size_t plen);
// Move the clock to `t_ns`, firing due events and answering each RTS the
// stack puts out with a CTS (an emulated Mac answers in ~0.5 ms).
void guest_advance_to(double t_ns);

// --- what the stack put on the wire -----------------------------------------

int wire_count(void);
const uint8_t *wire_frame(int i, size_t *len);
// The number of frames of LLAP `type` addressed to `dst`.
int wire_count_type(uint8_t dst, uint8_t type);
// The last short-DDP data frame to `dst` whose DDP type is `ddp_type`, or NULL.
const uint8_t *wire_last_ddp(uint8_t dst, uint8_t ddp_type, size_t *len);
void wire_clear(void);

// --- the clock and the scheduler record ---------------------------------------

double link_now_ns(void);
// Events queued right now.
int sched_pending(void);
// True if "source.event" was registered with scheduler_new_event_type.
bool sched_registered(const char *source_dot_event);
// True if the stack's frame sink is installed on the fake SCC.
bool link_sink_installed(void);

#endif // LINK_HARNESS_H
