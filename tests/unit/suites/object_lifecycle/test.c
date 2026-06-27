// Unit tests for the system-object-model substrate additions
// (proposal-system-object-model.md §6–§7):
//
//   - cascade delete over owning (attached) edges, post-order
//   - per-object destructor (frees the C struct behind instance_data)
//   - reference edges do NOT cascade (callback-backed, never attached)
//   - display label, ordering weight, visibility category accessors
//   - ordered attached-child iteration honours (order, attach_seq)
//   - member-level metadata (visibility category, method UI flags) is
//     readable off the static class table
//   - the meta node surfaces label / category

#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>

// === Destructor bookkeeping =================================================
//
// Each toy object's destructor appends its name to a global ordered log so
// tests can assert post-order teardown and that references were skipped.

#define MAX_LOG 32
static const char *g_dtor_log[MAX_LOG];
static int g_dtor_count;

static void reset_log(void) {
    g_dtor_count = 0;
    for (int i = 0; i < MAX_LOG; i++)
        g_dtor_log[i] = NULL;
}

static void recording_dtor(struct object *o) {
    if (g_dtor_count < MAX_LOG)
        g_dtor_log[g_dtor_count++] = object_name(o);
}

// Index of `name` in the destructor log, or -1 if it never ran.
static int log_index(const char *name) {
    for (int i = 0; i < g_dtor_count; i++)
        if (g_dtor_log[i] && strcmp(g_dtor_log[i], name) == 0)
            return i;
    return -1;
}

// === Toy classes ============================================================

static const class_desc_t toy_class = {
    .name = "toy",
    .members = NULL,
    .n_members = 0,
};

// Build a recording toy object: a destructor that logs its name on teardown.
static struct object *toy(const char *name) {
    struct object *o = object_new(&toy_class, NULL, name);
    object_set_destructor(o, recording_dtor);
    return o;
}

// A class with a single reference child resolved via a lookup callback. The
// target lives outside this object's owned subtree, so cascade must not free
// it.
static struct object *g_ref_target;
static struct object *ref_lookup(struct object *self, const char *name) {
    (void)self;
    (void)name;
    return g_ref_target;
}
static const member_t reftoy_members[] = {
    {.kind = M_CHILD,
     .name = "source",
     .doc = "Reference to a node this object points at but does not own",
     .child = {.reference = true, .lookup = ref_lookup}},
};
static const class_desc_t reftoy_class = {
    .name = "reftoy",
    .members = reftoy_members,
    .n_members = sizeof(reftoy_members) / sizeof(reftoy_members[0]),
};

// A class exercising the new member-level metadata (visibility category +
// method UI flags + verb label + task category + ordering weight).
static value_t meta_method_stub(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_none();
}
static const member_t metmeta_members[] = {
    {.kind = M_ATTR, .name = "shown", .doc = "basic attribute", .attr = {.type = V_UINT, .get = NULL, .set = NULL}},
    {.kind = M_ATTR,
     .name = "raw_reg",
     .flags = M_CAT_ADVANCED,
     .label = "Raw register",
     .attr = {.type = V_UINT, .get = NULL, .set = NULL}},
    {.kind = M_ATTR,
     .name = "saved_phase",
     .flags = M_CAT_INTERNAL,
     .attr = {.type = V_UINT, .get = NULL, .set = NULL}},
    {.kind = M_METHOD,
     .name = "export",
     .order = 5,
     .method = {.args = NULL,
                .nargs = 0,
                .result = V_NONE,
                .fn = meta_method_stub,
                .ui_flags = MM_MUTATE,
                .verb_label = "Save image…",
                .task_category = "storage"}},
    {.kind = M_METHOD,
     .name = "eject",
     .method = {.args = NULL,
                .nargs = 0,
                .result = V_NONE,
                .fn = meta_method_stub,
                .ui_flags = MM_DESTRUCTIVE | MM_MUTATE,
                .task_category = "storage"}},
};
static const class_desc_t metmeta_class = {
    .name = "metmeta",
    .members = metmeta_members,
    .n_members = sizeof(metmeta_members) / sizeof(metmeta_members[0]),
};

// === Tests ==================================================================

// object_delete runs the per-object destructor before freeing the wrapper.
TEST(test_destructor_runs_on_delete) {
    reset_log();
    struct object *o = toy("solo");
    object_delete(o);
    ASSERT_EQ_INT(1, g_dtor_count);
    ASSERT_TRUE(log_index("solo") == 0);
}

// object_delete_tree frees the whole owned subtree post-order: every child
// is torn down before its parent.
TEST(test_cascade_post_order) {
    reset_log();
    struct object *root = toy("root");
    struct object *a = toy("a");
    struct object *a1 = toy("a1");
    struct object *b = toy("b");
    object_attach(root, a);
    object_attach(a, a1);
    object_attach(root, b);

    object_delete_tree(root);

    // All four wrappers freed exactly once.
    ASSERT_EQ_INT(4, g_dtor_count);
    // Post-order invariant: children strictly precede their owning parent.
    ASSERT_TRUE(log_index("a1") >= 0 && log_index("a1") < log_index("a"));
    ASSERT_TRUE(log_index("a") < log_index("root"));
    ASSERT_TRUE(log_index("b") < log_index("root"));
}

// Reference edges are callback-backed and never attached, so cascade leaves
// the referenced target alive.
TEST(test_reference_not_cascaded) {
    reset_log();
    object_root_reset();

    struct object *owner = object_new(&reftoy_class, NULL, "owner");
    object_attach(object_root(), owner);

    struct object *target = toy("target");
    object_attach(object_root(), target); // a sibling, not owned by `owner`
    g_ref_target = target;

    // The reference resolves to the target through the lookup callback.
    node_t n = object_resolve(object_root(), "owner.source");
    ASSERT_TRUE(node_valid(n));
    value_t v = node_get(n);
    ASSERT_TRUE(v.kind == V_OBJECT && v.obj == target);
    value_free(&v);

    // Cascading the owner must not touch the referenced target.
    object_delete_tree(owner);
    ASSERT_TRUE(log_index("target") < 0); // target destructor did NOT run

    // The target is still alive and resolvable on its own.
    node_t nt = object_resolve(object_root(), "target");
    ASSERT_TRUE(node_valid(nt));

    g_ref_target = NULL;
    object_root_reset();
    object_delete(target); // now its destructor runs
    ASSERT_TRUE(log_index("target") >= 0);
}

// Label falls back to name and can be overridden; order/category round-trip.
TEST(test_label_order_category) {
    struct object *o = object_new(&toy_class, NULL, "machine");
    ASSERT_TRUE(strcmp(object_label(o), "machine") == 0); // fallback to name
    object_set_label(o, "Macintosh IIcx");
    ASSERT_TRUE(strcmp(object_label(o), "Macintosh IIcx") == 0);

    ASSERT_EQ_INT(0, object_order(o)); // default
    object_set_order(o, 7);
    ASSERT_EQ_INT(7, object_order(o));

    ASSERT_EQ_INT(M_CAT_BASIC, object_category(o)); // default
    object_set_category(o, M_CAT_ADVANCED);
    ASSERT_EQ_INT(M_CAT_ADVANCED, object_category(o));

    object_delete(o);
}

// Ordered iteration visits attached children by (order, attach_seq).
static const char *g_visit_log[MAX_LOG];
static int g_visit_count;
static void visit_cb(struct object *parent, struct object *child, void *ud) {
    (void)parent;
    (void)ud;
    if (g_visit_count < MAX_LOG)
        g_visit_log[g_visit_count++] = object_name(child);
}

TEST(test_ordered_iteration) {
    struct object *parent = object_new(&toy_class, NULL, "parent");
    // Attach in scrambled order; assign explicit ordering weights.
    struct object *c_mid = object_new(&toy_class, NULL, "mid");
    struct object *c_first = object_new(&toy_class, NULL, "first");
    struct object *c_last = object_new(&toy_class, NULL, "last");
    object_attach(parent, c_mid);
    object_attach(parent, c_first);
    object_attach(parent, c_last);
    object_set_order(c_first, -10);
    object_set_order(c_mid, 0);
    object_set_order(c_last, 100);

    g_visit_count = 0;
    object_each_attached_ordered(parent, visit_cb, NULL);
    ASSERT_EQ_INT(3, g_visit_count);
    ASSERT_TRUE(strcmp(g_visit_log[0], "first") == 0);
    ASSERT_TRUE(strcmp(g_visit_log[1], "mid") == 0);
    ASSERT_TRUE(strcmp(g_visit_log[2], "last") == 0);

    // Equal weight → stable by attach order. c_mid and c_first both 0 below.
    object_set_order(c_last, 0);
    object_set_order(c_first, 0);
    object_set_order(c_mid, 0);
    g_visit_count = 0;
    object_each_attached_ordered(parent, visit_cb, NULL);
    // Attach order was: mid, first, last → that is the tiebreak sequence.
    ASSERT_TRUE(strcmp(g_visit_log[0], "mid") == 0);
    ASSERT_TRUE(strcmp(g_visit_log[1], "first") == 0);
    ASSERT_TRUE(strcmp(g_visit_log[2], "last") == 0);

    object_delete(c_mid);
    object_delete(c_first);
    object_delete(c_last);
    object_delete(parent);
}

// Member-level metadata is readable off the static class table.
TEST(test_member_metadata) {
    const member_t *shown = class_find_member(&metmeta_class, "shown");
    const member_t *raw = class_find_member(&metmeta_class, "raw_reg");
    const member_t *saved = class_find_member(&metmeta_class, "saved_phase");
    const member_t *exp = class_find_member(&metmeta_class, "export");
    const member_t *ej = class_find_member(&metmeta_class, "eject");
    ASSERT_TRUE(shown && raw && saved && exp && ej);

    // Visibility categories.
    ASSERT_EQ_INT(M_CAT_BASIC, shown->flags & M_CAT_MASK);
    ASSERT_EQ_INT(M_CAT_ADVANCED, raw->flags & M_CAT_MASK);
    ASSERT_EQ_INT(M_CAT_INTERNAL, saved->flags & M_CAT_MASK);

    // Display label + ordering weight.
    ASSERT_TRUE(raw->label && strcmp(raw->label, "Raw register") == 0);
    ASSERT_EQ_INT(5, exp->order);

    // Method UI metadata.
    ASSERT_TRUE((exp->method.ui_flags & MM_MUTATE) != 0);
    ASSERT_TRUE((exp->method.ui_flags & MM_DESTRUCTIVE) == 0);
    ASSERT_TRUE(exp->method.verb_label && strcmp(exp->method.verb_label, "Save image…") == 0);
    ASSERT_TRUE(exp->method.task_category && strcmp(exp->method.task_category, "storage") == 0);
    ASSERT_TRUE((ej->method.ui_flags & MM_DESTRUCTIVE) != 0);
}

// The meta node surfaces label and category as resolvable attributes.
TEST(test_meta_label_category) {
    object_root_reset();
    struct object *o = object_new(&toy_class, NULL, "widget");
    object_set_label(o, "Widget 9000");
    object_set_category(o, M_CAT_ADVANCED);
    object_attach(object_root(), o);

    node_t nl = object_resolve(object_root(), "widget.meta.label");
    ASSERT_TRUE(node_valid(nl));
    value_t vl = node_get(nl);
    ASSERT_TRUE(vl.kind == V_STRING && strcmp(vl.s, "Widget 9000") == 0);
    value_free(&vl);

    node_t nc = object_resolve(object_root(), "widget.meta.category");
    ASSERT_TRUE(node_valid(nc));
    value_t vc = node_get(nc);
    ASSERT_TRUE(vc.kind == V_STRING && strcmp(vc.s, "advanced") == 0);
    value_free(&vc);

    object_root_reset();
    object_delete(o);
}

int main(void) {
    RUN(test_destructor_runs_on_delete);
    RUN(test_cascade_post_order);
    RUN(test_reference_not_cascaded);
    RUN(test_label_order_category);
    RUN(test_ordered_iteration);
    RUN(test_member_metadata);
    RUN(test_meta_label_category);
    return 0;
}
