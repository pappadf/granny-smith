// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for gs_eval — the one JS → C entry point the browser frontend
// reaches the object model through.  Nothing else exercises the path strings
// and argument documents the frontend actually sends, so a frontend that
// sends a path the core never resolves (a shell statement, a count on the
// wrong node) gets `{error}` back and, if it does not check, reports success.
// These tests pin the core side of that contract.

#include "api.h"
#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>

// === Toy class "a": one read/write attribute =============================

static uint32_t g_pc = 0;

// Read `a.pc`.
static value_t a_get_pc(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, g_pc);
}

// Write `a.pc`.
static value_t a_set_pc(struct object *self, const member_t *m, value_t v) {
    (void)self;
    (void)m;
    bool ok = false;
    g_pc = (uint32_t)val_as_u64(&v, &ok);
    value_free(&v);
    return val_none();
}

static const member_t a_members[] = {
    {.kind = M_ATTR, .name = "pc", .doc = "pc", .attr = {.type = V_UINT, .get = a_get_pc, .set = a_set_pc}},
};
static const class_desc_t a_class = {.name = "a", .members = a_members, .n_members = 1};

// === Toy class "bucket": a sparse indexed collection =====================

#define DEV_MAX 4
// A bucket of up to DEV_MAX devices; a removed slot leaves a hole.
typedef struct {
    struct object *slot[DEV_MAX];
} bucket_t;

// A device exposes one read-only id.
typedef struct {
    int id;
} device_t;

// Read `device.id`.
static value_t dev_get_id(struct object *self, const member_t *m) {
    (void)m;
    device_t *d = (device_t *)object_data(self);
    return val_int(d ? d->id : -1);
}

static const member_t dev_members[] = {
    {.kind = M_ATTR, .name = "id", .flags = VAL_RO, .doc = "id", .attr = {.type = V_INT, .get = dev_get_id}},
};
static const class_desc_t dev_class = {.name = "device", .members = dev_members, .n_members = 1};

// Indexed-child getter: the device at `index`, or NULL for a hole.
static struct object *bucket_get(struct object *self, int index) {
    bucket_t *b = (bucket_t *)object_data(self);
    return (b && index >= 0 && index < DEV_MAX) ? b->slot[index] : NULL;
}

// Indexed-child iterator: the next live index after `prev`, or -1.
static int bucket_next(struct object *self, int prev) {
    bucket_t *b = (bucket_t *)object_data(self);
    for (int i = prev + 1; b && i < DEV_MAX; i++)
        if (b->slot[i])
            return i;
    return -1;
}

static const member_t bucket_members[] = {
    {.kind = M_CHILD,
     .name = "devices",
     .child = {.cls = &dev_class, .indexed = true, .get = bucket_get, .next = bucket_next}},
};
static const class_desc_t bucket_class = {.name = "bucket", .members = bucket_members, .n_members = 1};

// === Fixture ==============================================================

static bucket_t g_bucket;
static device_t g_dev[DEV_MAX];

// Attach "a" and a bucket holding devices 0 and 1 under a fresh root.
static void fixture_up(struct object **a, struct object **bucket) {
    object_root_reset();
    memset(&g_bucket, 0, sizeof(g_bucket));
    *a = object_new(&a_class, NULL, "a");
    object_attach(object_root(), *a);
    *bucket = object_new(&bucket_class, &g_bucket, "bucket");
    object_attach(object_root(), *bucket);
    for (int i = 0; i < 2; i++) {
        g_dev[i].id = 10 + i;
        g_bucket.slot[i] = object_new(&dev_class, &g_dev[i], NULL);
    }
}

// Tear the fixture down.
static void fixture_down(struct object *a, struct object *bucket) {
    for (int i = 0; i < DEV_MAX; i++)
        if (g_bucket.slot[i])
            object_delete(g_bucket.slot[i]);
    object_detach(bucket);
    object_delete(bucket);
    object_detach(a);
    object_delete(a);
    object_root_reset();
}

static char out[4096];

// === Tests ================================================================

// A shell assignment is not a gs_eval path: `object_resolve` stops at the
// space, so the whole string is refused.  web2's register editor sent exactly
// this and, not checking for `{error}`, reported every edit as a success.
TEST(test_shell_assignment_is_not_a_path) {
    struct object *a, *b;
    fixture_up(&a, &b);
    g_pc = 0x1111;
    ASSERT_EQ_INT(-1, gs_eval("a.pc = 0x2222", NULL, out, sizeof(out)));
    ASSERT_TRUE(strstr(out, "\"error\"") != NULL);
    ASSERT_TRUE(strstr(out, "did not resolve") != NULL);
    ASSERT_EQ_INT(0x1111, (int)g_pc); // nothing was written
    fixture_down(a, b);
}

// The typed setter: an attribute path plus exactly one argument writes it.
TEST(test_typed_setter_writes) {
    struct object *a, *b;
    fixture_up(&a, &b);
    g_pc = 0;
    ASSERT_EQ_INT(0, gs_eval("a.pc", "[8738]", out, sizeof(out)));
    ASSERT_EQ_INT(0x2222, (int)g_pc);
    ASSERT_EQ_INT(0, gs_eval("a.pc", NULL, out, sizeof(out)));
    ASSERT_TRUE(strstr(out, "8738") != NULL);
    fixture_down(a, b);
}

// The synthetic `count` hangs off the collection's owner, not off the
// indexed member: `bucket.count` resolves, `bucket.devices.count` does not.
// web2's breakpoint list read the second form and so never listed anything.
TEST(test_count_is_on_the_owner) {
    struct object *a, *b;
    fixture_up(&a, &b);
    ASSERT_EQ_INT(-1, gs_eval("bucket.devices.count", NULL, out, sizeof(out)));
    ASSERT_TRUE(strstr(out, "did not resolve") != NULL);
    ASSERT_EQ_INT(0, gs_eval("bucket.count", NULL, out, sizeof(out)));
    ASSERT_TRUE(strstr(out, "2") != NULL);
    fixture_down(a, b);
}

// Indices are stable and never reused, so `count` cannot drive an index walk:
// after removing entry 0 the count is 1 but `devices[0]` is a hole.  The
// enumeration surface is `meta.indices(<member>)`.
TEST(test_enumerate_with_meta_indices) {
    struct object *a, *b;
    fixture_up(&a, &b);
    object_delete(g_bucket.slot[0]);
    g_bucket.slot[0] = NULL;
    ASSERT_EQ_INT(0, gs_eval("bucket.count", NULL, out, sizeof(out)));
    ASSERT_TRUE(strstr(out, "1") != NULL);
    ASSERT_EQ_INT(-1, gs_eval("bucket.devices[0].id", NULL, out, sizeof(out)));
    ASSERT_EQ_INT(0, gs_eval("bucket.meta.indices", "[\"devices\"]", out, sizeof(out)));
    ASSERT_TRUE(strstr(out, "[1]") != NULL);
    ASSERT_EQ_INT(0, gs_eval("bucket.devices[1].id", NULL, out, sizeof(out)));
    ASSERT_TRUE(strstr(out, "11") != NULL);
    fixture_down(a, b);
}

// A request cut short in transit (the web bridge's fixed-size args buffer
// truncates at a code-point boundary, often right after a ',') must be
// refused, never run with its trailing arguments silently dropped (N-46).
TEST(test_truncated_args_are_refused) {
    struct object *a, *b;
    fixture_up(&a, &b);
    const char *bad[] = {
        "[8738,", // array cut after a comma
        "[", // bare opener
        "[8738", // no closing bracket
        "[8738]x", // garbage after the document
        "{\"v\":1,", // object cut after a comma
        "{", // bare opener
        "{\"v\":1} [2]", // a second document
        "[]x", // empty array, then garbage
        "{}x", // empty object, then garbage
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        g_pc = 0;
        ASSERT_EQ_INT(-1, gs_eval("a.pc", bad[i], out, sizeof(out)));
        ASSERT_TRUE(strstr(out, "args_json") != NULL);
        ASSERT_EQ_INT(0, (int)g_pc); // nothing was written
    }
    fixture_down(a, b);
}

// Well-formed documents, with surrounding whitespace, still parse.
TEST(test_wellformed_args_still_parse) {
    struct object *a, *b;
    fixture_up(&a, &b);
    ASSERT_EQ_INT(0, gs_eval("a.pc", " [ 8738 ] ", out, sizeof(out)));
    ASSERT_EQ_INT(0x2222, (int)g_pc);
    ASSERT_EQ_INT(0, gs_eval("bucket.meta.indices", "[\"devices\"]\n", out, sizeof(out)));
    ASSERT_EQ_INT(0, gs_eval("a.pc", "[]", out, sizeof(out))); // no args: a read
    fixture_down(a, b);
}

int main(void) {
    RUN(test_shell_assignment_is_not_a_path);
    RUN(test_typed_setter_writes);
    RUN(test_count_is_on_the_owner);
    RUN(test_enumerate_with_meta_indices);
    RUN(test_truncated_args_are_refused);
    RUN(test_wellformed_args_still_parse);
    return 0;
}
