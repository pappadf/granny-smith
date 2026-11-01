// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_router.h
// The router in the built-in AppleTalk stack (appletalk_router.c).
//
// With AppleTalk on, node 33 is the seed router of its LocalTalk network
// (Inside AppleTalk ch. 5): it owns the network number and the zone name,
// announces them to the guest with RTMP Data every 10 s, answers RTMP
// Requests, and serves ZIP -- Query/Reply on DDP type 6 and GetMyZone,
// GetZoneList and GetLocalZones over ATP -- on the Zone Information Socket.
// Until a link to another instance exists, its routing table holds only the
// local network: a gateway that leads nowhere.  `seed` turns it off, which
// leaves the routerless cable the stack always had.

#ifndef APPLETALK_ROUTER_H
#define APPLETALK_ROUTER_H

#include "appletalk_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct atalk_conn;
struct object;

// The defaults: network 1 (any of $0001-$FEFF; the web app picks its own per
// machine) in the zone "Granny Smith".
#define ATALK_ROUTER_DEFAULT_NET  1
#define ATALK_ROUTER_DEFAULT_ZONE "Granny Smith"
#define ATALK_ROUTER_NET_MAX      0xFEFF // the top of the nonextended range (Inside AppleTalk 4-7)

// RTMP's send timer (Inside AppleTalk 5-13 and C-11): RTMP Data every 10 s.
#define ATALK_RTMP_SEND_NS 10000000000ull

// The router's counters, published as `appletalk.router.stats`.
typedef struct {
    uint64_t rtmp_data_out; // RTMP Data broadcasts on the local network
    uint64_t rtmp_requests; // RTMP Requests and Route Data Requests answered
    uint64_t rtmp_link_out; // RTMP Data sent over links
    uint64_t rtmp_link_in; // RTMP Data received over links
    uint64_t zip_queries; // ZIP Queries answered
    uint64_t zip_queries_out; // ZIP Queries sent for zones not yet known
    uint64_t zip_replies_in; // zones learnt from ZIP Replies
    uint64_t zip_atp_requests; // GetMyZone, GetZoneList and GetLocalZones answered
    uint64_t nbp_brrq; // NBP broadcast requests routed
    uint64_t nbp_fwdreq_out; // NBP forward requests sent toward other networks
    uint64_t nbp_fwdreq_in; // NBP forward requests turned into local lookups
    uint64_t forwarded_out; // datagrams from this network forwarded over a link
    uint64_t forwarded_transit; // datagrams forwarded from one link to another
    uint64_t dropped_no_route; // datagrams for a network with no usable route
    uint64_t dropped_hops; // datagrams that had crossed 15 routers already
    uint64_t dropped_filter; // datagrams from a link for the host's services, filtered
} atalk_router_stats_t;

// Lifecycle (called from appletalk.c): init once when the network comes up
// (takes the ZIS); register_timers with each connection as it is built; plug
// when a connection takes the cable (true) or leaves it (false).
void atalk_router_init(void);
void atalk_router_register_timers(struct atalk_conn *conn);
void atalk_router_plug(bool plugged);

// True while the router runs (`seed`).
bool atalk_router_active(void);

// Set this network's number, 1..ATALK_ROUTER_NET_MAX: what
// appletalk.router.net's setter does.  0, or -1 (nothing changed) if it is out
// of range or the router is linked (a linked network cannot be renumbered).
int atalk_router_set_net(uint16_t net);

// The local zone name in MacRoman, `*len` bytes (not NUL-terminated).
const uint8_t *atalk_router_zone_mac(size_t *len);

// True if NBP zone `zone` (MacRoman, `len` bytes) names the local network's
// zone: "*" ("this zone") or the zone name, compared case-insensitively.
bool atalk_router_zone_is_local(const uint8_t *zone, int len);

// Datagrams for the router from the local network.  `ddp` is the parsed
// header; `buf` is the DDP data.  The RTMP one returns false when the router
// is off (the request goes unanswered, as on a routerless cable).
bool atalk_router_rtmp_request_in(const ddp_header_t *ddp, const uint8_t *buf, size_t len);
void atalk_router_zip_in(const ddp_header_t *ddp, const uint8_t *buf, size_t len);

// Count NBP routing work done in appletalk.c: a BrRq routed, a FwdReq sent
// toward another network, a FwdReq turned into a local lookup.
void atalk_router_note_brrq(void);
void atalk_router_note_fwdreq(void);
void atalk_router_note_fwdreq_in(void);

// The networks of zone `zone` (MacRoman, `len` bytes; "*" is the local zone)
// that the router can reach now, local first; returns how many it wrote.
int atalk_router_nets_in_zone(const uint8_t *zone, int len, uint16_t *nets, int cap);

// A datagram from this network -- the guest's, or one the host stack sent --
// addressed to another network: `dg` is the extended DDP header and data as
// on the wire.  The router forwards it over the link its table names, or
// drops it if there is no route.
void atalk_router_from_local(const uint8_t *dg, size_t len);

// While the router hands the host a datagram from a link, whether its sender
// may use the host's services (always true for the local network).  The NBP
// layer asks this before answering a FwdReq for the host's names.
bool atalk_router_host_shared_with_sender(void);

// Object model (appletalk.router).
const atalk_router_stats_t *atalk_router_get_stats(void);
void atalk_router_install_objects(struct object *parent);

#endif // APPLETALK_ROUTER_H
