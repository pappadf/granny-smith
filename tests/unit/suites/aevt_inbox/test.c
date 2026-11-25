// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The Apple-event endpoint's tables (docs/internals/core/network/ppc_appleevents.md §8):
// the inbox a guest's events land in, and the event table a script's sends
// fill.  See Makefile.

#include "appletalk_aevt.h"
#include "object.h"
#include "stub_ppc.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>

// The endpoint is the network's, set up once; each test plugs in a fresh
// connection, whose inbox, events and counters start empty.
static aevt_link_t *g_link;

static void setup(void) {
    static bool installed;
    if (!installed) {
        atalk_aevt_init();
        atalk_aevt_install_objects(object_root());
        installed = true;
    }
    atalk_aevt_plug(NULL);
    atalk_aevt_link_free(g_link);
    g_link = atalk_aevt_link_new();
    atalk_aevt_plug(g_link);
    stub_ppc_reset();
}

static uint64_t u64_at(const char *path) {
    value_t v = node_get(object_resolve(object_root(), path));
    uint64_t n = val_as_u64(&v, NULL);
    value_free(&v);
    return n;
}

// aevt.send(target, event), called as a script calls it.
static value_t send(const char *target, const char *event) {
    node_t n = object_resolve(object_root(), "aevt.send");
    value_t args[2] = {val_str(target), val_str(event)};
    value_t r = node_call(n, 2, args);
    value_free(&args[0]);
    value_free(&args[1]);
    return r;
}

// An event from the guest: misc/dosc with no parameters.
static void deliver(uint32_t return_id) {
    atalk_aevt_deliver(stub_ppc_session(), "guest", "misc", "dosc", return_id, false, NULL, 0);
}

// A full inbox still answers.  It returned before the reply, so the guest's
// AESend waited out its own timeout, and nothing counted the event.
// The reply goes to the session the event came in on, not to one looked up
// again by an id narrowed to 16 bits: this session's is 0x12345.
TEST(a_full_inbox_still_answers) {
    setup();
    for (uint32_t i = 0; i < 33; i++)
        deliver(100 + i);
    ASSERT_EQ_INT(32, (int)u64_at("aevt.inbox.count"));
    ASSERT_EQ_INT(33, (int)u64_at("aevt.stats.received"));
    ASSERT_EQ_INT(1, (int)u64_at("aevt.stats.dropped"));
    ASSERT_EQ_INT(33, stub_ppc_blocks); // every one answered
    ASSERT_EQ_INT(33, (int)u64_at("aevt.stats.auto_replies"));
    ASSERT_TRUE(stub_ppc_last_block_session == stub_ppc_session());
}

// inbox.clear() makes room: the next event is kept again.
TEST(clearing_the_inbox_makes_room) {
    setup();
    for (uint32_t i = 0; i < 32; i++)
        deliver(200 + i);
    value_t r = node_call(object_resolve(object_root(), "aevt.inbox.clear"), 0, NULL);
    ASSERT_TRUE(!val_is_error(&r));
    value_free(&r);
    ASSERT_EQ_INT(0, (int)u64_at("aevt.inbox.count"));
    deliver(300);
    ASSERT_EQ_INT(1, (int)u64_at("aevt.inbox.count"));
    ASSERT_EQ_INT(0, (int)u64_at("aevt.stats.dropped"));
}

// The event table holds 256 sends a run.  The next is an error the script
// sees (raised; gs.try turns it into a fallback).
TEST(a_full_event_table_is_a_script_error) {
    setup();
    for (int i = 0; i < 256; i++) {
        value_t v = send("nowhere", "aevt/oapp{}");
        ASSERT_TRUE(!val_is_error(&v));
        value_free(&v);
    }
    value_t full = send("nowhere", "aevt/oapp{}");
    ASSERT_TRUE(val_is_error(&full));
    ASSERT_TRUE(strstr(val_as_str(&full), "limit 256") != NULL);
    value_free(&full);
}

int main(void) {
    RUN(a_full_inbox_still_answers);
    RUN(clearing_the_inbox_makes_room);
    RUN(a_full_event_table_is_a_script_error);
    printf("aevt_inbox: all tests passed\n");
    return 0;
}
