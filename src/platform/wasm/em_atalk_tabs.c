// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_atalk_tabs.c
// The tab mesh: every tab of this browser profile whose machine has
// AppleTalk on links its router to every other's, with no user action
// (appletalk_link.h; docs/internals/core/network/appletalk_router.md).
//
// It runs in the emulator's own worker -- the thread the router runs on --
// over one BroadcastChannel, "gs-atalk", which workers can open.  Nothing
// crosses the page's main thread, so a hidden tab, whose main-thread timers
// the browser throttles, keeps its links (the worker itself keeps running on
// a timer while hidden, em_main.c).
//
// Messages on the channel (structured-clone objects):
//
//   {t: "hello", id, net}         every ANNOUNCE_MS while this tab is eligible:
//                                 a machine runs with its router on
//   {t: "rec", from, to, d}       one link record (Uint8Array) for tab `to`
//   {t: "bye", id}                this tab leaves (AppleTalk off, page gone)
//
// A tab that hears another's hello opens a link to it; both ends do, so each
// pair has one link, one on each side.  A tab not heard for LOST_MS is gone:
// its link is lost and its routes go bad.  Link records are addressed by tab
// id; everyone hears them and all but the addressee drop them.

#include "appletalk_internal.h"
#include "appletalk_link.h"
#include "appletalk_router.h"

#include <emscripten.h>
#include <stdint.h>
#include <string.h>

// One peer tab's link: slot i here is peer i in the JS table.
static atalk_link_t *s_links[ATALK_LINK_MAX];

// The receive buffer the JS side copies a record into before calling in.
static uint8_t s_rx[ATALK_LINK_REC_MAX];

// The link transport: one record to peer `slot` (ctx), posted on the channel.
// clang-format off
EM_JS(void, tabs_post_js, (int slot, const uint8_t *rec, int len), {
    var m = Module.gsAtalkTabs;
    if (!m || !m.ch) return;
    var peer = m.slots[slot];
    if (!peer) return;
    m.ch.postMessage({t: "rec", from: m.id, to: peer, d: HEAPU8.slice(rec, rec + len)});
});
// clang-format on

static void tabs_send(void *ctx, const uint8_t *rec, size_t len, bool reliable) {
    (void)reliable; // BroadcastChannel delivers in order, and does not drop
    tabs_post_js((int)(intptr_t)ctx, rec, (int)len);
}

// The router is done with the link (it closed it, or the other end did).
// clang-format off
EM_JS(void, tabs_forget_js, (int slot), {
    var m = Module.gsAtalkTabs;
    if (m && m.slots[slot]) {
        delete m.bySlotPeer[m.slots[slot]];
        m.slots[slot] = null;
    }
});
// clang-format on

static void tabs_close(void *ctx) {
    int slot = (int)(intptr_t)ctx;
    if (slot >= 0 && slot < ATALK_LINK_MAX)
        s_links[slot] = NULL;
    tabs_forget_js(slot);
}

static const atalk_link_transport_t k_tabs_transport = {.send = tabs_send, .close = tabs_close};

// --- called from the JS side ---------------------------------------------------

// The buffer a record is copied into before em_atalk_tab_rx.
EMSCRIPTEN_KEEPALIVE uint8_t *em_atalk_tab_rxbuf(void) {
    return s_rx;
}

// Whether this tab takes part: its machine is on the cable with the router
// on.  Its network number goes in the hello (0: not eligible).
EMSCRIPTEN_KEEPALIVE int em_atalk_tab_net(void) {
    return atalk_router_active() && atalk_scheduler() ? (int)atalk_this_net() : 0;
}

// A new peer in JS slot `slot`: open the link.  0, or -1 if the router has no
// free link.
EMSCRIPTEN_KEEPALIVE int em_atalk_tab_open(int slot) {
    if (slot < 0 || slot >= ATALK_LINK_MAX || s_links[slot])
        return -1;
    s_links[slot] = atalk_link_open(&k_tabs_transport, (void *)(intptr_t)slot, "tabs", false);
    return s_links[slot] ? 0 : -1;
}

// A record for the link in `slot`, `len` bytes in em_atalk_tab_rxbuf().
EMSCRIPTEN_KEEPALIVE void em_atalk_tab_rx(int slot, int len) {
    if (slot >= 0 && slot < ATALK_LINK_MAX && s_links[slot] && len > 0 && len <= (int)sizeof(s_rx))
        atalk_link_receive(s_links[slot], s_rx, (size_t)len);
}

// The peer in `slot` left or went silent.
EMSCRIPTEN_KEEPALIVE void em_atalk_tab_lost(int slot, int said_bye) {
    if (slot < 0 || slot >= ATALK_LINK_MAX || !s_links[slot])
        return;
    atalk_link_t *l = s_links[slot];
    s_links[slot] = NULL;
    atalk_link_lost(l, said_bye ? "the other tab left" : "the other tab stopped answering");
}

// --- the mesh ----------------------------------------------------------------

// clang-format off
EM_JS(void, tabs_start_js, (int max_links), {
    if (typeof BroadcastChannel === "undefined") return; // no tabs to mesh with
    if (Module.gsAtalkTabs && Module.gsAtalkTabs.ch) return;
    var ANNOUNCE_MS = 2000, LOST_MS = 7000;
    var m = Module.gsAtalkTabs = {
        id: (self.crypto && crypto.randomUUID) ? crypto.randomUUID() : String(Math.random()).slice(2),
        ch: new BroadcastChannel("gs-atalk"),
        slots: new Array(max_links).fill(null), // slot -> peer id
        bySlotPeer: {},                          // peer id -> slot
        seen: {},                                // peer id -> last heard (ms)
        eligible: false
    };
    function slotOf(peer, open) {
        var s = m.bySlotPeer[peer];
        if (s !== undefined) return s;
        if (!open) return -1;
        for (var i = 0; i < m.slots.length; i++) {
            if (m.slots[i] === null) {
                m.slots[i] = peer;
                m.bySlotPeer[peer] = i;
                if (_em_atalk_tab_open(i) !== 0) {
                    m.slots[i] = null;
                    delete m.bySlotPeer[peer];
                    return -1;
                }
                return i;
            }
        }
        return -1;
    }
    function lose(peer, bye) {
        var s = m.bySlotPeer[peer];
        delete m.seen[peer];
        if (s === undefined) return;
        delete m.bySlotPeer[peer];
        m.slots[s] = null;
        _em_atalk_tab_lost(s, bye ? 1 : 0);
    }
    m.ch.onmessage = function(ev) {
        var msg = ev.data;
        if (!msg || msg.from === m.id || msg.id === m.id) return;
        if (msg.t === "hello") {
            m.seen[msg.id] = Date.now();
            if (m.eligible && msg.net > 0) slotOf(msg.id, true);
        } else if (msg.t === "rec") {
            if (msg.to !== m.id || !m.eligible) return;
            m.seen[msg.from] = Date.now();
            // A record from a tab whose hello we missed opens its link too
            var s = slotOf(msg.from, true);
            if (s < 0) return;
            var d = msg.d;
            if (!d || d.length > 640) return;
            HEAPU8.set(d, _em_atalk_tab_rxbuf());
            _em_atalk_tab_rx(s, d.length);
        } else if (msg.t === "bye") {
            lose(msg.id, true);
        }
    };
    m.tick = function() {
        var net = _em_atalk_tab_net();
        var was = m.eligible;
        m.eligible = net > 0;
        if (m.eligible) {
            m.ch.postMessage({t: "hello", id: m.id, net: net});
        } else if (was) {
            m.ch.postMessage({t: "bye", id: m.id});
            for (var p in m.bySlotPeer) lose(p, true);
        }
        var now = Date.now();
        for (var p2 in m.seen)
            if (now - m.seen[p2] > LOST_MS) lose(p2, false);
    };
    m.timer = setInterval(m.tick, ANNOUNCE_MS);
    m.tick();
});
// clang-format on

// === Friend links: carried by the page =========================================
//
// A link to another person's instance travels over a WebRTC data channel,
// which only the page's main thread can hold.  For each one the page opens
// a private BroadcastChannel and asks the core (appletalk.router.link_channel)
// to link through it; records cross between this worker and the page there:
//
//   worker -> page   {t: "rec", d, r}   one record; r: wants the reliable channel
//                    {t: "closed"}      the router is done with the link
//   page -> worker   {t: "rec", d}      one record from the other instance
//                    {t: "lost", why}   the peer connection went away

#include "platform_hooks.h"

#include <stdio.h>

static atalk_link_t *s_friends[ATALK_LINK_MAX];

// clang-format off
EM_JS(int, friend_attach_js, (int slot, const char *name), {
    if (typeof BroadcastChannel === "undefined") return -1;
    var f = Module.gsAtalkFriends = (Module.gsAtalkFriends || {});
    var ch = new BroadcastChannel(UTF8ToString(name));
    f[slot] = ch;
    ch.onmessage = function(ev) {
        var msg = ev.data;
        if (!msg) return;
        if (msg.t === "rec" && msg.d && msg.d.length <= 640) {
            HEAPU8.set(msg.d, _em_atalk_tab_rxbuf());
            _em_atalk_friend_rx(slot, msg.d.length);
        } else if (msg.t === "lost") {
            _em_atalk_friend_lost(slot);
        }
    };
    return 0;
});

EM_JS(void, friend_post_js, (int slot, const uint8_t *rec, int len, int reliable), {
    var f = Module.gsAtalkFriends;
    if (f && f[slot]) f[slot].postMessage({t: "rec", d: HEAPU8.slice(rec, rec + len), r: reliable !== 0});
});

EM_JS(void, friend_detach_js, (int slot), {
    var f = Module.gsAtalkFriends;
    if (f && f[slot]) {
        try { f[slot].postMessage({t: "closed"}); } catch (e) {}
        f[slot].close();
        delete f[slot];
    }
});
// clang-format on

static void friend_send(void *ctx, const uint8_t *rec, size_t len, bool reliable) {
    friend_post_js((int)(intptr_t)ctx, rec, (int)len, reliable ? 1 : 0);
}

static void friend_close(void *ctx) {
    int slot = (int)(intptr_t)ctx;
    if (slot >= 0 && slot < ATALK_LINK_MAX)
        s_friends[slot] = NULL;
    friend_detach_js(slot);
}

static const atalk_link_transport_t k_friend_transport = {.send = friend_send, .close = friend_close};

// A record from the page for the friend link in `slot`.
EMSCRIPTEN_KEEPALIVE void em_atalk_friend_rx(int slot, int len) {
    if (slot >= 0 && slot < ATALK_LINK_MAX && s_friends[slot] && len > 0 && len <= (int)sizeof(s_rx))
        atalk_link_receive(s_friends[slot], s_rx, (size_t)len);
}

// The page lost the peer connection of the friend link in `slot`.
EMSCRIPTEN_KEEPALIVE void em_atalk_friend_lost(int slot) {
    if (slot < 0 || slot >= ATALK_LINK_MAX || !s_friends[slot])
        return;
    atalk_link_t *l = s_friends[slot];
    s_friends[slot] = NULL;
    atalk_link_lost(l, "the connection to your friend was lost");
    friend_detach_js(slot);
}

int platform_atalk_link_channel(const char *name, bool friend_link, char *err, size_t err_len) {
    int slot = -1;
    for (int i = 0; i < ATALK_LINK_MAX && slot < 0; i++)
        if (!s_friends[i])
            slot = i;
    if (slot < 0 || !name || !name[0]) {
        snprintf(err, err_len, "%s", slot < 0 ? "every link slot is in use" : "no channel named");
        return -1;
    }
    if (friend_attach_js(slot, name) != 0) {
        snprintf(err, err_len, "this browser has no BroadcastChannel");
        return -1;
    }
    s_friends[slot] = atalk_link_open(&k_friend_transport, (void *)(intptr_t)slot, "webrtc", friend_link);
    if (!s_friends[slot]) {
        friend_detach_js(slot);
        snprintf(err, err_len, "the router has no free link slot");
        return -1;
    }
    return 0;
}

// A random network number, 1..65279, from the browser's generator.
// clang-format off
EM_JS(int, tabs_random_net_js, (int max), {
    var a = new Uint32Array(1);
    if (self.crypto && crypto.getRandomValues) crypto.getRandomValues(a); else a[0] = Math.random() * 4294967296;
    return 1 + (a[0] % max);
});
// clang-format on

void em_atalk_tabs_init(void) {
    // Every tab its own network number: tabs that all kept the default (1)
    // would refuse each other's links.  A clash between two random numbers
    // (about 1 in 65000 per pair) is refused the same way, and says so.
    atalk_router_set_net((uint16_t)tabs_random_net_js(ATALK_ROUTER_NET_MAX));
    tabs_start_js(ATALK_LINK_MAX);
}
