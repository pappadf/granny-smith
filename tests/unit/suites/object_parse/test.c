// Unit tests for parse.{c,h} — the unified literal parser.

#include "object.h"
#include "parse.h"
#include "test_assert.h"
#include "value.h"

#include <string.h>

static value_t parse_str(const char *s) {
    return parse_literal_full(s, NULL, 0);
}

// Decimal integer.
TEST(test_int_decimal) {
    value_t v = parse_str("42");
    ASSERT_EQ_INT(V_UINT, v.kind);
    ASSERT_EQ_INT(42, (int)v.u);
    value_free(&v);

    v = parse_str("-13");
    ASSERT_EQ_INT(V_INT, v.kind);
    ASSERT_EQ_INT(-13, (int)v.i);
    value_free(&v);
}

// Hex integer in all spellings.
TEST(test_int_hex) {
    value_t v = parse_str("0x1234");
    ASSERT_EQ_INT(V_UINT, v.kind);
    ASSERT_EQ_INT(0x1234, (int)v.u);
    value_free(&v);

    v = parse_str("$DEAD_BEEF");
    ASSERT_EQ_INT(V_UINT, v.kind);
    ASSERT_TRUE(v.u == 0xDEADBEEFu);
    value_free(&v);
}

// Binary integer.
TEST(test_int_binary) {
    value_t v = parse_str("0b1010");
    ASSERT_EQ_INT(V_UINT, v.kind);
    ASSERT_EQ_INT(10, (int)v.u);
    value_free(&v);
}

// Octal and 0d prefix.
TEST(test_int_octal_dec) {
    value_t v = parse_str("0o17");
    ASSERT_EQ_INT(V_UINT, v.kind);
    ASSERT_EQ_INT(15, (int)v.u);
    value_free(&v);

    v = parse_str("0d100");
    ASSERT_EQ_INT(V_UINT, v.kind);
    ASSERT_EQ_INT(100, (int)v.u);
    value_free(&v);
}

// Underscore digit separators are ignored.
TEST(test_int_underscores) {
    value_t v = parse_str("1_000_000");
    ASSERT_EQ_INT(V_UINT, v.kind);
    ASSERT_EQ_INT(1000000, (int)v.u);
    value_free(&v);
}

// `u` and `i` suffixes force kind.
TEST(test_int_suffix) {
    value_t v = parse_str("100u");
    ASSERT_EQ_INT(V_UINT, v.kind);
    value_free(&v);

    v = parse_str("100i");
    ASSERT_EQ_INT(V_INT, v.kind);
    value_free(&v);
}

// Floats: decimal, scientific, hex-float.
TEST(test_floats) {
    value_t v = parse_str("1.0");
    ASSERT_EQ_INT(V_FLOAT, v.kind);
    ASSERT_TRUE(v.f == 1.0);
    value_free(&v);

    v = parse_str("1e6");
    ASSERT_EQ_INT(V_FLOAT, v.kind);
    ASSERT_TRUE(v.f == 1e6);
    value_free(&v);

    v = parse_str("1.5e-3");
    ASSERT_EQ_INT(V_FLOAT, v.kind);
    value_free(&v);

    v = parse_str("0x1.8p+1");
    ASSERT_EQ_INT(V_FLOAT, v.kind);
    ASSERT_TRUE(v.f == 3.0); // 0x1.8 * 2 = 3.0
    value_free(&v);
}

// Boolean literal spellings (all six are reserved words).
TEST(test_bools) {
    const char *yes_forms[] = {"true", "on", "yes"};
    const char *no_forms[] = {"false", "off", "no"};
    for (size_t i = 0; i < 3; i++) {
        value_t v = parse_str(yes_forms[i]);
        ASSERT_EQ_INT(V_BOOL, v.kind);
        ASSERT_TRUE(v.b);
        value_free(&v);
    }
    for (size_t i = 0; i < 3; i++) {
        value_t v = parse_str(no_forms[i]);
        ASSERT_EQ_INT(V_BOOL, v.kind);
        ASSERT_TRUE(!v.b);
        value_free(&v);
    }
}

// Strings: double-quoted with the standard escapes.
TEST(test_strings) {
    value_t v = parse_str("\"hello\"");
    ASSERT_EQ_INT(V_STRING, v.kind);
    ASSERT_TRUE(strcmp(v.s, "hello") == 0);
    value_free(&v);

    v = parse_str("\"line1\\nline2\\ttab\"");
    ASSERT_EQ_INT(V_STRING, v.kind);
    ASSERT_TRUE(strcmp(v.s, "line1\nline2\ttab") == 0);
    value_free(&v);

    v = parse_str("\"with \\\"quotes\\\" inside\"");
    ASSERT_EQ_INT(V_STRING, v.kind);
    ASSERT_TRUE(strcmp(v.s, "with \"quotes\" inside") == 0);
    value_free(&v);

    v = parse_str("\"\\x4Aoy\"");
    ASSERT_EQ_INT(V_STRING, v.kind);
    ASSERT_TRUE(strcmp(v.s, "Joy") == 0);
    value_free(&v);
}

// Unterminated string is an error.
TEST(test_string_unterminated) {
    value_t v = parse_str("\"oops");
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
}

// Bytes via the NUMBER:N suffix produce a fixed-width big-endian buffer.
TEST(test_bytes_int_suffix) {
    value_t v = parse_str("0xDEAD_BEEF:4");
    ASSERT_EQ_INT(V_BYTES, v.kind);
    ASSERT_EQ_INT(4, (int)v.bytes.n);
    ASSERT_EQ_INT(0xDE, v.bytes.p[0]);
    ASSERT_EQ_INT(0xAD, v.bytes.p[1]);
    ASSERT_EQ_INT(0xBE, v.bytes.p[2]);
    ASSERT_EQ_INT(0xEF, v.bytes.p[3]);
    value_free(&v);
}

// Enum tags resolve against the supplied table.
TEST(test_enum_lookup) {
    static const char *const phases[] = {"idle", "command", "data", "status"};
    value_t v = parse_literal_full("data", phases, 4);
    ASSERT_EQ_INT(V_ENUM, v.kind);
    ASSERT_EQ_INT(2, v.enm.idx);
    value_free(&v);

    v = parse_literal_full("missing", phases, 4);
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
}

// Reserved-word rejection for object_validate_name.
TEST(test_reserved_word_check) {
    char err[160];
    ASSERT_TRUE(!object_validate_name("true", err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "reserved") != NULL);
    ASSERT_TRUE(!object_validate_name("while", err, sizeof(err)));
    ASSERT_TRUE(!object_validate_name("if", err, sizeof(err)));
    ASSERT_TRUE(!object_validate_name("else", err, sizeof(err)));
    ASSERT_TRUE(!object_validate_name("on", err, sizeof(err)));
    ASSERT_TRUE(!object_validate_name("yes", err, sizeof(err)));

    // Bad identifiers.
    ASSERT_TRUE(!object_validate_name("", err, sizeof(err)));
    ASSERT_TRUE(!object_validate_name("1abc", err, sizeof(err)));
    ASSERT_TRUE(!object_validate_name("a.b", err, sizeof(err)));
    ASSERT_TRUE(!object_validate_name("a-b", err, sizeof(err)));

    // Acceptable.
    ASSERT_TRUE(object_validate_name("pc", err, sizeof(err)));
    ASSERT_TRUE(object_validate_name("d0", err, sizeof(err)));
    ASSERT_TRUE(object_validate_name("MBState", err, sizeof(err)));
    ASSERT_TRUE(object_validate_name("_secret", err, sizeof(err)));
}

// Trailing garbage past a literal is an error.
TEST(test_trailing_garbage) {
    value_t v = parse_str("42 extra");
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
}

// Empty input is an error.
TEST(test_empty) {
    value_t v = parse_str("");
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
}

// Reserved-word identifiers (not bool spellings) inside parse_literal
// cannot be used as bare-identifier literals.
TEST(test_reserved_other_rejected_as_literal) {
    value_t v = parse_str("if");
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
    v = parse_str("while");
    ASSERT_TRUE(val_is_error(&v));
    value_free(&v);
}

int main(void) {
    RUN(test_int_decimal);
    RUN(test_int_hex);
    RUN(test_int_binary);
    RUN(test_int_octal_dec);
    RUN(test_int_underscores);
    RUN(test_int_suffix);
    RUN(test_floats);
    RUN(test_bools);
    RUN(test_strings);
    RUN(test_string_unterminated);
    RUN(test_bytes_int_suffix);
    RUN(test_enum_lookup);
    RUN(test_reserved_word_check);
    RUN(test_trailing_garbage);
    RUN(test_empty);
    RUN(test_reserved_other_rejected_as_literal);
    return 0;
}
