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
    printf("[PASS] All atalk_link tests passed\n");
    return 0;
}
