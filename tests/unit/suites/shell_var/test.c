// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Unit tests for the shell binding store (shell_var.c).
//
//   - every binder refuses a name longer than the readers accept
//   - a binding watches every object its value holds -- a VK_OBJECT, and
//     one inside a list, a map or a nested list -- and reads as stale once
//     any of them is destroyed, instead of handing out a dangling pointer
//   - overwriting or freeing a binding unregisters its watches (run under
//     ASan: a forgotten one is a callback into freed memory)

#include "object.h"
#include "shell_var.h"
#include "test_assert.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>

static const class_desc_t toy_class = {.name = "toy", .members = NULL, .n_members = 0};

// A list of the given items (ownership moves into the list).
static value_t list_of(const value_t *items, size_t n) {
    value_t *a = (value_t *)calloc(n, sizeof(*a));
    for (size_t i = 0; i < n; i++)
        a[i] = items[i];
    return val_list(a, n);
}

// True when `$name` reads as the destroyed-object error.
static bool reads_stale(const char *name) {
    value_t v = shell_binding_get(name);
    bool stale = val_is_error(&v) && v.err && strstr(v.err, "destroyed") != NULL;
    value_free(&v);
    return stale;
}

// === Name length =============================================================

TEST(test_overlong_names_refused) {
    char name[OBJ_IDENT_MAX + 2];
    memset(name, 'n', sizeof(name) - 1);
    name[OBJ_IDENT_MAX + 1] = '\0'; // one over the limit
    char err[160] = "";
    ASSERT_TRUE(shell_binding_let(name, val_int(1), err, sizeof(err)) < 0);
    ASSERT_TRUE(strstr(err, "too long") != NULL);
    err[0] = '\0';
    ASSERT_TRUE(shell_var_set(name, "x", err, sizeof(err)) < 0);
    ASSERT_TRUE(strstr(err, "too long") != NULL);
    // --var takes the same rule as let for any other unreadable name
    ASSERT_TRUE(shell_var_set("1abc", "x", err, sizeof(err)) < 0);
    ASSERT_TRUE(shell_var_set("if", "x", err, sizeof(err)) < 0);

    name[OBJ_IDENT_MAX] = '\0'; // exactly the limit
    ASSERT_TRUE(shell_binding_let(name, val_int(1), err, sizeof(err)) == 0);
    ASSERT_TRUE(shell_var_set(name, "x", err, sizeof(err)) == 0);
    shell_binding_remove_top(name);
}

// === Objects inside a stored value ===========================================

TEST(test_scalar_object_goes_stale) {
    struct object *o = object_new(&toy_class, NULL, "o");
    char err[160];
    ASSERT_TRUE(shell_binding_let("x", val_obj(o), err, sizeof(err)) == 0);
    ASSERT_TRUE(!reads_stale("x"));
    object_delete(o);
    ASSERT_TRUE(reads_stale("x"));
    shell_binding_remove_top("x");
}

TEST(test_list_item_goes_stale) {
    struct object *a = object_new(&toy_class, NULL, "a");
    struct object *b = object_new(&toy_class, NULL, "b");
    char err[160];
    value_t v = list_of((value_t[]){val_int(1), val_obj(a), val_obj(b)}, 3);
    ASSERT_TRUE(shell_binding_let("xs", v, err, sizeof(err)) == 0);
    ASSERT_TRUE(!reads_stale("xs"));
    object_delete(b);
    ASSERT_TRUE(reads_stale("xs"));
    // Freeing the binding unregisters a's watch but not b's (already fired)
    shell_binding_remove_top("xs");
    object_delete(a); // ASan: no callback into the freed binding
}

TEST(test_nested_list_and_map_go_stale) {
    struct object *a = object_new(&toy_class, NULL, "a");
    struct object *c = object_new(&toy_class, NULL, "c");
    char err[160];
    // [[a]]
    value_t inner = list_of((value_t[]){val_obj(a)}, 1);
    ASSERT_TRUE(shell_binding_let("nest", list_of((value_t[]){inner}, 1), err, sizeof(err)) == 0);
    // {k: c}
    struct value_entry *e = (struct value_entry *)calloc(1, sizeof(*e));
    e->key = strdup("k");
    e->val = val_obj(c);
    ASSERT_TRUE(shell_binding_let("m", val_map(e, 1), err, sizeof(err)) == 0);

    object_delete(a);
    ASSERT_TRUE(reads_stale("nest"));
    ASSERT_TRUE(!reads_stale("m"));
    object_delete(c);
    ASSERT_TRUE(reads_stale("m"));
    shell_binding_remove_top("nest");
    shell_binding_remove_top("m");
}

// Overwriting a binding drops the old value's watches and arms the new.
TEST(test_overwrite_rearms) {
    struct object *a = object_new(&toy_class, NULL, "a");
    struct object *b = object_new(&toy_class, NULL, "b");
    char err[160];
    ASSERT_TRUE(shell_binding_let("w", list_of((value_t[]){val_obj(a), val_obj(a)}, 2), err, sizeof(err)) == 0);
    ASSERT_TRUE(shell_binding_mutate("w", list_of((value_t[]){val_obj(b)}, 1)) == 0);
    object_delete(a); // no longer held: the binding stays live
    ASSERT_TRUE(!reads_stale("w"));
    object_delete(b);
    ASSERT_TRUE(reads_stale("w"));
    // A stale binding rebound to a plain value is live again
    ASSERT_TRUE(shell_binding_mutate("w", val_int(7)) == 0);
    value_t v = shell_binding_get("w");
    ASSERT_TRUE(v.kind == VK_INT && v.i == 7);
    value_free(&v);
    shell_binding_remove_top("w");
}

// Popping a function scope frees its bindings and their watches.
TEST(test_scope_pop_unregisters) {
    struct object *a = object_new(&toy_class, NULL, "a");
    char err[160];
    ASSERT_TRUE(shell_binding_push_scope() == 0);
    ASSERT_TRUE(shell_binding_let("p", list_of((value_t[]){val_obj(a)}, 1), err, sizeof(err)) == 0);
    shell_binding_pop_scope();
    object_delete(a); // ASan: no callback into the freed binding
}

// shell.vars walks the table: a stale entry is handed out as its error,
// never as the value with its dangling pointers.
static bool each_cb(const char *name, const value_t *v, void *ud) {
    if (strcmp(name, "sv") == 0)
        *(bool *)ud = val_is_error(v);
    return true;
}

TEST(test_each_hides_stale_value) {
    struct object *a = object_new(&toy_class, NULL, "a");
    char err[160];
    ASSERT_TRUE(shell_binding_let("sv", list_of((value_t[]){val_obj(a)}, 1), err, sizeof(err)) == 0);
    bool was_err = true;
    shell_var_each(each_cb, &was_err);
    ASSERT_TRUE(!was_err);
    object_delete(a);
    shell_var_each(each_cb, &was_err);
    ASSERT_TRUE(was_err);
    shell_binding_remove_top("sv");
}

int main(void) {
    shell_var_init();
    RUN(test_overlong_names_refused);
    RUN(test_scalar_object_goes_stale);
    RUN(test_list_item_goes_stale);
    RUN(test_nested_list_and_map_go_stale);
    RUN(test_overwrite_rearms);
    RUN(test_scope_pop_unregisters);
    RUN(test_each_hides_stale_value);
    return 0;
}
