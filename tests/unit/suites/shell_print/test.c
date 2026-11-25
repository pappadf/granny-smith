// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the REPL's object printing (shell.c).  See Makefile.
//
// The attribute table used to call each member's getter directly, so it
// skipped the presentation_flags merge node_get does: a getter that relied
// on its member's VFLAG_HEX printed in decimal there, and a VFLAG_SENSITIVE
// attribute printed its payload in clear.

#include "object.h"
#include "shell_internal.h"
#include "test_assert.h"
#include "value.h"

#include <stdio.h>
#include <string.h>

// === The captured output ====================================================

static char s_out[4096];
static size_t s_out_len;

// The output sink (out.h): collect what the shell prints
void out_route(const char *text, size_t len) {
    if (s_out_len + len >= sizeof s_out)
        len = sizeof s_out - 1 - s_out_len;
    memcpy(s_out + s_out_len, text, len);
    s_out_len += len;
    s_out[s_out_len] = '\0';
}

// Forget what was captured
static void out_reset(void) {
    s_out_len = 0;
    s_out[0] = '\0';
}

// === The shell's other collaborators, unreached here ========================

struct completion;
struct cpu;
struct scheduler;
struct value;

bool job_annotate(const char *kind, const struct value *fields, const struct value *reduced, bool *used_reduced) {
    (void)kind, (void)fields, (void)reduced, (void)used_reduced;
    return false; // no job: print without annotations
}
bool scheduler_is_running(struct scheduler *s) {
    (void)s;
    return false;
}
void shell_complete(const char *line, int cursor_pos, struct completion *out) {
    (void)line, (void)cursor_pos, (void)out;
}
void completion_free(struct completion *c) {
    (void)c;
}
uint32_t cpu_get_pc(struct cpu *cpu) {
    (void)cpu;
    return 0;
}
const char *system_machine_model_id(void) {
    return "";
}
// shell_init hands the Lua runner its error reporter (gs_lua.h, report.h).
void script_report_error(const char *file, int line, const char *msg, const char *text) {
    (void)file, (void)line, (void)msg, (void)text;
}
typedef void (*gs_lua_reporter_fn)(const char *file, int line, const char *msg, const char *text);
void gs_lua_set_reporter(gs_lua_reporter_fn fn) {
    (void)fn;
}

// === A toy class ============================================================

// A register whose member declares hex; the getter does not set it itself
static DEF_GETTER(toy_get_reg) {
    return val_uint(4, 0x1234);
}

// A secret whose member declares it sensitive
static DEF_GETTER(toy_get_pin) {
    return val_uint(4, 4711);
}

static const member_t toy_members[] = {
    {.kind = MK_ATTR,
     .name = "reg",
     .doc = "a register",
     .attr = {.type = VK_UINT, .presentation_flags = VFLAG_HEX, .get = toy_get_reg}      },
    {.kind = MK_ATTR,
     .name = "pin",
     .doc = "a secret",
     .attr = {.type = VK_UINT, .presentation_flags = VFLAG_SENSITIVE, .get = toy_get_pin}},
};
static const class_desc_t toy_class = {.name = "toy", .members = toy_members, .n_members = 2};

// === Tests ==================================================================

// The table honours the member's VFLAG_HEX as node_get does
TEST(object_table_honours_declared_hex) {
    struct object *o = object_new(&toy_class, NULL, "toy");
    out_reset();
    value_t v = val_obj(o);
    shell_print_value(&v);
    ASSERT_TRUE(strstr(s_out, "0x1234") != NULL);
    object_delete(o);
}

// A VFLAG_SENSITIVE attribute never prints its payload
TEST(object_table_hides_a_sensitive_attribute) {
    struct object *o = object_new(&toy_class, NULL, "toy");
    out_reset();
    value_t v = val_obj(o);
    shell_print_value(&v);
    ASSERT_TRUE(strstr(s_out, "4711") == NULL);
    ASSERT_TRUE(strstr(s_out, "pin") != NULL);
    object_delete(o);
}

// The same list-of-objects table: cells honour the flags too
TEST(object_list_table_hides_a_sensitive_cell) {
    struct object *a = object_new(&toy_class, NULL, "a");
    struct object *b = object_new(&toy_class, NULL, "b");
    value_t items[2] = {val_obj(a), val_obj(b)};
    value_t list = {
        .kind = VK_LIST, .list = {.items = items, .len = 2}
    };
    out_reset();
    shell_print_value(&list);
    ASSERT_TRUE(strstr(s_out, "4711") == NULL);
    ASSERT_TRUE(strstr(s_out, "0x1234") != NULL);
    object_delete(a);
    object_delete(b);
}

int main(void) {
    RUN(object_table_honours_declared_hex);
    RUN(object_table_hides_a_sensitive_attribute);
    RUN(object_list_table_hides_a_sensitive_cell);
    printf("All shell_print tests passed\n");
    return 0;
}
