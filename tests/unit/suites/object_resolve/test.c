// Unit tests for object_resolve and the indexed-child contract.
//
// Covers (per M2 plan):
//   - named child resolution
//   - indexed children with sparse stable indices (proposal §2.1)
//   - the next() iterator skipping holes
//   - reserved-word rejection at registration

#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>

// === Toy class A: named-attributes only ====================================

static int g_a_pc_value = 0;
static value_t a_get_pc(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, (uint64_t)g_a_pc_value);
}

static const member_t a_members[] = {
    {.kind = M_ATTR, .name = "pc", .flags = VAL_RO, .attr = {.type = V_UINT, .get = a_get_pc, .set = NULL}},
};
static const class_desc_t a_class = {
    .name = "a",
    .members = a_members,
    .n_members = 1,
};

// === Toy class B: indexed child collection ================================
//
// Holds up to 8 entries, each "device" is a tiny object_t. Removed
// slots leave NULL holes; max_idx tracks the highest index ever
// allocated so add() returns max_idx+1 (sparse stable indices).

#define DEV_MAX 8
typedef struct {
    struct object *slot[DEV_MAX]; // NULL → hole
    int max_idx; // largest index ever allocated; -1 if none
} bucket_t;

static struct object *bucket_get(struct object *self, int index) {
    bucket_t *b = (bucket_t *)object_data(self);
    if (!b || index < 0 || index >= DEV_MAX)
        return NULL;
    return b->slot[index];
}

static int bucket_count(struct object *self) {
    bucket_t *b = (bucket_t *)object_data(self);
    int n = 0;
    if (!b)
        return 0;
    for (int i = 0; i < DEV_MAX; i++)
        if (b->slot[i])
            n++;
    return n;
}

static int bucket_next(struct object *self, int prev_index) {
    bucket_t *b = (bucket_t *)object_data(self);
    if (!b)
        return -1;
    for (int i = prev_index + 1; i < DEV_MAX; i++)
        if (b->slot[i])
            return i;
    return -1;
}

// Devices have one read-only attribute "id" so resolution can descend.
typedef struct {
    int id;
} device_t;
static value_t dev_get_id(struct object *self, const member_t *m) {
    (void)m;
    device_t *d = (device_t *)object_data(self);
    return val_int(d ? d->id : -1);
}
static const member_t dev_members[] = {
    {.kind = M_ATTR, .name = "id", .flags = VAL_RO, .attr = {.type = V_INT, .get = dev_get_id, .set = NULL}},
};
static const class_desc_t dev_class = {
    .name = "device",
    .members = dev_members,
    .n_members = 1,
};

// Bucket exposes one indexed child member named "devices".
static const member_t bucket_members[] = {
    {.kind = M_CHILD,
     .name = "devices",
     .flags = 0,
     .child = {.cls = &dev_class,
               .indexed = true,
               .get = bucket_get,
               .count = bucket_count,
               .next = bucket_next,
               .lookup = NULL}},
};
static const class_desc_t bucket_class = {
    .name = "bucket",
    .members = bucket_members,
    .n_members = 1,
};

static int bucket_add(bucket_t *b, int *id) {
    if (b->max_idx + 1 >= DEV_MAX)
        return -1;
    int idx = ++b->max_idx;
    device_t *d = (device_t *)calloc(1, sizeof(device_t));
    d->id = *id;
    b->slot[idx] = object_new(&dev_class, d, NULL);
    return idx;
}
static void bucket_remove(bucket_t *b, int idx) {
    if (idx < 0 || idx >= DEV_MAX || !b->slot[idx])
        return;
    free(object_data(b->slot[idx]));
    object_delete(b->slot[idx]);
    b->slot[idx] = NULL;
    // max_idx is NOT lowered — sparse stable indices.
}

// =========================================================================

TEST(test_named_child_resolves) {
    object_root_reset();
    struct object *a = object_new(&a_class, NULL, "a");
    object_attach(object_root(), a);

    g_a_pc_value = 0x1234;
    node_t n = object_resolve(object_root(), "a.pc");
    ASSERT_TRUE(node_valid(n));
    value_t v = node_get(n);
    ASSERT_EQ_INT(V_UINT, v.kind);
    ASSERT_EQ_INT(0x1234, (int)v.u);
    value_free(&v);

    object_detach(a);
    object_delete(a);
    object_root_reset();
}

TEST(test_indexed_children_sparse_stable) {
    object_root_reset();
    bucket_t b = {.max_idx = -1};
    struct object *bucket = object_new(&bucket_class, &b, "bucket");
    object_attach(object_root(), bucket);

    int ids[3] = {100, 200, 300};
    int i0 = bucket_add(&b, &ids[0]); // 0
    int i1 = bucket_add(&b, &ids[1]); // 1
    int i2 = bucket_add(&b, &ids[2]); // 2
    ASSERT_EQ_INT(0, i0);
    ASSERT_EQ_INT(1, i1);
    ASSERT_EQ_INT(2, i2);
    ASSERT_EQ_INT(3, bucket_count(bucket));

    // Remove the middle one — leaves a hole at index 1.
    bucket_remove(&b, 1);
    ASSERT_EQ_INT(2, bucket_count(bucket));
    ASSERT_TRUE(bucket_get(bucket, 1) == NULL);

    // Add a new device — must get index 3, NOT recycle 1.
    int id_new = 400;
    int i3 = bucket_add(&b, &id_new);
    ASSERT_EQ_INT(3, i3);
    ASSERT_TRUE(bucket_get(bucket, 1) == NULL); // still a hole

    // Resolve through the indexed segment using `[N]` form.
    node_t n0 = object_resolve(object_root(), "bucket.devices[0].id");
    ASSERT_TRUE(node_valid(n0));
    value_t v = node_get(n0);
    ASSERT_EQ_INT(100, (int)v.i);
    value_free(&v);

    // Hole at index 1: resolution succeeds at the indexed-child node
    // (it has a member descriptor with an index), but reading the
    // value yields V_ERROR via node_get because get(1) returns NULL.
    node_t n1 = object_resolve(object_root(), "bucket.devices[1]");
    ASSERT_TRUE(node_valid(n1));
    value_t v1 = node_get(n1);
    ASSERT_TRUE(val_is_error(&v1));
    value_free(&v1);

    // [3].id resolves and reads cleanly.
    node_t n3 = object_resolve(object_root(), "bucket.devices[3].id");
    ASSERT_TRUE(node_valid(n3));
    value_t v3 = node_get(n3);
    ASSERT_EQ_INT(400, (int)v3.i);
    value_free(&v3);

    // Cleanup
    bucket_remove(&b, 0);
    bucket_remove(&b, 2);
    bucket_remove(&b, 3);
    object_detach(bucket);
    object_delete(bucket);
    object_root_reset();
}

TEST(test_indexed_next_skips_holes) {
    object_root_reset();
    bucket_t b = {.max_idx = -1};
    struct object *bucket = object_new(&bucket_class, &b, "bucket");
    object_attach(object_root(), bucket);

    int ids[5] = {1, 2, 3, 4, 5};
    for (int i = 0; i < 5; i++)
        bucket_add(&b, &ids[i]); // indices 0..4

    // Remove 1 and 3 — live indices: 0, 2, 4
    bucket_remove(&b, 1);
    bucket_remove(&b, 3);

    // Walk via the next() iterator (the contract documented in object.h).
    int seen[8];
    int n_seen = 0;
    int idx = bucket_next(bucket, -1);
    while (idx != -1 && n_seen < 8) {
        seen[n_seen++] = idx;
        idx = bucket_next(bucket, idx);
    }
    ASSERT_EQ_INT(3, n_seen);
    ASSERT_EQ_INT(0, seen[0]);
    ASSERT_EQ_INT(2, seen[1]);
    ASSERT_EQ_INT(4, seen[2]);

    bucket_remove(&b, 0);
    bucket_remove(&b, 2);
    bucket_remove(&b, 4);
    object_detach(bucket);
    object_delete(bucket);
    object_root_reset();
}

// === Reserved-word rejection ===============================================

// Mock class with a reserved-word member name. object_validate_class
// must reject it. Note: registration entry points should always
// validate before attaching (root.c does this; M3 alias.add
// will too).
static const member_t bad_members[] = {
    {.kind = M_ATTR, .name = "while", .flags = VAL_RO, .attr = {.type = V_UINT, .get = a_get_pc, .set = NULL}},
};
static const class_desc_t bad_class = {
    .name = "bad",
    .members = bad_members,
    .n_members = 1,
};

static const member_t dup_members[] = {
    {.kind = M_ATTR, .name = "x", .flags = VAL_RO, .attr = {.type = V_UINT, .get = a_get_pc, .set = NULL}},
    {.kind = M_ATTR, .name = "x", .flags = VAL_RO, .attr = {.type = V_UINT, .get = a_get_pc, .set = NULL}},
};
static const class_desc_t dup_class = {
    .name = "dup",
    .members = dup_members,
    .n_members = 2,
};

TEST(test_reserved_word_in_class_rejected) {
    char err[200];
    ASSERT_TRUE(!object_validate_class(&bad_class, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "reserved") != NULL);
}

TEST(test_duplicate_member_rejected) {
    char err[200];
    ASSERT_TRUE(!object_validate_class(&dup_class, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "duplicate") != NULL);
}

TEST(test_well_formed_class_accepted) {
    char err[200];
    ASSERT_TRUE(object_validate_class(&a_class, err, sizeof(err)));
}

// === Path edge cases ======================================================

TEST(test_empty_path_resolves_to_root) {
    object_root_reset();
    node_t n = object_resolve(object_root(), "");
    ASSERT_TRUE(node_valid(n));
    ASSERT_TRUE(n.obj == object_root());
    object_root_reset();
}

TEST(test_unknown_segment_fails) {
    object_root_reset();
    node_t n = object_resolve(object_root(), "ghost");
    ASSERT_TRUE(!node_valid(n));
    object_root_reset();
}

int main(void) {
    RUN(test_named_child_resolves);
    RUN(test_indexed_children_sparse_stable);
    RUN(test_indexed_next_skips_holes);
    RUN(test_reserved_word_in_class_rejected);
    RUN(test_duplicate_member_rejected);
    RUN(test_well_formed_class_accepted);
    RUN(test_empty_path_resolves_to_root);
    RUN(test_unknown_segment_fails);
    return 0;
}
