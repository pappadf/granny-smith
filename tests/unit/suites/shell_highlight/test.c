// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Unit tests for shell.highlight (src/core/shell/highlight.c).
//
// A toy tree -- machine.cpu.pc, machine.floppy.drive[0..1] with insert,
// scheduler.mode (an enum) and run, debug.breakpoints.add with an enum
// argument -- and the alias $pc.  A 30-line corpus is checked as
// `text:class` pairs: statement forms, argument vs expression mode,
// unresolved paths, enums, strings with interpolation, partial input.
// Byte offsets are checked on non-ASCII text.

#include "alias.h"
#include "commands.h"
#include "highlight.h"
#include "object.h"
#include "shell_funcs.h"
#include "test_assert.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// === Toy tree ===================================================================

static value_t get_zero(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, 0);
}
static value_t get_mode(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_enum(0, NULL, 0);
}
static value_t set_any(struct object *self, const member_t *m, value_t v) {
    (void)self;
    (void)m;
    value_free(&v);
    return val_none();
}
static value_t method_none(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_none();
}

static const member_t cpu_members[] = {
    {.kind = M_ATTR, .name = "pc", .doc = "PC", .attr = {.type = V_UINT, .get = get_zero, .set = set_any}},
};
static const class_desc_t cpu_class = {.name = "cpu", .members = cpu_members, .n_members = 1};

static const arg_decl_t insert_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Image"},
    {.name = "writable", .kind = V_BOOL, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Writable"},
};
static const member_t drive_members[] = {
    {.kind = M_METHOD,
     .name = "insert",
     .doc = "Insert",
     .method = {.args = insert_args, .nargs = 2, .result = V_NONE, .fn = method_none}},
};
static const class_desc_t drive_class = {.name = "drive", .members = drive_members, .n_members = 1};

static struct object *g_drives[2];
static struct object *drive_get(struct object *self, int i) {
    (void)self;
    return (i >= 0 && i < 2) ? g_drives[i] : NULL;
}
static int drive_next(struct object *self, int prev) {
    (void)self;
    return prev + 1 < 2 ? prev + 1 : -1;
}
static const collection_desc_t floppy_entries = {
    .entry = &drive_class, .by_index = {.get = drive_get, .next = drive_next}
};

static const member_t floppy_members[] = {
    {.kind = M_CHILD, .name = "drive", .doc = "Drives", .child = {.collection = &floppy_entries}},
};
static const class_desc_t floppy_class = {.name = "floppy", .members = floppy_members, .n_members = 1};
static const class_desc_t machine_class = {.name = "machine", .members = NULL, .n_members = 0};

static const char *const k_modes[] = {"paced", "accelerated", "turbo", NULL};
static const arg_decl_t run_args[] = {
    {.name = "n", .kind = V_UINT, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Instructions"},
};
static const member_t sched_members[] = {
    {.kind = M_ATTR,
     .name = "mode",
     .doc = "Mode",
     .attr = {.type = V_ENUM, .get = get_mode, .set = set_any, .enum_values = k_modes}},
    {.kind = M_METHOD,
     .name = "run",
     .doc = "Run",
     .method = {.args = run_args, .nargs = 1, .result = V_NONE, .fn = method_none}    },
};
static const class_desc_t sched_class = {.name = "scheduler", .members = sched_members, .n_members = 2};

static const char *const k_spaces[] = {"logical", "physical", NULL};
static const arg_decl_t add_args[] = {
    {.name = "addr", .kind = V_UINT, .doc = "Address"},
    {.name = "condition", .kind = V_STRING, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Condition"},
    {.name = "space", .kind = V_ENUM, .validation_flags = OBJ_ARG_OPTIONAL, .enum_values = k_spaces, .doc = "Space"},
};
static const member_t bp_members[] = {
    {.kind = M_METHOD,
     .name = "add",
     .doc = "Add",
     .method = {.args = add_args, .nargs = 3, .result = V_NONE, .fn = method_none}},
};
static const class_desc_t bp_class = {.name = "breakpoints", .members = bp_members, .n_members = 1};
static const class_desc_t debug_class = {.name = "debug", .members = NULL, .n_members = 0};

static const arg_decl_t echo_args[] = {
    {.name = "args", .kind = V_ANY, .validation_flags = OBJ_ARG_REST | OBJ_ARG_OPTIONAL, .doc = "Words"},
};
static const member_t util_members[] = {
    {.kind = M_METHOD,
     .name = "echo",
     .doc = "Echo",
     .method = {.args = echo_args, .nargs = 1, .result = V_NONE, .fn = method_none}},
};
static const class_desc_t util_class = {.name = "util", .members = util_members, .n_members = 1};

// The function registry, stubbed: `myfn` is a def function.
static int g_fn_token;
script_func_t *shell_func_find(const char *name) {
    return strcmp(name, "myfn") == 0 ? (script_func_t *)&g_fn_token : NULL;
}
void shell_func_release(script_func_t *f) {
    (void)f;
}

static void build_tree(void) {
    object_root_reset();
    alias_reset();
    struct object *machine = object_new(&machine_class, NULL, "machine");
    object_attach(object_root(), machine);
    object_attach(machine, object_new(&cpu_class, NULL, "cpu"));
    object_attach(machine, object_new(&floppy_class, NULL, "floppy"));
    g_drives[0] = object_new(&drive_class, NULL, NULL);
    g_drives[1] = object_new(&drive_class, NULL, NULL);
    object_attach(object_root(), object_new(&sched_class, NULL, "scheduler"));
    struct object *debug = object_new(&debug_class, NULL, "debug");
    object_attach(object_root(), debug);
    object_attach(debug, object_new(&bp_class, NULL, "breakpoints"));
    object_attach(object_root(), object_new(&util_class, NULL, "util"));
    char err[128];
    alias_register_builtin("pc", "machine.cpu.pc", err, sizeof(err));
    // `bp` is a command running debug.breakpoints.add.
    shell_command_clear_user();
    if (shell_command_define("bp", "debug.breakpoints.add", err, sizeof(err)) != 0)
        fprintf(stderr, "command bp: %s\n", err);
}

// === Rendering ====================================================================

static uint64_t map_u(const value_t *m, const char *key) {
    for (size_t i = 0; i < m->map.len; i++)
        if (strcmp(m->map.entries[i].key, key) == 0)
            return m->map.entries[i].val.u;
    return 0;
}
static const char *map_s(const value_t *m, const char *key) {
    for (size_t i = 0; i < m->map.len; i++)
        if (strcmp(m->map.entries[i].key, key) == 0)
            return m->map.entries[i].val.s;
    return "";
}

// The spans of `line` as "text:class text:class …"; also checks that the
// spans are ordered, non-overlapping and inside the line.
static void render(const char *line, char *out, size_t size) {
    value_t v = shell_highlight(line);
    ASSERT_EQ_INT(V_LIST, v.kind);
    size_t n = 0, prev_end = 0, len = strlen(line);
    out[0] = '\0';
    for (size_t i = 0; i < v.list.len; i++) {
        const value_t *m = &v.list.items[i];
        size_t s = (size_t)map_u(m, "start"), e = (size_t)map_u(m, "end");
        ASSERT_TRUE(s < e && e <= len && s >= prev_end);
        prev_end = e;
        n += (size_t)snprintf(out + n, size - n, "%s%.*s:%s", i ? " " : "", (int)(e - s), line + s, map_s(m, "class"));
    }
    value_free(&v);
}

static int g_failures;

static void check(const char *line, const char *want) {
    char got[2048];
    render(line, got, sizeof(got));
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "  line: %s\n  want: %s\n  got:  %s\n", line, want, got);
        g_failures++;
    }
}

// === Tests =====================================================================

TEST(test_corpus) {
    build_tree();
    g_failures = 0;
    // Paths: resolved segments by kind; the first unresolved one and all after.
    check("machine.cpu.pc", "machine:object cpu:object pc:attribute");
    check("machine.flopy.drive", "machine:object flopy:unknown drive:unknown");
    check("ghost", "ghost:unknown");
    // Indexed entries and argument mode (bare words are strings).
    check("machine.floppy.drive[0].insert /opfs/a.img writable=true",
          "machine:object floppy:object drive:object [:operator 0:number ]:operator insert:method "
          "writable:attribute =:operator true:keyword");
    check("machine.floppy.drive[7].insert x", "machine:object floppy:object drive:object [:operator 7:number "
                                              "]:operator insert:unknown");
    check("util.echo hello 42 0x1f", "util:object echo:method 42:number 0x1f:number");
    // A command word is its method, with the method's arguments.
    check("bp 0x40 cond physical", "bp:method 0x40:number physical:enum");
    check("command bp2 = debug.breakpoints.add",
          "command:decl bp2:method =:operator debug:object breakpoints:object add:method");
    check("bq 1", "bq:unknown 1:number");
    // Enums: an argument matching its declared values, positionally or by name.
    check("debug.breakpoints.add 0x40 cond physical",
          "debug:object breakpoints:object add:method 0x40:number physical:enum");
    check("debug.breakpoints.add 0x40 space=logical",
          "debug:object breakpoints:object add:method 0x40:number space:attribute =:operator logical:enum");
    check("debug.breakpoints.add 0x40 space=nowhere",
          "debug:object breakpoints:object add:method 0x40:number space:attribute =:operator");
    check("scheduler.mode = turbo", "scheduler:object mode:attribute =:operator turbo:enum");
    check("scheduler.mode = turbo + 1", "scheduler:object mode:attribute =:operator turbo:unknown +:operator 1:number");
    // Bindings, declarations, aliases.
    check("let x = $pc + 0x10", "let:decl x:variable =:operator $pc:alias +:operator 0x10:number");
    check("$x = $x * 2", "$x:variable =:operator $x:variable *:operator 2:number");
    check("$pc = 0x400", "$pc:alias =:operator 0x400:number");
    check("alias d = machine.floppy.drive[0]",
          "alias:decl d:alias =:operator machine:object floppy:object drive:object [:operator 0:number ]:operator");
    check("$d.insert disk.img", "$d:variable");
    // Keyword forms.
    check("if $x > 3 {", "if:keyword $x:variable >:operator 3:number {:operator");
    check("} elif $x == 0 {", "}:operator elif:keyword $x:variable ==:operator 0:number {:operator");
    check("} else {", "}:operator else:keyword {:operator");
    check("for i in 0..4 {", "for:keyword i:variable in:keyword 0:number ..:operator 4:number {:operator");
    check("def twice(a, b) {",
          "def:decl twice:method (:operator a:variable ,:operator b:variable ):operator {:operator");
    check("return a * 2", "return:keyword a:unknown *:operator 2:number");
    check("assert len($x) == 3 \"three\"",
          "assert:keyword len:method (:operator $x:variable ):operator ==:operator 3:number \"three\":string");
    check("while true { break }", "while:keyword true:keyword {:operator break:keyword }:operator");
    // Functions, call forms, strings, comments.
    check("myfn 1 2", "myfn:method 1:number 2:number");
    check("try(machine.cpu.pc, 0)",
          "try:method (:operator machine:object cpu:object pc:attribute ,:operator 0:number ):operator");
    check("util.echo \"pc=${$pc:08x} $x\" 'raw $x' # note",
          "util:object echo:method \"pc=:string ${:interp $pc:alias :08x}:interp  :string $x:variable \":string "
          "'raw $x':string # note:comment");
    check("let s = [1, \"two\", 3.5e2]",
          "let:decl s:variable =:operator [:operator 1:number ,:operator \"two\":string ,:operator 3.5e2:number "
          "]:operator");
    // Partial input: whatever lexes, never an error.
    check("util.echo \"unterminated ${machine.cp", "util:object echo:method \"unterminated :string ${:interp "
                                                   "machine:object cp:unknown");
    check("machine.floppy.drive[", "machine:object floppy:object drive:object [:operator");
    check("", "");
    ASSERT_EQ_INT(0, g_failures);
    object_root_reset();
}

TEST(test_offsets_are_utf8_bytes) {
    build_tree();
    // "ü" is two bytes: the string span covers 13 bytes, the number after it
    // starts at byte 14.
    value_t v = shell_highlight("util.echo \"über\" 5");
    ASSERT_EQ_INT(V_LIST, v.kind);
    ASSERT_EQ_INT(4, (int)v.list.len);
    ASSERT_EQ_INT(10, (int)map_u(&v.list.items[2], "start"));
    ASSERT_EQ_INT(17, (int)map_u(&v.list.items[2], "end"));
    ASSERT_TRUE(strcmp(map_s(&v.list.items[2], "class"), "string") == 0);
    ASSERT_EQ_INT(18, (int)map_u(&v.list.items[3], "start"));
    value_free(&v);
    object_root_reset();
}

TEST(test_blocks_span_lines) {
    build_tree();
    g_failures = 0;
    check("if $x {\n  util.echo 1\n} else {\n  $x = 2 # two\n}",
          "if:keyword $x:variable {:operator util:object echo:method 1:number }:operator else:keyword {:operator "
          "$x:variable =:operator 2:number # two:comment }:operator");
    ASSERT_EQ_INT(0, g_failures);
    object_root_reset();
}

int main(void) {
    RUN(test_corpus);
    RUN(test_offsets_are_utf8_bytes);
    RUN(test_blocks_span_lines);
    return 0;
}
