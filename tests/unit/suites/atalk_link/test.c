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

int main(void) {
    RUN(boot_installs_the_frame_sink_and_delete_removes_it);
    RUN(enq_for_our_node_is_acked_and_others_are_not);
    RUN(nbp_lookup_is_answered_after_the_rts_cts_handshake);
    printf("[PASS] All atalk_link tests passed\n");
    return 0;
}
