// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Unit tests for value.{c,h} — the tagged-union value type used across
// every object-model boundary.

#include "test_assert.h"

#include "value.h"
#include <stdint.h>

#include <stdlib.h>
#include <string.h>

// Inline kinds: value_free is a no-op and safe to repeat.
TEST(test_inline_free_is_noop) {
    value_t v = val_uint(4, 0xDEADBEEF);
    value_free(&v);
    ASSERT_EQ_INT(V_NONE, v.kind);
    // Safe second call.
    value_free(&v);
    ASSERT_EQ_INT(V_NONE, v.kind);

    value_t b = val_bool(true);
    value_free(&b);
    value_free(&b);

    value_t i = val_int(-7);
    value_free(&i);

    value_t f = val_float(3.14);
    value_free(&f);

    value_t n = val_none();
    value_free(&n);

    value_t obj = val_obj(NULL);
    value_free(&obj);
}

// Strings: heap-allocated, freed by value_free, constructor strdups.
TEST(test_string_ownership) {
    const char *src = "hello";
    value_t v = val_str(src);
    ASSERT_EQ_INT(V_STRING, v.kind);
    ASSERT_TRUE(v.s != NULL);
    ASSERT_TRUE(v.s != src); // strdup'd, not borrowed
    ASSERT_TRUE(strcmp(v.s, src) == 0);
    value_free(&v);
    ASSERT_EQ_INT(V_NONE, v.kind);
    ASSERT_TRUE(v.s == NULL);
}

// Errors: heap-allocated, freed by value_free; printf-style formatting.
TEST(test_error_ownership) {
    value_t v = val_err("err code %d at %s", 42, "test");
    ASSERT_TRUE(val_is_error(&v));
    ASSERT_TRUE(strstr(v.err, "42") != NULL);
    ASSERT_TRUE(strstr(v.err, "test") != NULL);
    value_free(&v);
    ASSERT_EQ_INT(V_NONE, v.kind);
}

// Bytes: heap-allocated, freed by value_free.
TEST(test_bytes_ownership) {
    const uint8_t buf[] = {0xDE, 0xAD, 0xBE, 0xEF};
    value_t v = val_bytes(buf, sizeof(buf));
    ASSERT_EQ_INT(V_BYTES, v.kind);
    ASSERT_EQ_INT(4, (int)v.bytes.n);
    ASSERT_TRUE(memcmp(v.bytes.p, buf, 4) == 0);
    value_free(&v);
    ASSERT_TRUE(v.bytes.p == NULL);
    ASSERT_EQ_INT(0, (int)v.bytes.n);

    // Zero-length bytes are valid.
    value_t z = val_bytes(NULL, 0);
    ASSERT_EQ_INT(V_BYTES, z.kind);
    value_free(&z);
}

// Lists: recursive ownership — freeing a list frees its items.
TEST(test_list_recursive_free) {
    value_t *items = (value_t *)calloc(3, sizeof(value_t));
    items[0] = val_str("first");
    items[1] = val_str("second");
    items[2] = val_str("third");
    value_t list = val_list(items, 3);
    ASSERT_EQ_INT(V_LIST, list.kind);
    ASSERT_EQ_INT(3, (int)list.list.len);
    value_free(&list);
    ASSERT_TRUE(list.list.items == NULL);
    ASSERT_EQ_INT(0, (int)list.list.len);
}

// Nested lists: free recurses through every level.
TEST(test_nested_list_free) {
    value_t *inner = (value_t *)calloc(2, sizeof(value_t));
    inner[0] = val_str("inner-a");
    inner[1] = val_bytes("xyz", 3);
    value_t inner_list = val_list(inner, 2);

    value_t *outer = (value_t *)calloc(2, sizeof(value_t));
    outer[0] = val_str("outer-a");
    outer[1] = inner_list;
    value_t outer_list = val_list(outer, 2);

    value_free(&outer_list);
    ASSERT_EQ_INT(V_NONE, outer_list.kind);
}

// value_dup duplicates heap kinds.  (value_copy, the second deep-copier,
// is gone.)
TEST(test_value_dup_heap_kinds) {
    value_t s = val_str("original");
    value_t c = value_dup(&s);
    ASSERT_EQ_INT(V_STRING, c.kind);
    ASSERT_TRUE(c.s != s.s);
    ASSERT_TRUE(strcmp(c.s, s.s) == 0);
    value_free(&s);
    // c remains valid.
    ASSERT_TRUE(strcmp(c.s, "original") == 0);
    value_free(&c);

    value_t list_src = val_list((value_t *)calloc(1, sizeof(value_t)), 1);
    list_src.list.items[0] = val_str("inside");
    value_t list_copy = value_dup(&list_src);
    ASSERT_EQ_INT(1, (int)list_copy.list.len);
    ASSERT_TRUE(list_copy.list.items != list_src.list.items);
    value_free(&list_src);
    ASSERT_TRUE(strcmp(list_copy.list.items[0].s, "inside") == 0);
    value_free(&list_copy);
}

// Truthiness rules.
TEST(test_truthiness) {
    value_t t;

    t = val_bool(true);
    ASSERT_TRUE(val_as_bool(&t));
    value_free(&t);
    t = val_bool(false);
    ASSERT_TRUE(!val_as_bool(&t));
    value_free(&t);

    t = val_int(0);
    ASSERT_TRUE(!val_as_bool(&t));
    value_free(&t);
    t = val_int(-1);
    ASSERT_TRUE(val_as_bool(&t));
    value_free(&t);

    t = val_uint(4, 0);
    ASSERT_TRUE(!val_as_bool(&t));
    value_free(&t);
    t = val_uint(4, 1);
    ASSERT_TRUE(val_as_bool(&t));
    value_free(&t);

    t = val_float(0.0);
    ASSERT_TRUE(!val_as_bool(&t));
    value_free(&t);
    t = val_float(0.5);
    ASSERT_TRUE(val_as_bool(&t));
    value_free(&t);

    t = val_str("");
    ASSERT_TRUE(!val_as_bool(&t));
    value_free(&t);
    t = val_str("x");
    ASSERT_TRUE(val_as_bool(&t));
    value_free(&t);

    t = val_bytes(NULL, 0);
    ASSERT_TRUE(!val_as_bool(&t));
    value_free(&t);
    t = val_bytes("a", 1);
    ASSERT_TRUE(val_as_bool(&t));
    value_free(&t);

    t = val_none();
    ASSERT_TRUE(!val_as_bool(&t));
    value_free(&t);

    t = val_err("nope");
    ASSERT_TRUE(!val_as_bool(&t));
    value_free(&t);
}

// Maps: builder → V_MAP with insertion order, unique keys, heap-owned
// keys, recursively-owned values.
TEST(test_map_builder) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "one", val_int(1));
    val_map_put(b, "two", val_str("2"));
    val_map_put(b, "one", val_int(11)); // duplicate key replaces in place
    value_t m = val_map_finish(b);
    ASSERT_EQ_INT(V_MAP, m.kind);
    ASSERT_EQ_INT(2, (int)m.map.len);
    // Insertion order preserved; the replaced key kept its slot.
    ASSERT_TRUE(strcmp(m.map.entries[0].key, "one") == 0);
    ASSERT_TRUE(strcmp(m.map.entries[1].key, "two") == 0);
    const value_t *one = value_map_get(&m, "one");
    ASSERT_TRUE(one != NULL);
    ASSERT_EQ_INT(11, (int)one->i);
    ASSERT_TRUE(value_map_get(&m, "absent") == NULL);
    ASSERT_TRUE(val_as_bool(&m)); // non-empty map is truthy
    value_free(&m);
    ASSERT_EQ_INT(V_NONE, m.kind);
    ASSERT_TRUE(m.map.entries == NULL);

    // Empty map: valid, falsy.
    value_t e = val_map_finish(val_map_new());
    ASSERT_EQ_INT(V_MAP, e.kind);
    ASSERT_EQ_INT(0, (int)e.map.len);
    ASSERT_TRUE(!val_as_bool(&e));
    value_free(&e);
}

// Maps nest recursively (maps in lists in maps); free releases every level.
TEST(test_map_nested_free) {
    value_map_builder_t *inner = val_map_new();
    val_map_put(inner, "s", val_str("deep"));

    value_t *items = (value_t *)calloc(2, sizeof(value_t));
    items[0] = val_map_finish(inner);
    items[1] = val_str("mid");

    value_map_builder_t *outer = val_map_new();
    val_map_put(outer, "list", val_list(items, 2));
    val_map_put(outer, "top", val_int(7));
    value_t m = val_map_finish(outer);

    const value_t *lst = value_map_get(&m, "list");
    ASSERT_TRUE(lst && lst->kind == V_LIST && lst->list.len == 2);
    const value_t *deep = value_map_get(&lst->list.items[0], "s");
    ASSERT_TRUE(deep && strcmp(deep->s, "deep") == 0);

    value_free(&m); // recursive free of the whole tree (ASan-checked in CI)
    ASSERT_EQ_INT(V_NONE, m.kind);
}

// value_dup deep-copies maps: independent lifetimes.
TEST(test_map_copy) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "k", val_str("v"));
    value_t *items = (value_t *)calloc(1, sizeof(value_t));
    items[0] = val_int(3);
    val_map_put(b, "nums", val_list(items, 1));
    value_t src = val_map_finish(b);

    value_t dup = value_dup(&src);
    value_t cpy = value_dup(&src);
    value_free(&src);

    ASSERT_EQ_INT(V_MAP, dup.kind);
    ASSERT_TRUE(strcmp(value_map_get(&dup, "k")->s, "v") == 0);
    ASSERT_EQ_INT(3, (int)value_map_get(&dup, "nums")->list.items[0].i);
    ASSERT_EQ_INT(V_MAP, cpy.kind);
    ASSERT_TRUE(strcmp(value_map_get(&cpy, "k")->s, "v") == 0);
    value_free(&dup);
    value_free(&cpy);
}

// Cleanup attribute (VALUE_AUTO) frees on scope exit.
TEST(test_value_auto_cleanup) {
    bool ran = true;
    {
        VALUE_AUTO v = val_str("scoped");
        (void)v;
        ran = (v.kind == V_STRING);
    }
    ASSERT_TRUE(ran);
    // No leak — confirmed by valgrind in CI; here we just verify the
    // attribute is accepted by the compiler.
}

// === One boolean vocabulary ================================================
//
// There were two coercion tables that disagreed on case: validate_slot's was
// case-sensitive, log.c's parse_onoff case-INsensitive and narrower.  So
// `debug.log cpu stdout=ON` worked while the same spelling failed on every
// typed bool argument.  Case-sensitive wins, matching the identifier rules.
//
// This deliberately does NOT cover parse.c's and script.c's true/false/none:
// those are language keyword literals, not coercions, and must not start
// accepting "yes".
TEST(test_parse_bool_vocabulary) {
    bool b = false;
    struct {
        const char *s;
        bool want;
    } yes[] = {
        {"true",  true },
        {"on",    true },
        {"yes",   true },
        {"1",     true },
        {"false", false},
        {"off",   false},
        {"no",    false},
        {"0",     false}
    };
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        ASSERT_TRUE(val_parse_bool(yes[i].s, &b));
        ASSERT_EQ_INT((int)b, (int)yes[i].want);
    }
}

TEST(test_parse_bool_is_case_sensitive_and_rejects_junk) {
    bool b = false;
    ASSERT_TRUE(!val_parse_bool("ON", &b)); // upper case is not accepted
    ASSERT_TRUE(!val_parse_bool("True", &b));
    ASSERT_TRUE(!val_parse_bool("maybe", &b));
    ASSERT_TRUE(!val_parse_bool("", &b));
    ASSERT_TRUE(!val_parse_bool(NULL, &b));
}

// val_bytes holds `p != NULL whenever n > 0`, including when the allocation
// fails.  value_copy's V_BYTES arm used to leave `n` at the source length
// with `p` NULL, and the next reader -- format_value_default, same_kind_equal
// -- dereferenced NULL with a non-zero length.
TEST(test_bytes_invariant_holds) {
    value_t a = val_bytes("abc", 3);
    ASSERT_EQ_INT((int)a.bytes.n, 3);
    ASSERT_TRUE(a.bytes.p != NULL);
    value_free(&a);

    // Zero length: no allocation, and n and p agree.
    value_t z = val_bytes(NULL, 0);
    ASSERT_EQ_INT((int)z.bytes.n, 0);
    ASSERT_TRUE(z.bytes.p == NULL);
    value_free(&z);

    // A duplicate keeps the invariant too.
    value_t src = val_bytes("xyzw", 4);
    value_t d = value_dup(&src);
    ASSERT_EQ_INT((int)d.bytes.n, 4);
    ASSERT_TRUE(d.bytes.p != NULL && d.bytes.p != src.bytes.p);
    value_free(&d);
    value_free(&src);
}

// === Lazy ranges ===========================================================
//
// range() used to materialise a V_LIST capped at 2^20 entries, which at
// sizeof(value_t) == 32 permitted a 32 MB single calloc on the 32-bit wasm
// heap -- to run a loop.  A range now carries three integers and never
// allocates, so range(a,b) and a..b are the SAME value and the two spellings
// stop having opposite safety properties.
TEST(test_range_is_lazy_and_counts_correctly) {
    value_t r = val_range(0, 10);
    ASSERT_EQ_INT(V_RANGE, r.kind);
    ASSERT_EQ_INT((int)val_range_count(&r), 10);
    ASSERT_EQ_INT((int)r.range.step, 1);

    value_t s = val_range_step(0, 10, 3); // 0, 3, 6, 9
    ASSERT_EQ_INT((int)val_range_count(&s), 4);

    value_t d = val_range_step(10, 0, -3); // 10, 7, 4, 1
    ASSERT_EQ_INT((int)val_range_count(&d), 4);

    value_t empty = val_range(5, 5);
    ASSERT_EQ_INT((int)val_range_count(&empty), 0);
    value_t backwards = val_range(5, 1); // positive step, stop < start
    ASSERT_EQ_INT((int)val_range_count(&backwards), 0);
}

// The count is computed in uint64 so the full int64 span cannot overflow the
// subtraction, which is undefined in int64 and was the narrower of the two
// overflows the old range() had.
TEST(test_range_count_does_not_overflow) {
    value_t huge = val_range(INT64_MIN, INT64_MAX);
    uint64_t n = val_range_count(&huge);
    ASSERT_TRUE(n == (uint64_t)INT64_MAX + (uint64_t)INT64_MAX + 1u);
}

int main(void) {
    RUN(test_inline_free_is_noop);
    RUN(test_string_ownership);
    RUN(test_error_ownership);
    RUN(test_bytes_ownership);
    RUN(test_list_recursive_free);
    RUN(test_nested_list_free);
    RUN(test_value_dup_heap_kinds);
    RUN(test_truthiness);
    RUN(test_map_builder);
    RUN(test_map_nested_free);
    RUN(test_map_copy);
    RUN(test_value_auto_cleanup);
    RUN(test_parse_bool_vocabulary);
    RUN(test_parse_bool_is_case_sensitive_and_rejects_junk);
    RUN(test_bytes_invariant_holds);
    RUN(test_range_is_lazy_and_counts_correctly);
    RUN(test_range_count_does_not_overflow);
    return 0;
}
