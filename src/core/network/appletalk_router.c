// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_router.c
// The router of the emulated LocalTalk network: seed router toward the
// guest, and an AppleTalk router toward other instances over links.  See
// appletalk_router.h and appletalk_link.h; the design and the page numbers
// (Inside AppleTalk, 2nd ed.) are in docs/internals/core/network/appletalk_router.md.
//
// Everything here runs on the worker thread, like the rest of the stack: the
// guest's datagrams arrive through appletalk.c, the links' records from their
// transports, the timers are guest-time scheduler events, and the object
// model's calls come through the mailbox.

#include "appletalk_router.h"

#include "appletalk.h"
#include "appletalk_link.h"
#include "log.h"
#include "macroman.h"
#include "object.h"
#include "platform_hooks.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("appletalk");

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// RTMP (Inside AppleTalk ch. 5, C-8)
#define RTMP_FUNC_REQUEST     1 // RTMP Request: THIS-NET and A-ROUTER, please
#define RTMP_FUNC_RDR_SPLIT   2 // Route Data Request, with split horizon
#define RTMP_FUNC_RDR_NOSPLIT 3 // Route Data Request, the whole table
#define RTMP_ID_BITS          8 // a LocalTalk node ID is 8 bits long
#define RTMP_VERSION          0x82 // the version indicator ($000082, 5-15)
#define RTMP_HEADER_LEN       7 // net, ID length, ID, version indicator
#define RTMP_DIST_UNREACHABLE 16 // a route this long does not exist
#define RTMP_DIST_NOTIFY      31 // "this entry went bad" (notify neighbor, 5-12)
#define RTMP_VALIDITY_NS      20000000000ull // the validity timer (5-13)
#define RTMP_TRIGGER_NS       100000000ull // a changed table goes out after 100 ms

// ZIP (Inside AppleTalk ch. 8, C-10)
#define ZIP_FUNC_QUERY          1
#define ZIP_FUNC_REPLY          2
#define ZIP_FUNC_GETNETINFO     5
#define ZIP_FUNC_EXT_REPLY      8
#define ZIP_ATP_GET_MY_ZONE     7 // in ATP user byte 0
#define ZIP_ATP_GET_ZONE_LIST   8
#define ZIP_ATP_GET_LOCAL_ZONES 9
#define ZIP_QUERY_NS            10000000000ull // query retransmission (8-12)

// Links
#define LINK_PING_NS        5000000000ull // PING every 5 s of guest time
#define LINK_HELLO_RETRY_NS 1000000000ull // HELLO again every second until the other end answers

// DDP extended header field offsets (Inside AppleTalk 4-15)
#define DG_DST_NET  4
#define DG_SRC_NET  6
#define DG_DST_NODE 8
#define DG_SRC_NODE 9
#define DG_DST_SOCK 10
#define DG_SRC_SOCK 11
#define DG_TYPE     12
#define DDP_HOP_MAX 15 // a datagram that has crossed 15 routers goes no further

// Ports: 0 is the local LocalTalk network, 1.. the links
#define PORT_LOCAL 0
#define MAX_ROUTES 128

typedef enum { ROUTE_GOOD, ROUTE_SUSPECT, ROUTE_BAD0, ROUTE_BAD1 } route_state_t;

// One routing-table entry, with its zone-information-table part (8-4).
typedef struct {
    bool in_use;
    uint16_t net;
    uint8_t distance; // 0: directly connected
    int port; // PORT_LOCAL or 1 + link index
    route_state_t state;
    bool zone_known;
    uint8_t zone[ATALK_NBP_NAME_MAX]; // MacRoman
    uint8_t zone_len;
} route_t;

// One link to another router.
struct atalk_link {
    bool in_use;
    atalk_link_state_t state;
    atalk_link_transport_t transport;
    void *ctx;
    char kind[16]; // "tabs", "webrtc", "socket", "loopback"
    bool friend_link; // the other end is another person's instance
    uint16_t peer_net; // the network at the other end (its distance-0 tuple), 0 until known
    char peer_name[64]; // its display name, from its HELLO
    char close_reason[96]; // why it closed
    int64_t rtt_ns; // the last PING's round trip, -1 until one came back
    uint64_t rx_records, tx_records;
};

// The router's state: one per process, like the network it serves.
typedef struct {
    bool seed; // the router runs (appletalk.router.seed)
    uint16_t net; // this network's number
    char zone[ATALK_NBP_TEXT_CAP]; // its zone name, UTF-8
    uint8_t zone_mac[ATALK_NBP_NAME_MAX]; // ...and in MacRoman, as it goes on the wire
    size_t zone_mac_len;
    char name[64]; // this instance's display name, sent in HELLO
    bool share_host_services; // friends may use the host's services (§5.6 of the design)
    bool plugged; // a connection is on the cable
    route_t routes[MAX_ROUTES];
    atalk_link_t links[ATALK_LINK_MAX];
    atalk_timer_t rtmp_timer; // send-RTMP
    atalk_timer_t validity_timer; // ages routes
    atalk_timer_t trigger_timer; // sends a changed table soon
    atalk_timer_t zip_timer; // re-queries unknown zones
    atalk_timer_t ping_timer; // link liveness and round trip
    const atalk_link_t *rx_link; // the link whose datagram is being delivered to the host, if any
    atalk_router_stats_t stats;
} router_t;

static router_t g_router = {
    .seed = true,
    .net = ATALK_ROUTER_DEFAULT_NET,
};

static void router_send_rtmp_all(void);
static void link_send(atalk_link_t *l, uint8_t kind, const uint8_t *body, size_t len, bool reliable);

// === Small helpers ============================================================

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int link_index(const atalk_link_t *l) {
    return (int)(l - g_router.links);
}
static int link_port(const atalk_link_t *l) {
    return 1 + link_index(l);
}
static atalk_link_t *port_link(int port) {
    if (port < 1 || port > ATALK_LINK_MAX)
        return NULL;
    atalk_link_t *l = &g_router.links[port - 1];
    return l->in_use && l->state == ATALK_LINK_OPEN ? l : NULL;
}

// Set the zone name from UTF-8; false (and nothing changed) if it is not a
// valid NBP field.
static bool router_set_zone_text(const char *zone) {
    uint8_t mac[ATALK_NBP_NAME_MAX];
    int n = macroman_from_utf8(zone, mac, sizeof(mac));
    if (n <= 0)
        return false;
    snprintf(g_router.zone, sizeof(g_router.zone), "%s", zone);
    memcpy(g_router.zone_mac, mac, (size_t)n);
    g_router.zone_mac_len = (size_t)n;
    return true;
}

// Zone names compare case-insensitively (Inside AppleTalk, Appendix D).
static bool zone_equal(const uint8_t *a, int alen, const uint8_t *b, int blen) {
    if (alen != blen)
        return false;
    for (int i = 0; i < alen; i++)
        if (macroman_fold(a[i]) != macroman_fold(b[i]))
            return false;
    return true;
}

uint16_t atalk_this_net(void) {
    // Without a router a node knows no network number: 0 (Inside AppleTalk 4-7)
    return g_router.seed ? g_router.net : 0;
}

bool atalk_router_active(void) {
    return g_router.seed;
}

const uint8_t *atalk_router_zone_mac(size_t *len) {
    *len = g_router.zone_mac_len;
    return g_router.zone_mac;
}

bool atalk_router_zone_is_local(const uint8_t *zone, int len) {
    if (len == 1 && zone[0] == '*')
        return true; // "this zone"
    return zone_equal(zone, len, g_router.zone_mac, (int)g_router.zone_mac_len);
}

// === The routing table =========================================================

static route_t *route_find(uint16_t net) {
    for (int i = 0; i < MAX_ROUTES; i++)
        if (g_router.routes[i].in_use && g_router.routes[i].net == net)
            return &g_router.routes[i];
    return NULL;
}

// The local network's entry: distance 0 on the local port, its zone known.
// It follows the router's configuration, so it is rebuilt rather than kept.
static void route_set_local(void) {
    for (int i = 0; i < MAX_ROUTES; i++)
        if (g_router.routes[i].in_use && g_router.routes[i].port == PORT_LOCAL)
            g_router.routes[i].in_use = false;
    if (!g_router.seed)
        return;
    route_t *r = route_find(g_router.net);
    if (r)
        r->in_use = false; // a remote entry for our own number: ours wins
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (!g_router.routes[i].in_use) {
            r = &g_router.routes[i];
            memset(r, 0, sizeof(*r));
            r->in_use = true;
            r->net = g_router.net;
            r->port = PORT_LOCAL;
            r->state = ROUTE_GOOD;
            r->zone_known = true;
            memcpy(r->zone, g_router.zone_mac, g_router.zone_mac_len);
            r->zone_len = (uint8_t)g_router.zone_mac_len;
            return;
        }
    }
}

static route_t *route_alloc(uint16_t net) {
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (!g_router.routes[i].in_use) {
            route_t *r = &g_router.routes[i];
            memset(r, 0, sizeof(*r));
            r->in_use = true;
            r->net = net;
            return r;
        }
    }
    return NULL;
}

// Usable for forwarding: good or suspect (bad entries only advertise that
// they went bad).
static bool route_usable(const route_t *r) {
    return r && (r->state == ROUTE_GOOD || r->state == ROUTE_SUSPECT);
}

// Something changed: send the table to every link soon (coalesced).
static void route_changed(void) {
    atalk_timer_arm(&g_router.trigger_timer, 0, RTMP_TRIGGER_NS);
}

// Mark every route through `port` bad, as a link that went down does.
static void routes_fail_port(int port) {
    bool any = false;
    for (int i = 0; i < MAX_ROUTES; i++) {
        route_t *r = &g_router.routes[i];
        if (r->in_use && r->port == port && r->state < ROUTE_BAD0) {
            r->state = ROUTE_BAD0;
            any = true;
        }
    }
    if (any)
        route_changed();
}

// Replace-Entry (5-23): the route now goes through `port` at tuple distance + 1.
static void route_replace(route_t *r, uint8_t tuple_distance, int port) {
    if (r->port != port || r->distance != tuple_distance + 1 || r->state != ROUTE_GOOD)
        route_changed();
    r->distance = (uint8_t)(tuple_distance + 1);
    r->port = port;
    r->state = ROUTE_GOOD;
}

// One routing tuple from an RTMP Data packet that came in on `port`
// (Update-the-Entry and Create-New-Entry, 5-22 and 5-23).
static void route_tuple(uint16_t net, uint8_t distance, int port) {
    route_t *r = route_find(net);
    if (r && r->port == PORT_LOCAL)
        return; // our own network: we are its router
    if (!r) {
        if (distance >= RTMP_DIST_UNREACHABLE - 1)
            return; // too far, or bad: nothing to learn
        r = route_alloc(net);
        if (!r) {
            LOG(2, "atalk router: routing table full, network %u not added", (unsigned)net);
            return;
        }
        r->port = port;
        r->distance = (uint8_t)(distance + 1);
        r->state = ROUTE_GOOD;
        r->zone_known = false; // the ZIP process will ask (8-4)
        LOG(2, "atalk router: network %u via link %d, distance %u", (unsigned)net, port - 1, (unsigned)r->distance);
        route_changed();
        atalk_timer_arm(&g_router.zip_timer, 0, ATALK_TIMER_MIN_NS); // ask for its zone now
        return;
    }
    if (r->state >= ROUTE_BAD0 && distance < DDP_HOP_MAX) {
        // A network coming back may come back in another zone: ask again
        route_replace(r, distance, port);
        r->zone_known = false;
        atalk_timer_arm(&g_router.zip_timer, 0, ATALK_TIMER_MIN_NS);
        return;
    }
    if (r->distance >= distance + 1 && distance < DDP_HOP_MAX) {
        route_replace(r, distance, port);
        return;
    }
    if (r->port == port) {
        // We forward through the sender: the network is now further away
        // than we thought, or gone
        if (distance != RTMP_DIST_NOTIFY) {
            uint8_t d = (uint8_t)(distance + 1);
            if (d < RTMP_DIST_UNREACHABLE) {
                if (r->distance != d)
                    route_changed();
                r->distance = d;
                r->state = ROUTE_GOOD;
            } else {
                r->in_use = false;
                route_changed();
            }
        } else if (r->state < ROUTE_BAD0) {
            r->state = ROUTE_BAD0;
            route_changed();
        }
    }
}

// The validity timer (5-23): good -> suspect -> bad0 -> bad1 -> gone, for
// every entry that is not directly connected.
static void validity_timer_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    bool changed = false;
    for (int i = 0; i < MAX_ROUTES; i++) {
        route_t *r = &g_router.routes[i];
        if (!r->in_use || r->distance == 0)
            continue;
        switch (r->state) {
        case ROUTE_GOOD:
            r->state = ROUTE_SUSPECT;
            break;
        case ROUTE_SUSPECT:
            r->state = ROUTE_BAD0;
            changed = true;
            break;
        case ROUTE_BAD0:
            r->state = ROUTE_BAD1;
            break;
        case ROUTE_BAD1:
            LOG(2, "atalk router: network %u aged out", (unsigned)r->net);
            r->in_use = false; // its zone goes with it (8-6)
            changed = true;
            break;
        }
    }
    if (changed)
        route_changed();
    if (g_router.seed && g_router.plugged)
        atalk_timer_arm(&g_router.validity_timer, 0, RTMP_VALIDITY_NS);
}

// === RTMP ======================================================================

// The header of an RTMP Data packet (5-13): the sender's network number
// (0 on a port that is not an AppleTalk network, 5-15), node ID length and
// ID, and the nonextended version indicator.  Returns its length.
static int rtmp_header(uint8_t *buf, uint16_t sender_net) {
    wr16(buf, sender_net);
    buf[2] = RTMP_ID_BITS;
    buf[3] = ATALK_HOST_NODE;
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = RTMP_VERSION;
    return RTMP_HEADER_LEN;
}

// Copy-in-tuples (5-24) for packets out through `port`: every entry but
// those split horizon leaves out (learnt through that same port), bad ones
// as notify-neighbor tuples.  The local network's own entry is included on
// the local port: the guest's stub reads only the header.
static int rtmp_tuples(uint8_t *buf, int cap, int port, bool split) {
    int n = 0;
    for (int i = 0; i < MAX_ROUTES && n + 3 <= cap; i++) {
        const route_t *r = &g_router.routes[i];
        if (!r->in_use)
            continue;
        if (split && port != PORT_LOCAL && r->port == port)
            continue; // split horizon (5-11)
        wr16(&buf[n], r->net);
        buf[n + 2] = r->state >= ROUTE_BAD0 ? RTMP_DIST_NOTIFY : r->distance;
        n += 3;
    }
    return n;
}

// Send a datagram whose DDP data is `data` on link `l`, as the router (node
// 33) to the router at the other end: network 0 ("this link"), node 33.
static void link_send_ddp(atalk_link_t *l, uint8_t dst_sock, uint8_t src_sock, uint8_t type, const uint8_t *data,
                          size_t len) {
    uint8_t dg[DDP_EXTENDED_HEADER_SIZE + DDP_MAX_DATA_SIZE];
    if (len > DDP_MAX_DATA_SIZE)
        return;
    size_t total = DDP_EXTENDED_HEADER_SIZE + len;
    dg[0] = (uint8_t)((total >> 8) & 0x03); // hop count 0
    dg[1] = (uint8_t)total;
    dg[2] = dg[3] = 0; // no checksum
    wr16(&dg[DG_DST_NET], 0);
    wr16(&dg[DG_SRC_NET], atalk_this_net());
    dg[DG_DST_NODE] = ATALK_HOST_NODE;
    dg[DG_SRC_NODE] = ATALK_HOST_NODE;
    dg[DG_DST_SOCK] = dst_sock;
    dg[DG_SRC_SOCK] = src_sock;
    dg[DG_TYPE] = type;
    memcpy(&dg[DDP_EXTENDED_HEADER_SIZE], data, len);
    link_send(l, ATALK_LINK_DDP, dg, total, false);
}

// RTMP Data out through one link (sender network 0: not an AppleTalk port).
static void rtmp_send_link(atalk_link_t *l) {
    uint8_t buf[DDP_MAX_DATA_SIZE];
    int n = rtmp_header(buf, 0);
    n += rtmp_tuples(buf + n, (int)sizeof(buf) - n, link_port(l), true);
    link_send_ddp(l, ATALK_RTMP_SOCKET, ATALK_RTMP_SOCKET, DDP_TYPE_RTMP_RESPONSE, buf, (size_t)n);
    g_router.stats.rtmp_link_out++;
}

// RTMP Data broadcast on the local network, from and to the RTMP socket.
static void rtmp_send_local(void) {
    if (!atalk_link_ready())
        return; // the guest's AppleTalk is not up: nobody to tell
    uint8_t buf[DDP_MAX_DATA_SIZE];
    int n = rtmp_header(buf, g_router.net);
    n += rtmp_tuples(buf + n, (int)sizeof(buf) - n, PORT_LOCAL, true);
    atalk_socket_addr_t dest = {.net = 0, .node = 0xFF, .socket = ATALK_RTMP_SOCKET};
    if (atalk_ddp_send_to(&dest, ATALK_RTMP_SOCKET, DDP_TYPE_RTMP_RESPONSE, buf, n) == 0)
        g_router.stats.rtmp_data_out++;
}

static void router_send_rtmp_all(void) {
    rtmp_send_local();
    for (int i = 0; i < ATALK_LINK_MAX; i++)
        if (g_router.links[i].in_use && g_router.links[i].state == ATALK_LINK_OPEN)
            rtmp_send_link(&g_router.links[i]);
}

// The send-RTMP timer: every port, then re-arm.
static void rtmp_timer_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    if (!g_router.seed || !g_router.plugged)
        return;
    router_send_rtmp_all();
    atalk_timer_arm(&g_router.rtmp_timer, 0, ATALK_RTMP_SEND_NS);
}

// A changed table goes to the links at once (and the guest's stub does not
// care about tuples, so the local network waits for the timer).
static void trigger_timer_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    if (!g_router.seed)
        return;
    for (int i = 0; i < ATALK_LINK_MAX; i++)
        if (g_router.links[i].in_use && g_router.links[i].state == ATALK_LINK_OPEN)
            rtmp_send_link(&g_router.links[i]);
}

// RTMP Data from the router at the other end of `l` (5-22).
static void rtmp_data_in(atalk_link_t *l, const uint8_t *buf, size_t len) {
    if (len < RTMP_HEADER_LEN || buf[2] != RTMP_ID_BITS) {
        LOG(3, "atalk router: malformed RTMP Data on link %d", link_index(l));
        return;
    }
    g_router.stats.rtmp_link_in++;
    int port = link_port(l);
    for (size_t p = RTMP_HEADER_LEN; p + 3 <= len; p += 3) {
        uint16_t net = rd16(&buf[p]);
        uint8_t distance = buf[p + 2] & 0x7F; // the high bit marks an extended tuple
        if (buf[p + 2] & 0x80) {
            p += 3; // extended tuple: range end and version follow; not from our routers
            continue;
        }
        if (net == 0)
            continue;
        if (distance == 0) {
            // The sender's own network.  Ours too? Two networks with one number
            // cannot be routed: refuse the link (§5.1 of the design).
            if (net == atalk_this_net()) {
                char why[96];
                snprintf(why, sizeof(why), "both sides use network %u", (unsigned)net);
                LOG(1, "atalk router: link %d refused: %s", link_index(l), why);
                atalk_link_close(l, why);
                return;
            }
            l->peer_net = net;
        }
        route_tuple(net, distance, port);
    }
}

bool atalk_router_rtmp_request_in(const ddp_header_t *ddp, const uint8_t *buf, size_t len) {
    if (!g_router.seed)
        return false;
    if (len < 1) {
        LOG(3, "RTMP request with no function");
        return false;
    }
    uint8_t func = buf[0];
    if (func != RTMP_FUNC_REQUEST && func != RTMP_FUNC_RDR_SPLIT && func != RTMP_FUNC_RDR_NOSPLIT) {
        LOG(3, "RTMP request with function %u", (unsigned)func);
        return false;
    }
    // An RTMP Request gets a Response: RTMP Data without tuples, directed to
    // the requesting socket (5-17).  A Route Data Request gets the table
    // (5-19), with or without split horizon.
    uint8_t out[DDP_MAX_DATA_SIZE];
    int n = rtmp_header(out, g_router.net);
    if (func != RTMP_FUNC_REQUEST)
        n += rtmp_tuples(out + n, (int)sizeof(out) - n, PORT_LOCAL, func == RTMP_FUNC_RDR_SPLIT);
    atalk_socket_addr_t dest = {.net = ddp->src_net, .node = ddp->llap.src, .socket = ddp->src_socket};
    LOG(3, "RTMP %s from node %u: network %u, router %u", func == RTMP_FUNC_REQUEST ? "Request" : "RDR",
        (unsigned)ddp->llap.src, (unsigned)g_router.net, (unsigned)ATALK_HOST_NODE);
    if (atalk_ddp_send_to(&dest, ATALK_RTMP_SOCKET, DDP_TYPE_RTMP_RESPONSE, out, n) == 0)
        g_router.stats.rtmp_requests++;
    return true;
}

// === ZIP =======================================================================

// Append the zone of `r` as a ZIP Reply pair (net, zone name); false if full.
static bool zip_put_pair(uint8_t *out, int *n, int cap, const route_t *r) {
    if (*n + 3 + r->zone_len > cap)
        return false;
    wr16(&out[*n], r->net);
    out[*n + 2] = r->zone_len;
    memcpy(&out[*n + 3], r->zone, r->zone_len);
    *n += 3 + r->zone_len;
    return true;
}

// ZIP Query (8-11): a list of network numbers whose zones the requester
// wants.  The router answers with a Reply carrying the zone of every one it
// knows, and stays silent if it knows none (8-4).
static void zip_query_in(const ddp_header_t *ddp, const uint8_t *buf, size_t len) {
    if (len < 2)
        return;
    int count = buf[1];
    if (len < 2 + (size_t)count * 2)
        return; // shorter than its network count says
    uint8_t out[DDP_MAX_DATA_SIZE];
    int n = 2, pairs = 0;
    out[0] = ZIP_FUNC_REPLY;
    for (int i = 0; i < count; i++) {
        const route_t *r = route_find(rd16(&buf[2 + i * 2]));
        if (r && r->zone_known && route_usable(r) && zip_put_pair(out, &n, (int)sizeof(out), r))
            pairs++;
    }
    if (pairs == 0)
        return;
    out[1] = (uint8_t)pairs;
    atalk_socket_addr_t dest = {.net = ddp->src_net, .node = ddp->llap.src, .socket = ddp->src_socket};
    if (atalk_ddp_send_to(&dest, ATALK_ZIS_SOCKET, DDP_TYPE_ZIP, out, n) == 0)
        g_router.stats.zip_queries++;
}

// ZIP Reply or Extended Reply (8-11): fill in the zones we asked for.  Every
// network here has exactly one zone (LocalTalk is nonextended).
static void zip_reply_in(const uint8_t *buf, size_t len) {
    size_t p = 2;
    while (p + 3 <= len) {
        uint16_t net = rd16(&buf[p]);
        uint8_t zl = buf[p + 2];
        if (zl == 0 || zl > ATALK_NBP_NAME_MAX || p + 3 + zl > len)
            return;
        route_t *r = route_find(net);
        if (r && !r->zone_known) {
            memcpy(r->zone, &buf[p + 3], zl);
            r->zone_len = zl;
            r->zone_known = true;
            g_router.stats.zip_replies_in++;
            LOG(2, "atalk router: network %u is in zone '%.*s'", (unsigned)net, (int)zl, (const char *)r->zone);
        }
        p += 3 + zl;
    }
}

void atalk_router_zip_in(const ddp_header_t *ddp, const uint8_t *buf, size_t len) {
    if (!g_router.seed || len < 1)
        return;
    switch (buf[0]) {
    case ZIP_FUNC_QUERY:
        zip_query_in(ddp, buf, len);
        break;
    case ZIP_FUNC_REPLY:
    case ZIP_FUNC_EXT_REPLY:
        zip_reply_in(buf, len);
        break;
    case ZIP_FUNC_GETNETINFO:
        // GetNetInfo is for nodes on extended networks (8-8); LocalTalk is not
        // one, and its nodes ask with GetMyZone instead.
        LOG(3, "ZIP GetNetInfo on a nonextended network: ignored");
        break;
    default:
        LOG(4, "ZIP function %u ignored", (unsigned)buf[0]);
        break;
    }
}

// The ZIP timer (8-5): one Query per link for the networks learnt through it
// whose zones are still unknown, until each is known.
static void zip_timer_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    if (!g_router.seed)
        return;
    bool pending = false;
    for (int li = 0; li < ATALK_LINK_MAX; li++) {
        atalk_link_t *l = &g_router.links[li];
        if (!l->in_use || l->state != ATALK_LINK_OPEN)
            continue;
        uint8_t q[DDP_MAX_DATA_SIZE];
        int n = 2, nets = 0;
        q[0] = ZIP_FUNC_QUERY;
        for (int i = 0; i < MAX_ROUTES && n + 2 <= (int)sizeof(q) && nets < 255; i++) {
            const route_t *r = &g_router.routes[i];
            if (r->in_use && !r->zone_known && r->port == link_port(l) && route_usable(r)) {
                wr16(&q[n], r->net);
                n += 2;
                nets++;
            }
        }
        if (nets == 0)
            continue;
        q[1] = (uint8_t)nets;
        link_send_ddp(l, ATALK_ZIS_SOCKET, ATALK_ZIS_SOCKET, DDP_TYPE_ZIP, q, (size_t)n);
        g_router.stats.zip_queries_out++;
        pending = true;
    }
    if (pending)
        atalk_timer_arm(&g_router.zip_timer, 0, ZIP_QUERY_NS);
}

// The distinct zones of the internet, local first, for GetZoneList.  Fills
// `zones` with pointers into the table; returns the count.
static int zone_list(const route_t **zones, int cap) {
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < MAX_ROUTES && n < cap; i++) {
            const route_t *r = &g_router.routes[i];
            if (!r->in_use || !r->zone_known || !route_usable(r) || (pass == 0) != (r->port == PORT_LOCAL))
                continue;
            bool dup = false;
            for (int k = 0; k < n && !dup; k++)
                dup = zone_equal(zones[k]->zone, zones[k]->zone_len, r->zone, r->zone_len);
            if (!dup)
                zones[n++] = r;
        }
    }
    return n;
}

int atalk_router_nets_in_zone(const uint8_t *zone, int len, uint16_t *nets, int cap) {
    // "*" is the requester's zone (8-19), which may span other networks too:
    // with one zone name on every instance, it is all of them
    if (len == 1 && zone[0] == '*') {
        zone = g_router.zone_mac;
        len = (int)g_router.zone_mac_len;
    }
    int n = 0;
    for (int pass = 0; pass < 2; pass++) { // local first
        for (int i = 0; i < MAX_ROUTES && n < cap; i++) {
            const route_t *r = &g_router.routes[i];
            if (!r->in_use || !r->zone_known || !route_usable(r) || (pass == 0) != (r->port == PORT_LOCAL))
                continue;
            if (zone_equal(zone, len, r->zone, r->zone_len))
                nets[n++] = r->net;
        }
    }
    return n;
}

// ZIP's ATP requests on the ZIS (8-7, 8-13): GetMyZone, GetZoneList and
// GetLocalZones, at-least-once, one response packet each.  The user bytes of
// a request carry the function and (for the list requests) a 1-based start
// index; those of a response carry LastFlag and the number of names in it.
// GetMyZone and GetLocalZones on a nonextended network both name the
// requester's own zone (8-7).
static void zis_atp_request(const ddp_header_t *ddp, atp_packet_t *req, void *ctx) {
    (void)ctx;
    if (!g_router.seed)
        return; // no router: the request goes unanswered, as on a routerless cable
    uint8_t func = req->user[0];
    if (func != ZIP_ATP_GET_MY_ZONE && func != ZIP_ATP_GET_ZONE_LIST && func != ZIP_ATP_GET_LOCAL_ZONES) {
        LOG(3, "ZIP ATP function %u from node %u ignored", (unsigned)func, (unsigned)ddp->llap.src);
        return;
    }
    const route_t *zones[MAX_ROUTES];
    int nz = 0;
    if (func == ZIP_ATP_GET_ZONE_LIST) {
        nz = zone_list(zones, MAX_ROUTES);
    } else {
        const route_t *local = route_find(g_router.net);
        if (local)
            zones[nz++] = local;
    }
    int start = (int)((req->user[2] << 8) | req->user[3]);
    if (func == ZIP_ATP_GET_MY_ZONE || start < 1)
        start = 1;
    uint8_t data[ATP_MAX_ATP_PAYLOAD];
    int dlen = 0, count = 0, i = start - 1;
    for (; i < nz; i++) {
        if (dlen + 1 + zones[i]->zone_len > (int)sizeof(data))
            break; // a name is never split across responses (8-7)
        data[dlen++] = zones[i]->zone_len;
        memcpy(&data[dlen], zones[i]->zone, zones[i]->zone_len);
        dlen += zones[i]->zone_len;
        count++;
        if (func == ZIP_ATP_GET_MY_ZONE)
            break;
    }
    uint8_t user[4] = {0, 0, (uint8_t)(count >> 8), (uint8_t)count};
    // GetMyZone's response has no LastFlag; the list requests' says whether
    // this response holds the last name (an index past the end: empty, last)
    if (func != ZIP_ATP_GET_MY_ZONE)
        user[0] = i >= nz ? 1 : 0;
    LOG(3, "ZIP %s from node %u: %d zone(s)",
        func == ZIP_ATP_GET_MY_ZONE     ? "GetMyZone"
        : func == ZIP_ATP_GET_ZONE_LIST ? "GetZoneList"
                                        : "GetLocalZones",
        (unsigned)ddp->llap.src, count);
    if (atp_responder_send_simple(ddp, req, user, data, dlen, false) > 0)
        g_router.stats.zip_atp_requests++;
}

void atalk_router_note_brrq(void) {
    g_router.stats.nbp_brrq++;
}

void atalk_router_note_fwdreq_in(void) {
    g_router.stats.nbp_fwdreq_in++;
}

void atalk_router_note_fwdreq(void) {
    g_router.stats.nbp_fwdreq_out++;
}

// === Forwarding ================================================================

// Hand a datagram to the network this router serves: to the host stack
// (node 33, or 0 -- "any router" on this network) or onto LocalTalk.  From a
// link, the host is reachable only on the router's own sockets unless the
// user shares the host's services with friends (§5.6 of the design).
static void deliver_local(uint8_t *dg, size_t len, atalk_link_t *from) {
    uint8_t node = dg[DG_DST_NODE];
    if (node == ATALK_HOST_NODE || node == 0) {
        uint8_t sock = dg[DG_DST_SOCK];
        bool control = sock == ATALK_RTMP_SOCKET || sock == ATALK_NIS_SOCKET || sock == ATALK_ZIS_SOCKET;
        bool shared = from && from->friend_link && g_router.share_host_services;
        if (from && !control && !shared) {
            g_router.stats.dropped_filter++;
            LOG(4, "atalk router: datagram for host socket %u from link %d filtered", (unsigned)sock, link_index(from));
            return;
        }
        g_router.rx_link = from;
        atalk_deliver_datagram(dg, len, true);
        g_router.rx_link = NULL;
        return;
    }
    atalk_deliver_datagram(dg, len, false);
}

// Route one datagram (extended DDP header and data) that arrived on
// `in_port`: for this network, deliver it; for another, forward it over the
// link the table names, one hop further.
static void route_datagram(uint8_t *dg, size_t len, int in_port, atalk_link_t *from) {
    if (len < DDP_EXTENDED_HEADER_SIZE)
        return;
    uint16_t dst = rd16(&dg[DG_DST_NET]);
    if (dst == 0 || dst == atalk_this_net()) {
        if (in_port != PORT_LOCAL)
            deliver_local(dg, len, from);
        return; // from the local network to itself: appletalk.c delivered it
    }
    route_t *r = route_find(dst);
    if (!route_usable(r) || r->port == PORT_LOCAL) {
        g_router.stats.dropped_no_route++;
        LOG(4, "atalk router: no route to network %u", (unsigned)dst);
        return;
    }
    if (r->port == in_port) {
        g_router.stats.dropped_no_route++;
        return; // it would go back where it came from
    }
    uint8_t hop = (uint8_t)((dg[0] >> 2) & 0x0F);
    if (hop >= DDP_HOP_MAX) {
        g_router.stats.dropped_hops++;
        return; // crossed 15 routers already (4-13)
    }
    dg[0] = (uint8_t)((dg[0] & 0xC3) | ((hop + 1) << 2)); // one more hop; the checksum does not cover it
    atalk_link_t *l = port_link(r->port);
    if (!l) {
        g_router.stats.dropped_no_route++;
        return;
    }
    if (in_port == PORT_LOCAL)
        g_router.stats.forwarded_out++;
    else
        g_router.stats.forwarded_transit++;
    link_send(l, ATALK_LINK_DDP, dg, len, false);
}

void atalk_router_from_local(const uint8_t *dg, size_t len) {
    if (!g_router.seed || len < DDP_EXTENDED_HEADER_SIZE || len > DDP_EXTENDED_HEADER_SIZE + DDP_MAX_DATA_SIZE)
        return;
    uint8_t copy[DDP_EXTENDED_HEADER_SIZE + DDP_MAX_DATA_SIZE];
    memcpy(copy, dg, len);
    // A sender that did not know THIS-NET yet put 0 as its network
    if (rd16(&copy[DG_SRC_NET]) == 0)
        wr16(&copy[DG_SRC_NET], g_router.net);
    route_datagram(copy, len, PORT_LOCAL, NULL);
}

bool atalk_router_host_shared_with_sender(void) {
    return g_router.rx_link == NULL || (g_router.rx_link->friend_link && g_router.share_host_services);
}

// A DDP record from a link.  A datagram for network 0 is for the router at
// this end ("this link"): its RTMP Data and ZIP.  Anything else is routed.
static void link_ddp_in(atalk_link_t *l, const uint8_t *rec, size_t len) {
    if (len < DDP_EXTENDED_HEADER_SIZE || len > DDP_EXTENDED_HEADER_SIZE + DDP_MAX_DATA_SIZE)
        return;
    size_t dlen = (size_t)(((rec[0] & 0x03) << 8) | rec[1]);
    if (dlen != len)
        return; // the length field disagrees with the record
    uint8_t dg[DDP_EXTENDED_HEADER_SIZE + DDP_MAX_DATA_SIZE];
    memcpy(dg, rec, len);
    if (rd16(&dg[DG_DST_NET]) == 0) {
        const uint8_t *data = dg + DDP_EXTENDED_HEADER_SIZE;
        size_t n = len - DDP_EXTENDED_HEADER_SIZE;
        if (dg[DG_TYPE] == DDP_TYPE_RTMP_RESPONSE && dg[DG_DST_SOCK] == ATALK_RTMP_SOCKET)
            rtmp_data_in(l, data, n);
        else if (dg[DG_TYPE] == DDP_TYPE_ZIP && dg[DG_DST_SOCK] == ATALK_ZIS_SOCKET) {
            ddp_header_t h = {.extended = true,
                              .src_net = rd16(&dg[DG_SRC_NET]),
                              .dst_net = 0,
                              .dst_socket = dg[DG_DST_SOCK],
                              .src_socket = dg[DG_SRC_SOCK],
                              .type = dg[DG_TYPE]};
            h.llap.dst = dg[DG_DST_NODE];
            h.llap.src = dg[DG_SRC_NODE];
            atalk_router_zip_in(&h, data, n);
        }
        return;
    }
    route_datagram(dg, len, link_port(l), l);
}

// === Links =====================================================================

static void link_send(atalk_link_t *l, uint8_t kind, const uint8_t *body, size_t len, bool reliable) {
    if (!l->in_use || l->state == ATALK_LINK_CLOSED || 1 + len > ATALK_LINK_REC_MAX)
        return;
    uint8_t rec[ATALK_LINK_REC_MAX];
    rec[0] = kind;
    if (len)
        memcpy(&rec[1], body, len);
    l->tx_records++;
    l->transport.send(l->ctx, rec, 1 + len, reliable);
}

static void link_send_hello(atalk_link_t *l) {
    uint8_t b[4 + 64];
    size_t nl = strlen(g_router.name);
    if (nl > 63)
        nl = 63;
    b[0] = ATALK_LINK_VERSION;
    wr16(&b[1], atalk_this_net());
    b[3] = (uint8_t)((l->friend_link ? ATALK_LINK_FLAG_FRIEND : 0));
    b[4] = (uint8_t)nl;
    memcpy(&b[5], g_router.name, nl);
    link_send(l, ATALK_LINK_HELLO, b, 5 + nl, true);
}

// Release the link's slot and its routes.  The transport is closed by the
// caller when it should be.
static void link_finish(atalk_link_t *l, const char *reason) {
    if (l->state == ATALK_LINK_CLOSED)
        return;
    l->state = ATALK_LINK_CLOSED;
    snprintf(l->close_reason, sizeof(l->close_reason), "%s", reason ? reason : "closed");
    LOG(1, "atalk router: link %d (%s) closed: %s", link_index(l), l->kind, l->close_reason);
    routes_fail_port(link_port(l));
}

atalk_link_t *atalk_link_open(const atalk_link_transport_t *transport, void *ctx, const char *kind, bool friend_link) {
    if (!transport || !transport->send)
        return NULL;
    atalk_link_t *l = NULL;
    // A slot never used, else the oldest closed one
    for (int i = 0; i < ATALK_LINK_MAX && !l; i++)
        if (!g_router.links[i].in_use)
            l = &g_router.links[i];
    for (int i = 0; i < ATALK_LINK_MAX && !l; i++)
        if (g_router.links[i].state == ATALK_LINK_CLOSED)
            l = &g_router.links[i];
    if (!l)
        return NULL;
    // Routes still pointing at a recycled slot's port belong to its old link
    for (int i = 0; i < MAX_ROUTES; i++)
        if (g_router.routes[i].in_use && g_router.routes[i].port == link_port(l))
            g_router.routes[i].in_use = false;
    memset(l, 0, sizeof(*l));
    l->in_use = true;
    l->state = ATALK_LINK_CONNECTING;
    l->transport = *transport;
    l->ctx = ctx;
    snprintf(l->kind, sizeof(l->kind), "%s", kind ? kind : "link");
    l->friend_link = friend_link;
    l->rtt_ns = -1;
    LOG(1, "atalk router: link %d (%s) opening", link_index(l), l->kind);
    link_send_hello(l);
    if (g_router.plugged)
        atalk_timer_arm(&g_router.ping_timer, 0, LINK_HELLO_RETRY_NS);
    return l;
}

void atalk_link_lost(atalk_link_t *link, const char *reason) {
    if (link && link->in_use)
        link_finish(link, reason);
}

void atalk_link_close(atalk_link_t *link, const char *reason) {
    if (!link || !link->in_use || link->state == ATALK_LINK_CLOSED)
        return;
    size_t nr = reason ? strlen(reason) : 0;
    uint8_t b[1 + 95];
    if (nr > 95)
        nr = 95;
    b[0] = (uint8_t)nr;
    memcpy(&b[1], reason ? reason : "", nr);
    link_send(link, ATALK_LINK_BYE, b, 1 + nr, true);
    link_finish(link, reason);
    if (link->transport.close)
        link->transport.close(link->ctx);
}

atalk_link_state_t atalk_link_state(const atalk_link_t *link) {
    return link ? link->state : ATALK_LINK_CLOSED;
}

uint16_t atalk_link_peer_net(const atalk_link_t *link) {
    return link ? link->peer_net : 0;
}

static void link_hello_in(atalk_link_t *l, const uint8_t *b, size_t len) {
    if (len < 5 || 5 + (size_t)b[4] > len) {
        atalk_link_close(l, "malformed HELLO");
        return;
    }
    if (b[0] != ATALK_LINK_VERSION) {
        char why[64];
        snprintf(why, sizeof(why), "link protocol version %u, expected %u", (unsigned)b[0], ATALK_LINK_VERSION);
        atalk_link_close(l, why);
        return;
    }
    uint16_t net = rd16(&b[1]);
    if (net != 0 && net == atalk_this_net()) {
        char why[96];
        snprintf(why, sizeof(why), "both sides use network %u", (unsigned)net);
        atalk_link_close(l, why);
        return;
    }
    size_t nl = b[4] < sizeof(l->peer_name) ? b[4] : sizeof(l->peer_name) - 1;
    memcpy(l->peer_name, &b[5], nl);
    l->peer_name[nl] = '\0';
    l->peer_net = net;
    if (l->state == ATALK_LINK_CONNECTING) {
        // Our own HELLO may have been lost before the other end listened:
        // answer this one, so it opens too
        link_send_hello(l);
        l->state = ATALK_LINK_OPEN;
        LOG(1, "atalk router: link %d (%s) open to network %u '%s'", link_index(l), l->kind, (unsigned)net,
            l->peer_name);
        if (g_router.seed)
            rtmp_send_link(l); // tell the other end our table at once
    }
}

void atalk_link_receive(atalk_link_t *l, const uint8_t *rec, size_t len) {
    if (!l || !l->in_use || l->state == ATALK_LINK_CLOSED || len < 1)
        return;
    l->rx_records++;
    const uint8_t *b = rec + 1;
    size_t n = len - 1;
    switch (rec[0]) {
    case ATALK_LINK_HELLO:
        link_hello_in(l, b, n);
        break;
    case ATALK_LINK_DDP:
        if (l->state == ATALK_LINK_OPEN && g_router.seed)
            link_ddp_in(l, b, n);
        break;
    case ATALK_LINK_PING:
        if (n == 8)
            link_send(l, ATALK_LINK_PONG, b, n, true);
        break;
    case ATALK_LINK_PONG:
        if (n == 8) {
            uint64_t sent = 0;
            for (int i = 0; i < 8; i++)
                sent = (sent << 8) | b[i];
            uint64_t now = atalk_now_ns();
            if (now >= sent)
                l->rtt_ns = (int64_t)(now - sent);
        }
        break;
    case ATALK_LINK_BYE: {
        char why[96] = "closed by the other end";
        if (n >= 1 && (size_t)b[0] + 1 <= n && b[0] > 0) {
            size_t wl = b[0] < sizeof(why) ? b[0] : sizeof(why) - 1;
            memcpy(why, &b[1], wl);
            why[wl] = '\0';
        }
        link_finish(l, why);
        if (l->transport.close)
            l->transport.close(l->ctx);
        break;
    }
    default:
        LOG(3, "atalk router: link %d: unknown record kind %u", link_index(l), (unsigned)rec[0]);
        break;
    }
}

// The ping timer: a guest-time stamp to every open link.
static void ping_timer_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    uint64_t now = atalk_now_ns();
    uint8_t b[8];
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(now >> (56 - 8 * i));
    bool connecting = false;
    for (int i = 0; i < ATALK_LINK_MAX; i++) {
        atalk_link_t *l = &g_router.links[i];
        if (l->in_use && l->state == ATALK_LINK_OPEN)
            link_send(l, ATALK_LINK_PING, b, sizeof(b), true);
        if (l->in_use && l->state == ATALK_LINK_CONNECTING) {
            link_send_hello(l); // the first may have gone before the other end listened
            connecting = true;
        }
    }
    // Every second while a link waits for its other end, else every 5 s
    if (g_router.plugged)
        atalk_timer_arm(&g_router.ping_timer, 0, connecting ? LINK_HELLO_RETRY_NS : LINK_PING_NS);
}

// === Lifecycle =================================================================

// Start or stop the router's timers to match its state.
// A timer already pending -- one a checkpoint restore replayed -- keeps its
// time, so a restored machine runs on exactly as the saved one would have.
static void router_timers_update(void) {
    if (g_router.seed && g_router.plugged) {
        if (!atalk_timer_pending(&g_router.rtmp_timer))
            atalk_timer_arm(&g_router.rtmp_timer, 0, ATALK_RTMP_SEND_NS);
        if (!atalk_timer_pending(&g_router.validity_timer))
            atalk_timer_arm(&g_router.validity_timer, 0, RTMP_VALIDITY_NS);
        if (!atalk_timer_pending(&g_router.ping_timer))
            atalk_timer_arm(&g_router.ping_timer, 0, LINK_PING_NS);
    } else {
        atalk_timer_cancel_all(&g_router.rtmp_timer);
        atalk_timer_cancel_all(&g_router.validity_timer);
        atalk_timer_cancel_all(&g_router.trigger_timer);
        atalk_timer_cancel_all(&g_router.zip_timer);
        atalk_timer_cancel_all(&g_router.ping_timer);
    }
}

void atalk_router_init(void) {
    if (g_router.zone_mac_len == 0)
        router_set_zone_text(ATALK_ROUTER_DEFAULT_ZONE);
    if (!g_router.name[0])
        snprintf(g_router.name, sizeof(g_router.name), "%s", "Granny Smith");
    route_set_local();
    static const atp_socket_handler_t handler = {.handle_request = zis_atp_request};
    if (atp_register_socket_handler(ATALK_ZIS_SOCKET, &handler, NULL) != 0)
        LOG(0, "Error: the router could not take the Zone Information Socket");
}

void atalk_router_register_timers(struct atalk_conn *conn) {
    atalk_timer_init(conn, &g_router.rtmp_timer, "atalk_router", "rtmp_send", &rtmp_timer_cb);
    atalk_timer_init(conn, &g_router.validity_timer, "atalk_router", "validity", &validity_timer_cb);
    atalk_timer_init(conn, &g_router.trigger_timer, "atalk_router", "rtmp_trigger", &trigger_timer_cb);
    atalk_timer_init(conn, &g_router.zip_timer, "atalk_router", "zip_query", &zip_timer_cb);
    atalk_timer_init(conn, &g_router.ping_timer, "atalk_router", "link_ping", &ping_timer_cb);
}

void atalk_router_plug(bool plugged) {
    g_router.plugged = plugged;
    router_timers_update();
}

const atalk_router_stats_t *atalk_router_get_stats(void) {
    return &g_router.stats;
}

// === Object model: appletalk.router ===========================================

static DEF_GETTER(router_attr_seed) {
    return val_bool(g_router.seed);
}
static DEF_SETTER(router_attr_set_seed) {
    if (in.b == g_router.seed)
        return val_none();
    g_router.seed = in.b;
    route_set_local();
    router_timers_update();
    // A running guest learns the change from the next RTMP Data (on) or by
    // aging A-ROUTER (off, about 50 s, 5-18); THIS-NET it keeps until it
    // restarts AppleTalk.
    LOG(1, "atalk router: %s", in.b ? "on" : "off");
    if (in.b)
        rtmp_send_local();
    return val_none();
}
static DEF_GETTER(router_attr_net) {
    return val_uint(2, g_router.net);
}
int atalk_router_set_net(uint16_t net) {
    if (net < 1 || net > ATALK_ROUTER_NET_MAX)
        return -1;
    if (net == g_router.net)
        return 0;
    for (int i = 0; i < ATALK_LINK_MAX; i++)
        if (g_router.links[i].in_use && g_router.links[i].state != ATALK_LINK_CLOSED)
            return -1;
    g_router.net = net;
    route_set_local();
    return 0;
}

static DEF_SETTER(router_attr_set_net) {
    if (in.u < 1 || in.u > ATALK_ROUTER_NET_MAX)
        return val_err("network number %llu is out of range: 1..%u", (unsigned long long)in.u,
                       (unsigned)ATALK_ROUTER_NET_MAX);
    if (atalk_router_set_net((uint16_t)in.u) != 0)
        return val_err("cannot renumber the network while it is linked to another");
    return val_none();
}
static DEF_GETTER(router_attr_zone) {
    return val_str(g_router.zone);
}
static DEF_SETTER(router_attr_set_zone) {
    char err[192] = "";
    if (atalk_nbp_name_check("zone name", in.s, err, sizeof(err)) != 0 || !router_set_zone_text(in.s)) {
        value_free(&in);
        return val_err("%s", err[0] ? err : "invalid zone name");
    }
    value_free(&in);
    route_set_local();
    return val_none();
}
static DEF_GETTER(router_attr_name) {
    return val_str(g_router.name);
}
static DEF_SETTER(router_attr_set_name) {
    size_t n = in.s ? strlen(in.s) : 0;
    if (n >= sizeof(g_router.name)) {
        value_free(&in);
        return val_err("a display name is at most %d bytes", (int)sizeof(g_router.name) - 1);
    }
    memcpy(g_router.name, in.s ? in.s : "", n);
    g_router.name[n] = '\0';
    value_free(&in);
    return val_none();
}
static DEF_GETTER(router_attr_share) {
    return val_bool(g_router.share_host_services);
}
static DEF_SETTER(router_attr_set_share) {
    g_router.share_host_services = in.b;
    return val_none();
}

static const member_t router_stats_members[] = {
    OBJ_U64_FIELD(atalk_router_stats_t, rtmp_data_out, "RTMP Data broadcasts on the local network"),
    OBJ_U64_FIELD(atalk_router_stats_t, rtmp_requests, "RTMP Requests and Route Data Requests answered"),
    OBJ_U64_FIELD(atalk_router_stats_t, rtmp_link_out, "RTMP Data sent over links"),
    OBJ_U64_FIELD(atalk_router_stats_t, rtmp_link_in, "RTMP Data received over links"),
    OBJ_U64_FIELD(atalk_router_stats_t, zip_queries, "ZIP Queries answered"),
    OBJ_U64_FIELD(atalk_router_stats_t, zip_queries_out, "ZIP Queries sent for unknown zones"),
    OBJ_U64_FIELD(atalk_router_stats_t, zip_replies_in, "Zones learnt from ZIP Replies"),
    OBJ_U64_FIELD(atalk_router_stats_t, zip_atp_requests, "ZIP GetMyZone/GetZoneList/GetLocalZones answered"),
    OBJ_U64_FIELD(atalk_router_stats_t, nbp_brrq, "NBP broadcast requests routed"),
    OBJ_U64_FIELD(atalk_router_stats_t, nbp_fwdreq_out, "NBP forward requests sent to other networks"),
    OBJ_U64_FIELD(atalk_router_stats_t, nbp_fwdreq_in, "NBP forward requests turned into local lookups"),
    OBJ_U64_FIELD(atalk_router_stats_t, forwarded_out, "Datagrams from this network forwarded over a link"),
    OBJ_U64_FIELD(atalk_router_stats_t, forwarded_transit, "Datagrams forwarded from one link to another"),
    OBJ_U64_FIELD(atalk_router_stats_t, dropped_no_route, "Datagrams dropped: no route to their network"),
    OBJ_U64_FIELD(atalk_router_stats_t, dropped_hops, "Datagrams dropped: 15 hops already"),
    OBJ_U64_FIELD(atalk_router_stats_t, dropped_filter, "Datagrams from a link for the host's services, filtered"),
};

static const class_desc_t router_stats_class = {
    .name = "atalk_router_stats",
    .members = router_stats_members,
    .n_members = ARRAY_LEN(router_stats_members),
};

// --- appletalk.router.routes[i] -------------------------------------------

static object_cache_t g_route_objects;
static object_cache_t g_link_objects;

static const route_t *route_of(struct object *self) {
    int i = object_entry_index(self);
    return (i >= 0 && i < MAX_ROUTES && g_router.routes[i].in_use) ? &g_router.routes[i] : NULL;
}
static DEF_GETTER(route_attr_net) {
    const route_t *r = route_of(self);
    return val_uint(2, r ? r->net : 0);
}
static DEF_GETTER(route_attr_distance) {
    const route_t *r = route_of(self);
    return val_uint(1, r ? r->distance : 0);
}
static DEF_GETTER(route_attr_state) {
    static const char *const names[] = {"good", "suspect", "bad", "bad"};
    const route_t *r = route_of(self);
    return val_str(r ? names[r->state] : "");
}
static DEF_GETTER(route_attr_via) {
    const route_t *r = route_of(self);
    if (!r)
        return val_str("");
    if (r->port == PORT_LOCAL)
        return val_str("local");
    char buf[24];
    snprintf(buf, sizeof(buf), "link %d", r->port - 1);
    return val_str(buf);
}
static DEF_GETTER(route_attr_zone) {
    const route_t *r = route_of(self);
    char buf[ATALK_NBP_TEXT_CAP] = "";
    if (r && r->zone_known)
        macroman_to_utf8(r->zone, r->zone_len, buf, sizeof(buf));
    return val_str(buf);
}

static const member_t route_members[] = {
    {.kind = MK_ATTR,
     .name = "net",
     .doc = "Network number",
     .attr = {.type = VK_UINT, .width = 2, .get = route_attr_net}     },
    {.kind = MK_ATTR,
     .name = "distance",
     .doc = "Hops to it: 0 for this network",
     .attr = {.type = VK_UINT, .width = 1, .get = route_attr_distance}},
    {.kind = MK_ATTR,
     .name = "state",
     .doc = "good, suspect (not confirmed lately) or bad (going away)",
     .attr = {.type = VK_STRING, .get = route_attr_state}             },
    {.kind = MK_ATTR,
     .name = "via",
     .doc = "local, or the link it is reached through (link N = links[N])",
     .attr = {.type = VK_STRING, .get = route_attr_via}               },
    {.kind = MK_ATTR,
     .name = "zone",
     .doc = "Its zone name; empty until ZIP has learnt it",
     .attr = {.type = VK_STRING, .get = route_attr_zone}              },
};

static const class_desc_t route_class = {
    .name = "atalk_route",
    .members = route_members,
    .n_members = ARRAY_LEN(route_members),
};

static struct object *routes_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= MAX_ROUTES || !g_router.routes[index].in_use)
        return NULL;
    return object_cache_at(&g_route_objects, index, NULL);
}

// Named lookup by network number, so `appletalk.router.routes["54"]` resolves.
static struct object *routes_lookup(struct object *self, const char *name) {
    (void)self;
    char *end = NULL;
    unsigned long net = strtoul(name, &end, 0);
    if (!end || *end || net == 0 || net > ATALK_ROUTER_NET_MAX)
        return NULL;
    for (int i = 0; i < MAX_ROUTES; i++)
        if (g_router.routes[i].in_use && g_router.routes[i].net == net)
            return object_cache_at(&g_route_objects, i, NULL);
    return NULL;
}

static const collection_desc_t routes_entries = {
    .entry = &route_class,
    .by_index = {.get = routes_get, .slots = MAX_ROUTES},
    .by_key = {.lookup = routes_lookup},
};

static const member_t routes_members[] = {
    OBJ_ENTRIES(&routes_entries, "The routing table: every network this router can reach"),
};

static const class_desc_t routes_class = {
    .name = "atalk_routes",
    .members = routes_members,
    .n_members = ARRAY_LEN(routes_members),
};

// --- appletalk.router.links[i] --------------------------------------------

static atalk_link_t *link_of(struct object *self) {
    int i = object_entry_index(self);
    return (i >= 0 && i < ATALK_LINK_MAX && g_router.links[i].in_use) ? &g_router.links[i] : NULL;
}
static DEF_GETTER(link_attr_state) {
    static const char *const names[] = {"connecting", "open", "closed"};
    const atalk_link_t *l = link_of(self);
    return val_str(l ? names[l->state] : "");
}
static DEF_GETTER(link_attr_transport) {
    const atalk_link_t *l = link_of(self);
    return val_str(l ? l->kind : "");
}
static DEF_GETTER(link_attr_friend) {
    const atalk_link_t *l = link_of(self);
    return val_bool(l && l->friend_link);
}
static DEF_GETTER(link_attr_peer_net) {
    const atalk_link_t *l = link_of(self);
    return val_uint(2, l ? l->peer_net : 0);
}
static DEF_GETTER(link_attr_peer_name) {
    const atalk_link_t *l = link_of(self);
    return val_str(l ? l->peer_name : "");
}
static DEF_GETTER(link_attr_peer_zone) {
    const atalk_link_t *l = link_of(self);
    const route_t *r = l && l->peer_net ? route_find(l->peer_net) : NULL;
    char buf[ATALK_NBP_TEXT_CAP] = "";
    if (r && r->zone_known)
        macroman_to_utf8(r->zone, r->zone_len, buf, sizeof(buf));
    return val_str(buf);
}
static DEF_GETTER(link_attr_rtt) {
    const atalk_link_t *l = link_of(self);
    return val_int(l && l->rtt_ns >= 0 ? l->rtt_ns / 1000000 : -1);
}
static DEF_GETTER(link_attr_reason) {
    const atalk_link_t *l = link_of(self);
    return val_str(l ? l->close_reason : "");
}
static DEF_METHOD(link_method_close) {
    atalk_link_t *l = link_of(self);
    if (!l || l->state == ATALK_LINK_CLOSED)
        return val_err("the link is not open");
    atalk_link_close(l, "closed by the user");
    return val_none();
}

static const member_t link_members[] = {
    {.kind = MK_ATTR,
     .name = "state",
     .doc = "connecting, open or closed",
     .attr = {.type = VK_STRING, .get = link_attr_state}                                                                                  },
    {.kind = MK_ATTR,
     .name = "transport",
     .doc = "What carries it: tabs, webrtc, socket or loopback",
     .attr = {.type = VK_STRING, .get = link_attr_transport}                                                                              },
    {.kind = MK_ATTR,
     .name = "friend",
     .doc = "The other end is another person's instance",
     .attr = {.type = VK_BOOL, .get = link_attr_friend}                                                                                   },
    {.kind = MK_ATTR,
     .name = "peer_net",
     .doc = "The network at the other end",
     .attr = {.type = VK_UINT, .width = 2, .get = link_attr_peer_net}                                                                     },
    {.kind = MK_ATTR,
     .name = "peer_zone",
     .doc = "Its zone, once ZIP has learnt it",
     .attr = {.type = VK_STRING, .get = link_attr_peer_zone}                                                                              },
    {.kind = MK_ATTR,
     .name = "peer_name",
     .doc = "The other instance's display name",
     .attr = {.type = VK_STRING, .get = link_attr_peer_name}                                                                              },
    {.kind = MK_ATTR,
     .name = "rtt_ms",
     .doc = "Round trip of the last PING in guest milliseconds; -1 before the first",
     .attr = {.type = VK_INT, .get = link_attr_rtt}                                                                                       },
    {.kind = MK_ATTR,   .name = "reason", .doc = "Why it closed",                     .attr = {.type = VK_STRING, .get = link_attr_reason}},
    {.kind = MK_METHOD,
     .name = "close",
     .doc = "Close the link",
     .method = {.args = NULL, .nargs = 0, .result = VK_NONE, .fn = link_method_close, .ui_flags = MM_MUTATE}                              },
};

static const class_desc_t link_class = {
    .name = "atalk_link",
    .members = link_members,
    .n_members = ARRAY_LEN(link_members),
};

static struct object *links_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= ATALK_LINK_MAX || !g_router.links[index].in_use)
        return NULL;
    return object_cache_at(&g_link_objects, index, NULL);
}

static const collection_desc_t links_entries = {
    .entry = &link_class,
    .by_index = {.get = links_get, .slots = ATALK_LINK_MAX},
};

static const member_t links_members[] = {
    OBJ_ENTRIES(&links_entries, "Links to other instances' routers"),
};

static const class_desc_t links_class = {
    .name = "atalk_links",
    .members = links_members,
    .n_members = ARRAY_LEN(links_members),
};

static DEF_METHOD(router_method_link_udp) {
    char err[192] = "";
    const char *local = argv[0].s;
    const char *remote = argv[1].s;
    bool friend_link = argc > 2 && argv[2].kind == VK_BOOL && argv[2].b;
    int rc = platform_atalk_link_udp(local, remote, friend_link, err, sizeof(err));
    if (rc == -2)
        return val_err("links over the host's network are not available on this platform");
    if (rc != 0)
        return val_err("%s", err[0] ? err : "cannot open the link");
    return val_none();
}

static const arg_decl_t link_udp_args[] = {
    {.name = "local", .kind = VK_STRING, .doc = "host:port this end binds"},
    {.name = "remote", .kind = VK_STRING, .doc = "host:port of the other instance's router"},
    {.name = "friend",
     .kind = VK_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "The other end is another person's"},
};

static const member_t router_members[] = {
    {.kind = MK_ATTR,
     .name = "seed",
     .doc = "Run as this network's router (RTMP, ZIP); off leaves a routerless cable",
     .attr = {.type = VK_BOOL, .get = router_attr_seed, .set = router_attr_set_seed}                                },
    {.kind = MK_ATTR,
     .name = "net",
     .doc = "This network's number, 1..65279; a running guest picks a change up when it restarts AppleTalk",
     .attr = {.type = VK_UINT, .width = 2, .get = router_attr_net, .set = router_attr_set_net}                      },
    {.kind = MK_ATTR,
     .name = "zone",
     .doc = "This network's zone name, as the Chooser lists it",
     .attr = {.type = VK_STRING,
              .validation_flags = OBJ_ARG_NONEMPTY,
              .get = router_attr_zone,
              .set = router_attr_set_zone}                                                                          },
    {.kind = MK_ATTR,
     .name = "name",
     .doc = "This instance's display name, sent to linked instances",
     .attr = {.type = VK_STRING, .get = router_attr_name, .set = router_attr_set_name}                              },
    {.kind = MK_METHOD,
     .name = "link_udp",
     .doc = "Link to another instance's router over UDP (headless only)",
     .method =
         {.args = link_udp_args, .nargs = 3, .result = VK_NONE, .fn = router_method_link_udp, .ui_flags = MM_MUTATE}},
    {.kind = MK_ATTR,
     .name = "share_host_services",
     .doc = "Let friends' Macs use this host's services (Shared Folders, printer, program linking)",
     .attr = {.type = VK_BOOL, .get = router_attr_share, .set = router_attr_set_share}                              },
};

static const class_desc_t router_class = {
    .name = "atalk_router",
    .doc = "The network's router: its number, zone, links and routes",
    .members = router_members,
    .n_members = ARRAY_LEN(router_members),
};

void atalk_router_install_objects(struct object *parent) {
    struct object *router = object_new(&router_class, NULL, "router");
    if (!router)
        return;
    object_set_category(router, M_CAT_ADVANCED);
    object_attach(parent, router);
    struct object *stats = object_new(&router_stats_class, (void *)&g_router.stats, "stats");
    if (stats)
        object_attach(router, stats);
    struct object *routes = object_new(&routes_class, NULL, "routes");
    if (routes)
        object_attach(router, routes);
    struct object *links = object_new(&links_class, NULL, "links");
    if (links)
        object_attach(router, links);
    // Entry objects are handed out by the collection callbacks and never
    // attached; like the subtree, they live as long as the process.
    g_route_objects = (object_cache_t)OBJECT_CACHE(&route_class, NULL);
    object_cache_set_parent(&g_route_objects, routes);
    g_link_objects = (object_cache_t)OBJECT_CACHE(&link_class, NULL);
    object_cache_set_parent(&g_link_objects, links);
}
