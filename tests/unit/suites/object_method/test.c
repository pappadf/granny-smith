// Unit tests for M4: argument-position expressions and method
// dispatch over the object model.
//
// Strategy: register a small toy `math2` class with methods that
// exercise the same patterns the real `math` object uses
// (close/abs/min/max). Drive both via node_call and via expr_eval
// over `$(class.method(args))` to confirm the call form parses and
// evaluates inside expressions.
//
// `math2.poly` covers the V_ANY result slot (issue #106): a method
// whose result kind follows its argument, the shape
// debug.mac.globals.read has. These tests only bite in an
// assertion-enabled build — node_call's return-kind assert is what
// used to abort the process on the V_BYTES branch.

#include "expr.h"
#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// === Toy math2 class — same shape as the real math object ===============

static value_t math2_close(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 3)
        return val_err("expected 3 args");
    bool ok = true;
    double a = val_as_f64(&argv[0], &ok);
    if (!ok)
        return val_err("a non-numeric");
    double b = val_as_f64(&argv[1], &ok);
    if (!ok)
        return val_err("b non-numeric");
    double e = val_as_f64(&argv[2], &ok);
    if (!ok)
        return val_err("eps non-numeric");
    double d = a - b;
    if (d < 0)
        d = -d;
    return val_bool(d <= e);
}

static value_t math2_min(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2)
        return val_err("expected 2 args");
    bool ok = true;
    double a = val_as_f64(&argv[0], &ok);
    if (!ok)
        return val_err("a non-numeric");
    double b = val_as_f64(&argv[1], &ok);
    if (!ok)
        return val_err("b non-numeric");
    return val_int((int64_t)(a < b ? a : b));
}

static value_t math2_id(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("need one arg");
    return val_int((int64_t)val_as_i64(&argv[0], NULL));
}

// A polymorphic method: `poly(width)` returns a uint for a 1/2/4-byte
// width and a byte buffer for anything wider — the same contract
// debug.mac.globals.read has. Declared `.result = V_ANY`.
static value_t math2_poly(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("need one arg");
    int64_t width = val_as_i64(&argv[0], NULL);
    if (width == 1 || width == 2 || width == 4)
        return val_uint((uint8_t)width, 0x11223344u >> (8 * (4 - width)));
    if (width <= 0)
        return val_err("width must be positive");
    uint8_t buf[64];
    if (width > (int64_t)sizeof(buf))
        return val_err("width too large");
    for (int64_t i = 0; i < width; i++)
        buf[i] = (uint8_t)i;
    return val_bytes(buf, (size_t)width);
}

// Accepts any kind in its one slot (V_ANY on an argument slot) and
// reports what it was handed, so the sentinel is exercised on the
// input side too.
static value_t math2_kind_of(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1)
        return val_err("need one arg");
    return val_int((int64_t)argv[0].kind);
}

static const arg_decl_t close_args[] = {
    {.name = "a",   .kind = V_FLOAT, .doc = "a"  },
    {.name = "b",   .kind = V_FLOAT, .doc = "b"  },
    {.name = "eps", .kind = V_FLOAT, .doc = "eps"},
};
static const arg_decl_t pair_args[] = {
    {.name = "a", .kind = V_FLOAT, .doc = "a"},
    {.name = "b", .kind = V_FLOAT, .doc = "b"},
};
static const arg_decl_t one_arg[] = {
    {.name = "x", .kind = V_INT, .doc = "x"},
};
static const arg_decl_t width_arg[] = {
    {.name = "width", .kind = V_INT, .doc = "byte width"},
};
static const arg_decl_t any_arg[] = {
    {.name = "v", .kind = V_ANY, .doc = "a value of any kind"},
};

static const member_t math2_members[] = {
    {.kind = M_METHOD,
     .name = "close",
     .method = {.args = close_args, .nargs = 3, .result = V_BOOL, .fn = math2_close}                               },
    {.kind = M_METHOD, .name = "min",  .method = {.args = pair_args, .nargs = 2, .result = V_INT, .fn = math2_min} },
    {.kind = M_METHOD, .name = "id",   .method = {.args = one_arg, .nargs = 1, .result = V_INT, .fn = math2_id}    },
    {.kind = M_METHOD, .name = "poly", .method = {.args = width_arg, .nargs = 1, .result = V_ANY, .fn = math2_poly}},
    {.kind = M_METHOD,
     .name = "kind_of",
     .method = {.args = any_arg, .nargs = 1, .result = V_INT, .fn = math2_kind_of}                                 },
};

static const class_desc_t math2_class = {
    .name = "math2",
    .members = math2_members,
    .n_members = sizeof(math2_members) / sizeof(math2_members[0]),
};

static void install_math2(void) {
    object_root_reset();
    struct object *o = object_new(&math2_class, NULL, "math2");
    object_attach(object_root(), o);
}

static value_t eval_with_root(const char *src) {
    expr_ctx_t ctx = {.root = object_root()};
    return expr_eval(src, &ctx);
}

// === Tests ===============================================================

TEST(test_node_call_succeeds) {
    install_math2();
    node_t n = object_resolve(object_root(), "math2.min");
    ASSERT_TRUE(node_valid(n));
    ASSERT_TRUE(n.member && n.member->kind == M_METHOD);

    value_t argv[2] = {val_int(3), val_int(7)};
    value_t r = node_call(n, 2, argv);
    ASSERT_EQ_INT(V_INT, r.kind);
    ASSERT_EQ_INT(3, (int)r.i);
    value_free(&r);
    for (int i = 0; i < 2; i++)
        value_free(&argv[i]);
    object_root_reset();
}

TEST(test_node_call_too_few_args) {
    install_math2();
    node_t n = object_resolve(object_root(), "math2.min");
    ASSERT_TRUE(node_valid(n));
    value_t one = val_int(3);
    value_t r = node_call(n, 1, &one);
    ASSERT_TRUE(val_is_error(&r));
    value_free(&r);
    value_free(&one);
    object_root_reset();
}

TEST(test_call_form_inside_expr) {
    install_math2();
    value_t v = eval_with_root("math2.min(3, 7)");
    ASSERT_EQ_INT(V_INT, v.kind);
    ASSERT_EQ_INT(3, (int)v.i);
    value_free(&v);
    object_root_reset();
}

TEST(test_method_in_arithmetic) {
    install_math2();
    // Result of method call participates in arithmetic.
    value_t v = eval_with_root("math2.min(10, 20) + 1");
    bool ok = false;
    int64_t i = val_as_i64(&v, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(11, (int)i);
    value_free(&v);
    object_root_reset();
}

TEST(test_method_predicate) {
    install_math2();
    // close(1.0, 1.0001, 0.001) → true
    value_t v = eval_with_root("math2.close(1.0, 1.0001, 0.001)");
    ASSERT_EQ_INT(V_BOOL, v.kind);
    ASSERT_TRUE(v.b);
    value_free(&v);

    // close(1.0, 1.5, 0.001) → false
    v = eval_with_root("math2.close(1.0, 1.5, 0.001)");
    ASSERT_EQ_INT(V_BOOL, v.kind);
    ASSERT_TRUE(!v.b);
    value_free(&v);
    object_root_reset();
}

TEST(test_method_error_propagates) {
    install_math2();
    // math2.id requires 1 arg; calling with zero args inside an
    // expression should propagate as V_ERROR through arithmetic.
    value_t v = eval_with_root("math2.id() + 1");
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
    object_root_reset();
}

TEST(test_zero_arg_method_call_explicit) {
    install_math2();
    // proposal §3.3: zero-arg calls require parens in expression
    // context. `math2.id()` would call (and error here on missing
    // arg); `math2.id` without parens is an attribute read of a
    // method member, which node_get should reject.
    node_t n = object_resolve(object_root(), "math2.id");
    ASSERT_TRUE(node_valid(n));
    value_t r = node_get(n);
    ASSERT_TRUE(val_is_error(&r)); // method read via getter is an error
    value_free(&r);
    object_root_reset();
}

// === Polymorphic (V_ANY) result slot — issue #106 =========================

TEST(test_variant_result_uint_branch) {
    install_math2();
    node_t n = object_resolve(object_root(), "math2.poly");
    ASSERT_TRUE(node_valid(n));
    value_t four = val_int(4);
    value_t r = node_call(n, 1, &four);
    ASSERT_EQ_INT(V_UINT, r.kind);
    ASSERT_EQ_INT(0x11223344, (int)r.u);
    value_free(&r);
    value_free(&four);
    object_root_reset();
}

TEST(test_variant_result_bytes_branch) {
    install_math2();
    // The regression: a V_BYTES result from a slot that also yields
    // V_UINT used to trip node_call's return-kind assertion and abort
    // the process (debug.mac.globals.read "KeyMap").
    node_t n = object_resolve(object_root(), "math2.poly");
    ASSERT_TRUE(node_valid(n));
    value_t sixteen = val_int(16);
    value_t r = node_call(n, 1, &sixteen);
    ASSERT_EQ_INT(V_BYTES, r.kind);
    ASSERT_EQ_INT(16, (int)r.bytes.n);
    ASSERT_EQ_INT(3, (int)r.bytes.p[3]);
    value_free(&r);
    value_free(&sixteen);
    object_root_reset();
}

TEST(test_variant_result_error_branch) {
    install_math2();
    node_t n = object_resolve(object_root(), "math2.poly");
    ASSERT_TRUE(node_valid(n));
    value_t zero = val_int(0);
    value_t r = node_call(n, 1, &zero);
    ASSERT_TRUE(val_is_error(&r));
    value_free(&r);
    value_free(&zero);
    object_root_reset();
}

TEST(test_variant_result_in_expr) {
    install_math2();
    // Both branches reachable from expression context: arithmetic on
    // the uint one, len() on the bytes one.
    value_t v = eval_with_root("math2.poly(2) + 1");
    bool ok = false;
    int64_t i = val_as_i64(&v, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(0x1123, (int)i); // 0x11223344 >> 16, + 1
    value_free(&v);

    v = eval_with_root("len(math2.poly(10))");
    i = val_as_i64(&v, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(10, (int)i);
    value_free(&v);
    object_root_reset();
}

TEST(test_any_arg_slot_accepts_every_kind) {
    install_math2();
    node_t n = object_resolve(object_root(), "math2.kind_of");
    ASSERT_TRUE(node_valid(n));

    value_t s = val_str("hello");
    value_t r = node_call(n, 1, &s);
    ASSERT_EQ_INT(V_STRING, (int)r.i); // passed through uncoerced
    value_free(&r);
    value_free(&s);

    value_t b = val_bool(true);
    r = node_call(n, 1, &b);
    ASSERT_EQ_INT(V_BOOL, (int)r.i);
    value_free(&r);
    value_free(&b);
    object_root_reset();
}

// A V_ANY attribute slot is a class-author mistake: attributes need a
// concrete kind for the getter/setter round-trip. Registration rejects it.
static value_t any_attr_get(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, 0);
}
static const member_t any_attr_members[] = {
    {.kind = M_ATTR, .name = "x", .flags = VAL_RO, .attr = {.type = V_ANY, .get = any_attr_get}},
};
static const class_desc_t any_attr_class = {
    .name = "anyattr",
    .members = any_attr_members,
    .n_members = 1,
};

TEST(test_any_attribute_slot_rejected) {
    char err[200];
    ASSERT_TRUE(!object_validate_class(&any_attr_class, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "ANY") != NULL);
}

int main(void) {
    RUN(test_node_call_succeeds);
    RUN(test_node_call_too_few_args);
    RUN(test_call_form_inside_expr);
    RUN(test_method_in_arithmetic);
    RUN(test_method_predicate);
    RUN(test_method_error_propagates);
    RUN(test_zero_arg_method_call_explicit);
    RUN(test_variant_result_uint_branch);
    RUN(test_variant_result_bytes_branch);
    RUN(test_variant_result_error_branch);
    RUN(test_variant_result_in_expr);
    RUN(test_any_arg_slot_accepts_every_kind);
    RUN(test_any_attribute_slot_rejected);
    return 0;
}
