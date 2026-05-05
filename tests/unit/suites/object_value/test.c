// Unit tests for value.{c,h} — the tagged-union value type used across
// every object-model boundary.

#include "test_assert.h"
#include "value.h"

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

// value_copy duplicates heap kinds.
TEST(test_value_copy) {
    value_t s = val_str("original");
    value_t c = value_copy(&s);
    ASSERT_EQ_INT(V_STRING, c.kind);
    ASSERT_TRUE(c.s != s.s);
    ASSERT_TRUE(strcmp(c.s, s.s) == 0);
    value_free(&s);
    // c remains valid.
    ASSERT_TRUE(strcmp(c.s, "original") == 0);
    value_free(&c);

    value_t list_src = val_list((value_t *)calloc(1, sizeof(value_t)), 1);
    list_src.list.items[0] = val_str("inside");
    value_t list_copy = value_copy(&list_src);
    ASSERT_EQ_INT(1, (int)list_copy.list.len);
    ASSERT_TRUE(list_copy.list.items != list_src.list.items);
    value_free(&list_src);
    ASSERT_TRUE(strcmp(list_copy.list.items[0].s, "inside") == 0);
    value_free(&list_copy);
}

// Truthiness rule per proposal §2.5.
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

int main(void) {
    RUN(test_inline_free_is_noop);
    RUN(test_string_ownership);
    RUN(test_error_ownership);
    RUN(test_bytes_ownership);
    RUN(test_list_recursive_free);
    RUN(test_nested_list_free);
    RUN(test_value_copy);
    RUN(test_truthiness);
    RUN(test_value_auto_cleanup);
    return 0;
}
