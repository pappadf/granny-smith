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

#include "lint.h"
#include "meta.h"
#include "object.h"
#include "status.h"
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
    {.name = "n", .kind = VK_INT, .doc = "Steps"},
};

static const member_t toy_members[] = {
    {.kind = MK_ATTR,
     .name = "pc",
     .doc = "Program counter",
     .attr = {.type = VK_UINT, .get = toy_get_pc, .set = NULL}                       },
    {.kind = MK_METHOD,
     .name = "step",
     .doc = "Advance by N",
     .method = {.args = toy_step_args, .nargs = 1, .result = VK_NONE, .fn = toy_step}},
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

// Find a VK_STRING in a VK_LIST.
static bool list_contains(const value_t *list, const char *name) {
    if (!list || list->kind != VK_LIST || !name)
        return false;
    for (size_t i = 0; i < list->list.len; i++) {
        const value_t *v = &list->list.items[i];
        if (v->kind == VK_STRING && v->s && strcmp(v->s, name) == 0)
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
    ASSERT_TRUE(v.kind == VK_STRING);
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
    ASSERT_TRUE(v.kind == VK_STRING);
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
    ASSERT_TRUE(alist.kind == VK_LIST);
    ASSERT_TRUE(list_contains(&alist, "pc"));
    ASSERT_TRUE(!list_contains(&alist, "step")); // methods not in attributes
    value_free(&alist);

    node_t m = object_resolve(object_root(), "toy.meta.methods");
    ASSERT_TRUE(node_valid(m));
    value_t mlist = node_get(m);
    ASSERT_TRUE(mlist.kind == VK_LIST);
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
    ASSERT_TRUE(list.kind == VK_LIST);
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
    ASSERT_TRUE(list.kind == VK_LIST);
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
        {.kind = MK_ATTR,
         .name = "meta",
         .doc = "reserved name",
         .attr = {.type = VK_UINT, .get = toy_get_pc, .set = NULL}},
    };
    static const class_desc_t bad_class = {
        .name = "Bad",
        .members = bad_members,
        .n_members = 1,
    };
    char err[200];
    ASSERT_TRUE(object_validate_class(&bad_class, err, sizeof(err)) != STATUS_OK);
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
    ASSERT_TRUE(result.kind == VK_LIST);
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
    {.kind = MK_CHILD, .name = "image", .doc = "Medium", .child = {.cls = &empty_class, .lookup = img_lookup}},
};
static const class_desc_t dev_class = {.name = "Dev", .members = dev_members, .n_members = 1};

static struct object *devs_get(struct object *self, int index) {
    (void)self;
    return (index >= 0 && index < 8) ? g_dev_objs[index] : NULL;
}

static const collection_desc_t devs_entries = {
    .entry = &dev_class, .by_index = {.get = devs_get, .slots = 8}
};

static const member_t devs_members[] = {
    {.kind = MK_CHILD, .name = "entries", .doc = "Devices", .child = {.collection = &devs_entries}},
};
static const class_desc_t devs_class = {.name = "Devs", .members = devs_members, .n_members = 1};

static struct object *cats_lookup(struct object *self, const char *name) {
    (void)self;
    for (int i = 0; i < 2; i++)
        if (strcmp(name, g_cat_names[i]) == 0)
            return g_cat_objs[i];
    return NULL;
}

static const char *cats_next_key(struct object *self, const char *prev) {
    (void)self;
    if (!prev)
        return g_cat_names[0];
    return strcmp(prev, g_cat_names[0]) == 0 ? g_cat_names[1] : NULL;
}

static const collection_desc_t cats_entries = {
    .entry = &empty_class, .by_key = {.lookup = cats_lookup, .next_key = cats_next_key}
};

static const member_t cats_members[] = {
    {.kind = MK_CHILD, .name = "entries", .doc = "Categories", .child = {.collection = &cats_entries}},
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
    bool ok = v.kind == VK_STRING && v.s && strcmp(v.s, want) == 0;
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
    ASSERT_TRUE(c.kind == VK_UINT && c.u == 2);

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

// === meta.members keys ====================================================

static const arg_decl_t tk_args[] = {
    {.name = "mode", .kind = VK_ENUM, .enum_values = (const char *const[]){"a", "b", NULL}, .doc = "Mode"},
};

static const member_t tk_members[] = {
    {.kind = MK_ATTR,
     .name = "x",
     .doc = "An x",
     .attr = {.type = VK_UINT, .presentation_flags = VFLAG_HEX, .get = toy_get_pc}},
    {.kind = MK_METHOD,
     .name = "go",
     .doc = "Go",
     .method = {.args = tk_args, .nargs = 1, .result = VK_NONE, .fn = toy_step}   },
    {.kind = MK_METHOD,
     .name = "save",
     .doc = "Save",
     .method = {.args = NULL, .nargs = 0, .result = VK_NONE, .fn = toy_step}      },
};
static const class_desc_t tk_class = {.name = "Tk", .members = tk_members, .n_members = 3, .doc = "A toy node"};

// Lint: each example must pass the caller's check.
static const member_t lx_members[] = {
    {.kind = MK_METHOD,
     .name = "bare",
     .doc = "No examples",
     .method = {.args = NULL, .nargs = 0, .result = VK_NONE, .fn = toy_step}},
    {.kind = MK_METHOD,
     .name = "good",
     .examples = (const char *const[]){"lx.good", NULL},
     .doc = "A resolving example",
     .method = {.args = NULL, .nargs = 0, .result = VK_NONE, .fn = toy_step}},
    {.kind = MK_METHOD,
     .name = "bad",
     .examples = (const char *const[]){"lx.good", "lx.bda", NULL},
     .doc = "One example that does not resolve",
     .method = {.args = NULL, .nargs = 0, .result = VK_NONE, .fn = toy_step}},
};
static const class_desc_t lx_class = {.name = "Lx", .members = lx_members, .n_members = 3, .doc = "A lint node"};

static bool example_ok(const char *example) {
    return strstr(example, "bda") == NULL;
}

static bool has_line(const value_t *list, const char *line) {
    for (size_t i = 0; i < list->list.len; i++)
        if (strcmp(list->list.items[i].s, line) == 0)
            return true;
    return false;
}

TEST(test_lint_examples) {
    object_root_reset();
    struct object *lx = object_new(&lx_class, NULL, "lx");
    object_attach(object_root(), lx);
    value_t out = object_lint_members(example_ok);
    ASSERT_EQ_INT(VK_LIST, out.kind);
    ASSERT_TRUE(has_line(&out, "lx.bad: example does not resolve: lx.bda"));
    ASSERT_EQ_INT(1, (int)out.list.len);
    value_free(&out);
    // Without a check, nothing is reported (a method needs no examples).
    out = object_lint_members(NULL);
    ASSERT_EQ_INT(0, (int)out.list.len);
    value_free(&out);
    object_detach(lx);
    object_delete(lx);
    object_root_reset();
}

// Lint: a class's gaps are reported once, under its first object's path, and
// a basic-tier node needs a doc.
static const arg_decl_t lg_args[] = {
    {.name = "x", .kind = VK_ANY, .doc = "Anything"},
};
static const member_t lg_members[] = {
    {.kind = MK_METHOD,
     .name = "m",
     .doc = "Untyped argument",
     .method = {.args = lg_args, .nargs = 1, .result = VK_NONE, .fn = toy_step}},
};
static const class_desc_t lg_class = {.name = "Lg", .members = lg_members, .n_members = 1, .doc = "A lint node"};
static const class_desc_t undocumented_class = {.name = "Undoc", .members = NULL, .n_members = 0};

TEST(test_lint_once_per_class) {
    object_root_reset();
    struct object *a = object_new(&lg_class, NULL, "lg1");
    struct object *b = object_new(&lg_class, NULL, "lg2");
    struct object *kid = object_new(&undocumented_class, NULL, "kid");
    object_attach(object_root(), a);
    object_attach(object_root(), b);
    object_attach(b, kid);
    value_t out = object_lint_members(NULL);
    ASSERT_EQ_INT(2, (int)out.list.len);
    ASSERT_TRUE(has_line(&out, "lg1.m arg 'x': untyped argument (VK_ANY/VK_NONE) without OBJ_ARG_POLY"));
    ASSERT_TRUE(has_line(&out, "lg2.kid: node shown in the basic tier has no doc"));
    value_free(&out);
    object_root_reset();
    object_delete(kid);
    object_delete(a);
    object_delete(b);
}

// object_walk: every object once, with its canonical path -- entries under
// their container, named children under their member.
typedef struct {
    char seen[16][64];
    int n;
} walk_seen_t;

static bool walk_note(struct object *o, const member_t *via, const char *path, bool basic, bool first, void *ud) {
    (void)o;
    (void)via;
    (void)basic;
    (void)first;
    walk_seen_t *w = (walk_seen_t *)ud;
    if (w->n < 16)
        snprintf(w->seen[w->n++], 64, "%s", path);
    return true;
}

static bool walk_saw(const walk_seen_t *w, const char *path) {
    for (int i = 0; i < w->n; i++)
        if (strcmp(w->seen[i], path) == 0)
            return true;
    return false;
}

TEST(test_walk_canonical_paths) {
    object_root_reset();
    struct object *devs = object_new(&devs_class, NULL, "devs");
    object_attach(object_root(), devs);
    memset(g_dev_objs, 0, sizeof(g_dev_objs));
    memset(g_img_objs, 0, sizeof(g_img_objs));
    g_dev_objs[2] = object_new(&dev_class, NULL, NULL);
    for (int i = 0; i < 2; i++)
        g_cat_objs[i] = object_new(&empty_class, NULL, NULL);
    struct object *cats = object_new(&cats_class, NULL, "cats");
    object_attach(object_root(), cats);
    walk_seen_t w = {0};
    const object_visitor_t v = {.object = walk_note};
    object_walk(object_root(), &v, &w);
    ASSERT_TRUE(walk_saw(&w, ""));
    ASSERT_TRUE(walk_saw(&w, "devs"));
    ASSERT_TRUE(walk_saw(&w, "devs[2]"));
    ASSERT_TRUE(walk_saw(&w, "cats[\"cpu\"]"));
    ASSERT_TRUE(walk_saw(&w, "cats[\"scsi\"]"));
    object_root_reset();
    object_delete(g_dev_objs[2]);
    g_dev_objs[2] = NULL;
    for (int i = 0; i < 2; i++)
        object_delete(g_cat_objs[i]);
    object_delete(devs);
    object_delete(cats);
}

// A value's map entry by key, or NULL.
static const value_t *map_get(const value_t *m, const char *key) {
    if (!m || m->kind != VK_MAP)
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
    ASSERT_TRUE(list.kind == VK_LIST && list.list.len == 3);
    const value_t *x = &list.list.items[0];
    const value_t *type = map_get(x, "type");
    ASSERT_TRUE(type && type->kind == VK_MAP);
    ASSERT_TRUE(strcmp(map_get(type, "kind")->s, "uint") == 0);
    ASSERT_TRUE(strcmp(map_get(type, "presentation")->s, "hex") == 0);
    ASSERT_TRUE(map_get(type, "enum")->kind == VK_NONE);
    ASSERT_TRUE(map_get(x, "task") == NULL);
    const value_t *go = &list.list.items[1];
    const value_t *args = map_get(go, "args");
    ASSERT_TRUE(args && args->kind == VK_LIST && args->list.len == 1);
    const value_t *atype = map_get(&args->list.items[0], "type");
    ASSERT_TRUE(strcmp(map_get(atype, "kind")->s, "enum") == 0);
    ASSERT_TRUE(map_get(atype, "enum")->kind == VK_LIST && map_get(atype, "enum")->list.len == 2);
    ASSERT_TRUE(map_get(&args->list.items[0], "optional")->kind == VK_BOOL);
    ASSERT_TRUE(map_get(&args->list.items[0], "default")->kind == VK_NONE);
    ASSERT_TRUE(strcmp(map_get(map_get(go, "result"), "kind")->s, "none") == 0);
    value_free(&list);

    // The root's view of `top`: doc from the class, domain.
    node_t r = object_resolve(object_root(), "meta.members");
    value_t rl = node_call(r, 0, NULL);
    ASSERT_TRUE(rl.kind == VK_LIST && rl.list.len == 1);
    ASSERT_TRUE(strcmp(map_get(&rl.list.items[0], "doc")->s, "A toy node") == 0);
    ASSERT_TRUE(strcmp(map_get(&rl.list.items[0], "domain")->s, "emulator") == 0);
    ASSERT_TRUE(map_get(&rl.list.items[0], "collection")->kind == VK_BOOL);
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
    RUN(test_meta_members_keys);
    RUN(test_lint_examples);
    RUN(test_lint_once_per_class);
    RUN(test_walk_canonical_paths);
    return 0;
}
