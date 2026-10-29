// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Unit tests for the Lua bridge (src/core/lua/gs_lua.c): a probe class
// attached to the root, read, written and called from Lua chunks; the value
// round trips both ways; named arguments; errors raised and caught; the
// globals rule; vars; the embedded library.

#include "gs_lua.h"
#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- The probe class -----------------------------------------------------------

typedef struct {
    int64_t n;
    char last_str[64]; // what set_s stored
    size_t last_list_len; // what echo_list received
} probe_t;

static probe_t g_probe;

static DEF_GETTER(probe_get_n) {
    return val_int(((probe_t *)object_data(self))->n);
}

static DEF_SETTER(probe_set_n) {
    probe_t *p = (probe_t *)object_data(self);
    bool ok;
    int64_t v = val_as_i64(&in, &ok);
    value_free(&in);
    if (!ok)
        return val_err("n: not a number");
    p->n = v;
    return val_none();
}

static DEF_GETTER(probe_get_s) {
    return val_str("hello");
}

static DEF_SETTER(probe_set_s) {
    probe_t *p = (probe_t *)object_data(self);
    snprintf(p->last_str, sizeof p->last_str, "%s", in.s ? in.s : "");
    value_free(&in);
    return val_none();
}

static DEF_GETTER(probe_get_items) {
    value_t *items = (value_t *)calloc(3, sizeof(value_t));
    items[0] = val_int(10);
    items[1] = val_none();
    items[2] = val_str("x");
    return val_list(items, 3);
}

static DEF_GETTER(probe_get_m) {
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "b", val_int(2));
    val_map_put(b, "a", val_int(1));
    return val_map_finish(b);
}

static const char *const probe_enum[] = {"one", "two", NULL};

static DEF_GETTER(probe_get_e) {
    return val_enum(1, probe_enum, 2);
}

static DEF_GETTER(probe_get_bytes) {
    return val_bytes("\x01\x00\x02", 3);
}

static const value_t probe_scale_default = {.kind = VK_INT, .i = 1};

static const arg_decl_t probe_add_args[] = {
    {.name = "a", .kind = VK_INT, .doc = "a"},
    {.name = "b", .kind = VK_INT, .doc = "b"},
    {.name = "scale",
     .kind = VK_INT,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .default_value = &probe_scale_default,
     .doc = "scale"},
};

// add(a, b, scale=1) -> (a + b) * scale
static DEF_METHOD(probe_add) {
    (void)argc;
    return val_int((argv[0].i + argv[1].i) * argv[2].i);
}

static DEF_METHOD(probe_fail) {
    return val_err("boom");
}

static const arg_decl_t probe_list_args[] = {
    {.name = "l", .kind = VK_LIST, .doc = "a list"},
};

// echo_list(l) -> its length; remembers it.
static DEF_METHOD(probe_echo_list) {
    (void)argc;
    ((probe_t *)object_data(self))->last_list_len = argv[0].list.len;
    return val_int((int64_t)argv[0].list.len);
}

static const arg_decl_t probe_map_args[] = {
    {.name = "m", .kind = VK_MAP, .doc = "a map"},
};

// map_keys(m) -> the keys in order, joined with commas.
static DEF_METHOD(probe_map_keys) {
    (void)argc;
    char buf[256] = {0};
    for (size_t i = 0; i < argv[0].map.len; i++) {
        if (i)
            strncat(buf, ",", sizeof buf - strlen(buf) - 1);
        strncat(buf, argv[0].map.entries[i].key, sizeof buf - strlen(buf) - 1);
    }
    return val_str(buf);
}

static const member_t probe_members[] = {
    {.kind = MK_ATTR,   .name = "n",     .doc = "n",     .attr = {.type = VK_INT, .get = probe_get_n, .set = probe_set_n}   },
    {.kind = MK_ATTR,   .name = "s",     .doc = "s",     .attr = {.type = VK_STRING, .get = probe_get_s, .set = probe_set_s}},
    {.kind = MK_ATTR,   .name = "items", .doc = "items", .attr = {.type = VK_LIST, .get = probe_get_items}                  },
    {.kind = MK_ATTR,   .name = "m",     .doc = "m",     .attr = {.type = VK_MAP, .get = probe_get_m}                       },
    {.kind = MK_ATTR,
     .name = "e",
     .doc = "e",
     .attr = {.type = VK_ENUM, .enum_values = probe_enum, .get = probe_get_e}                                               },
    {.kind = MK_ATTR,   .name = "raw",   .doc = "raw",   .attr = {.type = VK_BYTES, .get = probe_get_bytes}                 },
    {.kind = MK_METHOD,
     .name = "add",
     .doc = "add",
     .method = {.args = probe_add_args, .nargs = 3, .result = VK_INT, .fn = probe_add}                                      },
    {.kind = MK_METHOD, .name = "fail",  .doc = "fail",  .method = {.result = VK_NONE, .fn = probe_fail}                    },
    {.kind = MK_METHOD,
     .name = "echo_list",
     .doc = "echo_list",
     .method = {.args = probe_list_args, .nargs = 1, .result = VK_INT, .fn = probe_echo_list}                               },
    {.kind = MK_METHOD,
     .name = "map_keys",
     .doc = "map_keys",
     .method = {.args = probe_map_args, .nargs = 1, .result = VK_STRING, .fn = probe_map_keys}                              },
};

static const class_desc_t probe_class = {
    .name = "Probe",
    .members = probe_members,
    .n_members = sizeof(probe_members) / sizeof(probe_members[0]),
    .doc = "the bridge's probe",
};

static void install_probe(void) {
    static bool installed;
    if (installed)
        return;
    installed = true;
    object_attach(object_root(), object_new(&probe_class, &g_probe, "probe"));
}

// Run a chunk; 0 ended, 1 quit, -1 failed.
static int run(const char *src) {
    install_probe();
    return gs_lua_run_source(src, false);
}

// --- Tests -------------------------------------------------------------------------

TEST(test_read_and_write_attribute) {
    g_probe.n = 41;
    ASSERT_EQ_INT(0, run("assert(probe.n == 41); probe.n = probe.n + 1"));
    ASSERT_EQ_INT(42, (int)g_probe.n);
    ASSERT_EQ_INT(0, run("probe.s = 'stored'"));
    ASSERT_TRUE(strcmp(g_probe.last_str, "stored") == 0);
}

TEST(test_string_enum_bytes) {
    ASSERT_EQ_INT(0, run("assert(probe.s == 'hello'); assert(probe.e == 'two'); assert(probe.raw == '\\1\\0\\2')"));
}

TEST(test_list_is_one_based_with_none_sentinel) {
    ASSERT_EQ_INT(0, run("local l = probe.items; assert(#l == 3); assert(l[1] == 10); "
                         "assert(gs.is_none(l[2])); assert(l[3] == 'x')"));
}

TEST(test_map_keeps_insertion_order) {
    ASSERT_EQ_INT(0, run("local ks = {} for k, v in pairs(probe.m) do ks[#ks + 1] = k .. '=' .. v end "
                         "assert(table.concat(ks, ',') == 'b=2,a=1', table.concat(ks, ','))"));
    // ...and carries it back to the tree: a tree map is a value, not named
    // arguments, and so is a table gs.map tags.
    ASSERT_EQ_INT(0, run("assert(probe.map_keys(probe.m) == 'b,a')"));
    ASSERT_EQ_INT(0, run("assert(probe.map_keys(gs.map{only = 1}) == 'only')"));
}

TEST(test_call_positional_and_named) {
    ASSERT_EQ_INT(0, run("assert(probe.add(2, 3) == 5)"));
    ASSERT_EQ_INT(0, run("assert(probe.add(2, 3, {scale = 10}) == 50)"));
    ASSERT_EQ_INT(0, run("assert(probe.add{a = 1, b = 2, scale = 3} == 9)"));
    // A missing required argument is the validator's error, raised.
    ASSERT_EQ_INT(0, run("assert(not gs.ok(function() return probe.add(1) end))"));
}

TEST(test_list_argument) {
    ASSERT_EQ_INT(0, run("assert(probe.echo_list({1, 2, 3}) == 3)"));
    ASSERT_EQ_INT(3, (int)g_probe.last_list_len);
    ASSERT_EQ_INT(0, run("assert(probe.echo_list({}) == 0)"));
}

TEST(test_error_is_raised_and_catchable) {
    ASSERT_EQ_INT(-1, run("probe.fail()"));
    ASSERT_EQ_INT(0, run("local ok, e = pcall(probe.fail); assert(not ok); assert(e.text == 'boom'); "
                         "assert(tostring(e) == 'boom')"));
    ASSERT_EQ_INT(0, run("assert(gs.try(function() return probe.fail() end, 'fb') == 'fb')"));
    ASSERT_EQ_INT(0,
                  run("assert(gs.try(function() return probe.fail() end, function(e) return e.text end) == 'boom')"));
    ASSERT_EQ_INT(-1, run("local x = probe.nosuch"));
    ASSERT_EQ_INT(0, run("assert(not gs.ok(function() return probe.nosuch end))"));
}

TEST(test_globals_cannot_shadow_the_tree) {
    ASSERT_EQ_INT(-1, run("probe = 1"));
    ASSERT_EQ_INT(0, run("assert(gs.is_node(probe)); assert(gs.path(probe) == 'probe')"));
    ASSERT_EQ_INT(0, run("assert(nosuchglobal == nil)"));
}

TEST(test_proxy_identity_and_tostring) {
    ASSERT_EQ_INT(0, run("local a, b = probe, gs.node('probe'); assert(a == b); assert(tostring(a) == 'probe')"));
    ASSERT_EQ_INT(0, run("assert(gs.node('probe.n') == probe.n)"));
    ASSERT_EQ_INT(0, run("assert(gs.fmt('probe.s') == 'hello')"));
}

TEST(test_vars_and_library) {
    gs_lua_var_set("ROW", "x");
    ASSERT_EQ_INT(0, run("assert(vars.ROW == 'x'); assert(vars.NOPE == nil); assert(vars.TMP_DIR == 'tmp')"));
    ASSERT_EQ_INT(0, run("local v = 0xab; assert(F'${v:08x}' == '000000ab'); assert(F'[${v:8X}]' == '[      AB]')"));
    ASSERT_EQ_INT(0, run("local v = 0xab; assert(F'${v:d}-${probe.s}-${#probe.items}' == '171-hello-3')"));
    ASSERT_EQ_INT(0, run("assert(#gs.bytes(0x0102, 2) == 2); assert(tostring(gs.bytes(0x0102, 2)) == '0102')"));
    ASSERT_EQ_INT(0, run("assert(gs.hex(255, 4) == '00ff'); assert(gs.contains('abc', 'b'))"));
    ASSERT_EQ_INT(0, run("assert(debug == nil or gs.is_node(debug)); assert(type(luadebug.getinfo) == 'function')"));
}

TEST(test_quit_and_interactive) {
    ASSERT_EQ_INT(1, run("quit()"));
    // A quit passes through gs.try.
    ASSERT_EQ_INT(1, run("gs.try(function() quit() end, 0)"));
    install_probe();
    ASSERT_EQ_INT(0, gs_lua_run_source("1 + 1", true));
    ASSERT_EQ_INT(0, gs_lua_run_source("local x = 2", true));
    ASSERT_EQ_INT(-1, gs_lua_run_source("this is not lua", true));
}

TEST(test_interrupt_unwinds) {
    gs_lua_interrupt();
    ASSERT_EQ_INT(-1, run("while true do end"));
    ASSERT_EQ_INT(0, run("assert(true)"));
}

int main(void) {
    RUN(test_read_and_write_attribute);
    RUN(test_string_enum_bytes);
    RUN(test_list_is_one_based_with_none_sentinel);
    RUN(test_map_keeps_insertion_order);
    RUN(test_call_positional_and_named);
    RUN(test_list_argument);
    RUN(test_error_is_raised_and_catchable);
    RUN(test_globals_cannot_shadow_the_tree);
    RUN(test_proxy_identity_and_tostring);
    RUN(test_vars_and_library);
    RUN(test_quit_and_interactive);
    RUN(test_interrupt_unwinds);
    gs_lua_shutdown();
    return 0;
}
