// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// atalk_link: the real LLAP, DDP, NBP, ATP and ASP (appletalk.c) driven
// frame by frame from the guest's side of the wire (10-network unit 0.2).
//
// Before this suite the link and transport layers had no unit coverage at
// all: appletalk.c could not be linked into a suite (a stub of its entry
// point sat in the shared harness), and it calls into every layer above it.
// The eight network integration tests exercise it through real Mac clients;
// this suite is where a malformed frame, a lost CTS or a machine rebuild can
// be staged exactly.

#include "appletalk.h"
#include "link_harness.h"
#include "stub_upper.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>

// appletalk_init installs the stack's receiver on the machine's SCC, and
// appletalk_delete takes it off again (link_delete checks).
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

// A directed data frame from the guest is preceded by the guest's RTS, which
// we answer with CTS; one of ours goes out only after the guest's CTS.  This
// drives both directions through an NBP LkUp for a name we registered.
TEST(nbp_lookup_is_answered_after_the_rts_cts_handshake) {
    link_boot();
    atalk_nbp_service_desc_t desc = {.object = "Test Host", .type = "LinkTest", .socket = 200};
    atalk_nbp_entry_t *entry = NULL;
    ASSERT_EQ_INT(0, atalk_nbp_register(&desc, &entry));

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

    ASSERT_EQ_INT(0, atalk_nbp_unregister(entry));
    link_delete();
}

// Every event type the transport can schedule is registered the moment the
// stack comes up -- before a checkpoint restore replays the saved queue.  ATP
// used to register its two only when it first armed one, so a checkpoint taken
// with an AFP command or a print job in flight could not be restored
// (10-network N-05; the printer, LaserWriter and ADSP timers are stubbed out
// of this suite and are covered by the appletalk-afp-checkpoint row).
TEST(every_transport_timer_is_registered_at_init) {
    link_boot();
    ASSERT_EQ_INT(0, sched_pending()); // registered, not armed
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

// A machine rebuild -- machine.boot, a checkpoint load -- tears the stack down
// and brings a new one up in the same process.  Teardown reset nothing below
// ASP, so the new stack inherited the old one's outgoing ATP requests, XO
// cache and pending ASP write: here the old machine's WriteContinue was still
// "pending", and the new machine's first Write was never answered -- not after
// 600 s of guest time (10-network N-08).
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
// it on the wire (10-network N-09).
TEST(detaching_the_stack_stops_its_transmitter) {
    link_boot();
    atalk_nbp_service_desc_t desc = {.object = "Test Host", .type = "LinkTest", .socket = 200};
    atalk_nbp_entry_t *entry = NULL;
    ASSERT_EQ_INT(0, atalk_nbp_register(&desc, &entry));
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

    atalk_set_enabled(false);
    wire_clear();
    // Without answering any RTS: run well past eight RTS timeouts.
    double end = link_now_ns() + 50e6;
    while (link_now_ns() < end)
        guest_idle_until(link_now_ns() + 1e6);
    ASSERT_EQ_INT(0, wire_count());

    atalk_set_enabled(true);
    ASSERT_EQ_INT(0, atalk_nbp_unregister(entry));
    link_delete();
}

// The stack's checkpoint record round-trips: what was saved is applied.
TEST(the_checkpoint_record_is_restored) {
    link_boot();
    atalk_set_enabled(false);
    link_checkpoint();
    link_delete();
    g_aevt_set_calls = 0;
    link_boot_from_checkpoint(false);
    ASSERT_TRUE(!atalk_get_enabled());
    ASSERT_EQ_INT(1, g_aevt_set_calls);
    link_delete();
}

// A record read from a checkpoint in error is not applied.  The stack is
// process-wide: a restore that fails keeps the running machine, and whatever
// this stack applied on the way stays with it -- the old restore tested only
// the magic word, in a local the reader had not written, and then copied the
// Apple event strings out of it (10-network F-10).
TEST(a_record_from_a_failed_checkpoint_is_not_applied) {
    link_boot();
    atalk_set_enabled(false);
    link_checkpoint();
    link_delete();
    g_aevt_set_calls = 0;
    link_boot_from_checkpoint(true);
    ASSERT_TRUE(atalk_get_enabled()); // the default, not the record's
    ASSERT_EQ_INT(0, g_aevt_set_calls);
    link_delete();
}

// The configuration a user or script set travels in the checkpoint and comes
// back on restore: shares with their volume ids, server identity, printer
// settings.  A load used to drop every AFP volume while the restored guest
// still had one mounted -- its next call got ParamErr -- and the server name
// survived only because it was a process static nobody reset (10-network N-07,
// decision D-3).
TEST(configuration_survives_a_checkpoint) {
    link_boot();
    char err[128];
    ASSERT_TRUE(atalk_afp_volume_add("Share A", "/share/a", err, sizeof err) >= 0);
    ASSERT_TRUE(atalk_afp_volume_add("Share B", "/share/b", err, sizeof err) >= 0);
    int slot_b = atalk_afp_volume_find("Share B");
    unsigned id_b = atalk_afp_volume_vol_id(slot_b);
    ASSERT_EQ_INT(0, atalk_afp_set_name("Renamed Server", err, sizeof err));
    atalk_printer_capture_set(true);
    link_checkpoint();
    link_delete(); // empties the volume table, as a machine teardown does
    ASSERT_EQ_INT(-1, atalk_afp_volume_find("Share B"));
    atalk_afp_set_name("Something Else", err, sizeof err);
    atalk_printer_capture_set(false);

    link_boot_from_checkpoint(false);
    slot_b = atalk_afp_volume_find("Share B");
    ASSERT_TRUE(slot_b >= 0);
    ASSERT_EQ_INT((int)id_b, (int)atalk_afp_volume_vol_id(slot_b)); // the guest's cached id still names it
    ASSERT_TRUE(atalk_afp_volume_find("Share A") >= 0);
    ASSERT_TRUE(strcmp(atalk_afp_get_name(), "Renamed Server") == 0);
    ASSERT_TRUE(atalk_printer_capture_get());
    link_delete();
    atalk_printer_capture_set(false);
}

// A checkpoint load that fails after the new machine's stack came up: the
// stack was left bound to the new machine's SCC and scheduler, which the load
// then freed -- the next frame read freed memory (10-network N-06, found
// under Valgrind).  Now the stack is rebuilt for the machine that keeps
// running, with that machine's shares, not the checkpoint's.
TEST(a_failed_load_gives_the_stack_back_to_the_running_machine) {
    link_boot();
    char err[128];
    ASSERT_TRUE(atalk_afp_volume_add("Live Share", "/live", err, sizeof err) >= 0);
    link_checkpoint(); // a record with "Live Share" in it...
    ASSERT_EQ_INT(0, atalk_afp_volume_remove("Live Share", err, sizeof err));
    ASSERT_TRUE(atalk_afp_volume_add("Other Share", "/other", err, sizeof err) >= 0);
    // ...but the running machine now publishes "Other Share".

    link_load(true);
    ASSERT_TRUE(link_sink_on(0));
    ASSERT_TRUE(!link_sink_on(1));
    ASSERT_TRUE(atalk_afp_volume_find("Other Share") >= 0);
    ASSERT_EQ_INT(-1, atalk_afp_volume_find("Live Share"));
    // ...and it answers the guest on the machine that is still there.
    wire_clear();
    uint8_t enq[3] = {HOST_NODE, GUEST_NODE, LLAP_TYPE_ENQ};
    guest_frame(enq, sizeof enq);
    ASSERT_EQ_INT(1, wire_count_type(GUEST_NODE, LLAP_TYPE_ACK));
    link_delete();
}

// ...and a load that succeeds leaves the stack with the new machine: the old
// machine's delete does not dismantle it, and the record's shares are what
// the new machine publishes.
TEST(a_successful_load_moves_the_stack_to_the_new_machine) {
    link_boot();
    char err[128];
    ASSERT_TRUE(atalk_afp_volume_add("Saved Share", "/saved", err, sizeof err) >= 0);
    link_checkpoint();
    link_load(false);
    ASSERT_TRUE(link_sink_on(1));
    ASSERT_TRUE(!link_sink_on(0));
    ASSERT_TRUE(atalk_afp_volume_find("Saved Share") >= 0);
    link_delete();
}

// --- malformed input (10-network F-02, F-35) -------------------------------------

typedef struct {
    uint64_t malformed, unhandled, tx_dropped, ddp_in;
} counts_t;

static counts_t counts(void) {
    const atalk_stats_t *st = atalk_get_stats();
    return (counts_t){st->malformed, st->unhandled, st->tx_dropped, st->ddp_in};
}

// Feed one raw frame and check which counter it moved: exactly one, by one.
static void expect_drop(const uint8_t *frame, size_t len, int which /* 0 malformed, 1 unhandled */) {
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
    uint8_t ext[16] = {HOST_NODE, GUEST_NODE, 0x02};
    expect_drop(ext, sizeof ext, 1); // extended DDP
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
    ASSERT_EQ_INT(0, atalk_nbp_register(&desc, &entry));
    uint8_t lkup[] = {0x21, 0x42, 0, 0, GUEST_NODE, 253, 0, 1, '=', 8, 'L', 'i', 'n', 'k', 'T', 'e', 's', 't', 1, '*'};
    b = counts();
    guest_ddp(GUEST_NODE, 2, 253, 2, lkup, sizeof lkup);
    guest_idle_until(link_now_ns() + 100e6);
    ASSERT_EQ_INT(1, (int)(counts().tx_dropped - b.tx_dropped));
    ASSERT_EQ_INT(0, atalk_nbp_unregister(entry));
    link_delete();
}

// --- NBP with eight tuples (10-network F-07) ----------------------------------

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
    atalk_nbp_entry_t *entries[8];
    char names[8][8];
    for (int i = 0; i < 8; i++) {
        snprintf(names[i], sizeof names[i], "Host %d", i);
        atalk_nbp_service_desc_t desc = {.object = names[i], .type = "Eight", .socket = (unsigned)(200 + i)};
        ASSERT_EQ_INT(0, atalk_nbp_register(&desc, &entries[i]));
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
        ASSERT_EQ_INT(0, atalk_nbp_unregister(entries[i]));
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

// AEP (Inside AppleTalk ch. 6): the Echoer on socket 4 turns an Echo Request
// (function 1) round as an Echo Reply (function 2), data unchanged; anything
// else is not for it (10-network N-35: it echoed everything, unchanged).
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

int main(void) {
    RUN(boot_installs_the_frame_sink_and_delete_removes_it);
    RUN(enq_for_our_node_is_acked_and_others_are_not);
    RUN(nbp_lookup_is_answered_after_the_rts_cts_handshake);
    RUN(every_transport_timer_is_registered_at_init);
    RUN(a_rebuilt_stack_serves_a_write_the_old_one_left_pending);
    RUN(detaching_the_stack_stops_its_transmitter);
    RUN(the_checkpoint_record_is_restored);
    RUN(a_record_from_a_failed_checkpoint_is_not_applied);
    RUN(configuration_survives_a_checkpoint);
    RUN(a_failed_load_gives_the_stack_back_to_the_running_machine);
    RUN(a_successful_load_moves_the_stack_to_the_new_machine);
    RUN(a_malformed_ddp_frame_is_dropped_and_counted);
    RUN(every_discard_is_counted_by_reason);
    RUN(a_lookup_matching_eight_names_answers_with_eight_tuples);
    RUN(a_lookup_reply_with_eight_tuples_delivers_all_eight);
    RUN(the_echoer_answers_requests_on_socket_4_with_a_reply);
    printf("[PASS] All atalk_link tests passed\n");
    return 0;
}
