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
#include "value_format.h"

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
    {.kind = MK_ATTR, .name = "pc", .doc = "pc", .attr = {.type = VK_UINT, .get = a_get_pc, .set = a_set_pc}},
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
    {.kind = MK_ATTR, .name = "id", .doc = "id", .attr = {.type = VK_INT, .get = dev_get_id}},
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

static const collection_desc_t bucket_entries = {
    .entry = &dev_class, .by_index = {.get = bucket_get, .next = bucket_next}
};

static const member_t bucket_members[] = {
    {.kind = MK_CHILD, .name = "devices", .child = {.collection = &bucket_entries}},
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

// The result limit is the buffer less its NUL: a result of exactly that
// many bytes is the result; one byte more is an error naming both sizes,
// never a truncated document.
TEST(test_result_limit_is_exact_and_explicit) {
    struct object *a, *b;
    fixture_up(&a, &b);
    g_pc = 0x2222; // "8738": four bytes
    char small[5];
    ASSERT_EQ_INT(0, gs_eval("a.pc", NULL, small, sizeof(small)));
    ASSERT_TRUE(strcmp(small, "8738") == 0);
    ASSERT_EQ_INT(-1, gs_eval("a.pc", NULL, small, sizeof(small) - 1));
    g_pc = 100000; // "100000": six bytes
    ASSERT_EQ_INT(-1, gs_eval("a.pc", NULL, out, 6));
    char big[256];
    ASSERT_EQ_INT(0, gs_eval("a.pc", NULL, big, sizeof(big)));
    ASSERT_TRUE(strcmp(big, "100000") == 0);
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
// refused, never run with its trailing arguments silently dropped.
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

// Out-of-range JSON numbers and a backslash at the end of a string are
// refused, not saturated or read past.
TEST(test_out_of_range_args_are_refused) {
    struct object *a, *b;
    fixture_up(&a, &b);
    const char *bad[] = {"[99999999999999999999]", "[1e999]", "[\"abc\\"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        g_pc = 0;
        ASSERT_EQ_INT(-1, gs_eval("a.pc", bad[i], out, sizeof(out)));
        ASSERT_EQ_INT(0, (int)g_pc);
    }
    fixture_down(a, b);
}

// A hex-flagged VK_INT renders its bit pattern at its width in text, but stays
// a bare JSON number (a hex VK_UINT is a "0x…" string); strings escape
// control characters.
TEST(test_value_format_hex_int_and_escapes) {
    char buf[64];
    value_t v = val_int(-1);
    v.flags |= VFLAG_HEX;
    v.width = 4;
    value_format_into(&v, VFMT_TEXT, buf, sizeof(buf));
    ASSERT_TRUE(strcmp(buf, "0xffffffff") == 0);
    value_format_into(&v, VFMT_JSON_TAGGED, buf, sizeof(buf));
    ASSERT_TRUE(strcmp(buf, "-1") == 0);
    value_format_into(&v, VFMT_JSON, buf, sizeof(buf));
    ASSERT_TRUE(strcmp(buf, "-1") == 0);
    value_t slot = val_int(0xe); // a NuBus slot number reaches JSON as 14
    slot.flags |= VFLAG_HEX;
    value_format_into(&slot, VFMT_JSON_TAGGED, buf, sizeof(buf));
    ASSERT_TRUE(strcmp(buf, "14") == 0);
    value_format_into(&slot, VFMT_TEXT, buf, sizeof(buf));
    ASSERT_TRUE(strcmp(buf, "0xe") == 0);
    value_t u = val_uint(4, 0x1f); // a hex VK_UINT is still a JSON string
    u.flags |= VFLAG_HEX;
    value_format_into(&u, VFMT_JSON_TAGGED, buf, sizeof(buf));
    ASSERT_TRUE(strcmp(buf, "\"0x1f\"") == 0);
    value_t s = val_str("a\"b\\c\n\x01"
                        "d");
    value_format_into(&s, VFMT_JSON, buf, sizeof(buf));
    ASSERT_TRUE(strcmp(buf, "\"a\\\"b\\\\c\\n\\u0001d\"") == 0);
    value_free(&s);
}

int main(void) {
    RUN(test_shell_assignment_is_not_a_path);
    RUN(test_typed_setter_writes);
    RUN(test_result_limit_is_exact_and_explicit);
    RUN(test_count_is_on_the_owner);
    RUN(test_enumerate_with_meta_indices);
    RUN(test_truncated_args_are_refused);
    RUN(test_wellformed_args_still_parse);
    RUN(test_out_of_range_args_are_refused);
    RUN(test_value_format_hex_int_and_escapes);
    return 0;
}
