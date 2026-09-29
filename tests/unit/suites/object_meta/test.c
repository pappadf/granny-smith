// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Unit tests for the Meta class (`<path>.meta` introspection).
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

#include <stdio.h>
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
        {.kind = M_ATTR,
         .name = "meta",
         .flags = VAL_RO,
         .doc = "reserved name",
         .attr = {.type = V_UINT, .get = toy_get_pc, .set = NULL}},
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

// === Logical parents: callback-backed children have paths =================
//
// A small `scsi` tree shaped like the real one: an attached collection
// container `device` whose entries come from get(), each with a
// lookup-backed `image` child, plus a keyed collection `category`.

static struct object *g_dev_objs[8];
static struct object *g_img_objs[8];
static struct object *g_cat_objs[2];
static const char *g_cat_names[2] = {"cpu", "scsi"};

static const class_desc_t empty_class = {.name = "Empty", .members = NULL, .n_members = 0};

static struct object *img_lookup(struct object *self, const char *name) {
    (void)name;
    for (int i = 0; i < 8; i++)
        if (g_dev_objs[i] == self)
            return g_img_objs[i];
    return NULL;
}

static const member_t dev_members[] = {
    {.kind = M_CHILD, .name = "image", .doc = "Medium", .child = {.cls = &empty_class, .lookup = img_lookup}},
};
static const class_desc_t dev_class = {.name = "Dev", .members = dev_members, .n_members = 1};

static struct object *devs_get(struct object *self, int index) {
    (void)self;
    return (index >= 0 && index < 8) ? g_dev_objs[index] : NULL;
}

static const member_t devs_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .doc = "Devices",
     .child = {.cls = &dev_class, .indexed = true, .get = devs_get, .slots = 8}},
};
static const class_desc_t devs_class = {.name = "Devs", .members = devs_members, .n_members = 1};

static struct object *cats_lookup(struct object *self, const char *name) {
    (void)self;
    for (int i = 0; i < 2; i++)
        if (strcmp(name, g_cat_names[i]) == 0)
            return g_cat_objs[i];
    return NULL;
}

static int cats_keys(struct object *self, const char ***out) {
    (void)self;
    *out = g_cat_names;
    return 2;
}

static const member_t cats_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .doc = "Categories",
     .child = {.cls = &empty_class, .indexed = true, .lookup = cats_lookup, .keys = cats_keys}},
};
static const class_desc_t cats_class = {.name = "Cats", .members = cats_members, .n_members = 1};

// Resolve `path` + ".meta.path" and compare with `want`.
static bool path_is(const char *path, const char *want) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.meta.path", path);
    node_t n = object_resolve(object_root(), buf);
    if (!node_valid(n))
        return false;
    value_t v = node_get(n);
    bool ok = v.kind == V_STRING && v.s && strcmp(v.s, want) == 0;
    value_free(&v);
    return ok;
}

TEST(test_logical_parent_paths) {
    object_root_reset();
    struct object *scsi = object_new(&empty_class, NULL, "scsi");
    object_attach(object_root(), scsi);
    struct object *devs = object_new(&devs_class, NULL, "device");
    object_attach(scsi, devs);
    struct object *cats = object_new(&cats_class, NULL, "category");
    object_attach(object_root(), cats);
    memset(g_dev_objs, 0, sizeof(g_dev_objs));
    memset(g_img_objs, 0, sizeof(g_img_objs));
    g_dev_objs[3] = object_new(&dev_class, NULL, NULL);
    g_img_objs[3] = object_new(&empty_class, NULL, "image");
    object_set_logical_parent(g_dev_objs[3], devs, NULL, 3, NULL);
    object_set_logical_parent(g_img_objs[3], g_dev_objs[3], "image", -1, NULL);
    for (int i = 0; i < 2; i++) {
        g_cat_objs[i] = object_new(&empty_class, NULL, NULL);
        object_set_logical_parent(g_cat_objs[i], cats, NULL, -1, g_cat_names[i]);
    }

    ASSERT_TRUE(path_is("scsi.device", "scsi.device"));
    ASSERT_TRUE(path_is("scsi.device[3]", "scsi.device[3]"));
    ASSERT_TRUE(path_is("scsi.device[3].image", "scsi.device[3].image"));
    ASSERT_TRUE(path_is("category[\"scsi\"]", "category[\"scsi\"]"));
    ASSERT_TRUE(object_logical_parent(g_dev_objs[3]) == devs);
    ASSERT_TRUE(object_logical_index(g_dev_objs[3]) == 3);
    ASSERT_TRUE(object_logical_key(g_cat_objs[1]) && strcmp(object_logical_key(g_cat_objs[1]), "scsi") == 0);

    // A keyed collection counts its keys.
    node_t cn = object_resolve(object_root(), "category.count");
    ASSERT_TRUE(node_valid(cn));
    value_t c = node_get(cn);
    ASSERT_TRUE(c.kind == V_UINT && c.u == 2);

    // The parent going first clears the link: no dangling back-pointer, and
    // the child loses its path rather than printing a freed one.
    object_detach(devs);
    object_delete(devs);
    ASSERT_TRUE(object_logical_parent(g_dev_objs[3]) == NULL);
    char buf[64];
    object_compute_path(g_dev_objs[3], buf, sizeof(buf));
    ASSERT_TRUE(buf[0] == '\0');

    // The child going first unregisters itself from the parent.
    object_delete(g_img_objs[3]);
    object_delete(g_dev_objs[3]);
    for (int i = 0; i < 2; i++)
        object_delete(g_cat_objs[i]);
    object_detach(cats);
    object_delete(cats);
    object_detach(scsi);
    object_delete(scsi);
    object_root_reset();
}

TEST(test_resolver_adopts_unregistered_entries) {
    object_root_reset();
    struct object *devs = object_new(&devs_class, NULL, "device");
    object_attach(object_root(), devs);
    memset(g_dev_objs, 0, sizeof(g_dev_objs));
    g_dev_objs[5] = object_new(&dev_class, NULL, NULL);
    // No explicit registration: the first resolution adopts it.
    ASSERT_TRUE(path_is("device[5]", "device[5]"));
    ASSERT_TRUE(object_logical_parent(g_dev_objs[5]) == devs);
    object_delete(g_dev_objs[5]);
    g_dev_objs[5] = NULL;
    object_detach(devs);
    object_delete(devs);
    object_root_reset();
}

TEST(test_valid_keys) {
    ASSERT_TRUE(object_valid_key("scsi"));
    ASSERT_TRUE(object_valid_key("a-b.c_9"));
    ASSERT_TRUE(!object_valid_key(""));
    ASSERT_TRUE(!object_valid_key("has space"));
    ASSERT_TRUE(!object_valid_key("quote\""));
    char long_key[80];
    memset(long_key, 'k', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';
    ASSERT_TRUE(!object_valid_key(long_key));
}

// === Effective task and meta.members keys ================================

static const arg_decl_t tk_args[] = {
    {.name = "mode", .kind = V_ENUM, .enum_values = (const char *const[]){"a", "b", NULL}, .doc = "Mode"},
};

static const member_t tk_members[] = {
    {.kind = M_ATTR,
     .name = "x",
     .doc = "An x",
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = toy_get_pc}               },
    {.kind = M_METHOD,
     .name = "go",
     .doc = "Go",
     .method = {.args = tk_args, .nargs = 1, .result = V_NONE, .fn = toy_step}                },
    {.kind = M_METHOD,
     .name = "save",
     .doc = "Save",
     .method = {.task = "storage", .args = NULL, .nargs = 0, .result = V_NONE, .fn = toy_step}},
};
static const class_desc_t tk_class = {
    .name = "Tk", .members = tk_members, .n_members = 3, .doc = "A task node", .task = "debug"};

TEST(test_member_effective_task) {
    object_root_reset();
    struct object *top = object_new(&tk_class, NULL, "top");
    object_attach(object_root(), top);
    struct object *inner = object_new(&empty_class, NULL, "inner");
    object_attach(top, inner);
    // An attribute inherits its node's class task; a method's own wins.
    ASSERT_TRUE(strcmp(member_effective_task(top, &tk_members[0], NULL), "debug") == 0);
    ASSERT_TRUE(strcmp(member_effective_task(top, &tk_members[2], NULL), "storage") == 0);
    // A node without a task inherits from above; an object task overrides.
    ASSERT_TRUE(strcmp(member_effective_task(inner, NULL, NULL), "debug") == 0);
    object_set_task(inner, "io");
    ASSERT_TRUE(strcmp(member_effective_task(inner, NULL, NULL), "io") == 0);
    // A callback-backed entry walks to its logical parent.
    struct object *entry = object_new(&empty_class, NULL, NULL);
    object_set_logical_parent(entry, top, NULL, 0, NULL);
    ASSERT_TRUE(strcmp(member_effective_task(entry, NULL, NULL), "debug") == 0);
    // Nothing is inherited from the root.
    ASSERT_TRUE(member_effective_task(object_root(), NULL, NULL) == NULL);
    object_delete(entry);
    object_detach(inner);
    object_delete(inner);
    object_detach(top);
    object_delete(top);
    object_root_reset();
}

// A value's map entry by key, or NULL.
static const value_t *map_get(const value_t *m, const char *key) {
    if (!m || m->kind != V_MAP)
        return NULL;
    for (size_t i = 0; i < m->map.len; i++)
        if (strcmp(m->map.entries[i].key, key) == 0)
            return &m->map.entries[i].val;
    return NULL;
}

TEST(test_meta_members_keys) {
    object_root_reset();
    struct object *top = object_new(&tk_class, NULL, "top");
    object_attach(object_root(), top);
    node_t n = object_resolve(object_root(), "top.meta.members");
    ASSERT_TRUE(node_valid(n));
    value_t list = node_call(n, 0, NULL);
    ASSERT_TRUE(list.kind == V_LIST && list.list.len == 3);
    const value_t *x = &list.list.items[0];
    const value_t *type = map_get(x, "type");
    ASSERT_TRUE(type && type->kind == V_MAP);
    ASSERT_TRUE(strcmp(map_get(type, "kind")->s, "uint") == 0);
    ASSERT_TRUE(strcmp(map_get(type, "presentation")->s, "hex") == 0);
    ASSERT_TRUE(map_get(type, "enum")->kind == V_NONE);
    ASSERT_TRUE(strcmp(map_get(x, "task")->s, "debug") == 0);
    const value_t *go = &list.list.items[1];
    const value_t *args = map_get(go, "args");
    ASSERT_TRUE(args && args->kind == V_LIST && args->list.len == 1);
    const value_t *atype = map_get(&args->list.items[0], "type");
    ASSERT_TRUE(strcmp(map_get(atype, "kind")->s, "enum") == 0);
    ASSERT_TRUE(map_get(atype, "enum")->kind == V_LIST && map_get(atype, "enum")->list.len == 2);
    ASSERT_TRUE(map_get(&args->list.items[0], "optional")->kind == V_BOOL);
    ASSERT_TRUE(map_get(&args->list.items[0], "default")->kind == V_NONE);
    ASSERT_TRUE(strcmp(map_get(map_get(go, "result"), "kind")->s, "none") == 0);
    ASSERT_TRUE(strcmp(map_get(&list.list.items[2], "task")->s, "storage") == 0);
    value_free(&list);

    // The root's view of `top`: doc from the class, task, domain.
    node_t r = object_resolve(object_root(), "meta.members");
    value_t rl = node_call(r, 0, NULL);
    ASSERT_TRUE(rl.kind == V_LIST && rl.list.len == 1);
    ASSERT_TRUE(strcmp(map_get(&rl.list.items[0], "doc")->s, "A task node") == 0);
    ASSERT_TRUE(strcmp(map_get(&rl.list.items[0], "domain")->s, "emulator") == 0);
    ASSERT_TRUE(map_get(&rl.list.items[0], "collection")->kind == V_BOOL);
    value_free(&rl);
    object_detach(top);
    object_delete(top);
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
    RUN(test_logical_parent_paths);
    RUN(test_resolver_adopts_unregistered_entries);
    RUN(test_valid_keys);
    RUN(test_member_effective_task);
    RUN(test_meta_members_keys);
    return 0;
}
