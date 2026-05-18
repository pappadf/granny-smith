// Unit tests for the Meta class — proposal-introspection-via-meta-attribute.md.
//
// Covers:
//   - `<path>.meta` resolves to a synthetic Meta node bound to the path
//   - `meta` alone at the root resolves to the root's Meta node
//   - `cpu.meta.class`, `cpu.meta.path`, `cpu.meta.attributes`,
//     `cpu.meta.methods`, `cpu.meta.children` return the expected shapes
//   - self-introspection works: `<path>.meta.meta.attributes` lists the
//     Meta class's own attribute names
//   - cached Meta nodes survive across multiple lookups (same pointer)
//   - class registration rejects "meta" as a user-defined member name
//   - `meta.complete(...)` returns an empty list when no provider is
//     installed (tolerant degradation for unit-test contexts)

#include "meta.h"
#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>

// === Toy class with one attribute, one method, one child ==================

static int g_pc;
static value_t toy_get_pc(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, (uint64_t)g_pc);
}
static value_t toy_step(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    g_pc++;
    return val_none();
}

static const arg_decl_t toy_step_args[] = {
    {.name = "n", .kind = V_INT, .doc = "Steps"},
};

static const member_t toy_members[] = {
    {.kind = M_ATTR,
     .name = "pc",
     .doc = "Program counter",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = toy_get_pc, .set = NULL}},
    {.kind = M_METHOD,
     .name = "step",
     .doc = "Advance by N",
     .method = {.args = toy_step_args, .nargs = 1, .result = V_NONE, .fn = toy_step}},
};
static const class_desc_t toy_class = {
    .name = "Toy",
    .members = toy_members,
    .n_members = sizeof(toy_members) / sizeof(toy_members[0]),
};

// === Helpers ==============================================================

static struct object *attach_toy(const char *name) {
    struct object *o = object_new(&toy_class, NULL, name);
    object_attach(object_root(), o);
    return o;
}

// Find a V_STRING in a V_LIST.
static bool list_contains(const value_t *list, const char *name) {
    if (!list || list->kind != V_LIST || !name)
        return false;
    for (size_t i = 0; i < list->list.len; i++) {
        const value_t *v = &list->list.items[i];
        if (v->kind == V_STRING && v->s && strcmp(v->s, name) == 0)
            return true;
    }
    return false;
}

// === Tests ================================================================

TEST(test_meta_segment_resolves) {
    object_root_reset();
    attach_toy("toy");
    node_t n = object_resolve(object_root(), "toy.meta");
    ASSERT_TRUE(node_valid(n));
    ASSERT_TRUE(object_class(n.obj) == meta_class());
    object_root_reset();
}

TEST(test_root_meta_resolves) {
    object_root_reset();
    node_t n = object_resolve(object_root(), "meta");
    ASSERT_TRUE(node_valid(n));
    ASSERT_TRUE(object_class(n.obj) == meta_class());
    object_root_reset();
}

TEST(test_meta_class_returns_class_name) {
    object_root_reset();
    attach_toy("toy");
    node_t n = object_resolve(object_root(), "toy.meta.class");
    ASSERT_TRUE(node_valid(n));
    value_t v = node_get(n);
    ASSERT_TRUE(v.kind == V_STRING);
    ASSERT_TRUE(v.s && strcmp(v.s, "Toy") == 0);
    value_free(&v);
    object_root_reset();
}

TEST(test_meta_path_returns_inspected_path) {
    object_root_reset();
    attach_toy("toy");
    node_t n = object_resolve(object_root(), "toy.meta.path");
    ASSERT_TRUE(node_valid(n));
    value_t v = node_get(n);
    ASSERT_TRUE(v.kind == V_STRING);
    ASSERT_TRUE(v.s && strcmp(v.s, "toy") == 0);
    value_free(&v);
    object_root_reset();
}

TEST(test_meta_attributes_and_methods_lists) {
    object_root_reset();
    attach_toy("toy");

    node_t a = object_resolve(object_root(), "toy.meta.attributes");
    ASSERT_TRUE(node_valid(a));
    value_t alist = node_get(a);
    ASSERT_TRUE(alist.kind == V_LIST);
    ASSERT_TRUE(list_contains(&alist, "pc"));
    ASSERT_TRUE(!list_contains(&alist, "step")); // methods not in attributes
    value_free(&alist);

    node_t m = object_resolve(object_root(), "toy.meta.methods");
    ASSERT_TRUE(node_valid(m));
    value_t mlist = node_get(m);
    ASSERT_TRUE(mlist.kind == V_LIST);
    ASSERT_TRUE(list_contains(&mlist, "step"));
    ASSERT_TRUE(!list_contains(&mlist, "pc"));
    value_free(&mlist);

    object_root_reset();
}

TEST(test_root_meta_children_includes_attached) {
    object_root_reset();
    attach_toy("toy_a");
    attach_toy("toy_b");
    node_t n = object_resolve(object_root(), "meta.children");
    ASSERT_TRUE(node_valid(n));
    value_t list = node_get(n);
    ASSERT_TRUE(list.kind == V_LIST);
    ASSERT_TRUE(list_contains(&list, "toy_a"));
    ASSERT_TRUE(list_contains(&list, "toy_b"));
    value_free(&list);
    object_root_reset();
}

TEST(test_meta_meta_self_introspection) {
    object_root_reset();
    attach_toy("toy");
    node_t n = object_resolve(object_root(), "toy.meta.meta.attributes");
    ASSERT_TRUE(node_valid(n));
    value_t list = node_get(n);
    ASSERT_TRUE(list.kind == V_LIST);
    // The Meta class itself declares class/doc/path/children/attributes/methods.
    ASSERT_TRUE(list_contains(&list, "class"));
    ASSERT_TRUE(list_contains(&list, "path"));
    ASSERT_TRUE(list_contains(&list, "children"));
    ASSERT_TRUE(list_contains(&list, "attributes"));
    ASSERT_TRUE(list_contains(&list, "methods"));
    value_free(&list);
    object_root_reset();
}

TEST(test_meta_node_cached) {
    object_root_reset();
    struct object *toy = attach_toy("toy");
    node_t n1 = object_resolve(object_root(), "toy.meta");
    node_t n2 = object_resolve(object_root(), "toy.meta");
    ASSERT_TRUE(n1.obj == n2.obj); // same cached node returned both times
    // The cache lives on the inspected object's private slot.
    ASSERT_TRUE(object_get_meta(toy) == n1.obj);
    object_root_reset();
}

TEST(test_class_with_meta_member_rejected) {
    static const member_t bad_members[] = {
        {.kind = M_ATTR, .name = "meta", .flags = VAL_RO, .attr = {.type = V_UINT, .get = toy_get_pc, .set = NULL}},
    };
    static const class_desc_t bad_class = {
        .name = "Bad",
        .members = bad_members,
        .n_members = 1,
    };
    char err[200];
    ASSERT_TRUE(!object_validate_class(&bad_class, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "reserved") != NULL || strstr(err, "meta") != NULL);
}

TEST(test_meta_complete_returns_empty_without_provider) {
    object_root_reset();
    attach_toy("toy");
    // No provider installed in this test process — the method must still
    // resolve and return an (empty) list rather than crashing.
    node_t n = object_resolve(object_root(), "meta.complete");
    ASSERT_TRUE(node_valid(n));
    value_t args[2] = {val_str("toy.p"), val_int(5)};
    value_t result = node_call(n, 2, args);
    ASSERT_TRUE(result.kind == V_LIST);
    ASSERT_TRUE(result.list.len == 0);
    value_free(&args[0]);
    value_free(&args[1]);
    value_free(&result);
    object_root_reset();
}

int main(void) {
    RUN(test_meta_segment_resolves);
    RUN(test_root_meta_resolves);
    RUN(test_meta_class_returns_class_name);
    RUN(test_meta_path_returns_inspected_path);
    RUN(test_meta_attributes_and_methods_lists);
    RUN(test_root_meta_children_includes_attached);
    RUN(test_meta_meta_self_introspection);
    RUN(test_meta_node_cached);
    RUN(test_class_with_meta_member_rejected);
    RUN(test_meta_complete_returns_empty_without_provider);
    return 0;
}
