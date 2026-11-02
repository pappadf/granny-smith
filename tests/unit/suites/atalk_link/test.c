// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// atalk_link: the real LLAP, DDP, NBP, ATP and ASP (appletalk.c) driven
// frame by frame from the guest's side of the wire.
//
// Before this suite the link and transport layers had no unit coverage at
// all: appletalk.c could not be linked into a suite (a stub of its entry
// point sat in the shared harness), and it calls into every layer above it.
// The network integration tests exercise it through real Mac clients; this
// suite is where a malformed frame, a lost CTS or a machine plugged in and
// out can be staged exactly.

#include "appletalk.h"
#include "appletalk_asp.h"
#include "appletalk_internal.h"
#include "appletalk_link.h"
#include "appletalk_router.h"
#include "link_harness.h"
#include "stub_upper.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>

// Attach or detach the plugged-in connection from the link.
static void set_enabled(bool enabled) {
    char err[128];
    ASSERT_EQ_INT(0, atalk_set_enabled(enabled, err, sizeof err));
}

// Constructing a machine's connection installs the network's receiver on the
// machine's SCC, and deleting it takes it off again (link_delete checks).
TEST(boot_installs_the_frame_sink_and_delete_removes_it) {
    link_boot();
    ASSERT_TRUE(link_sink_installed());
    ASSERT_EQ_INT(HOST_NODE, (int)atalk_node_id());
    link_delete();
    ASSERT_TRUE(!link_sink_installed());
}

// Dynamic node assignment: a guest probing for an address sends ENQ; if the
// address is ours we must ACK so the guest picks another (Inside AppleTalk
// ch. 1).  An ENQ for any other node is none of our business.
TEST(enq_for_our_node_is_acked_and_others_are_not) {
    link_boot();
    uint8_t enq[3] = {HOST_NODE, GUEST_NODE, LLAP_TYPE_ENQ};
    guest_frame(enq, sizeof enq);
    ASSERT_EQ_INT(1, wire_count_type(GUEST_NODE, LLAP_TYPE_ACK));

    uint8_t other[3] = {HOST_NODE + 1, GUEST_NODE, LLAP_TYPE_ENQ};
    guest_frame(other, sizeof other);
    ASSERT_EQ_INT(1, wire_count_type(GUEST_NODE, LLAP_TYPE_ACK));
    link_delete();
}

// On a network of an internet (not the default number) the host also claims
// the nodes the network does not offer, so identical guests on two networks
// land on different nodes: for network 301, node 10 is offered and node 2
// is not.  The default network leaves every node to its guest.
TEST(a_numbered_network_offers_its_guest_only_its_own_nodes) {
    link_boot();
    ASSERT_EQ_INT(0, atalk_router_set_net(301));
    uint8_t taken[3] = {2, 2, LLAP_TYPE_ENQ};
    guest_frame(taken, sizeof taken);
    ASSERT_EQ_INT(1, wire_count_type(2, LLAP_TYPE_ACK));
    uint8_t offered[3] = {10, 10, LLAP_TYPE_ENQ};
    guest_frame(offered, sizeof offered);
    ASSERT_EQ_INT(0, wire_count_type(10, LLAP_TYPE_ACK));

    ASSERT_EQ_INT(0, atalk_router_set_net(1));
    uint8_t again[3] = {3, 3, LLAP_TYPE_ENQ};
    guest_frame(again, sizeof again);
    ASSERT_EQ_INT(0, wire_count_type(3, LLAP_TYPE_ACK));
    link_delete();
}

// A directed data frame from the guest is preceded by the guest's RTS, which
// we answer with CTS; one of ours goes out only after the guest's CTS.  This
// drives both directions through an NBP LkUp for a name we registered.
TEST(nbp_lookup_is_answered_after_the_rts_cts_handshake) {
    link_boot();
    atalk_nbp_service_desc_t desc = {.object = "Test Host", .type = "LinkTest", .socket = 200};
    atalk_nbp_entry_t *entry = NULL;
    ASSERT_EQ_INT(0, atalk_nbp_publish(&entry, &desc));

    uint8_t rts[3] = {HOST_NODE, GUEST_NODE, LLAP_TYPE_RTS};
    guest_frame(rts, sizeof rts);
    ASSERT_EQ_INT(1, wire_count_type(GUEST_NODE, LLAP_TYPE_CTS));

    // NBP LkUp (function 2, one tuple): =:LinkTest@*, reply to socket 253.
    uint8_t lkup[64];
    size_t n = 0;
    lkup[n++] = 0x21; // function 2, tuple count 1
    lkup[n++] = 0x42; // NBP ID
    lkup[n++] = 0x00; // tuple: network (2)
    lkup[n++] = 0x00;
    lkup[n++] = GUEST_NODE; // node
    lkup[n++] = 253; // socket
    lkup[n++] = 0x00; // enumerator
    const char *parts[3] = {"=", "LinkTest", "*"};
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(parts[i]);
        lkup[n++] = (uint8_t)l;
        memcpy(lkup + n, parts[i], l);
        n += l;
    }
    guest_ddp(GUEST_NODE, 2, 253, 2 /* DDP type NBP */, lkup, n);

    // The reply waits for our RTS to be answered.
    ASSERT_TRUE(wire_last_ddp(GUEST_NODE, 2, NULL) == NULL);
    guest_advance_to(link_now_ns() + 5e6);
    size_t len = 0;
    const uint8_t *reply = wire_last_ddp(GUEST_NODE, 2, &len);
    ASSERT_TRUE(reply != NULL);
    ASSERT_EQ_INT(0x31, reply[8]); // LkUp-Reply, one tuple
    ASSERT_EQ_INT(0x42, reply[9]); // same NBP ID
    ASSERT_EQ_INT(HOST_NODE, reply[12]); // the entity's node
    ASSERT_EQ_INT(200, reply[13]); // the entity's socket

    atalk_nbp_withdraw(&entry);
    link_delete();
}

// Every event type the transport can schedule is registered the moment the
// connection is plugged in -- before a checkpoint restore replays the saved
// queue -- so a checkpoint taken with an AFP command or a print job in flight
// can be restored (the printer, LaserWriter and ADSP timers are stubbed out
// of this suite and are covered by the appletalk-afp-checkpoint row).
TEST(every_transport_timer_is_registered_at_init) {
    link_boot();
    // Registered, not armed -- but for the router's send-RTMP, validity and
    // link-ping timers, which run from the moment the machine is on the cable
    ASSERT_EQ_INT(3, sched_pending());
    ASSERT_TRUE(sched_registered("atalk_router.rtmp_send"));
    ASSERT_TRUE(sched_registered("atalk_router.validity"));
    ASSERT_TRUE(sched_registered("atalk_router.link_ping"));
    ASSERT_TRUE(sched_registered("atalk_router.rtmp_trigger"));
    ASSERT_TRUE(sched_registered("atalk_router.zip_query"));
    ASSERT_TRUE(sched_registered("llap.rts_timeout"));
    ASSERT_TRUE(sched_registered("llap.rts_kick"));
    ASSERT_TRUE(sched_registered("atp.retry_timeout"));
    ASSERT_TRUE(sched_registered("atp.xo_release"));
    ASSERT_TRUE(sched_registered("asp.session_sweep"));
    link_delete();
}

// --- ASP, from the guest's side ------------------------------------------------

#define AFP_SOCKET         8
#define ATP_TREQ           0x40
#define ATP_TRESP          0x80
#define ATP_XO             0x20
#define ASP_OPEN_SESS      4
#define ASP_WRITE          6
#define ASP_WRITE_CONTINUE 7

// OpenSess from `node`'s workstation socket `wss`; returns the session id the
// reply carries, or 0.
static uint8_t asp_open_session(uint8_t node, uint8_t wss, uint16_t tid) {
    wire_clear();
    uint8_t atp[8] = {ATP_TREQ | ATP_XO, 0x01, (uint8_t)(tid >> 8), (uint8_t)tid, ASP_OPEN_SESS, wss, 0x01, 0x00};
    guest_ddp(node, AFP_SOCKET, 200, 3, atp, sizeof atp);
    guest_advance_to(link_now_ns() + 5e6);
    for (int i = 0; i < wire_count(); i++) {
        size_t len = 0;
        const uint8_t *f = wire_frame(i, &len);
        if (len >= 16 && f[2] == LLAP_TYPE_DDP_SHORT && f[0] == node && (f[8] & 0xC0) == ATP_TRESP)
            return f[13]; // user[1]: the session id
    }
    return 0;
}

// SPWrite of an FPWrite: the server answers with a WriteContinue TReq asking
// for the data.
static void asp_write(uint8_t node, uint8_t sid, uint16_t tid, uint16_t seq) {
    uint8_t atp[8 + 12] = {ATP_TREQ | ATP_XO, 0xFF, (uint8_t)(tid >> 8), (uint8_t)tid,
                           ASP_WRITE,         sid,  (uint8_t)(seq >> 8), (uint8_t)seq};
    atp[8] = 0x21; // FPWrite
    guest_ddp(node, AFP_SOCKET, 200, 3, atp, sizeof atp);
    guest_advance_to(link_now_ns() + 5e6);
}

// A new machine -- machine.boot, a checkpoint load -- gets a connection of its
// own: none of the old machine's outgoing ATP requests, XO cache or pending ASP
// write carries over.  Here the old machine's WriteContinue is never answered,
// and the new machine's first Write is served all the same.
TEST(a_rebuilt_stack_serves_a_write_the_old_one_left_pending) {
    link_boot();
    uint8_t s1 = asp_open_session(GUEST_NODE, 100, 0x1001);
    ASSERT_TRUE(s1 != 0);
    wire_clear();
    asp_write(GUEST_NODE, s1, 0x2001, 1);
    ASSERT_EQ_INT(1, wire_count_atp(GUEST_NODE, ATP_TREQ, ASP_WRITE_CONTINUE));
    // The guest never answers the WriteContinue: the machine goes away first.
    link_delete();

    link_boot();
    uint8_t s2 = asp_open_session(GUEST_NODE + 1, 101, 0x3001);
    ASSERT_TRUE(s2 != 0);
    wire_clear();
    asp_write(GUEST_NODE + 1, s2, 0x3002, 1);
    ASSERT_EQ_INT(1, wire_count_atp(GUEST_NODE + 1, ATP_TREQ, ASP_WRITE_CONTINUE));
    link_delete();
}

// Detaching the stack from the link (appletalk.enabled = false) left a frame
// that was waiting for its CTS in the queue, and the RTS timer kept putting
// it on the wire.
TEST(detaching_the_stack_stops_its_transmitter) {
    link_boot();
    atalk_nbp_service_desc_t desc = {.object = "Test Host", .type = "LinkTest", .socket = 200};
    atalk_nbp_entry_t *entry = NULL;
    ASSERT_EQ_INT(0, atalk_nbp_publish(&entry, &desc));
    // A directed lookup reply goes out as RTS, then data after our CTS; the
    // guest never grants it.
    uint8_t rts[3] = {HOST_NODE, GUEST_NODE, LLAP_TYPE_RTS};
    guest_frame(rts, sizeof rts);
    uint8_t lkup[] = {0x21, 0x42, 0, 0, GUEST_NODE, 253, 0, 1, '=', 8, 'L', 'i', 'n', 'k', 'T', 'e', 's', 't', 1, '*'};
    guest_ddp(GUEST_NODE, 2, 253, 2, lkup, sizeof lkup);
    // Our reply waits out the wire time reserved for the guest's data frame
    // (LLAP_MAX_FRAME_NS, ~21 ms), then its RTS goes out.  Stop at the first
    // one: after eight unanswered attempts (2 ms each) the frame is dropped
    // on its own.
    double give_up = link_now_ns() + 30e6;
    while (wire_count_type(GUEST_NODE, LLAP_TYPE_RTS) == 0 && link_now_ns() < give_up)
        guest_idle_until(link_now_ns() + 1e5);
    ASSERT_EQ_INT(1, wire_count_type(GUEST_NODE, LLAP_TYPE_RTS));

    set_enabled(false);
    wire_clear();
    // Without answering any RTS: run well past eight RTS timeouts.
    double end = link_now_ns() + 50e6;
    while (link_now_ns() < end)
        guest_idle_until(link_now_ns() + 1e6);
    ASSERT_EQ_INT(0, wire_count());

    set_enabled(true);
    atalk_nbp_withdraw(&entry);
    link_delete();
}

// The connection's checkpoint block round-trips: what was saved is applied.
TEST(the_checkpoint_block_is_restored) {
    link_boot();
    uint8_t ping[] = {1, 'p'};
    guest_ddp(GUEST_NODE, 4, 200, 4, ping, sizeof ping);
    uint64_t ddp_in = atalk_get_stats()->ddp_in;
    ASSERT_TRUE(ddp_in > 0);
    set_enabled(false);
    link_checkpoint();
    link_delete();
    link_boot_from_checkpoint(false);
    ASSERT_TRUE(!atalk_get_enabled());
    ASSERT_EQ_INT((int)ddp_in, (int)atalk_get_stats()->ddp_in); // the link's counters come back
    set_enabled(true);
    link_delete();
}

// A block read from a checkpoint in error is not applied: the connection
// starts as a new one would.
TEST(a_block_from_a_failed_checkpoint_is_not_applied) {
    link_boot();
    set_enabled(false);
    link_checkpoint();
    link_delete();
    link_boot_from_checkpoint(true);
    ASSERT_TRUE(atalk_get_enabled()); // the default, not the block's
    link_delete();
}

// --- the network outside the machine ---------------------------------------------
//
// The network -- its shares, the server's identity, the printer -- is host
// state: created once, and never torn down, carried or checkpointed by a
// machine.  Machines plug into it and out of it.

// A machine that goes leaves the network as it was, and the next machine finds
// it so: the shares (under the same volume ids), the server's name, the
// printer's settings.  Nothing of it travels in the checkpoint either.
TEST(the_network_outlives_every_machine) {
    link_boot();
    ASSERT_EQ_INT(1, g_printer_registers); // the printer joined the network once, at its creation
    char err[128];
    ASSERT_TRUE(atalk_afp_volume_add("Share A", "/share/a", err, sizeof err) >= 0);
    int slot = atalk_afp_volume_find("Share A");
    unsigned id = atalk_afp_volume_vol_id(slot);
    ASSERT_EQ_INT(0, atalk_afp_set_name("Renamed Server", err, sizeof err));
    atalk_printer_set_capture(true);
    link_checkpoint();
    link_delete();
    ASSERT_EQ_INT(slot, atalk_afp_volume_find("Share A")); // the share stays published
    ASSERT_TRUE(strcmp(atalk_afp_get_name(), "Renamed Server") == 0);
    ASSERT_TRUE(atalk_printer_get_capture());

    link_boot_from_checkpoint(false);
    ASSERT_EQ_INT((int)id, (int)atalk_afp_volume_vol_id(atalk_afp_volume_find("Share A")));
    ASSERT_EQ_INT(1, g_printer_registers); // not registered again
    link_delete();
    ASSERT_EQ_INT(0, atalk_afp_volume_remove("Share A", err, sizeof err));
    ASSERT_EQ_INT(0, atalk_afp_set_name("Test Server", err, sizeof err));
    atalk_printer_set_capture(false);
}

// Each machine built puts the printer's timers on its scheduler, and each
// one unplugged takes the printer's session with it, and restarts the
// printer.
TEST(machines_plug_into_the_printer_and_out_again) {
    int regs = g_printer_timer_registrations, unplugs = g_printer_unplugs;
    link_boot();
    ASSERT_EQ_INT(regs + 1, g_printer_timer_registrations);
    link_checkpoint();
    link_load(false); // the new machine takes the cable from the old one
    ASSERT_EQ_INT(regs + 2, g_printer_timer_registrations);
    ASSERT_EQ_INT(unplugs + 1, g_printer_unplugs);
    link_delete();
    ASSERT_EQ_INT(unplugs + 2, g_printer_unplugs);
}

// A checkpoint load that fails after the new machine was built: the new
// machine is destroyed, its connection with it, and the machine that keeps
// running was never unplugged.  The network never moved.
TEST(a_failed_load_leaves_the_running_machine_plugged_in) {
    link_boot();
    char err[128];
    ASSERT_TRUE(atalk_afp_volume_add("Live Share", "/live", err, sizeof err) >= 0);
    link_checkpoint();
    link_load(true);
    ASSERT_TRUE(link_sink_on(0));
    ASSERT_TRUE(!link_sink_on(1));
    ASSERT_TRUE(atalk_afp_volume_find("Live Share") >= 0);
    // ...and the network answers the guest on the machine that is still there.
    wire_clear();
    uint8_t enq[3] = {HOST_NODE, GUEST_NODE, LLAP_TYPE_ENQ};
    guest_frame(enq, sizeof enq);
    ASSERT_EQ_INT(1, wire_count_type(GUEST_NODE, LLAP_TYPE_ACK));
    link_delete();
    ASSERT_EQ_INT(0, atalk_afp_volume_remove("Live Share", err, sizeof err));
}

// ...and a load that succeeds leaves the cable with the new machine: taking
// it unplugged the old one, so the old machine's delete leaves the new
// machine's connection alone.
TEST(destroying_the_replaced_machine_leaves_the_new_one_plugged_in) {
    link_boot();
    link_checkpoint();
    link_load(false);
    ASSERT_TRUE(link_sink_on(1));
    ASSERT_TRUE(!link_sink_on(0));
    wire_clear();
    uint8_t enq[3] = {HOST_NODE, GUEST_NODE, LLAP_TYPE_ENQ};
    guest_frame(enq, sizeof enq);
    ASSERT_EQ_INT(1, wire_count_type(GUEST_NODE, LLAP_TYPE_ACK));
    link_delete();
}

// --- malformed input -------------------------------------------------------------

typedef struct {
    uint64_t malformed, unhandled, tx_dropped, ddp_in;
} counts_t;

static counts_t counts(void) {
    const atalk_stats_t *st = atalk_get_stats();
    return (counts_t){st->malformed, st->unhandled, st->tx_dropped, st->ddp_in};
}

// Feed one raw frame and check which counter it moved: exactly one, by one.
static void expect_drop(const uint8_t *frame, size_t len, int which /* 0 malformed, 1 unhandled, -1 neither */) {
    counts_t before = counts();
    guest_frame(frame, len);
    counts_t after = counts();
    ASSERT_EQ_INT(which == 0 ? 1 : 0, (int)(after.malformed - before.malformed));
    ASSERT_EQ_INT(which == 1 ? 1 : 0, (int)(after.unhandled - before.unhandled));
}

// A short-DDP frame whose DDP header cannot be what it claims.  These were
// three asserts on guest bytes: in this build (asserts on) the first frame
// below aborted the process; in the browser build (asserts compiled out) a
// frame under five bytes was parsed from stale bytes, and len - 5 wrapped.
TEST(a_malformed_ddp_frame_is_dropped_and_counted) {
    link_boot();
    uint8_t f[800];
    memset(f, 0, sizeof f);
    f[0] = HOST_NODE;
    f[1] = GUEST_NODE;
    f[2] = LLAP_TYPE_DDP_SHORT;
    // Every length from an empty DDP header to one byte short of a whole one.
    for (size_t ddp_len = 0; ddp_len < 5; ddp_len++)
        expect_drop(f, 3 + ddp_len, 0);
    // Whole headers whose 10-bit length field disagrees with the frame.
    for (size_t ddp_len = 5; ddp_len <= 12; ddp_len++) {
        f[3] = 0;
        f[4] = (uint8_t)(ddp_len + 1);
        expect_drop(f, 3 + ddp_len, 0);
        f[4] = 0;
        expect_drop(f, 3 + ddp_len, 0);
    }
    // Consistent, but longer than DDP allows (586 bytes of data).
    size_t big = 5 + 587;
    f[3] = (uint8_t)(big >> 8);
    f[4] = (uint8_t)big;
    expect_drop(f, 3 + big, 0);
    // None of it reached DDP.
    ASSERT_EQ_INT(0, (int)atalk_get_stats()->ddp_in);
    link_delete();
}

// Every other discard is counted too, by why.
TEST(every_discard_is_counted_by_reason) {
    link_boot();
    // Malformed.
    uint8_t two[2] = {HOST_NODE, GUEST_NODE};
    expect_drop(two, sizeof two, 0); // shorter than an LLAP header
    uint8_t enq4[4] = {HOST_NODE, GUEST_NODE, LLAP_TYPE_ENQ, 0};
    expect_drop(enq4, sizeof enq4, 0);
    uint8_t cts4[4] = {HOST_NODE, GUEST_NODE, LLAP_TYPE_CTS, 0};
    expect_drop(cts4, sizeof cts4, 0); // CTS had no length check at all
    uint8_t atp_short[4] = {0x40, 1, 0, 1};
    counts_t b = counts();
    guest_ddp(GUEST_NODE, 8, 200, 3, atp_short, sizeof atp_short);
    ASSERT_EQ_INT(1, (int)(counts().malformed - b.malformed));
    uint8_t atp_ctl0[8] = {0x00, 1, 0, 1, 0, 0, 0, 0}; // control type 0 is no ATP packet
    b = counts();
    guest_ddp(GUEST_NODE, 8, 200, 3, atp_ctl0, sizeof atp_ctl0);
    ASSERT_EQ_INT(1, (int)(counts().malformed - b.malformed));
    uint8_t nbp_trunc[] = {0x21, 0x42, 0, 0, GUEST_NODE, 253, 0, 1, '='}; // one tuple, cut short
    b = counts();
    guest_ddp(GUEST_NODE, 2, 253, 2, nbp_trunc, sizeof nbp_trunc);
    ASSERT_EQ_INT(1, (int)(counts().malformed - b.malformed));

    // Well-formed, but nothing here serves it.
    uint8_t ack[3] = {HOST_NODE, GUEST_NODE, LLAP_TYPE_ACK};
    expect_drop(ack, sizeof ack, 1);
    // An extended datagram for another network the router has no route to:
    // the router drops it, and counts it as its own.
    uint8_t ext[3 + 13] = {HOST_NODE, GUEST_NODE, 0x02, 0, 13, 0, 0, 0, 7, 0, 0, HOST_NODE, GUEST_NODE, 4, 200, 4};
    uint64_t no_route = atalk_router_get_stats()->dropped_no_route;
    expect_drop(ext, sizeof ext, -1);
    ASSERT_EQ_INT(1, (int)(atalk_router_get_stats()->dropped_no_route - no_route));
    uint8_t other[8] = {HOST_NODE + 1, GUEST_NODE, LLAP_TYPE_DDP_SHORT, 0, 5, 2, 253, 2};
    expect_drop(other, sizeof other, 1); // data for another node
    b = counts();
    guest_ddp(GUEST_NODE, 99, 200, 0x55, NULL, 0); // no such DDP type
    ASSERT_EQ_INT(1, (int)(counts().unhandled - b.unhandled));
    uint8_t treq[8] = {0x40, 1, 0, 2, 0, 0, 0, 0};
    b = counts();
    guest_ddp(GUEST_NODE, 77, 200, 3, treq, sizeof treq); // no ATP handler on socket 77
    ASSERT_EQ_INT(1, (int)(counts().unhandled - b.unhandled));
    uint8_t tresp[8] = {0x80 | 0x10, 0, 0x7F, 0x7F, 0, 0, 0, 0};
    b = counts();
    guest_ddp(GUEST_NODE, 8, 200, 3, tresp, sizeof tresp); // answers nothing we asked
    ASSERT_EQ_INT(1, (int)(counts().unhandled - b.unhandled));

    // One of ours the guest never grants: after eight RTS attempts it is dropped.
    atalk_nbp_service_desc_t desc = {.object = "Test Host", .type = "LinkTest", .socket = 200};
    atalk_nbp_entry_t *entry = NULL;
    ASSERT_EQ_INT(0, atalk_nbp_publish(&entry, &desc));
    uint8_t lkup[] = {0x21, 0x42, 0, 0, GUEST_NODE, 253, 0, 1, '=', 8, 'L', 'i', 'n', 'k', 'T', 'e', 's', 't', 1, '*'};
    b = counts();
    guest_ddp(GUEST_NODE, 2, 253, 2, lkup, sizeof lkup);
    guest_idle_until(link_now_ns() + 100e6);
    ASSERT_EQ_INT(1, (int)(counts().tx_dropped - b.tx_dropped));
    atalk_nbp_withdraw(&entry);
    link_delete();
}

// --- NBP with eight tuples ----------------------------------------------------

// Walk the tuples of an NBP packet at `p`; returns how many parse.
static int nbp_count_tuples(const uint8_t *p, size_t len) {
    size_t pos = 2;
    int n = 0;
    while (pos + 5 < len) {
        pos += 5;
        for (int field = 0; field < 3; field++) {
            if (pos >= len)
                return n;
            pos += 1 + p[pos];
        }
        if (pos > len)
            return n;
        n++;
    }
    return n;
}

TEST(a_lookup_matching_eight_names_answers_with_eight_tuples) {
    link_boot();
    atalk_nbp_entry_t *entries[8] = {0};
    char names[8][8];
    for (int i = 0; i < 8; i++) {
        snprintf(names[i], sizeof names[i], "Host %d", i);
        atalk_nbp_service_desc_t desc = {.object = names[i], .type = "Eight", .socket = (unsigned)(200 + i)};
        ASSERT_EQ_INT(0, atalk_nbp_publish(&entries[i], &desc));
    }
    uint8_t lkup[] = {0x21, 0x42, 0, 0, GUEST_NODE, 253, 0, 1, '=', 5, 'E', 'i', 'g', 'h', 't', 1, '*'};
    guest_ddp(GUEST_NODE, 2, 253, 2, lkup, sizeof lkup);
    guest_advance_to(link_now_ns() + 50e6);
    size_t len = 0;
    const uint8_t *reply = wire_last_ddp(GUEST_NODE, 2, &len);
    ASSERT_TRUE(reply != NULL);
    ASSERT_EQ_INT(0x38, reply[8]); // LkUp-Reply, eight tuples...
    ASSERT_EQ_INT(8, nbp_count_tuples(reply + 8, len - 8)); // ...and it carries them
    for (int i = 0; i < 8; i++)
        atalk_nbp_withdraw(&entries[i]);
    link_delete();
}

static int g_lookup_results;
static void count_lookup_result(void *ctx, const atalk_nbp_info_t *info) {
    (void)ctx, (void)info;
    g_lookup_results++;
}

TEST(a_lookup_reply_with_eight_tuples_delivers_all_eight) {
    link_boot();
    g_lookup_results = 0;
    ASSERT_EQ_INT(0, atalk_nbp_lookup("=", "PPCToolBox", "*", 252, count_lookup_result, NULL));
    // Our LkUp went out broadcast; find its NBP id.
    size_t len = 0;
    const uint8_t *req = wire_last_ddp(0xFF, 2, &len);
    ASSERT_TRUE(req != NULL);
    uint8_t nbp_id = req[9];

    uint8_t reply[400];
    size_t n = 0;
    reply[n++] = 0x38; // LkUp-Reply, eight tuples
    reply[n++] = nbp_id;
    for (int i = 0; i < 8; i++) {
        reply[n++] = 0;
        reply[n++] = 0;
        reply[n++] = GUEST_NODE;
        reply[n++] = (uint8_t)(100 + i);
        reply[n++] = 0;
        char obj[8];
        snprintf(obj, sizeof obj, "Mac %d", i);
        const char *fields[3] = {obj, "PPCToolBox", "*"};
        for (int f = 0; f < 3; f++) {
            size_t l = strlen(fields[f]);
            reply[n++] = (uint8_t)l;
            memcpy(reply + n, fields[f], l);
            n += l;
        }
    }
    guest_ddp(GUEST_NODE, 252, 2, 2, reply, n);
    ASSERT_EQ_INT(8, g_lookup_results);
    atalk_nbp_lookup_cancel();
    link_delete();
}

// --- NBP names are MacRoman on the wire (#168) ----------------------------

// Answer a guest LkUp for `object`:`type`@* (raw MacRoman bytes) once the
// RTS/CTS handshake is through; the reply frame, or NULL.
static const uint8_t *guest_lookup(const uint8_t *object, size_t olen, const char *type, size_t *len) {
    uint8_t rts[3] = {HOST_NODE, GUEST_NODE, LLAP_TYPE_RTS};
    guest_frame(rts, sizeof rts);
    uint8_t lkup[128];
    size_t n = 0;
    lkup[n++] = 0x21; // LkUp, one tuple
    lkup[n++] = 0x43; // NBP ID
    lkup[n++] = 0x00;
    lkup[n++] = 0x00;
    lkup[n++] = GUEST_NODE;
    lkup[n++] = 253;
    lkup[n++] = 0x00;
    lkup[n++] = (uint8_t)olen;
    memcpy(lkup + n, object, olen);
    n += olen;
    lkup[n++] = (uint8_t)strlen(type);
    memcpy(lkup + n, type, strlen(type));
    n += strlen(type);
    lkup[n++] = 1;
    lkup[n++] = '*';
    guest_ddp(GUEST_NODE, 2, 253, 2, lkup, n);
    guest_advance_to(link_now_ns() + 5e6);
    return wire_last_ddp(GUEST_NODE, 2, len);
}

// A UTF-8 name is published as MacRoman ("Café" is 43 61 66 8E, not the
// 43 61 66 C3 A9 the Chooser would draw as "Caf√©"), and a lookup matches it
// case-insensitively by Table D-2, accented letters included.
TEST(a_published_name_goes_out_in_macroman_and_folds_by_table_d2) {
    link_boot();
    atalk_nbp_service_desc_t desc = {.object = "Caf\xc3\xa9", .type = "LinkTest", .socket = 200};
    atalk_nbp_entry_t *entry = NULL;
    ASSERT_EQ_INT(0, atalk_nbp_publish(&entry, &desc));

    const uint8_t upper[] = {'C', 'A', 'F', 0x83}; // "CAFÉ" in MacRoman
    size_t len = 0;
    const uint8_t *reply = guest_lookup(upper, sizeof upper, "LINKTEST", &len);
    ASSERT_TRUE(reply != NULL);
    ASSERT_EQ_INT(0x31, reply[8]); // LkUp-Reply, one tuple
    ASSERT_EQ_INT(4, reply[15]); // the object field: four MacRoman bytes
    const uint8_t want[] = {'C', 'a', 'f', 0x8E};
    ASSERT_TRUE(memcmp(reply + 16, want, sizeof want) == 0);

    // The registry view hands it back as UTF-8.
    atalk_nbp_info_t info;
    bool found = false;
    for (int i = 0; i < atalk_nbp_entry_max(); i++)
        if (atalk_nbp_entry_info(i, &info) && strcmp(info.type, "LinkTest") == 0)
            found = strcmp(info.object, "Caf\xc3\xa9") == 0;
    ASSERT_TRUE(found);

    // "E" is not "É": diacriticals stay significant.
    const uint8_t plain[] = {'C', 'A', 'F', 'E'};
    wire_clear();
    ASSERT_TRUE(guest_lookup(plain, sizeof plain, "LinkTest", &len) == NULL);

    atalk_nbp_withdraw(&entry);
    link_delete();
}

static char g_lookup_object[ATALK_NBP_TEXT_CAP];
static void keep_lookup_object(void *ctx, const atalk_nbp_info_t *info) {
    (void)ctx;
    snprintf(g_lookup_object, sizeof g_lookup_object, "%s", info->object);
}

// Our own lookups: the pattern goes out in MacRoman (a UTF-8 "≈" is the
// wildcard byte $C5) and the names that come back are handed on as UTF-8.
TEST(a_lookup_goes_out_in_macroman_and_its_replies_come_back_in_utf8) {
    link_boot();
    g_lookup_object[0] = '\0';
    ASSERT_EQ_INT(0, atalk_nbp_lookup("Caf\xe2\x89\x88", "PPCToolBox", "*", 252, keep_lookup_object, NULL));
    size_t len = 0;
    const uint8_t *req = wire_last_ddp(0xFF, 2, &len);
    ASSERT_TRUE(req != NULL);
    const uint8_t pattern[] = {4, 'C', 'a', 'f', 0xC5};
    ASSERT_TRUE(memcmp(req + 15, pattern, sizeof pattern) == 0);

    uint8_t reply[64];
    size_t n = 0;
    reply[n++] = 0x31;
    reply[n++] = req[9];
    reply[n++] = 0;
    reply[n++] = 0;
    reply[n++] = GUEST_NODE;
    reply[n++] = 100;
    reply[n++] = 0;
    const uint8_t object[] = {5, 'N', 0x96, 'n', 'e', 'z'}; // "Nñnez"
    memcpy(reply + n, object, sizeof object);
    n += sizeof object;
    reply[n++] = 10;
    memcpy(reply + n, "PPCToolBox", 10);
    n += 10;
    reply[n++] = 1;
    reply[n++] = '*';
    guest_ddp(GUEST_NODE, 252, 2, 2, reply, n);
    ASSERT_TRUE(strcmp(g_lookup_object, "N\xc3\xb1nez") == 0);

    // A pattern MacRoman cannot hold does not go out at all.
    ASSERT_EQ_INT(-1, atalk_nbp_lookup("\xf0\x9f\x93\x81", "PPCToolBox", "*", 252, keep_lookup_object, NULL));
    atalk_nbp_lookup_cancel();
    link_delete();
}

// A name is refused when it is set, never sent as its UTF-8 bytes: one with
// a character MacRoman lacks, or longer than 32 MacRoman characters (which a
// name of accented letters can be well within 32 UTF-8 characters).
TEST(a_name_macroman_cannot_hold_is_refused) {
    char err[200];
    ASSERT_TRUE(atalk_nbp_name_check("server name", "\xf0\x9f\x93\x81 Files", err, sizeof err) != 0);
    ASSERT_TRUE(strstr(err, "\xf0\x9f\x93\x81") != NULL); // names the character
    ASSERT_TRUE(atalk_nbp_name_check("server name", "", err, sizeof err) != 0);

    char accents[32 * 2 + 3];
    size_t n = 0;
    for (int i = 0; i < 32; i++) {
        accents[n++] = (char)0xc3; // é
        accents[n++] = (char)0xa9;
    }
    accents[n] = '\0';
    ASSERT_EQ_INT(0, atalk_nbp_name_check("server name", accents, err, sizeof err)); // 32 MacRoman bytes
    accents[n++] = 'x';
    accents[n] = '\0';
    ASSERT_TRUE(atalk_nbp_name_check("server name", accents, err, sizeof err) != 0); // 33

    link_boot();
    atalk_nbp_service_desc_t desc = {.object = "\xf0\x9f\x93\x81", .type = "LinkTest", .socket = 200};
    atalk_nbp_entry_t *entry = NULL;
    ASSERT_EQ_INT(-1, atalk_nbp_publish(&entry, &desc));
    ASSERT_TRUE(entry == NULL);
    link_delete();
}

// AEP (Inside AppleTalk ch. 6): the Echoer on socket 4 turns an Echo Request
// (function 1) round as an Echo Reply (function 2), data unchanged; anything
// else is not for it (it echoed everything, unchanged).
TEST(the_echoer_answers_requests_on_socket_4_with_a_reply) {
    link_boot();
    uint8_t ping[] = {1, 'p', 'i', 'n', 'g'};
    guest_ddp(GUEST_NODE, 4, 200, 4, ping, sizeof ping);
    guest_advance_to(link_now_ns() + 30e6);
    size_t len = 0;
    const uint8_t *echo = wire_last_ddp(GUEST_NODE, 4, &len);
    ASSERT_TRUE(echo != NULL);
    ASSERT_EQ_INT((int)(8 + sizeof ping), (int)len);
    ASSERT_EQ_INT(200, echo[5]); // back to the requesting socket
    ASSERT_EQ_INT(2, echo[8]); // Echo Reply
    ASSERT_EQ_INT(0, memcmp(echo + 9, ping + 1, sizeof ping - 1));

    wire_clear();
    uint8_t reply[] = {2, 'x'};
    guest_ddp(GUEST_NODE, 4, 200, 4, reply, sizeof reply); // a reply is not echoed
    guest_ddp(GUEST_NODE, 5, 200, 4, ping, sizeof ping); // nor anything off socket 4
    guest_ddp(GUEST_NODE, 4, 200, 4, NULL, 0); // nor an empty packet
    guest_advance_to(link_now_ns() + 30e6);
    ASSERT_TRUE(wire_last_ddp(GUEST_NODE, 4, NULL) == NULL);
    link_delete();
}

// --- extended DDP headers ---------------------------------------------------------

// Build an extended-header DDP frame (Inside AppleTalk 4-15) from the guest
// at `src_net`.`GUEST_NODE` to `dst_net`.`dst_node`; returns its length.
static size_t ext_frame(uint8_t *f, uint16_t dst_net, uint16_t src_net, uint8_t dst_node, uint8_t dst_sock,
                        uint8_t src_sock, uint8_t type, const uint8_t *payload, size_t plen) {
    size_t ddp_len = 13 + plen;
    f[0] = HOST_NODE; // LLAP: the frame goes to our node
    f[1] = GUEST_NODE;
    f[2] = 0x02; // LLAP type: extended DDP
    f[3] = (uint8_t)(ddp_len >> 8); // hop count 0, length high bits
    f[4] = (uint8_t)ddp_len;
    f[5] = 0; // no checksum
    f[6] = 0;
    f[7] = (uint8_t)(dst_net >> 8);
    f[8] = (uint8_t)dst_net;
    f[9] = (uint8_t)(src_net >> 8);
    f[10] = (uint8_t)src_net;
    f[11] = dst_node;
    f[12] = GUEST_NODE;
    f[13] = dst_sock;
    f[14] = src_sock;
    f[15] = type;
    memcpy(f + 16, payload, plen);
    return 3 + ddp_len;
}

// The DDP checksum (Inside AppleTalk 4-17), from the destination network on.
static uint16_t ext_checksum(const uint8_t *frame, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 7; i < len; i++) {
        sum = (uint16_t)(sum + frame[i]);
        sum = (uint16_t)((sum << 1) | (sum >> 15));
    }
    return sum ? sum : 0xFFFF;
}

// The last extended-DDP frame to `dst`, or NULL.
static const uint8_t *last_ext(uint8_t dst, size_t *len) {
    for (int i = wire_count() - 1; i >= 0; i--) {
        size_t n = 0;
        const uint8_t *f = wire_frame(i, &n);
        if (n >= 16 && f[0] == dst && f[2] == 0x02) {
            *len = n;
            return f;
        }
    }
    return NULL;
}

// A guest that knows a router may address us with the extended header; the
// answer comes back in the same form, to the requester's network and node.
TEST(an_extended_request_gets_an_extended_reply) {
    link_boot();
    uint8_t f[64];
    uint8_t ping[] = {1, 'e', 'x', 't'};
    size_t n = ext_frame(f, 0, 0, HOST_NODE, 4, 200, 4, ping, sizeof ping);
    counts_t before = counts();
    guest_frame(f, n);
    ASSERT_EQ_INT(1, (int)(counts().ddp_in - before.ddp_in));
    guest_advance_to(link_now_ns() + 30e6);
    size_t len = 0;
    const uint8_t *r = last_ext(GUEST_NODE, &len);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT((int)(3 + 13 + sizeof ping), (int)len);
    ASSERT_EQ_INT(GUEST_NODE, r[11]); // destination node in the DDP header
    ASSERT_EQ_INT(HOST_NODE, r[12]); // source node
    ASSERT_EQ_INT(200, r[13]); // back to the requesting socket
    ASSERT_EQ_INT(4, r[14]); // from the Echoer
    ASSERT_EQ_INT(4, r[15]); // AEP
    ASSERT_EQ_INT(2, r[16]); // Echo Reply
    ASSERT_EQ_INT(0, memcmp(r + 17, ping + 1, sizeof ping - 1));

    // A broadcast with the extended header is accepted too.
    wire_clear();
    n = ext_frame(f, 0, 0, 0xFF, 4, 201, 4, ping, sizeof ping);
    guest_frame(f, n);
    guest_advance_to(link_now_ns() + 30e6);
    ASSERT_TRUE(last_ext(GUEST_NODE, &len) != NULL);
    link_delete();
}

// A nonzero checksum is verified; zero means none.  A length that disagrees
// with the frame, or a frame shorter than the header, is malformed.
TEST(an_extended_header_is_checked_before_it_is_used) {
    link_boot();
    uint8_t f[64];
    uint8_t ping[] = {1, 'c', 's'};
    size_t n = ext_frame(f, 0, 0, HOST_NODE, 4, 200, 4, ping, sizeof ping);
    uint16_t sum = ext_checksum(f, n);
    f[5] = (uint8_t)(sum >> 8);
    f[6] = (uint8_t)sum;
    counts_t before = counts();
    guest_frame(f, n); // the right checksum
    ASSERT_EQ_INT(1, (int)(counts().ddp_in - before.ddp_in));
    f[6] ^= 1;
    expect_drop(f, n, 0); // the wrong one
    f[5] = f[6] = 0;
    f[4] = (uint8_t)(f[4] + 1);
    expect_drop(f, n, 0); // length field one too long
    expect_drop(f, 3 + 12, 0); // shorter than an extended header
    // Another node on this network: nobody else is on this wire.
    n = ext_frame(f, 0, 0, HOST_NODE + 1, 4, 200, 4, ping, sizeof ping);
    expect_drop(f, n, 1);
    link_delete();
}

// --- the router: RTMP, ZIP, NBP BrRq ----------------------------------------------

// The last frame to `dst` whose DDP (short header) type is `type`, any length.
static const uint8_t *last_short(uint8_t dst, uint8_t type, size_t *len) {
    for (int i = wire_count() - 1; i >= 0; i--) {
        size_t n = 0;
        const uint8_t *f = wire_frame(i, &n);
        if (n >= 8 && f[0] == dst && f[2] == LLAP_TYPE_DDP_SHORT && f[7] == type) {
            *len = n;
            return f;
        }
    }
    return NULL;
}

// An RTMP Request (function 1) gets an RTMP Response: RTMP Data without
// tuples, directed to the requesting socket (Inside AppleTalk 5-17).  It names
// the network (1 by default) and the router (our node).
TEST(an_rtmp_request_is_answered_with_this_net_and_the_router) {
    link_boot();
    ASSERT_EQ_INT(1, atalk_this_net());
    uint8_t req[] = {1};
    guest_ddp(GUEST_NODE, 1, 253, 5, req, sizeof req);
    guest_advance_to(link_now_ns() + 30e6);
    size_t len = 0;
    const uint8_t *r = last_short(GUEST_NODE, 1, &len);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(3 + 5 + 7, (int)len); // net, ID length, ID, version indicator
    ASSERT_EQ_INT(253, r[5]); // to the requesting socket
    ASSERT_EQ_INT(1, r[6]); // from the RTMP socket
    ASSERT_EQ_INT(0, r[8]); // network 1
    ASSERT_EQ_INT(1, r[9]);
    ASSERT_EQ_INT(8, r[10]); // an 8-bit node ID...
    ASSERT_EQ_INT(HOST_NODE, r[11]); // ...ours
    ASSERT_EQ_INT(0x82, r[14]); // version $000082
    // A Route Data Request gets the table: the local network at distance 0
    wire_clear();
    uint8_t rdr[] = {3};
    guest_ddp(GUEST_NODE, 1, 253, 5, rdr, sizeof rdr);
    guest_advance_to(link_now_ns() + 30e6);
    r = last_short(GUEST_NODE, 1, &len);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(3 + 5 + 10, (int)len);
    ASSERT_EQ_INT(0, r[15]); // tuple: network 1, distance 0
    ASSERT_EQ_INT(1, r[16]);
    ASSERT_EQ_INT(0, r[17]);
    link_delete();
}

// RTMP Data goes out every 10 s, broadcast from the RTMP socket to the RTMP
// socket, with the routing tuples.
TEST(the_router_broadcasts_rtmp_data_every_ten_seconds) {
    link_boot();
    double t0 = link_now_ns();
    guest_advance_to(t0 + 9.9e9);
    size_t len = 0;
    ASSERT_TRUE(last_short(0xFF, 1, &len) == NULL);
    guest_advance_to(t0 + 10.1e9);
    const uint8_t *r = last_short(0xFF, 1, &len);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(3 + 5 + 10, (int)len);
    ASSERT_EQ_INT(1, r[5]); // to the RTMP socket
    ASSERT_EQ_INT(1, r[6]); // from it
    wire_clear();
    guest_advance_to(t0 + 20.1e9);
    ASSERT_TRUE(last_short(0xFF, 1, &len) != NULL); // and again
    link_delete();
}

// One ZIP ATP request to the ZIS (socket 6), at-least-once; returns the
// response frame.
static const uint8_t *zip_atp(uint8_t func, uint16_t index, uint16_t tid, size_t *len) {
    uint8_t treq[8] = {0x40, 1, (uint8_t)(tid >> 8), (uint8_t)tid, func, 0, (uint8_t)(index >> 8), (uint8_t)index};
    wire_clear();
    guest_ddp(GUEST_NODE, 6, 254, 3, treq, sizeof treq);
    guest_advance_to(link_now_ns() + 30e6);
    return last_short(GUEST_NODE, 3, len);
}

// GetMyZone, GetZoneList and GetLocalZones (Inside AppleTalk 8-7): the one zone
// there is, "Granny Smith"; an index past it gets an empty last answer.
TEST(zip_atp_requests_name_the_zone) {
    link_boot();
    static const char zone[] = "Granny Smith";
    size_t zl = sizeof zone - 1;
    for (uint8_t func = 7; func <= 9; func++) {
        size_t len = 0;
        const uint8_t *r = zip_atp(func, 1, (uint16_t)(0x100 + func), &len);
        ASSERT_TRUE(r != NULL);
        ASSERT_EQ_INT(0x80 | 0x10, r[8] & 0xD0); // TResp, EOM
        ASSERT_EQ_INT(func == 7 ? 0 : 1, r[12]); // LastFlag (none for GetMyZone)
        ASSERT_EQ_INT(1, r[15]); // one name
        ASSERT_EQ_INT((int)zl, r[16]);
        ASSERT_EQ_INT(0, memcmp(r + 17, zone, zl));
        ASSERT_EQ_INT((int)(3 + 5 + 8 + 1 + zl), (int)len);
    }
    size_t len = 0;
    const uint8_t *r = zip_atp(8, 2, 0x200, &len);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(1, r[12]); // last
    ASSERT_EQ_INT(0, r[15]); // and empty
    ASSERT_EQ_INT(3 + 5 + 8, (int)len);
    link_delete();
}

// A ZIP Query for this network gets a Reply with its zone; one for networks
// the router does not know gets nothing (Inside AppleTalk 8-4).
TEST(a_zip_query_for_this_network_gets_its_zone) {
    link_boot();
    uint8_t q[] = {1, 1, 0, 1};
    guest_ddp(GUEST_NODE, 6, 252, 6, q, sizeof q);
    guest_advance_to(link_now_ns() + 30e6);
    size_t len = 0;
    const uint8_t *r = last_short(GUEST_NODE, 6, &len);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(252, r[5]);
    ASSERT_EQ_INT(2, r[8]); // Reply
    ASSERT_EQ_INT(1, r[9]); // one network
    ASSERT_EQ_INT(0, r[10]); // network 1
    ASSERT_EQ_INT(1, r[11]);
    ASSERT_EQ_INT(12, r[12]); // "Granny Smith"
    wire_clear();
    uint8_t q2[] = {1, 1, 0x12, 0x34};
    guest_ddp(GUEST_NODE, 6, 252, 6, q2, sizeof q2);
    guest_advance_to(link_now_ns() + 30e6);
    ASSERT_TRUE(last_short(GUEST_NODE, 6, &len) == NULL);
    link_delete();
}

// A BrRq to the router's NIS (8-18): a LkUp is broadcast on the local network
// with "*" replaced by the zone name and net 0 by this network, and the host's
// own names answer the requester.  A zone nobody is in gets nothing.
TEST(a_brrq_becomes_a_local_lookup) {
    link_boot();
    atalk_nbp_service_desc_t desc = {.object = "Test Host", .type = "LinkTest", .socket = 200};
    atalk_nbp_entry_t *entry = NULL;
    ASSERT_EQ_INT(0, atalk_nbp_publish(&entry, &desc));
    uint8_t brrq[] = {0x11, 0x42, 0, 0, GUEST_NODE, 253, 0, 1, '=', 8, 'L', 'i', 'n', 'k', 'T', 'e', 's', 't', 1, '*'};
    guest_ddp(GUEST_NODE, 2, 253, 2, brrq, sizeof brrq);
    guest_advance_to(link_now_ns() + 30e6);
    size_t len = 0;
    const uint8_t *lk = last_short(0xFF, 2, &len);
    ASSERT_TRUE(lk != NULL);
    ASSERT_EQ_INT(0x21, lk[8]); // LkUp, one tuple
    ASSERT_EQ_INT(0x42, lk[9]); // the requester's NBP id
    ASSERT_EQ_INT(1, lk[11]); // the requester's net filled in: network 1
    ASSERT_EQ_INT(GUEST_NODE, lk[12]);
    size_t zone_at = 15 + 2 + 9; // after the object "=" and the type "LinkTest"
    ASSERT_EQ_INT(12, lk[zone_at]); // the zone: "Granny Smith"
    ASSERT_EQ_INT(0, memcmp(lk + zone_at + 1, "Granny Smith", 12));
    const uint8_t *rep = last_short(GUEST_NODE, 2, &len);
    ASSERT_TRUE(rep != NULL);
    ASSERT_EQ_INT(0x31, rep[8]); // LkUp-Reply with our name
    ASSERT_EQ_INT(1, rep[11]); // at network 1

    wire_clear();
    uint8_t other[] = {0x11, 0x43, 0, 0, GUEST_NODE, 253, 0, 1, '=', 1, '=', 5, 'O', 't', 'h', 'e', 'r'};
    guest_ddp(GUEST_NODE, 2, 253, 2, other, sizeof other);
    guest_advance_to(link_now_ns() + 30e6);
    ASSERT_TRUE(last_short(0xFF, 2, &len) == NULL);
    ASSERT_TRUE(last_short(GUEST_NODE, 2, &len) == NULL);
    atalk_nbp_withdraw(&entry);
    link_delete();
}

// --- the router between instances: links, RTMP, ZIP, forwarding -----------------

// A scripted peer router at the other end of a loopback link: what the router
// sends is captured here, and the test plays the peer's part.
#define PEER_REC_MAX 256
typedef struct {
    int n;
    size_t len[PEER_REC_MAX];
    uint8_t rec[PEER_REC_MAX][ATALK_LINK_REC_MAX];
    bool closed;
} peer_t;
static peer_t g_peers[2];

static void peer_send(void *ctx, const uint8_t *rec, size_t len, bool reliable) {
    (void)reliable;
    peer_t *p = ctx;
    if (p->n < PEER_REC_MAX) {
        memcpy(p->rec[p->n], rec, len);
        p->len[p->n++] = len;
    }
}
static void peer_close(void *ctx) {
    ((peer_t *)ctx)->closed = true;
}
static const atalk_link_transport_t k_peer_transport = {.send = peer_send, .close = peer_close};

// Open a link to peer `i`, whose network is `net`, and finish its HELLO.
static atalk_link_t *peer_link(int i, uint16_t net, bool friend_link) {
    memset(&g_peers[i], 0, sizeof g_peers[i]);
    atalk_link_t *l = atalk_link_open(&k_peer_transport, &g_peers[i], "loopback", friend_link);
    ASSERT_TRUE(l != NULL);
    ASSERT_EQ_INT(ATALK_LINK_HELLO, g_peers[i].rec[0][0]); // ours goes out at once
    uint8_t hello[] = {ATALK_LINK_HELLO, 1, (uint8_t)(net >> 8), (uint8_t)net, 0, 4, 'P', 'e', 'e', 'r'};
    atalk_link_receive(l, hello, sizeof hello);
    ASSERT_EQ_INT(ATALK_LINK_OPEN, atalk_link_state(l));
    return l;
}

// Feed the router one datagram from the peer: an extended DDP header, `data`.
static void peer_ddp(atalk_link_t *l, uint16_t dst_net, uint8_t dst_node, uint8_t dst_sock, uint16_t src_net,
                     uint8_t src_node, uint8_t src_sock, uint8_t type, const uint8_t *data, size_t dlen, uint8_t hop) {
    uint8_t rec[ATALK_LINK_REC_MAX];
    size_t total = 13 + dlen;
    rec[0] = ATALK_LINK_DDP;
    rec[1] = (uint8_t)((hop << 2) | (total >> 8));
    rec[2] = (uint8_t)total;
    rec[3] = rec[4] = 0;
    rec[5] = (uint8_t)(dst_net >> 8);
    rec[6] = (uint8_t)dst_net;
    rec[7] = (uint8_t)(src_net >> 8);
    rec[8] = (uint8_t)src_net;
    rec[9] = dst_node;
    rec[10] = src_node;
    rec[11] = dst_sock;
    rec[12] = src_sock;
    rec[13] = type;
    memcpy(rec + 14, data, dlen);
    atalk_link_receive(l, rec, 1 + total);
}

// The peer's RTMP Data: `tuples` is net/distance pairs.
static void peer_rtmp(atalk_link_t *l, const uint16_t *tuples, int n) {
    uint8_t d[7 + 3 * 16] = {0, 0, 8, 33, 0, 0, 0x82};
    for (int i = 0; i < n; i++) {
        d[7 + 3 * i] = (uint8_t)(tuples[2 * i] >> 8);
        d[8 + 3 * i] = (uint8_t)tuples[2 * i];
        d[9 + 3 * i] = (uint8_t)tuples[2 * i + 1];
    }
    peer_ddp(l, 0, 33, 1, 0, 33, 1, 1, d, (size_t)(7 + 3 * n), 0);
}

// The last DDP record sent to peer `i` whose DDP type is `type`, or NULL;
// `*len` is the datagram's length (without the record's kind byte).
static const uint8_t *peer_last_ddp(int i, uint8_t type, size_t *len) {
    for (int k = g_peers[i].n - 1; k >= 0; k--) {
        const uint8_t *r = g_peers[i].rec[k];
        if (r[0] == ATALK_LINK_DDP && g_peers[i].len[k] >= 14 && r[13] == type) {
            *len = g_peers[i].len[k] - 1;
            return r + 1;
        }
    }
    return NULL;
}

// The distance the router advertises for `net` in a datagram of RTMP Data,
// or -1 if the tuple is not there.
static int rtmp_distance(const uint8_t *dg, size_t len, uint16_t net) {
    for (size_t p = 13 + 7; p + 3 <= len; p += 3)
        if (((dg[p] << 8) | dg[p + 1]) == net)
            return dg[p + 2];
    return -1;
}

// Link up: HELLO both ways, then the router tells the peer its table at once.
// The peer's networks come in by RTMP, one hop further, and the router asks
// for their zones; with a reply the zones are known and listed.
TEST(a_linked_router_learns_the_peers_networks_and_zones) {
    link_boot();
    atalk_link_t *l = peer_link(0, 54, true);
    size_t len = 0;
    const uint8_t *rtmp = peer_last_ddp(0, 1, &len);
    ASSERT_TRUE(rtmp != NULL);
    ASSERT_EQ_INT(0, rtmp_distance(rtmp, len, 1)); // our network, directly connected
    ASSERT_EQ_INT(0, (rtmp[13] << 8) | rtmp[14]); // sender network 0 on a link (5-15)

    uint16_t tuples[] = {54, 0, 99, 1};
    peer_rtmp(l, tuples, 2);
    ASSERT_EQ_INT(54, atalk_link_peer_net(l));
    guest_advance_to(link_now_ns() + 1e6); // the ZIP timer asks at once
    const uint8_t *q = peer_last_ddp(0, 6, &len);
    ASSERT_TRUE(q != NULL);
    ASSERT_EQ_INT(1, q[13]); // ZIP Query
    ASSERT_EQ_INT(2, q[14]); // for two networks
    uint8_t reply[] = {2, 2, 0, 54, 9, 'A', 'l', 'e', 'x', 0x27, 's', ' ', 'M', 'c', 0, 99, 4, 'F', 'a', 'r', '!'};
    peer_ddp(l, 1, 33, 6, 54, 33, 6, 6, reply, sizeof reply, 1);

    // GetZoneList from the guest: ours first, then the two learnt
    uint8_t treq[8] = {0x40, 1, 0x33, 0x01, 8, 0, 0, 1};
    wire_clear();
    guest_ddp(GUEST_NODE, 6, 254, 3, treq, sizeof treq);
    guest_advance_to(link_now_ns() + 30e6);
    const uint8_t *r = last_short(GUEST_NODE, 3, &len);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(1, r[12]); // last
    ASSERT_EQ_INT(3, r[15]); // three zones
    ASSERT_EQ_INT(12, r[16]);
    ASSERT_EQ_INT(0, memcmp(r + 17, "Granny Smith", 12));
    ASSERT_EQ_INT(9, r[29]);
    ASSERT_EQ_INT(0, memcmp(r + 30, "Alex's Mc", 9));
    ASSERT_EQ_INT(4, r[39]);
    atalk_link_close(l, "test over");
    ASSERT_TRUE(g_peers[0].closed);
    link_delete();
}

// A guest datagram for a network behind the link is forwarded over it, one
// hop further; a datagram from the link for the guest goes onto LocalTalk
// from the router's node, with the guest's node in the DDP header.
TEST(datagrams_are_forwarded_both_ways) {
    link_boot();
    atalk_link_t *l = peer_link(0, 154, true);
    uint16_t tuples[] = {154, 0};
    peer_rtmp(l, tuples, 1);

    uint64_t fwd0 = atalk_router_get_stats()->forwarded_out;
    uint8_t f[64];
    uint8_t ping[] = {1, 'h', 'i'};
    size_t n = ext_frame(f, 154, 1, 77, 4, 200, 4, ping, sizeof ping);
    int before = g_peers[0].n;
    guest_frame(f, n);
    ASSERT_EQ_INT(before + 1, g_peers[0].n);
    const uint8_t *fw = g_peers[0].rec[before];
    ASSERT_EQ_INT(ATALK_LINK_DDP, fw[0]);
    ASSERT_EQ_INT(1, (fw[1] >> 2) & 0x0F); // one hop
    ASSERT_EQ_INT(77, fw[9]); // to node 77 on network 154
    ASSERT_EQ_INT(1, (fw[7] << 8) | fw[8]); // from network 1
    ASSERT_EQ_INT(1, (int)(atalk_router_get_stats()->forwarded_out - fwd0));

    wire_clear();
    uint8_t echo[] = {2, 'h', 'i'};
    peer_ddp(l, 1, GUEST_NODE, 200, 154, 77, 4, 4, echo, sizeof echo, 1);
    guest_advance_to(link_now_ns() + 30e6);
    size_t len = 0;
    const uint8_t *r = last_ext(GUEST_NODE, &len);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(HOST_NODE, r[1]); // LLAP: from the router
    ASSERT_EQ_INT(77, r[12]); // DDP: from node 77...
    ASSERT_EQ_INT(154, (r[9] << 8) | r[10]); // ...on network 154
    ASSERT_EQ_INT(1, (r[3] >> 2) & 0x0F); // the hop count it arrived with
    atalk_link_close(l, "test over");
    link_delete();
}

// The host's own services are not offered over links: a datagram for its AFP
// socket from a link is filtered, and a FwdReq becomes a LkUp on this network
// without the host answering for its names -- unless the user shares them
// with friends (tabs never: share_host_services is for friend links).
TEST(the_host_services_are_filtered_from_links) {
    link_boot();
    atalk_nbp_service_desc_t desc = {.object = "Test Host", .type = "LinkTest", .socket = 200};
    atalk_nbp_entry_t *entry = NULL;
    ASSERT_EQ_INT(0, atalk_nbp_publish(&entry, &desc));
    atalk_link_t *l = peer_link(0, 254, true);
    uint16_t tuples[] = {254, 0};
    peer_rtmp(l, tuples, 1);

    uint64_t filtered = atalk_router_get_stats()->dropped_filter;
    uint8_t treq[8] = {0x40, 1, 0, 9, 3, 0, 0, 0};
    peer_ddp(l, 1, 33, 8, 254, 77, 250, 3, treq, sizeof treq, 1);
    ASSERT_EQ_INT(1, (int)(atalk_router_get_stats()->dropped_filter - filtered));

    // FwdReq for =:LinkTest@Granny Smith from 254.77 socket 250
    uint8_t fwd[] = {0x41, 0x50, 0,  254, 77,  250, 0,   1,   '=', 8,   'L', 'i', 'n', 'k', 'T', 'e',
                     's',  't',  12, 'G', 'r', 'a', 'n', 'n', 'y', ' ', 'S', 'm', 'i', 't', 'h'};
    wire_clear();
    int before = g_peers[0].n;
    peer_ddp(l, 1, 0, 2, 254, 33, 2, 2, fwd, sizeof fwd, 1);
    guest_advance_to(link_now_ns() + 30e6);
    size_t len = 0;
    const uint8_t *lk = last_short(0xFF, 2, &len);
    ASSERT_TRUE(lk != NULL);
    ASSERT_EQ_INT(0x21, lk[8]); // a LkUp on this network
    ASSERT_EQ_INT(254, (lk[10] << 8) | lk[11]); // the requester's address kept
    ASSERT_TRUE(peer_last_ddp(0, 2, &len) == NULL); // and the host did not answer over the link
    (void)before;

    atalk_link_close(l, "test over");
    atalk_nbp_withdraw(&entry);
    link_delete();
}

// A BrRq from the guest for a zone on the other network becomes a FwdReq to
// that network's router (node 0, "any router" there), routed over the link.
TEST(a_brrq_for_a_remote_zone_is_forwarded) {
    link_boot();
    atalk_link_t *l = peer_link(0, 354, true);
    uint16_t tuples[] = {354, 0};
    peer_rtmp(l, tuples, 1);
    uint8_t reply[] = {2, 1, 354 >> 8, 354 & 0xFF, 4, 'A', 'l', 'e', 'x'};
    peer_ddp(l, 1, 33, 6, 354, 33, 6, 6, reply, sizeof reply, 1);

    uint64_t fwd0 = atalk_router_get_stats()->nbp_fwdreq_out;
    uint8_t brrq[] = {0x11, 0x44, 0, 0, GUEST_NODE, 253, 0, 1, '=', 1, '=', 4, 'A', 'l', 'e', 'x'};
    guest_ddp(GUEST_NODE, 2, 253, 2, brrq, sizeof brrq);
    size_t len = 0;
    const uint8_t *fw = peer_last_ddp(0, 2, &len);
    ASSERT_TRUE(fw != NULL);
    ASSERT_EQ_INT(354, (fw[4] << 8) | fw[5]); // to network 354...
    ASSERT_EQ_INT(0, fw[8]); // ...node 0, any router there
    ASSERT_EQ_INT(2, fw[10]); // its NIS
    ASSERT_EQ_INT(0x41, fw[13]); // FwdReq, one tuple
    ASSERT_EQ_INT(1, (fw[15] << 8) | fw[16]); // requester net 0 filled in as 1
    ASSERT_EQ_INT(1, (int)(atalk_router_get_stats()->nbp_fwdreq_out - fwd0));
    atalk_link_close(l, "test over");
    link_delete();
}

// Two links: a datagram from one for a network behind the other is forwarded
// in transit; RTMP to each link leaves out what was learnt from it (split
// horizon); a datagram that has crossed 15 routers goes no further.
TEST(transit_and_split_horizon) {
    link_boot();
    atalk_link_t *a = peer_link(0, 454, false);
    atalk_link_t *b = peer_link(1, 477, false);
    uint16_t ta[] = {454, 0};
    uint16_t tb[] = {477, 0};
    peer_rtmp(a, ta, 1);
    peer_rtmp(b, tb, 1);
    guest_advance_to(link_now_ns() + 200e6); // the triggered update goes out

    size_t len = 0;
    const uint8_t *to_a = peer_last_ddp(0, 1, &len);
    ASSERT_TRUE(to_a != NULL);
    ASSERT_EQ_INT(-1, rtmp_distance(to_a, len, 454)); // not told its own network back
    ASSERT_EQ_INT(1, rtmp_distance(to_a, len, 477)); // told the other one, one hop away
    ASSERT_EQ_INT(0, rtmp_distance(to_a, len, 1));

    uint64_t transit0 = atalk_router_get_stats()->forwarded_transit;
    uint64_t hops0 = atalk_router_get_stats()->dropped_hops;
    int before = g_peers[1].n;
    uint8_t data[] = {1, 2, 3};
    peer_ddp(a, 477, 9, 4, 454, 8, 4, 4, data, sizeof data, 2);
    ASSERT_EQ_INT(before + 1, g_peers[1].n);
    ASSERT_EQ_INT(3, (g_peers[1].rec[before][1] >> 2) & 0x0F); // 2 hops + ours
    ASSERT_EQ_INT(1, (int)(atalk_router_get_stats()->forwarded_transit - transit0));

    peer_ddp(a, 477, 9, 4, 454, 8, 4, 4, data, sizeof data, 15);
    ASSERT_EQ_INT(before + 1, g_peers[1].n); // dropped
    ASSERT_EQ_INT(1, (int)(atalk_router_get_stats()->dropped_hops - hops0));
    atalk_link_close(a, "test over");
    atalk_link_close(b, "test over");
    link_delete();
}

// Routes age without RTMP: suspect after one validity period, bad after two
// (advertised with distance 31), gone after four.  A lost link makes its
// routes bad at once.  A peer with our own network number is refused.
TEST(routes_age_and_links_fail) {
    link_boot();
    atalk_link_t *a = peer_link(0, 554, false);
    atalk_link_t *b = peer_link(1, 577, false);
    uint16_t ta[] = {554, 0};
    uint16_t tb[] = {577, 0};
    peer_rtmp(a, ta, 1);
    peer_rtmp(b, tb, 1);
    double t0 = link_now_ns();
    // Peer b keeps talking; peer a goes silent
    for (int s = 1; s <= 5; s++) {
        guest_advance_to(t0 + s * 10e9 - 1e9);
        peer_rtmp(b, tb, 1);
    }
    guest_advance_to(t0 + 50e9);
    size_t len = 0;
    const uint8_t *to_b = peer_last_ddp(1, 1, &len);
    ASSERT_TRUE(to_b != NULL);
    ASSERT_EQ_INT(31, rtmp_distance(to_b, len, 554)); // bad: notify neighbor
    guest_advance_to(t0 + 100e9);
    for (int s = 6; s <= 10; s++)
        peer_rtmp(b, tb, 1);
    to_b = peer_last_ddp(1, 1, &len);
    ASSERT_EQ_INT(-1, rtmp_distance(to_b, len, 554)); // gone

    atalk_link_lost(b, "transport gone");
    ASSERT_EQ_INT(ATALK_LINK_CLOSED, atalk_link_state(b));
    atalk_link_close(a, "test over");

    // Network 1 is ours: a peer claiming it is refused
    memset(&g_peers[0], 0, sizeof g_peers[0]);
    atalk_link_t *c = atalk_link_open(&k_peer_transport, &g_peers[0], "loopback", false);
    uint8_t hello[] = {ATALK_LINK_HELLO, 1, 0, 1, 0, 0};
    atalk_link_receive(c, hello, sizeof hello);
    ASSERT_EQ_INT(ATALK_LINK_CLOSED, atalk_link_state(c));
    ASSERT_TRUE(g_peers[0].closed);
    link_delete();
}

// The other end starts its side of an open link again (its tab was frozen
// and dropped by the others, while this end never noticed): its first HELLO
// is answered with a reply and our table, so its side opens; a reply is
// never answered, so two open ends do not trade HELLOs for ever.
TEST(a_hello_on_an_open_link_is_answered_once) {
    link_boot();
    atalk_link_t *l = peer_link(0, 612, false);
    // The connecting side's answer carried the reply flag
    ASSERT_EQ_INT(ATALK_LINK_HELLO, g_peers[0].rec[1][0]);
    ASSERT_TRUE((g_peers[0].rec[1][4] & ATALK_LINK_FLAG_REPLY) != 0);
    ASSERT_TRUE((g_peers[0].rec[0][4] & ATALK_LINK_FLAG_REPLY) == 0); // the first was not one

    g_peers[0].n = 0;
    uint8_t again[] = {ATALK_LINK_HELLO, 1, 612 >> 8, 612 & 0xFF, 0, 4, 'P', 'e', 'e', 'r'};
    atalk_link_receive(l, again, sizeof again);
    ASSERT_EQ_INT(ATALK_LINK_OPEN, atalk_link_state(l));
    ASSERT_TRUE(g_peers[0].n >= 2);
    ASSERT_EQ_INT(ATALK_LINK_HELLO, g_peers[0].rec[0][0]);
    ASSERT_TRUE((g_peers[0].rec[0][4] & ATALK_LINK_FLAG_REPLY) != 0);
    size_t len = 0;
    ASSERT_TRUE(peer_last_ddp(0, 1, &len) != NULL); // the table again

    g_peers[0].n = 0;
    uint8_t reply[] = {ATALK_LINK_HELLO, 1, 612 >> 8, 612 & 0xFF, ATALK_LINK_FLAG_REPLY, 4, 'P', 'e', 'e', 'r'};
    atalk_link_receive(l, reply, sizeof reply);
    ASSERT_EQ_INT(0, g_peers[0].n);
    atalk_link_close(l, "test over");
    link_delete();
}

// A zone still unknown when its machine goes (the ZIP Query unanswered) is
// asked for again once the next machine takes the cable.  Unplugging stops
// the ZIP timer with the others, and plugging in did not start it again: the
// route stayed zoneless, and lookups in "*" never reached that network.
TEST(a_zone_unknown_across_a_new_machine_is_asked_again) {
    link_boot();
    atalk_link_t *l = peer_link(0, 640, false);
    uint16_t tuples[] = {640, 0};
    peer_rtmp(l, tuples, 1);
    guest_advance_to(link_now_ns() + 1e6);
    size_t len = 0;
    ASSERT_TRUE(peer_last_ddp(0, 6, &len) != NULL); // asked once, never answered
    link_delete();

    link_boot();
    g_peers[0].n = 0;
    guest_advance_to(link_now_ns() + 1e6);
    const uint8_t *q = peer_last_ddp(0, 6, &len);
    ASSERT_TRUE(q != NULL);
    ASSERT_EQ_INT(1, q[13]); // ZIP Query
    atalk_link_close(l, "test over");
    link_delete();
}

// --- ATP exactly-once requests a handler declines -------------------------------

#define DECLINE_SOCKET 60
static int g_decline_calls;
static bool g_decline;
static void decline_or_answer(const ddp_header_t *ddp, atp_packet_t *req, void *ctx) {
    (void)ctx;
    g_decline_calls++;
    if (g_decline)
        atp_xo_forget(ddp, req);
    else
        atp_responder_send_simple(ddp, req, NULL, (const uint8_t *)"ok", 2, false);
}

// A handler that leaves an XO request for its retry (PAP's SendData past the
// flow quantum) forgets it, and the retry reaches it as a new request.  The
// dispatcher's XO entry took the retry for a duplicate of a response being
// prepared, and dropped it -- and every later one -- until the release timer.
// Once answered, a retransmit is the duplicate again: the cached response
// goes back and the handler is not run twice.
TEST(a_declined_xo_request_is_seen_again_on_its_retry) {
    link_boot();
    static const atp_socket_handler_t handler = {.handle_request = decline_or_answer};
    ASSERT_EQ_INT(0, atp_register_socket_handler(DECLINE_SOCKET, &handler, NULL));
    g_decline_calls = 0;
    g_decline = true;
    uint8_t treq[8] = {ATP_TREQ | ATP_XO, 0x01, 0x30, 0x01, 0, 0, 0, 0};
    wire_clear();
    guest_ddp(GUEST_NODE, DECLINE_SOCKET, 200, 3, treq, sizeof treq);
    guest_advance_to(link_now_ns() + 5e6);
    ASSERT_EQ_INT(1, g_decline_calls);
    ASSERT_EQ_INT(0, wire_count_atp(GUEST_NODE, ATP_TRESP, 0xFF));

    guest_ddp(GUEST_NODE, DECLINE_SOCKET, 200, 3, treq, sizeof treq); // the retry
    guest_advance_to(link_now_ns() + 5e6);
    ASSERT_EQ_INT(2, g_decline_calls);
    ASSERT_EQ_INT(0, wire_count_atp(GUEST_NODE, ATP_TRESP, 0xFF));

    g_decline = false;
    guest_ddp(GUEST_NODE, DECLINE_SOCKET, 200, 3, treq, sizeof treq);
    guest_advance_to(link_now_ns() + 5e6);
    ASSERT_EQ_INT(3, g_decline_calls);
    ASSERT_EQ_INT(1, wire_count_atp(GUEST_NODE, ATP_TRESP, 0xFF));

    guest_ddp(GUEST_NODE, DECLINE_SOCKET, 200, 3, treq, sizeof treq); // a true duplicate
    guest_advance_to(link_now_ns() + 5e6);
    ASSERT_EQ_INT(3, g_decline_calls);
    ASSERT_EQ_INT(2, wire_count_atp(GUEST_NODE, ATP_TRESP, 0xFF)); // from the cache
    atp_unregister_socket_handler(DECLINE_SOCKET);
    link_delete();
}

// --- ASP sessions ----------------------------------------------------------------

#define ASP_CLOSE_SESS 1
#define ASP_COMMAND    2
#define ASP_GET_STAT   3

// The last ATP response to `node`, or NULL.
static const uint8_t *last_tresp(uint8_t node, size_t *len) {
    for (int i = wire_count() - 1; i >= 0; i--) {
        size_t l = 0;
        const uint8_t *f = wire_frame(i, &l);
        if (l >= 16 && f[0] == node && f[2] == LLAP_TYPE_DDP_SHORT && f[7] == 3 && (f[8] & 0xC0) == ATP_TRESP) {
            if (len)
                *len = l;
            return f;
        }
    }
    return NULL;
}

static uint32_t tresp_result(const uint8_t *f) {
    return ((uint32_t)f[12] << 24) | ((uint32_t)f[13] << 16) | ((uint32_t)f[14] << 8) | f[15];
}

// An ASP request with user bytes [func, sid, 0, seq] and `data`.
static void asp_request(uint8_t node, uint8_t func, uint8_t sid, uint16_t tid, const uint8_t *data, size_t dlen) {
    uint8_t atp[8 + 64] = {ATP_TREQ | ATP_XO, 0x01, (uint8_t)(tid >> 8), (uint8_t)tid, func, sid, 0, 1};
    if (dlen)
        memcpy(atp + 8, data, dlen);
    guest_ddp(node, AFP_SOCKET, 200, 3, atp, 8 + dlen);
    guest_advance_to(link_now_ns() + 5e6);
}

static int live_sessions(void) {
    int n = 0;
    for (int i = 0; i < atalk_asp_session_max(); i++)
        n += atalk_asp_session_in_use(i);
    return n;
}

// CloseSess names its session in user byte 1 and carries no data (Inside
// AppleTalk Fig. 11-10).  The server read the session from the ATP data, so a
// real client's CloseSess closed nothing.
TEST(close_sess_closes_the_session) {
    link_boot();
    g_asp_closes = 0;
    uint8_t sid = asp_open_session(GUEST_NODE, 100, 0x1001);
    ASSERT_TRUE(sid != 0);
    ASSERT_EQ_INT(1, live_sessions());
    wire_clear();
    asp_request(GUEST_NODE, ASP_CLOSE_SESS, sid, 0x1002, NULL, 0);
    ASSERT_EQ_INT(0, live_sessions());
    ASSERT_EQ_INT(1, g_asp_closes);
    size_t len = 0;
    const uint8_t *r = last_tresp(GUEST_NODE, &len);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(16, (int)len); // no data...
    ASSERT_EQ_INT(0, (int)tresp_result(r)); // ...and zero user bytes
    link_delete();
}

// OpenSess refusals carry the spec's codes: ServerBusy when full, BadVersNum
// for anything but version 1.0.
TEST(open_sess_refusals_use_asp_error_codes) {
    link_boot();
    for (int i = 0; i < atalk_asp_session_max(); i++)
        ASSERT_TRUE(asp_open_session((uint8_t)(10 + i), 100, (uint16_t)(0x2000 + i)) != 0);
    wire_clear();
    uint8_t atp[8] = {ATP_TREQ | ATP_XO, 0x01, 0x21, 0x00, 4 /* OpenSess */, 100, 0x01, 0x00};
    guest_ddp(30, AFP_SOCKET, 200, 3, atp, sizeof atp);
    guest_advance_to(link_now_ns() + 5e6);
    const uint8_t *r = last_tresp(30, NULL);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(0xFBD1, (r[14] << 8) | r[15]); // aspServerBusy
    ASSERT_EQ_INT(0, r[13]); // no session id

    wire_clear();
    uint8_t v2[8] = {ATP_TREQ | ATP_XO, 0x01, 0x21, 0x01, 4, 100, 0x02, 0x00}; // version 2.0
    guest_ddp(31, AFP_SOCKET, 200, 3, v2, sizeof v2);
    guest_advance_to(link_now_ns() + 5e6);
    r = last_tresp(31, NULL);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(0xFBD6, (r[14] << 8) | r[15]); // aspBadVersNum
    link_delete();
}

// Session ids are one byte on the wire and unique among live sessions; the
// old ones were a counter's low byte, so two live sessions 256 opens apart
// shared an id and commands for one landed on the other.
TEST(session_ids_stay_unique_across_many_opens) {
    link_boot();
    uint8_t held = asp_open_session(GUEST_NODE, 100, 0x1001);
    ASSERT_TRUE(held != 0);
    for (int i = 0; i < 300; i++) {
        uint8_t sid = asp_open_session(GUEST_NODE + 1, 101, (uint16_t)(0x3000 + i));
        ASSERT_TRUE(sid != 0 && sid != held);
        asp_request(GUEST_NODE + 1, ASP_CLOSE_SESS, sid, (uint16_t)(0x6000 + i), NULL, 0);
    }
    ASSERT_EQ_INT(1, live_sessions());
    link_delete();
}

// A session answers only the node that opened it, and a command for a session
// that is not open is answered SessClosed and never reaches the client -- it
// used to run as "session 0", with no login.
TEST(commands_reach_only_their_own_session) {
    link_boot();
    uint8_t sid = asp_open_session(GUEST_NODE, 100, 0x1001);
    ASSERT_TRUE(sid != 0);
    g_afp_calls = 0;
    uint8_t cmd[] = {0x08}; // any opcode
    wire_clear();
    asp_request(GUEST_NODE + 4, ASP_COMMAND, sid, 0x1002, cmd, sizeof cmd); // another node, same id
    ASSERT_EQ_INT(0, g_afp_calls);
    const uint8_t *r = last_tresp(GUEST_NODE + 4, NULL);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT((int)0xFFFFEC62u, (int)tresp_result(r)); // SessClosed
    wire_clear();
    asp_request(GUEST_NODE, ASP_COMMAND, (uint8_t)(sid + 1), 0x1003, cmd, sizeof cmd); // no such session
    ASSERT_EQ_INT(0, g_afp_calls);
    wire_clear();
    asp_request(GUEST_NODE, ASP_COMMAND, sid, 0x1004, cmd, sizeof cmd); // the real one
    ASSERT_EQ_INT(1, g_afp_calls);
    ASSERT_EQ_INT(0x08, g_afp_last_opcode);
    link_delete();
}

// GetStatus returns the status block and runs nothing, whatever it carries.
// With data it used to be a second command channel: the first data byte was
// dispatched as an AFP opcode from session 0.
TEST(get_status_runs_no_command) {
    link_boot();
    g_afp_calls = 0;
    uint8_t data[] = {0, 0, 0, 0, 0, 0, 0x08, 0, 0};
    asp_request(GUEST_NODE, ASP_GET_STAT, 0, 0x1001, data, sizeof data);
    ASSERT_EQ_INT(0, g_afp_calls);
    ASSERT_TRUE(last_tresp(GUEST_NODE, NULL) != NULL);
    link_delete();
}

#define ASP_TICKLE 5

// The server tickles each open session every 30 s: a workstation that hears
// nothing from its server for two minutes closes the session.  A session
// the guest stops tickling is still closed after two minutes.
TEST(the_server_tickles_each_session) {
    link_boot();
    g_asp_closes = 0;
    uint8_t sid = asp_open_session(GUEST_NODE, 100, 0x1001);
    ASSERT_TRUE(sid != 0);
    wire_clear();
    guest_advance_to(link_now_ns() + 100e9);
    int tickles = wire_count_atp(GUEST_NODE, ATP_TREQ, ASP_TICKLE);
    ASSERT_TRUE(tickles >= 2 && tickles <= 4);
    ASSERT_EQ_INT(1, live_sessions());
    guest_advance_to(link_now_ns() + 40e9);
    ASSERT_EQ_INT(0, live_sessions());
    ASSERT_EQ_INT(1, g_asp_closes);
    link_delete();
}

// A session whose command waits for its data (a remote volume's folder or
// file on its way) is not idle, however long the wait.  System 6's AppleShare
// retries the command a while and then waits in silence, with a tickle now
// and then at most, and the session used to expire under the waiting command
// after two minutes.  The server tickles it meanwhile, and answers when the
// data is in.
TEST(a_session_waiting_on_a_deferred_command_stays_open) {
    link_boot();
    g_asp_closes = 0;
    uint8_t sid = asp_open_session(GUEST_NODE, 100, 0x1001);
    ASSERT_TRUE(sid != 0);
    g_afp_calls = 0;
    g_afp_defer = true;
    uint8_t cmd[] = {0x08};
    asp_request(GUEST_NODE, ASP_COMMAND, sid, 0x1002, cmd, sizeof cmd);
    ASSERT_EQ_INT(1, g_afp_calls);
    wire_clear();
    guest_advance_to(link_now_ns() + 300e9); // five minutes
    ASSERT_EQ_INT(1, live_sessions());
    ASSERT_TRUE(wire_count_atp(GUEST_NODE, ATP_TREQ, ASP_TICKLE) >= 8);
    ASSERT_TRUE(last_tresp(GUEST_NODE, NULL) == NULL);
    g_afp_defer = false;
    asp_retry_deferred();
    guest_advance_to(link_now_ns() + 5e6);
    ASSERT_EQ_INT(2, g_afp_calls);
    const uint8_t *r = last_tresp(GUEST_NODE, NULL);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT(0, (int)tresp_result(r));
    ASSERT_EQ_INT(0, g_asp_closes);
    link_delete();
}

// Each session has its own pending write: with one for the whole server, a
// second session's Write while the first waited got no reply.
TEST(two_sessions_write_at_once) {
    link_boot();
    uint8_t s1 = asp_open_session(GUEST_NODE, 100, 0x1001);
    uint8_t s2 = asp_open_session(GUEST_NODE + 1, 101, 0x1002);
    ASSERT_TRUE(s1 != 0 && s2 != 0);
    wire_clear();
    asp_write(GUEST_NODE, s1, 0x2001, 1);
    asp_write(GUEST_NODE + 1, s2, 0x2002, 1);
    ASSERT_EQ_INT(1, wire_count_atp(GUEST_NODE, ATP_TREQ, ASP_WRITE_CONTINUE));
    ASSERT_EQ_INT(1, wire_count_atp(GUEST_NODE + 1, ATP_TREQ, ASP_WRITE_CONTINUE));
    // A second Write in the same session while its first waits is answered
    // with an error, not dropped.
    wire_clear();
    asp_write(GUEST_NODE, s1, 0x2003, 2);
    const uint8_t *r = last_tresp(GUEST_NODE, NULL);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT((int)0xFFFFEC6Au, (int)tresp_result(r)); // MiscErr
    link_delete();
}

// A restore is a server restart.  The guest held a session when the checkpoint
// was taken; the restored connection has none, so a command on it is answered
// SessClosed -- the AppleShare client then sees the connection closed and
// drops the volume -- never dropped, which would leave it waiting out a
// timeout.  A new session gets neither the old session's wire id nor its
// reference, so the stale one stays refused.
TEST(a_restored_connection_is_a_restarted_server) {
    link_boot();
    uint8_t sid = asp_open_session(GUEST_NODE, 100, 0x1001);
    ASSERT_TRUE(sid != 0);
    atalk_session_info_t info;
    ASSERT_TRUE(atalk_asp_session_info(0, &info));
    unsigned ref = info.session_ref;
    link_checkpoint();
    link_load(false);
    ASSERT_EQ_INT(0, live_sessions());

    g_afp_calls = 0;
    uint8_t cmd[] = {0x08};
    wire_clear();
    asp_request(GUEST_NODE, ASP_COMMAND, sid, 0x1002, cmd, sizeof cmd);
    ASSERT_EQ_INT(0, g_afp_calls);
    const uint8_t *r = last_tresp(GUEST_NODE, NULL);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ_INT((int)0xFFFFEC62u, (int)tresp_result(r)); // SessClosed

    uint8_t sid2 = asp_open_session(GUEST_NODE, 100, 0x1003);
    ASSERT_TRUE(sid2 != 0 && sid2 != sid);
    ASSERT_TRUE(atalk_asp_session_info(0, &info));
    ASSERT_TRUE(info.session_ref != ref);
    link_delete();
}

// A rename that cannot be published changes nothing: the entry keeps its old
// name, still found by a lookup (services stored the new name first, or
// withdrew first).
static int count_named(const char *object) {
    int n = 0;
    atalk_nbp_info_t info;
    for (int i = 0; i < atalk_nbp_entry_max(); i++)
        if (atalk_nbp_entry_info(i, &info) && strcmp(info.object, object) == 0)
            n++;
    return n;
}

TEST(a_publish_that_fails_changes_nothing) {
    link_boot();
    atalk_nbp_service_desc_t alpha = {.object = "Alpha", .type = "LinkTest", .socket = 200};
    atalk_nbp_service_desc_t beta = {.object = "Beta", .type = "LinkTest", .socket = 201};
    atalk_nbp_entry_t *a = NULL, *b = NULL;
    ASSERT_EQ_INT(0, atalk_nbp_publish(&a, &alpha));
    ASSERT_EQ_INT(0, atalk_nbp_publish(&b, &beta));
    atalk_nbp_entry_t *before = a;
    alpha.object = "Beta"; // taken
    ASSERT_EQ_INT(-1, atalk_nbp_publish(&a, &alpha));
    ASSERT_TRUE(a == before);
    ASSERT_EQ_INT(1, count_named("Alpha"));
    ASSERT_EQ_INT(1, count_named("Beta"));
    alpha.object = "Gamma"; // free: renamed in place
    ASSERT_EQ_INT(0, atalk_nbp_publish(&a, &alpha));
    ASSERT_TRUE(a == before);
    ASSERT_EQ_INT(0, count_named("Alpha"));
    ASSERT_EQ_INT(1, count_named("Gamma"));
    atalk_nbp_withdraw(&a);
    ASSERT_TRUE(a == NULL);
    ASSERT_EQ_INT(0, count_named("Gamma"));
    atalk_nbp_withdraw(&b);
    link_delete();
}

int main(void) {
    RUN(a_publish_that_fails_changes_nothing);
    RUN(boot_installs_the_frame_sink_and_delete_removes_it);
    RUN(enq_for_our_node_is_acked_and_others_are_not);
    RUN(a_numbered_network_offers_its_guest_only_its_own_nodes);
    RUN(nbp_lookup_is_answered_after_the_rts_cts_handshake);
    RUN(every_transport_timer_is_registered_at_init);
    RUN(a_rebuilt_stack_serves_a_write_the_old_one_left_pending);
    RUN(detaching_the_stack_stops_its_transmitter);
    RUN(the_checkpoint_block_is_restored);
    RUN(a_block_from_a_failed_checkpoint_is_not_applied);
    RUN(the_network_outlives_every_machine);
    RUN(machines_plug_into_the_printer_and_out_again);
    RUN(a_failed_load_leaves_the_running_machine_plugged_in);
    RUN(destroying_the_replaced_machine_leaves_the_new_one_plugged_in);
    RUN(a_malformed_ddp_frame_is_dropped_and_counted);
    RUN(every_discard_is_counted_by_reason);
    RUN(a_lookup_matching_eight_names_answers_with_eight_tuples);
    RUN(a_lookup_reply_with_eight_tuples_delivers_all_eight);
    RUN(a_published_name_goes_out_in_macroman_and_folds_by_table_d2);
    RUN(a_lookup_goes_out_in_macroman_and_its_replies_come_back_in_utf8);
    RUN(a_name_macroman_cannot_hold_is_refused);
    RUN(the_echoer_answers_requests_on_socket_4_with_a_reply);
    RUN(an_extended_request_gets_an_extended_reply);
    RUN(an_extended_header_is_checked_before_it_is_used);
    RUN(an_rtmp_request_is_answered_with_this_net_and_the_router);
    RUN(the_router_broadcasts_rtmp_data_every_ten_seconds);
    RUN(zip_atp_requests_name_the_zone);
    RUN(a_zip_query_for_this_network_gets_its_zone);
    RUN(a_brrq_becomes_a_local_lookup);
    RUN(a_linked_router_learns_the_peers_networks_and_zones);
    RUN(datagrams_are_forwarded_both_ways);
    RUN(the_host_services_are_filtered_from_links);
    RUN(a_brrq_for_a_remote_zone_is_forwarded);
    RUN(transit_and_split_horizon);
    RUN(routes_age_and_links_fail);
    RUN(a_hello_on_an_open_link_is_answered_once);
    RUN(a_zone_unknown_across_a_new_machine_is_asked_again);
    RUN(a_declined_xo_request_is_seen_again_on_its_retry);
    RUN(close_sess_closes_the_session);
    RUN(open_sess_refusals_use_asp_error_codes);
    RUN(session_ids_stay_unique_across_many_opens);
    RUN(commands_reach_only_their_own_session);
    RUN(get_status_runs_no_command);
    RUN(the_server_tickles_each_session);
    RUN(a_session_waiting_on_a_deferred_command_stays_open);
    RUN(two_sessions_write_at_once);
    RUN(a_restored_connection_is_a_restarted_server);
    printf("[PASS] All atalk_link tests passed\n");
    return 0;
}
