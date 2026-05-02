// Unit tests for expr.{c,h} — recursive-descent expression parser/evaluator.
//
// Covers: operator table against known results, type promotion, short-circuit
// semantics (proposal §3.2), and error propagation.

#include "expr.h"
#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Convenience: evaluate `src` with no root/alias bindings.
static value_t eval(const char *src) {
    expr_ctx_t ctx = {0};
    return expr_eval(src, &ctx);
}

// === Literal arithmetic =====================================================

TEST(test_literal_addition) {
    value_t v = eval("2 + 3");
    ASSERT_EQ_INT(V_UINT, v.kind);
    ASSERT_EQ_INT(5, (int)v.u);
    value_free(&v);
}

TEST(test_operator_precedence) {
    // 2 + 3 * 4 = 14, not 20 — multiplication binds tighter.
    value_t v = eval("2 + 3 * 4");
    ASSERT_EQ_INT(14, (int)v.u);
    value_free(&v);

    // Parentheses override.
    v = eval("(2 + 3) * 4");
    ASSERT_EQ_INT(20, (int)v.u);
    value_free(&v);
}

TEST(test_subtraction_and_unary_minus) {
    value_t v = eval("10 - 3");
    ASSERT_EQ_INT(7, (int)v.u);
    value_free(&v);

    v = eval("-5 + 8");
    ASSERT_EQ_INT(V_INT, v.kind);
    ASSERT_EQ_INT(3, (int)v.i);
    value_free(&v);
}

TEST(test_division_and_modulo) {
    value_t v = eval("10 / 3");
    ASSERT_EQ_INT(3, (int)v.u);
    value_free(&v);

    v = eval("10 % 3");
    ASSERT_EQ_INT(1, (int)v.u);
    value_free(&v);

    // Division by zero is an error.
    v = eval("1 / 0");
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
}

TEST(test_bitwise_ops) {
    value_t v = eval("0xF0 | 0x0F");
    ASSERT_EQ_INT(0xFF, (int)v.u);
    value_free(&v);

    v = eval("0xFF & 0x0F");
    ASSERT_EQ_INT(0x0F, (int)v.u);
    value_free(&v);

    v = eval("0xFF ^ 0x0F");
    ASSERT_EQ_INT(0xF0, (int)v.u);
    value_free(&v);

    // Bitwise binds tighter than logical: a | b would be 0xFF, not (a||b).
    v = eval("0x80 | 0x01");
    ASSERT_EQ_INT(0x81, (int)v.u);
    value_free(&v);
}

TEST(test_shift_ops) {
    value_t v = eval("1 << 8");
    ASSERT_EQ_INT(256, (int)v.u);
    value_free(&v);

    v = eval("256 >> 4");
    ASSERT_EQ_INT(16, (int)v.u);
    value_free(&v);
}

TEST(test_comparison_ops) {
    value_t v = eval("3 < 5");
    ASSERT_EQ_INT(V_BOOL, v.kind);
    ASSERT_TRUE(v.b);
    value_free(&v);

    v = eval("3 > 5");
    ASSERT_TRUE(!v.b);
    value_free(&v);

    v = eval("5 <= 5");
    ASSERT_TRUE(v.b);
    value_free(&v);

    v = eval("5 >= 6");
    ASSERT_TRUE(!v.b);
    value_free(&v);

    v = eval("42 == 42");
    ASSERT_TRUE(v.b);
    value_free(&v);

    v = eval("42 != 43");
    ASSERT_TRUE(v.b);
    value_free(&v);
}

// === Type promotion =========================================================

TEST(test_int_uint_promotion) {
    // Mixed signed/unsigned promotes to int (negative wins).
    value_t v = eval("-2 + 5");
    ASSERT_EQ_INT(V_INT, v.kind);
    ASSERT_EQ_INT(3, (int)v.i);
    value_free(&v);
}

TEST(test_int_to_float_promotion) {
    value_t v = eval("1 + 2.5");
    ASSERT_EQ_INT(V_FLOAT, v.kind);
    ASSERT_TRUE(v.f == 3.5);
    value_free(&v);

    v = eval("10 / 4.0");
    ASSERT_EQ_INT(V_FLOAT, v.kind);
    ASSERT_TRUE(v.f == 2.5);
    value_free(&v);
}

TEST(test_bool_to_uint_promotion) {
    value_t v = eval("true + 1");
    ASSERT_EQ_INT(2, (int)v.u);
    value_free(&v);
}

// === Short-circuit (§3.2) ===================================================

TEST(test_logand_short_circuit) {
    // The right side calls `unknown.path` which would error, but
    // short-circuit must skip it entirely.
    value_t v = eval("false && unknown.path");
    ASSERT_EQ_INT(V_BOOL, v.kind);
    ASSERT_TRUE(!v.b);
    value_free(&v);

    // True && X evaluates X.
    v = eval("true && true");
    ASSERT_TRUE(v.b);
    value_free(&v);

    v = eval("true && false");
    ASSERT_TRUE(!v.b);
    value_free(&v);
}

TEST(test_logor_short_circuit) {
    value_t v = eval("true || unknown.path");
    ASSERT_TRUE(v.b);
    value_free(&v);

    v = eval("false || true");
    ASSERT_TRUE(v.b);
    value_free(&v);

    v = eval("false || false");
    ASSERT_TRUE(!v.b);
    value_free(&v);
}

TEST(test_logand_chain_short_circuit) {
    // Chain: false && X && Y must not evaluate X or Y.
    value_t v = eval("false && bad.path && other.bad");
    ASSERT_EQ_INT(V_BOOL, v.kind);
    ASSERT_TRUE(!v.b);
    value_free(&v);
}

// === Ternary ================================================================

TEST(test_ternary) {
    value_t v = eval("1 < 2 ? 10 : 20");
    ASSERT_EQ_INT(10, (int)v.u);
    value_free(&v);

    v = eval("1 > 2 ? 10 : 20");
    ASSERT_EQ_INT(20, (int)v.u);
    value_free(&v);
}

// === Error propagation ======================================================

TEST(test_error_propagates_through_arithmetic) {
    // Bare identifier with no root bound → V_ERROR; arithmetic on it
    // propagates V_ERROR rather than coercing to zero.
    value_t v = eval("missing + 1");
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
}

TEST(test_error_propagates_through_negation) {
    value_t v = eval("!missing");
    // !error per proposal §3.2 is also "false". A V_ERROR turned into
    // a bool via val_as_bool is false, so !false = true. The proposal
    // gives `assert $(!cpu.broken)` as a valid idiom.
    ASSERT_EQ_INT(V_BOOL, v.kind);
    ASSERT_TRUE(v.b);
    value_free(&v);
}

// === Logical & bitwise unary ================================================

TEST(test_unary_not) {
    value_t v = eval("!true");
    ASSERT_TRUE(!v.b);
    value_free(&v);

    v = eval("!false");
    ASSERT_TRUE(v.b);
    value_free(&v);

    v = eval("!0");
    ASSERT_TRUE(v.b);
    value_free(&v);

    v = eval("!1");
    ASSERT_TRUE(!v.b);
    value_free(&v);
}

TEST(test_unary_bitnot) {
    value_t v = eval("~0u");
    // ~0 == 0xFFFFFFFFFFFFFFFF
    ASSERT_TRUE(v.u == 0xFFFFFFFFFFFFFFFFull);
    value_free(&v);
}

// === String operations ======================================================

TEST(test_string_concat) {
    value_t v = eval("\"foo\" + \"bar\"");
    ASSERT_EQ_INT(V_STRING, v.kind);
    ASSERT_TRUE(strcmp(v.s, "foobar") == 0);
    value_free(&v);
}

TEST(test_string_equality) {
    value_t v = eval("\"abc\" == \"abc\"");
    ASSERT_TRUE(v.b);
    value_free(&v);

    v = eval("\"abc\" != \"xyz\"");
    ASSERT_TRUE(v.b);
    value_free(&v);
}

// === Trailing garbage =======================================================

TEST(test_trailing_garbage) {
    value_t v = eval("1 + 2 extra");
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
}

// === String interpolation ===================================================

TEST(test_interpolate_simple) {
    expr_ctx_t ctx = {0};
    value_t v = expr_interpolate_string("answer = ${1 + 2}", &ctx);
    ASSERT_EQ_INT(V_STRING, v.kind);
    ASSERT_TRUE(strcmp(v.s, "answer = 3") == 0);
    value_free(&v);
}

TEST(test_interpolate_no_braces_passthrough) {
    expr_ctx_t ctx = {0};
    value_t v = expr_interpolate_string("plain string", &ctx);
    ASSERT_TRUE(strcmp(v.s, "plain string") == 0);
    value_free(&v);
}

TEST(test_interpolate_unterminated) {
    expr_ctx_t ctx = {0};
    value_t v = expr_interpolate_string("oops ${1 + 2", &ctx);
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
}

// === M5 format specs (proposal §4.2.1) ====================================

TEST(test_interp_spec_decimal) {
    expr_ctx_t ctx = {0};
    value_t v = expr_interpolate_string("n=${42:d}", &ctx);
    ASSERT_TRUE(strcmp(v.s, "n=42") == 0);
    value_free(&v);
}

TEST(test_interp_spec_hex_lower) {
    expr_ctx_t ctx = {0};
    value_t v = expr_interpolate_string("x=${255:x}", &ctx);
    ASSERT_TRUE(strcmp(v.s, "x=ff") == 0);
    value_free(&v);
}

TEST(test_interp_spec_hex_upper) {
    expr_ctx_t ctx = {0};
    value_t v = expr_interpolate_string("X=${255:X}", &ctx);
    ASSERT_TRUE(strcmp(v.s, "X=FF") == 0);
    value_free(&v);
}

TEST(test_interp_spec_zero_padded_hex) {
    expr_ctx_t ctx = {0};
    value_t v = expr_interpolate_string("a=${0x4002b4:08x}", &ctx);
    ASSERT_TRUE(strcmp(v.s, "a=004002b4") == 0);
    value_free(&v);
}

TEST(test_interp_spec_zero_padded_decimal) {
    expr_ctx_t ctx = {0};
    value_t v = expr_interpolate_string("n=${42:05d}", &ctx);
    ASSERT_TRUE(strcmp(v.s, "n=00042") == 0);
    value_free(&v);
}

TEST(test_interp_spec_string) {
    expr_ctx_t ctx = {0};
    value_t v = expr_interpolate_string("s=${\"hi\":s}", &ctx);
    ASSERT_TRUE(strcmp(v.s, "s=hi") == 0);
    value_free(&v);
}

TEST(test_interp_spec_printf_escape_hatch) {
    expr_ctx_t ctx = {0};
    value_t v = expr_interpolate_string("n=${5:%-3d}|", &ctx);
    // %-3d → "5  " (left-justified, width 3)
    ASSERT_TRUE(strcmp(v.s, "n=5  |") == 0);
    value_free(&v);
}

TEST(test_interp_spec_default_when_no_spec) {
    expr_ctx_t ctx = {0};
    // No spec — uses native formatter. UInt without VAL_HEX → decimal.
    value_t v = expr_interpolate_string("v=${42}", &ctx);
    ASSERT_TRUE(strcmp(v.s, "v=42") == 0);
    value_free(&v);
}

TEST(test_interp_multiple_chunks) {
    expr_ctx_t ctx = {0};
    value_t v = expr_interpolate_string("${1+1}+${2*3}=${1+1+2*3}", &ctx);
    ASSERT_TRUE(strcmp(v.s, "2+6=8") == 0);
    value_free(&v);
}

TEST(test_interp_colon_inside_string_doesnt_split_spec) {
    expr_ctx_t ctx = {0};
    // The colon inside the string literal must not be mistaken for a
    // format-spec separator (proposal §4.2.1 — split at top-level colon).
    value_t v = expr_interpolate_string("${\"a:b\"}", &ctx);
    ASSERT_TRUE(strcmp(v.s, "a:b") == 0);
    value_free(&v);
}

int main(void) {
    RUN(test_literal_addition);
    RUN(test_operator_precedence);
    RUN(test_subtraction_and_unary_minus);
    RUN(test_division_and_modulo);
    RUN(test_bitwise_ops);
    RUN(test_shift_ops);
    RUN(test_comparison_ops);
    RUN(test_int_uint_promotion);
    RUN(test_int_to_float_promotion);
    RUN(test_bool_to_uint_promotion);
    RUN(test_logand_short_circuit);
    RUN(test_logor_short_circuit);
    RUN(test_logand_chain_short_circuit);
    RUN(test_ternary);
    RUN(test_error_propagates_through_arithmetic);
    RUN(test_error_propagates_through_negation);
    RUN(test_unary_not);
    RUN(test_unary_bitnot);
    RUN(test_string_concat);
    RUN(test_string_equality);
    RUN(test_trailing_garbage);
    RUN(test_interpolate_simple);
    RUN(test_interpolate_no_braces_passthrough);
    RUN(test_interpolate_unterminated);
    RUN(test_interp_spec_decimal);
    RUN(test_interp_spec_hex_lower);
    RUN(test_interp_spec_hex_upper);
    RUN(test_interp_spec_zero_padded_hex);
    RUN(test_interp_spec_zero_padded_decimal);
    RUN(test_interp_spec_string);
    RUN(test_interp_spec_printf_escape_hatch);
    RUN(test_interp_spec_default_when_no_spec);
    RUN(test_interp_multiple_chunks);
    RUN(test_interp_colon_inside_string_doesnt_split_spec);
    return 0;
}
