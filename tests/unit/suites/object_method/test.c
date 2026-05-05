// Unit tests for M4: argument-position expressions and method
// dispatch over the object model.
//
// Strategy: register a small toy `math2` class with methods that
// exercise the same patterns the real `math` object uses
// (close/abs/min/max). Drive both via node_call and via expr_eval
// over `$(class.method(args))` to confirm the call form parses and
// evaluates inside expressions.

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

static const member_t math2_members[] = {
    {.kind = M_METHOD,
     .name = "close",
     .method = {.args = close_args, .nargs = 3, .result = V_BOOL, .fn = math2_close}                             },
    {.kind = M_METHOD, .name = "min", .method = {.args = pair_args, .nargs = 2, .result = V_INT, .fn = math2_min}},
    {.kind = M_METHOD, .name = "id",  .method = {.args = one_arg, .nargs = 1, .result = V_INT, .fn = math2_id}   },
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

int main(void) {
    RUN(test_node_call_succeeds);
    RUN(test_node_call_too_few_args);
    RUN(test_call_form_inside_expr);
    RUN(test_method_in_arithmetic);
    RUN(test_method_predicate);
    RUN(test_method_error_propagates);
    RUN(test_zero_arg_method_call_explicit);
    return 0;
}
