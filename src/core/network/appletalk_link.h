// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_link.h
// The router's links to other instances' routers (appletalk_router.c).
//
// A link is a point-to-point port between two Granny Smith routers: a
// half-router link in Inside AppleTalk's terms (5-4), with no network number
// of its own.  What carries it is a transport -- in-process for tests, a
// socket in the headless build, a BroadcastChannel between tabs or a WebRTC
// data channel in the browser -- behind the small interface below.  The
// router frames everything it sends as records:
//
//   HELLO  version, the sender's network number, link flags, display name
//   DDP    one datagram: extended DDP header + data, as on the wire
//   PING   a guest-time stamp; PONG echoes it back
//   BYE    an orderly close, with the reason
//
// Routing itself rides in DDP records as ordinary RTMP and ZIP datagrams,
// so the link layer knows nothing about topology.  Everything here runs on
// the worker thread: a transport delivers what it receives from there.

#ifndef APPLETALK_LINK_H
#define APPLETALK_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Record kinds (first byte of every record)
#define ATALK_LINK_HELLO 1
#define ATALK_LINK_DDP   2
#define ATALK_LINK_PING  3
#define ATALK_LINK_PONG  4
#define ATALK_LINK_BYE   5

#define ATALK_LINK_VERSION 1 // the HELLO's protocol version
#define ATALK_LINK_MAX     16 // links one router holds at once
#define ATALK_LINK_REC_MAX 640 // the longest record: a kind byte and a whole datagram

// HELLO flags
#define ATALK_LINK_FLAG_FRIEND 0x01 // the link reaches another person (not a tab of this browser)

// How the router sends on a link.  `send` hands one record to the transport;
// `reliable` asks for in-order, retransmitted delivery (control records),
// otherwise best effort will do (DDP: the protocols above retransmit).
// `close` tells the transport the router is done with the link; it must not
// call back into the link afterwards.
typedef struct {
    void (*send)(void *ctx, const uint8_t *rec, size_t len, bool reliable);
    void (*close)(void *ctx);
} atalk_link_transport_t;

typedef struct atalk_link atalk_link_t;

typedef enum { ATALK_LINK_CONNECTING, ATALK_LINK_OPEN, ATALK_LINK_CLOSED } atalk_link_state_t;

// Open a link over `transport` (ctx is its own).  `kind` names the transport
// ("tabs", "webrtc", "socket", "loopback"); `friend_link` says the other end
// is another person's instance, which is what decides whether
// share_host_services applies.  The router sends its HELLO at once.  Returns
// NULL when every link slot is taken.
atalk_link_t *atalk_link_open(const atalk_link_transport_t *transport, void *ctx, const char *kind, bool friend_link);

// One record arrived from the other end.
void atalk_link_receive(atalk_link_t *link, const uint8_t *rec, size_t len);

// The transport lost the other end (or was asked to stop): the link closes
// and its routes go bad.  `reason` is shown to the user.
void atalk_link_lost(atalk_link_t *link, const char *reason);

// Close a link from this side: BYE, then the transport's close.
void atalk_link_close(atalk_link_t *link, const char *reason);

// Inspection (object model and tests).
atalk_link_state_t atalk_link_state(const atalk_link_t *link);
uint16_t atalk_link_peer_net(const atalk_link_t *link);

#endif // APPLETALK_LINK_H
