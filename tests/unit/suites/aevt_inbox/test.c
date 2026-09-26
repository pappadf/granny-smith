// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The Apple-event endpoint's tables (docs/core/network/ppc_appleevents.md §8):
// the inbox a guest's events land in, and the event table a script's sends
// fill.  See Makefile.

#include "appletalk_aevt.h"
#include "expr.h"
#include "object.h"
#include "stub_ppc.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>

static void setup(void) {
    atalk_aevt_remove_objects();
    object_root_reset();
    atalk_aevt_init();
    atalk_aevt_install_objects(object_root());
    stub_ppc_reset();
}

static uint64_t u64_at(const char *path) {
    value_t v = node_get(object_resolve(object_root(), path));
    uint64_t n = val_as_u64(&v, NULL);
    value_free(&v);
    return n;
}

static value_t eval(const char *src) {
    expr_ctx_t ctx = {.root = object_root()};
    return expr_eval(src, &ctx);
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
// sees -- a statement producing one stops the script -- and try() turns it
// into the fallback a script that wants to carry on can test.
TEST(a_full_event_table_is_a_script_error) {
    setup();
    for (int i = 0; i < 256; i++) {
        value_t v = eval("aevt.send(\"nowhere\", \"aevt/oapp{}\")");
        ASSERT_TRUE(!val_is_error(&v));
        value_free(&v);
    }
    value_t full = eval("aevt.send(\"nowhere\", \"aevt/oapp{}\")");
    ASSERT_TRUE(val_is_error(&full));
    ASSERT_TRUE(strstr(val_as_str(&full), "limit 256") != NULL);
    value_free(&full);
    value_t fallback = eval("try(aevt.send(\"nowhere\", \"aevt/oapp{}\"), 7)");
    ASSERT_EQ_INT(7, (int)val_as_i64(&fallback, NULL));
    value_free(&fallback);
}

int main(void) {
    RUN(a_full_inbox_still_answers);
    RUN(clearing_the_inbox_makes_room);
    RUN(a_full_event_table_is_a_script_error);
    printf("aevt_inbox: all tests passed\n");
    return 0;
}
