// Unit tests for the alias table (proposal §4.4).
//
// Covers:
//   - registration of built-in and user aliases
//   - built-in immutability (cannot be removed)
//   - closed-namespace resolution (no fallthrough to root children)
//   - reserved-word rejection at registration
//   - duplicate handling and replacement semantics

#include "alias.h"
#include "object.h"
#include "test_assert.h"

#include <stdlib.h>
#include <string.h>

// Each test starts from a clean alias table.
static void reset(void) {
    alias_reset();
}

TEST(test_register_builtin_basic) {
    reset();
    char err[160];
    ASSERT_EQ_INT(0, alias_register_builtin("pc", "cpu.pc", err, sizeof(err)));
    const char *p = alias_lookup("pc", NULL);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(strcmp(p, "cpu.pc") == 0);
    ASSERT_EQ_INT(1, (int)alias_count());
}

TEST(test_register_builtin_idempotent) {
    reset();
    char err[160];
    ASSERT_EQ_INT(0, alias_register_builtin("pc", "cpu.pc", err, sizeof(err)));
    // Same (name, path) — no-op.
    ASSERT_EQ_INT(0, alias_register_builtin("pc", "cpu.pc", err, sizeof(err)));
    ASSERT_EQ_INT(1, (int)alias_count());
    // Different path for an existing built-in — rejected.
    ASSERT_TRUE(alias_register_builtin("pc", "other.path", err, sizeof(err)) < 0);
    ASSERT_TRUE(strstr(err, "built-in") != NULL);
}

TEST(test_user_add_and_lookup) {
    reset();
    char err[160];
    ASSERT_EQ_INT(0, alias_add_user("foo", "cpu.d0", err, sizeof(err)));
    alias_kind_t k = ALIAS_BUILTIN;
    const char *p = alias_lookup("foo", &k);
    ASSERT_TRUE(p && strcmp(p, "cpu.d0") == 0);
    ASSERT_EQ_INT(ALIAS_USER, k);
}

TEST(test_user_replace) {
    reset();
    char err[160];
    ASSERT_EQ_INT(0, alias_add_user("foo", "cpu.d0", err, sizeof(err)));
    // Replacing a user alias is allowed and changes the target.
    ASSERT_EQ_INT(0, alias_add_user("foo", "cpu.d1", err, sizeof(err)));
    const char *p = alias_lookup("foo", NULL);
    ASSERT_TRUE(p && strcmp(p, "cpu.d1") == 0);
}

TEST(test_user_collides_with_builtin) {
    reset();
    char err[160];
    ASSERT_EQ_INT(0, alias_register_builtin("pc", "cpu.pc", err, sizeof(err)));
    int rc = alias_add_user("pc", "user.thing", err, sizeof(err));
    ASSERT_TRUE(rc < 0);
    ASSERT_TRUE(strstr(err, "built-in") != NULL);
    // Built-in is still intact.
    ASSERT_TRUE(strcmp(alias_lookup("pc", NULL), "cpu.pc") == 0);
}

TEST(test_remove_user_works_builtin_immutable) {
    reset();
    char err[160];
    ASSERT_EQ_INT(0, alias_register_builtin("pc", "cpu.pc", err, sizeof(err)));
    ASSERT_EQ_INT(0, alias_add_user("foo", "cpu.d0", err, sizeof(err)));

    // User alias removes cleanly.
    ASSERT_EQ_INT(0, alias_remove_user("foo", err, sizeof(err)));
    ASSERT_TRUE(alias_lookup("foo", NULL) == NULL);

    // Built-in cannot be removed.
    int rc = alias_remove_user("pc", err, sizeof(err));
    ASSERT_TRUE(rc < 0);
    ASSERT_TRUE(strstr(err, "built-in") != NULL);
    ASSERT_TRUE(alias_lookup("pc", NULL) != NULL);
}

TEST(test_remove_unknown_fails) {
    reset();
    char err[160];
    int rc = alias_remove_user("ghost", err, sizeof(err));
    ASSERT_TRUE(rc < 0);
    ASSERT_TRUE(strstr(err, "no such") != NULL);
}

TEST(test_reserved_word_rejected) {
    reset();
    char err[160];
    ASSERT_TRUE(alias_register_builtin("while", "cpu.pc", err, sizeof(err)) < 0);
    ASSERT_TRUE(strstr(err, "reserved") != NULL);

    ASSERT_TRUE(alias_add_user("true", "cpu.pc", err, sizeof(err)) < 0);
    ASSERT_TRUE(strstr(err, "reserved") != NULL);

    // Boolean-literal spellings are reserved (proposal §2.3).
    ASSERT_TRUE(alias_add_user("on", "cpu.pc", err, sizeof(err)) < 0);
    ASSERT_TRUE(alias_add_user("yes", "cpu.pc", err, sizeof(err)) < 0);
}

// Closed-namespace: a `$name` lookup must NOT silently fall through
// to "try `name` as a root child." That's the proposal §4.4.2 rule.
// We assert that alias_lookup returns NULL for a name that has no
// alias entry, even if the name happens to coincide with a class
// member somewhere in the tree.
TEST(test_closed_namespace) {
    reset();
    char err[160];
    ASSERT_EQ_INT(0, alias_register_builtin("pc", "cpu.pc", err, sizeof(err)));
    // `cpu` is a path identifier but never an alias: lookup must miss.
    ASSERT_TRUE(alias_lookup("cpu", NULL) == NULL);
    // Random name with no alias.
    ASSERT_TRUE(alias_lookup("ghost", NULL) == NULL);
}

TEST(test_clear_user_keeps_builtin) {
    reset();
    char err[160];
    ASSERT_EQ_INT(0, alias_register_builtin("pc", "cpu.pc", err, sizeof(err)));
    ASSERT_EQ_INT(0, alias_register_builtin("d0", "cpu.d0", err, sizeof(err)));
    ASSERT_EQ_INT(0, alias_add_user("foo", "cpu.d1", err, sizeof(err)));
    ASSERT_EQ_INT(0, alias_add_user("bar", "cpu.d2", err, sizeof(err)));

    alias_clear_user();
    ASSERT_EQ_INT(2, (int)alias_count()); // only built-ins remain
    ASSERT_TRUE(alias_lookup("pc", NULL) != NULL);
    ASSERT_TRUE(alias_lookup("d0", NULL) != NULL);
    ASSERT_TRUE(alias_lookup("foo", NULL) == NULL);
    ASSERT_TRUE(alias_lookup("bar", NULL) == NULL);
}

TEST(test_invalid_identifier_rejected) {
    reset();
    char err[160];
    // Empty / non-identifier / leading digit.
    ASSERT_TRUE(alias_register_builtin("", "cpu.pc", err, sizeof(err)) < 0);
    ASSERT_TRUE(alias_register_builtin("1foo", "cpu.pc", err, sizeof(err)) < 0);
    ASSERT_TRUE(alias_register_builtin("foo.bar", "cpu.pc", err, sizeof(err)) < 0);
    ASSERT_TRUE(alias_register_builtin("foo-bar", "cpu.pc", err, sizeof(err)) < 0);
    ASSERT_EQ_INT(0, (int)alias_count());
}

// Iteration order: registration order. Useful for `shell.alias.list`
// to print stable output.
typedef struct {
    const char *names[8];
    int count;
} collect_t;
static bool collect_cb(const char *name, const char *path, alias_kind_t kind, void *ud) {
    (void)path;
    (void)kind;
    collect_t *c = (collect_t *)ud;
    if (c->count < 8)
        c->names[c->count++] = name;
    return true;
}

TEST(test_iteration_order) {
    reset();
    char err[160];
    alias_register_builtin("pc", "cpu.pc", err, sizeof(err));
    alias_register_builtin("d0", "cpu.d0", err, sizeof(err));
    alias_add_user("foo", "cpu.d1", err, sizeof(err));
    collect_t c = {0};
    alias_each(collect_cb, &c);
    ASSERT_EQ_INT(3, c.count);
    ASSERT_TRUE(strcmp(c.names[0], "pc") == 0);
    ASSERT_TRUE(strcmp(c.names[1], "d0") == 0);
    ASSERT_TRUE(strcmp(c.names[2], "foo") == 0);
}

int main(void) {
    RUN(test_register_builtin_basic);
    RUN(test_register_builtin_idempotent);
    RUN(test_user_add_and_lookup);
    RUN(test_user_replace);
    RUN(test_user_collides_with_builtin);
    RUN(test_remove_user_works_builtin_immutable);
    RUN(test_remove_unknown_fails);
    RUN(test_reserved_word_rejected);
    RUN(test_closed_namespace);
    RUN(test_clear_user_keeps_builtin);
    RUN(test_invalid_identifier_rejected);
    RUN(test_iteration_order);
    return 0;
}
