// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_router.c
// The seed router of the emulated LocalTalk network.  See appletalk_router.h.
//
// Everything here runs on the worker thread, like the rest of the stack: the
// guest's datagrams arrive through appletalk.c, the RTMP send timer is a
// guest-time scheduler event, and the object model's calls come through the
// mailbox.

#include "appletalk_router.h"

#include "appletalk.h"
#include "log.h"
#include "macroman.h"
#include "object.h"
#include "value.h"

#include <stdio.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("appletalk");

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// RTMP (Inside AppleTalk ch. 5, C-11)
#define RTMP_FUNC_REQUEST     1 // RTMP Request: THIS-NET and A-ROUTER, please
#define RTMP_FUNC_RDR_SPLIT   2 // Route Data Request, with split horizon
#define RTMP_FUNC_RDR_NOSPLIT 3 // Route Data Request, the whole table
#define RTMP_ID_BITS          8 // a LocalTalk node ID is 8 bits long
#define RTMP_VERSION          0x82 // the version indicator ($000082 on nonextended networks, 5-15)

// ZIP (Inside AppleTalk ch. 8, C-12)
#define ZIP_FUNC_QUERY          1
#define ZIP_FUNC_REPLY          2
#define ZIP_FUNC_GETNETINFO     5
#define ZIP_FUNC_EXT_REPLY      8
#define ZIP_ATP_GET_MY_ZONE     7 // in ATP user byte 0
#define ZIP_ATP_GET_ZONE_LIST   8
#define ZIP_ATP_GET_LOCAL_ZONES 9

// The router's state: one per process, like the network it serves.
typedef struct {
    bool seed; // the router runs (appletalk.router.seed)
    uint16_t net; // this network's number
    char zone[ATALK_NBP_TEXT_CAP]; // its zone name, UTF-8
    uint8_t zone_mac[ATALK_NBP_NAME_MAX]; // ...and in MacRoman, as it goes on the wire
    size_t zone_mac_len;
    bool plugged; // a connection is on the cable
    atalk_timer_t rtmp_timer; // the send-RTMP timer
    atalk_router_stats_t stats;
} router_t;

static router_t g_router = {
    .seed = true,
    .net = ATALK_ROUTER_DEFAULT_NET,
};

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
    if (len != (int)g_router.zone_mac_len)
        return false;
    for (int i = 0; i < len; i++)
        if (macroman_fold(zone[i]) != macroman_fold(g_router.zone_mac[i]))
            return false; // zone names compare case-insensitively (Appendix D)
    return true;
}

// === RTMP ======================================================================

// Build an RTMP Data packet (Inside AppleTalk 5-13): the sender's network
// number and node ID, the nonextended version indicator, then the routing
// tuples when `with_tuples` (an RTMP Response carries none, 5-17).  Returns
// the length.
static int rtmp_build(uint8_t *buf, bool with_tuples) {
    int n = 0;
    buf[n++] = (uint8_t)(g_router.net >> 8); // sender's network number
    buf[n++] = (uint8_t)g_router.net;
    buf[n++] = RTMP_ID_BITS; // sender's node ID: its length in bits...
    buf[n++] = ATALK_HOST_NODE; // ...and the ID
    buf[n++] = 0; // version indicator $000082
    buf[n++] = 0;
    buf[n++] = RTMP_VERSION;
    if (with_tuples) {
        // The routing table: the local network, directly connected
        buf[n++] = (uint8_t)(g_router.net >> 8);
        buf[n++] = (uint8_t)g_router.net;
        buf[n++] = 0; // distance 0
    }
    return n;
}

// Broadcast RTMP Data on the local network, from and to the RTMP socket.
static void rtmp_broadcast(void) {
    if (!atalk_link_ready())
        return; // the guest's AppleTalk is not up: nobody to tell
    uint8_t buf[16];
    int n = rtmp_build(buf, true);
    atalk_socket_addr_t dest = {.net = 0, .node = 0xFF, .socket = ATALK_RTMP_SOCKET};
    if (atalk_ddp_send_to(&dest, ATALK_RTMP_SOCKET, DDP_TYPE_RTMP_RESPONSE, buf, n) == 0)
        g_router.stats.rtmp_data_out++;
}

// The send-RTMP timer: broadcast, then re-arm.
static void rtmp_timer_cb(void *source, uint64_t data) {
    (void)source;
    (void)data;
    if (!g_router.seed || !g_router.plugged)
        return;
    rtmp_broadcast();
    atalk_timer_arm(&g_router.rtmp_timer, 0, ATALK_RTMP_SEND_NS);
}

// Start or stop the send-RTMP timer to match the router's state.
static void rtmp_timer_update(void) {
    if (g_router.seed && g_router.plugged)
        atalk_timer_arm(&g_router.rtmp_timer, 0, ATALK_RTMP_SEND_NS);
    else
        atalk_timer_cancel_all(&g_router.rtmp_timer);
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
    // (5-19); with one network the split-horizon and whole-table answers are
    // the same.
    uint8_t out[16];
    int n = rtmp_build(out, func != RTMP_FUNC_REQUEST);
    atalk_socket_addr_t dest = {.net = ddp->src_net, .node = ddp->llap.src, .socket = ddp->src_socket};
    LOG(3, "RTMP %s from node %u: network %u, router %u", func == RTMP_FUNC_REQUEST ? "Request" : "RDR",
        (unsigned)ddp->llap.src, (unsigned)g_router.net, (unsigned)ATALK_HOST_NODE);
    if (atalk_ddp_send_to(&dest, ATALK_RTMP_SOCKET, DDP_TYPE_RTMP_RESPONSE, out, n) == 0)
        g_router.stats.rtmp_requests++;
    return true;
}

// === ZIP =======================================================================

// ZIP Query (8-11): a list of network numbers whose zones the requester wants.
// The router answers with a Reply carrying the zone of every one it knows --
// here only its own network -- and stays silent if it knows none.
static void zip_query_in(const ddp_header_t *ddp, const uint8_t *buf, size_t len) {
    if (len < 2)
        return;
    int count = buf[1];
    if (len < 2 + (size_t)count * 2)
        return; // shorter than its network count says
    bool asked = false;
    for (int i = 0; i < count; i++) {
        uint16_t net = (uint16_t)((buf[2 + i * 2] << 8) | buf[3 + i * 2]);
        if (net == g_router.net)
            asked = true;
    }
    if (!asked)
        return;
    uint8_t out[4 + 1 + ATALK_NBP_NAME_MAX];
    int n = 0;
    out[n++] = ZIP_FUNC_REPLY;
    out[n++] = 1; // one network's zones list
    out[n++] = (uint8_t)(g_router.net >> 8);
    out[n++] = (uint8_t)g_router.net;
    out[n++] = (uint8_t)g_router.zone_mac_len; // the zone name, length first
    memcpy(&out[n], g_router.zone_mac, g_router.zone_mac_len);
    n += (int)g_router.zone_mac_len;
    atalk_socket_addr_t dest = {.net = ddp->src_net, .node = ddp->llap.src, .socket = ddp->src_socket};
    if (atalk_ddp_send_to(&dest, ATALK_ZIS_SOCKET, DDP_TYPE_ZIP, out, n) == 0)
        g_router.stats.zip_queries++;
}

void atalk_router_zip_in(const ddp_header_t *ddp, const uint8_t *buf, size_t len) {
    if (!g_router.seed || len < 1)
        return;
    switch (buf[0]) {
    case ZIP_FUNC_QUERY:
        zip_query_in(ddp, buf, len);
        break;
    case ZIP_FUNC_GETNETINFO:
        // GetNetInfo is for nodes on extended networks (8-8); LocalTalk is not
        // one, and its nodes ask with GetMyZone instead.
        LOG(3, "ZIP GetNetInfo on a nonextended network: ignored");
        break;
    default:
        // Replies and Notifies are for routers we do not exchange with yet
        LOG(4, "ZIP function %u ignored", (unsigned)buf[0]);
        break;
    }
}

// ZIP's ATP requests on the ZIS (8-7, 8-13): GetMyZone, GetZoneList and
// GetLocalZones, at-least-once, one response packet each.  The user bytes of
// a request carry the function and (for the list requests) a 1-based start
// index; those of a response carry LastFlag and the number of names in it.
static void zis_atp_request(const ddp_header_t *ddp, atp_packet_t *req, void *ctx) {
    (void)ctx;
    if (!g_router.seed)
        return; // no router: the request goes unanswered, as on a routerless cable
    uint8_t func = req->user[0];
    uint16_t start = (uint16_t)((req->user[2] << 8) | req->user[3]);
    uint8_t data[1 + ATALK_NBP_NAME_MAX];
    int dlen = 0;
    uint8_t user[4] = {0, 0, 0, 0};
    switch (func) {
    case ZIP_ATP_GET_MY_ZONE:
    case ZIP_ATP_GET_ZONE_LIST:
    case ZIP_ATP_GET_LOCAL_ZONES:
        // One zone on the internet, and it is the requester's: every one of
        // them lists it, from index 1 (8-7).  GetMyZone ignores the index.
        if (func == ZIP_ATP_GET_MY_ZONE || start <= 1) {
            data[dlen++] = (uint8_t)g_router.zone_mac_len;
            memcpy(&data[dlen], g_router.zone_mac, g_router.zone_mac_len);
            dlen += (int)g_router.zone_mac_len;
            user[3] = 1; // one name in this response
        }
        // GetMyZone's response has no LastFlag; the list requests' says this
        // was the last name (an index past the end gets an empty last answer).
        user[0] = func == ZIP_ATP_GET_MY_ZONE ? 0 : 1;
        break;
    default:
        LOG(3, "ZIP ATP function %u from node %u ignored", (unsigned)func, (unsigned)ddp->llap.src);
        return;
    }
    LOG(3, "ZIP %s from node %u: %d zone(s)",
        func == ZIP_ATP_GET_MY_ZONE     ? "GetMyZone"
        : func == ZIP_ATP_GET_ZONE_LIST ? "GetZoneList"
                                        : "GetLocalZones",
        (unsigned)ddp->llap.src, (int)user[3]);
    if (atp_responder_send_simple(ddp, req, user, data, dlen, false) > 0)
        g_router.stats.zip_atp_requests++;
}

void atalk_router_note_brrq(void) {
    g_router.stats.nbp_brrq++;
}

// === Lifecycle =================================================================

void atalk_router_init(void) {
    if (g_router.zone_mac_len == 0)
        router_set_zone_text(ATALK_ROUTER_DEFAULT_ZONE);
    static const atp_socket_handler_t handler = {.handle_request = zis_atp_request};
    if (atp_register_socket_handler(ATALK_ZIS_SOCKET, &handler, NULL) != 0)
        LOG(0, "Error: the router could not take the Zone Information Socket");
}

void atalk_router_register_timers(struct atalk_conn *conn) {
    atalk_timer_init(conn, &g_router.rtmp_timer, "atalk_router", "rtmp_send", &rtmp_timer_cb);
}

void atalk_router_plug(bool plugged) {
    g_router.plugged = plugged;
    rtmp_timer_update();
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
    rtmp_timer_update();
    // A running guest learns the change from the next RTMP Data (on) or by
    // aging A-ROUTER (off, about 50 s, 5-18); THIS-NET it keeps until it
    // restarts AppleTalk.
    LOG(1, "atalk router: %s", in.b ? "on" : "off");
    if (in.b)
        rtmp_broadcast();
    return val_none();
}
static DEF_GETTER(router_attr_net) {
    return val_uint(2, g_router.net);
}
static DEF_SETTER(router_attr_set_net) {
    if (in.u < 1 || in.u > ATALK_ROUTER_NET_MAX)
        return val_err("network number %llu is out of range: 1..%u", (unsigned long long)in.u,
                       (unsigned)ATALK_ROUTER_NET_MAX);
    g_router.net = (uint16_t)in.u;
    return val_none();
}
static DEF_GETTER(router_attr_zone) {
    return val_str(g_router.zone);
}
static DEF_SETTER(router_attr_set_zone) {
    char err[192];
    if (atalk_nbp_name_check("zone name", in.s, err, sizeof(err)) != 0 || !router_set_zone_text(in.s)) {
        value_free(&in);
        return val_err("%s", err[0] ? err : "invalid zone name");
    }
    value_free(&in);
    return val_none();
}

static const member_t router_stats_members[] = {
    OBJ_U64_FIELD(atalk_router_stats_t, rtmp_data_out, "RTMP Data broadcasts on the local network"),
    OBJ_U64_FIELD(atalk_router_stats_t, rtmp_requests, "RTMP Requests and Route Data Requests answered"),
    OBJ_U64_FIELD(atalk_router_stats_t, zip_queries, "ZIP Queries answered"),
    OBJ_U64_FIELD(atalk_router_stats_t, zip_atp_requests, "ZIP GetMyZone/GetZoneList/GetLocalZones answered"),
    OBJ_U64_FIELD(atalk_router_stats_t, nbp_brrq, "NBP broadcast requests turned into local lookups"),
};

static const class_desc_t router_stats_class = {
    .name = "atalk_router_stats",
    .members = router_stats_members,
    .n_members = ARRAY_LEN(router_stats_members),
};

static const member_t router_members[] = {
    {.kind = MK_ATTR,
     .name = "seed",
     .doc = "Run as this network's router (RTMP, ZIP); off leaves a routerless cable",
     .attr = {.type = VK_BOOL, .get = router_attr_seed, .set = router_attr_set_seed}          },
    {.kind = MK_ATTR,
     .name = "net",
     .doc = "This network's number, 1..65279; a running guest picks a change up when it restarts AppleTalk",
     .attr = {.type = VK_UINT, .width = 2, .get = router_attr_net, .set = router_attr_set_net}},
    {.kind = MK_ATTR,
     .name = "zone",
     .doc = "This network's zone name, as the Chooser lists it",
     .attr = {.type = VK_STRING,
              .validation_flags = OBJ_ARG_NONEMPTY,
              .get = router_attr_zone,
              .set = router_attr_set_zone}                                                    },
};

static const class_desc_t router_class = {
    .name = "atalk_router",
    .doc = "The network's router: its number, zone, and the RTMP/ZIP it answers",
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
}
