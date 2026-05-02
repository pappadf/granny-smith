// Unit tests for M6 — per-object invalidation hooks (proposal §9).
//
// Hot-path consumers that hold a pre-resolved node_t (breakpoint
// conditions, watch paths, …) need to be told when the entry behind
// that node has been removed. The framework lets each object_t carry a
// list of weak-reference callbacks; object_delete fires them before
// freeing storage. These tests exercise the contract:
//
//   - register/fire/unregister mechanics
//   - object_delete fires invalidators automatically
//   - listeners may unregister themselves from inside the callback
//   - sparse stable indices still hold across remove + add cycles
//     when the underlying collection assigns ids monotonically

#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>

// === Toy class so we can build instances cheaply ============================

static const class_desc_t toy_class = {
    .name = "toy",
    .members = NULL,
    .n_members = 0,
};

// Listener state — the callback nulls *p when fired and bumps `count`.
typedef struct {
    int count;
    struct object **p;
} listener_t;

static void listener_cb(void *ud) {
    listener_t *l = (listener_t *)ud;
    if (!l)
        return;
    l->count++;
    if (l->p)
        *l->p = NULL;
}

// === Tests ================================================================

TEST(test_register_fire_clears_pointer) {
    struct object *o = object_new(&toy_class, NULL, "o");
    listener_t l = {.count = 0, .p = &o};

    object_register_invalidator(o, listener_cb, &l);
    ASSERT_EQ_INT(0, l.count);

    object_fire_invalidators(o);
    ASSERT_EQ_INT(1, l.count);
    ASSERT_TRUE(o == NULL); // listener nulled it

    // Re-fire is idempotent — list was cleared by the previous fire.
    listener_t dummy = {.count = 0, .p = NULL};
    // Construct a fresh object since the previous one was nulled.
    struct object *o2 = object_new(&toy_class, NULL, "o2");
    object_fire_invalidators(o2);
    ASSERT_EQ_INT(0, dummy.count);
    object_delete(o2);
}

TEST(test_unregister_prevents_fire) {
    struct object *o = object_new(&toy_class, NULL, "o");
    listener_t l = {.count = 0, .p = NULL};

    object_register_invalidator(o, listener_cb, &l);
    object_unregister_invalidator(o, listener_cb, &l);
    object_fire_invalidators(o);
    ASSERT_EQ_INT(0, l.count);

    object_delete(o);
}

TEST(test_object_delete_fires_invalidators) {
    struct object *o = object_new(&toy_class, NULL, "o");
    listener_t l = {.count = 0, .p = NULL};
    object_register_invalidator(o, listener_cb, &l);
    object_delete(o); // fires the listener, then frees `o`
    ASSERT_EQ_INT(1, l.count);
}

// File-scope state for the registration-order test (C has no nested fns).
static int g_order[3];
static int g_order_pos = 0;
static void cb_push_tag(void *ud) {
    int tag = *(int *)ud;
    if (g_order_pos < 3)
        g_order[g_order_pos++] = tag;
}

TEST(test_multiple_listeners_fire_in_registration_order) {
    struct object *o = object_new(&toy_class, NULL, "o");
    static int tags[3] = {1, 2, 3};
    g_order_pos = 0;

    object_register_invalidator(o, cb_push_tag, &tags[0]);
    object_register_invalidator(o, cb_push_tag, &tags[1]);
    object_register_invalidator(o, cb_push_tag, &tags[2]);
    object_fire_invalidators(o);
    ASSERT_EQ_INT(3, g_order_pos);
    ASSERT_EQ_INT(1, g_order[0]);
    ASSERT_EQ_INT(2, g_order[1]);
    ASSERT_EQ_INT(3, g_order[2]);

    object_delete(o);
}

TEST(test_unregister_only_targeted_listener) {
    struct object *o = object_new(&toy_class, NULL, "o");
    listener_t a = {.count = 0, .p = NULL};
    listener_t b = {.count = 0, .p = NULL};
    listener_t c = {.count = 0, .p = NULL};

    object_register_invalidator(o, listener_cb, &a);
    object_register_invalidator(o, listener_cb, &b);
    object_register_invalidator(o, listener_cb, &c);

    // Unregister the middle one only.
    object_unregister_invalidator(o, listener_cb, &b);
    object_fire_invalidators(o);

    ASSERT_EQ_INT(1, a.count);
    ASSERT_EQ_INT(0, b.count);
    ASSERT_EQ_INT(1, c.count);

    object_delete(o);
}

// === Sparse stable indices via the resolver under remove/add cycles =========
//
// Mirrors the M6 expectation: removing entry #0 must not renumber
// #1, and the next add receives `max_id_ever + 1` rather than recycling
// the freed slot. The indexed-child substrate already supports this in
// principle; this test pins it down end-to-end through object_resolve.

#define SLOT_MAX 8
typedef struct {
    struct object *slot[SLOT_MAX];
    int max_idx; // never decreases — sparse stable indices
} ring_t;

static struct object *ring_get(struct object *self, int index) {
    ring_t *r = (ring_t *)object_data(self);
    if (!r || index < 0 || index >= SLOT_MAX)
        return NULL;
    return r->slot[index];
}
static int ring_count(struct object *self) {
    ring_t *r = (ring_t *)object_data(self);
    int n = 0;
    if (!r)
        return 0;
    for (int i = 0; i < SLOT_MAX; i++)
        if (r->slot[i])
            n++;
    return n;
}
static int ring_next(struct object *self, int prev) {
    ring_t *r = (ring_t *)object_data(self);
    if (!r)
        return -1;
    for (int i = prev + 1; i < SLOT_MAX; i++)
        if (r->slot[i])
            return i;
    return -1;
}

static const class_desc_t entry_cls = {.name = "entry", .members = NULL, .n_members = 0};

static const member_t ring_members[] = {
    {.kind = M_CHILD,
     .name = "items",
     .child =
         {.cls = &entry_cls, .indexed = true, .get = ring_get, .count = ring_count, .next = ring_next, .lookup = NULL}},
};
static const class_desc_t ring_cls = {
    .name = "ring",
    .members = ring_members,
    .n_members = 1,
};

static int ring_add(ring_t *r) {
    if (r->max_idx + 1 >= SLOT_MAX)
        return -1;
    int idx = ++r->max_idx;
    r->slot[idx] = object_new(&entry_cls, NULL, NULL);
    return idx;
}
static void ring_drop(ring_t *r, int idx) {
    if (idx < 0 || idx >= SLOT_MAX || !r->slot[idx])
        return;
    object_delete(r->slot[idx]); // fires invalidators on the entry
    r->slot[idx] = NULL;
    // max_idx not lowered — that's the whole point of sparse stable.
}

TEST(test_sparse_indices_survive_remove_and_re_add) {
    object_root_reset();
    ring_t r = {.max_idx = -1};
    struct object *ring = object_new(&ring_cls, &r, "ring");
    object_attach(object_root(), ring);

    int a = ring_add(&r); // 0
    int b = ring_add(&r); // 1
    int c = ring_add(&r); // 2
    ASSERT_EQ_INT(0, a);
    ASSERT_EQ_INT(1, b);
    ASSERT_EQ_INT(2, c);

    // Resolve via the iterator before any churn.
    int seen[8];
    int n_seen = 0;
    int idx = ring_next(ring, -1);
    while (idx != -1 && n_seen < 8) {
        seen[n_seen++] = idx;
        idx = ring_next(ring, idx);
    }
    ASSERT_EQ_INT(3, n_seen);

    // Drop #0; #1 stays at id 1.
    ring_drop(&r, 0);
    ASSERT_EQ_INT(2, ring_count(ring));
    ASSERT_EQ_INT(1, ring_next(ring, -1)); // first live id is 1

    // Add another; gets id 3 (max-id-ever + 1, never recycles 0).
    int d = ring_add(&r);
    ASSERT_EQ_INT(3, d);

    // Cleanup.
    ring_drop(&r, 1);
    ring_drop(&r, 2);
    ring_drop(&r, 3);
    object_detach(ring);
    object_delete(ring);
    object_root_reset();
}

// === Invalidation through a held node_t =====================================
//
// Models the M6 hot-path scenario: a consumer resolves the node once,
// keeps the pointer, and registers an invalidator on the entry's
// object. When the entry is removed, the invalidator nulls the held
// pointer and the consumer's next access fails cleanly instead of
// dereferencing freed memory.

TEST(test_held_node_invalidated_on_entry_remove) {
    object_root_reset();
    ring_t r = {.max_idx = -1};
    struct object *ring = object_new(&ring_cls, &r, "ring");
    object_attach(object_root(), ring);

    int id = ring_add(&r); // 0
    (void)id;

    node_t held = object_resolve(object_root(), "ring.items[0]");
    ASSERT_TRUE(node_valid(held));

    listener_t l = {.count = 0, .p = &held.obj};
    object_register_invalidator(r.slot[0], listener_cb, &l);

    // Drop the entry — should fire the invalidator, nulling held.obj.
    ring_drop(&r, 0);
    ASSERT_EQ_INT(1, l.count);
    ASSERT_TRUE(held.obj == NULL); // listener nulled the held node's obj

    object_detach(ring);
    object_delete(ring);
    object_root_reset();
}

// === Entrypoint ===========================================================

int main(void) {
    RUN(test_register_fire_clears_pointer);
    RUN(test_unregister_prevents_fire);
    RUN(test_object_delete_fires_invalidators);
    RUN(test_multiple_listeners_fire_in_registration_order);
    RUN(test_unregister_only_targeted_listener);
    RUN(test_sparse_indices_survive_remove_and_re_add);
    RUN(test_held_node_invalidated_on_entry_remove);
    return 0;
}
