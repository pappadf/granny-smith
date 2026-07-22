// Unit tests for the named-argument binder (node_bind_args) and the
// `name=expr` call-form grammar (proposal-named-args-boot-config §3).
//
// Strategy: register a toy `nt` class whose methods exercise the
// binding rules — required/optional-with-default fixed slots, a rest
// slot, and a legacy method with no args table — then drive them via
// node_bind_args directly and via expr_eval over `nt.method(...)`.

#include "expr.h"
#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>

// === Toy nt class ========================================================

// trip(a, b=7, c=9) → a*100 + b*10 + c — decodes which slot got what.
static value_t nt_trip(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 3)
        return val_err("expected 3 args after defaults");
    return val_int(argv[0].i * 100 + argv[1].i * 10 + argv[2].i);
}

// sum(first, rest...) → first + sum(rest) — rest-slot interaction.
static value_t nt_sum(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    int64_t total = 0;
    for (int i = 0; i < argc; i++)
        total += argv[i].i;
    return val_int(total);
}

// raw(...) — legacy variadic, no args table: named args must be rejected.
static value_t nt_raw(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argv;
    return val_int(argc);
}

static const value_t k_seven = {.kind = V_INT, .i = 7};
static const value_t k_nine = {.kind = V_INT, .i = 9};

static const arg_decl_t trip_args[] = {
    {.name = "a", .kind = V_INT, .doc = "a"},
    {.name = "b", .kind = V_INT, .validation_flags = OBJ_ARG_OPTIONAL, .default_value = &k_seven, .doc = "b"},
    {.name = "c", .kind = V_INT, .validation_flags = OBJ_ARG_OPTIONAL, .default_value = &k_nine, .doc = "c"},
};
static const arg_decl_t sum_args[] = {
    {.name = "first", .kind = V_INT, .doc = "first"},
    {.name = "rest", .kind = V_INT, .validation_flags = OBJ_ARG_REST, .doc = "rest"},
};

static const member_t nt_members[] = {
    {.kind = M_METHOD, .name = "trip", .method = {.args = trip_args, .nargs = 3, .result = V_INT, .fn = nt_trip}},
    {.kind = M_METHOD, .name = "sum",  .method = {.args = sum_args, .nargs = 2, .result = V_INT, .fn = nt_sum}  },
    {.kind = M_METHOD, .name = "raw",  .method = {.args = NULL, .nargs = 0, .result = V_INT, .fn = nt_raw}      },
};

static const class_desc_t nt_class = {
    .name = "nt",
    .members = nt_members,
    .n_members = sizeof(nt_members) / sizeof(nt_members[0]),
};

static void install_nt(void) {
    object_root_reset();
    struct object *o = object_new(&nt_class, NULL, "nt");
    object_attach(object_root(), o);
}

static value_t eval_with_root(const char *src) {
    expr_ctx_t ctx = {.root = object_root()};
    return expr_eval(src, &ctx);
}

// Bind + call helper for direct binder tests.
static value_t bind_call(const char *path, int pos_n, const value_t *pos, int named_n, const named_arg_t *named) {
    node_t n = object_resolve(object_root(), path);
    if (!node_valid(n))
        return val_err("no node");
    value_t bound[OBJ_BIND_MAX_ARGS];
    int bound_n = 0;
    value_t err = node_bind_args(n, pos_n, pos, named_n, named, bound, &bound_n);
    if (val_is_error(&err))
        return err;
    return node_call(n, bound_n, bound);
}

// === Binder-level tests ==================================================

TEST(test_named_only_fills_slots) {
    install_nt();
    named_arg_t named[2] = {
        {.name = "a", .value = val_int(1)},
        {.name = "c", .value = val_int(2)}
    };
    value_t r = bind_call("nt.trip", 0, NULL, 2, named);
    ASSERT_EQ_INT(V_INT, r.kind);
    ASSERT_EQ_INT(172, (int)r.i); // a=1, b default 7, c=2
    value_free(&r);
    object_root_reset();
}

TEST(test_positional_then_named) {
    install_nt();
    value_t pos[1] = {val_int(4)};
    named_arg_t named[1] = {
        {.name = "c", .value = val_int(3)}
    };
    value_t r = bind_call("nt.trip", 1, pos, 1, named);
    ASSERT_EQ_INT(V_INT, r.kind);
    ASSERT_EQ_INT(473, (int)r.i); // interior hole b filled from default
    value_free(&r);
    object_root_reset();
}

TEST(test_missing_required_via_named) {
    install_nt();
    named_arg_t named[1] = {
        {.name = "c", .value = val_int(3)}
    };
    value_t r = bind_call("nt.trip", 0, NULL, 1, named);
    ASSERT_TRUE(val_is_error(&r));
    ASSERT_TRUE(strstr(r.err, "missing argument 'a'") != NULL);
    value_free(&r);
    object_root_reset();
}

TEST(test_duplicate_with_positional) {
    install_nt();
    value_t pos[1] = {val_int(1)};
    named_arg_t named[1] = {
        {.name = "a", .value = val_int(2)}
    };
    value_t r = bind_call("nt.trip", 1, pos, 1, named);
    ASSERT_TRUE(val_is_error(&r));
    ASSERT_TRUE(strstr(r.err, "duplicate argument 'a'") != NULL);
    value_free(&r);
    object_root_reset();
}

TEST(test_duplicate_named) {
    install_nt();
    named_arg_t named[2] = {
        {.name = "b", .value = val_int(1)},
        {.name = "b", .value = val_int(2)}
    };
    value_t r = bind_call("nt.trip", 0, NULL, 2, named);
    ASSERT_TRUE(val_is_error(&r));
    ASSERT_TRUE(strstr(r.err, "duplicate argument 'b'") != NULL);
    value_free(&r);
    object_root_reset();
}

TEST(test_unknown_name_lists_declared) {
    install_nt();
    named_arg_t named[1] = {
        {.name = "z", .value = val_int(1)}
    };
    value_t r = bind_call("nt.trip", 0, NULL, 1, named);
    ASSERT_TRUE(val_is_error(&r));
    ASSERT_TRUE(strstr(r.err, "unknown argument 'z'") != NULL);
    ASSERT_TRUE(strstr(r.err, "a, b, c") != NULL);
    value_free(&r);
    object_root_reset();
}

TEST(test_rest_slot_not_nameable) {
    install_nt();
    named_arg_t named[1] = {
        {.name = "rest", .value = val_int(1)}
    };
    value_t r = bind_call("nt.sum", 0, NULL, 1, named);
    ASSERT_TRUE(val_is_error(&r));
    ASSERT_TRUE(strstr(r.err, "rest slot") != NULL);
    value_free(&r);
    object_root_reset();
}

TEST(test_rest_method_fixed_slot_by_name) {
    install_nt();
    named_arg_t named[1] = {
        {.name = "first", .value = val_int(5)}
    };
    value_t r = bind_call("nt.sum", 0, NULL, 1, named);
    ASSERT_EQ_INT(V_INT, r.kind);
    ASSERT_EQ_INT(5, (int)r.i);
    value_free(&r);
    object_root_reset();
}

TEST(test_rest_tail_stays_positional) {
    install_nt();
    value_t pos[3] = {val_int(1), val_int(2), val_int(3)};
    value_t r = bind_call("nt.sum", 3, pos, 0, NULL);
    ASSERT_EQ_INT(V_INT, r.kind);
    ASSERT_EQ_INT(6, (int)r.i);
    value_free(&r);
    object_root_reset();
}

TEST(test_no_arg_table_rejects_named) {
    install_nt();
    named_arg_t named[1] = {
        {.name = "x", .value = val_int(1)}
    };
    value_t r = bind_call("nt.raw", 0, NULL, 1, named);
    ASSERT_TRUE(val_is_error(&r));
    ASSERT_TRUE(strstr(r.err, "does not declare named arguments") != NULL);
    value_free(&r);
    object_root_reset();
}

TEST(test_no_arg_table_positional_passthrough) {
    install_nt();
    value_t pos[2] = {val_int(1), val_int(2)};
    value_t r = bind_call("nt.raw", 2, pos, 0, NULL);
    ASSERT_EQ_INT(V_INT, r.kind);
    ASSERT_EQ_INT(2, (int)r.i);
    value_free(&r);
    object_root_reset();
}

// === Call-form grammar tests =============================================

TEST(test_call_form_named) {
    install_nt();
    value_t v = eval_with_root("nt.trip(a=1, c=2)");
    ASSERT_EQ_INT(V_INT, v.kind);
    ASSERT_EQ_INT(172, (int)v.i);
    value_free(&v);
    object_root_reset();
}

TEST(test_call_form_named_reordered) {
    install_nt();
    value_t v = eval_with_root("nt.trip(b=1, a=2)");
    ASSERT_EQ_INT(V_INT, v.kind);
    ASSERT_EQ_INT(219, (int)v.i); // a=2, b=1, c default 9
    value_free(&v);
    object_root_reset();
}

TEST(test_call_form_mixed) {
    install_nt();
    value_t v = eval_with_root("nt.trip(4, c=3)");
    ASSERT_EQ_INT(V_INT, v.kind);
    ASSERT_EQ_INT(473, (int)v.i);
    value_free(&v);
    object_root_reset();
}

TEST(test_call_form_value_is_expression) {
    install_nt();
    value_t v = eval_with_root("nt.trip(1, c=1+1)");
    ASSERT_EQ_INT(V_INT, v.kind);
    ASSERT_EQ_INT(172, (int)v.i);
    value_free(&v);
    object_root_reset();
}

TEST(test_call_form_positional_after_named_errors) {
    install_nt();
    value_t v = eval_with_root("nt.trip(a=1, 2)");
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
    object_root_reset();
}

TEST(test_call_form_equality_still_parses) {
    install_nt();
    // `==` inside call args must not be taken for a named argument.
    value_t v = eval_with_root("nt.trip(1, 2, 3) == 123");
    ASSERT_EQ_INT(V_BOOL, v.kind);
    ASSERT_TRUE(v.b);
    value_free(&v);
    object_root_reset();
}

TEST(test_call_form_unknown_name_errors) {
    install_nt();
    value_t v = eval_with_root("nt.trip(z=1)");
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
    object_root_reset();
}

int main(void) {
    RUN(test_named_only_fills_slots);
    RUN(test_positional_then_named);
    RUN(test_missing_required_via_named);
    RUN(test_duplicate_with_positional);
    RUN(test_duplicate_named);
    RUN(test_unknown_name_lists_declared);
    RUN(test_rest_slot_not_nameable);
    RUN(test_rest_method_fixed_slot_by_name);
    RUN(test_rest_tail_stays_positional);
    RUN(test_no_arg_table_rejects_named);
    RUN(test_no_arg_table_positional_passthrough);
    RUN(test_call_form_named);
    RUN(test_call_form_named_reordered);
    RUN(test_call_form_mixed);
    RUN(test_call_form_value_is_expression);
    RUN(test_call_form_positional_after_named_errors);
    RUN(test_call_form_equality_still_parses);
    RUN(test_call_form_unknown_name_errors);
    return 0;
}
