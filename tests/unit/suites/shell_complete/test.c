// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// Unit tests for argument-value completion (src/core/shell/cmd_complete.c).
//
// Filesystem candidates are offered for a string argument declared VAL_PATH,
// and only for one: an argument merely NAMED `path` (an object path, say)
// gets none.  The VFS is a stub directory holding `disk.img` and `roms/`.

#include "cmd_complete.h"
#include "commands.h"
#include "object.h"
#include "shell_var.h"
#include "test_assert.h"
#include "value.h"
#include "vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// === Stubs =====================================================================

void shell_var_each(shell_var_iter_fn fn, void *ud) {
    (void)fn;
    (void)ud;
}

struct vfs_dir {
    int pos;
};
static struct vfs_dir g_dir;
static int g_opendir_calls;

static int stub_readdir(vfs_dir_t *d, vfs_dirent_t *out) {
    static const char *const names[] = {"disk.img", "roms"};
    if (d->pos >= 2)
        return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", names[d->pos]);
    out->has_stat = true;
    out->st.mode = d->pos == 1 ? VFS_MODE_DIR : VFS_MODE_FILE;
    d->pos++;
    return 1;
}
static void stub_closedir(vfs_dir_t *d) {
    (void)d;
}
static const vfs_backend_t g_backend = {.scheme = "stub", .readdir = stub_readdir, .closedir = stub_closedir};

int vfs_opendir(const char *path, vfs_dir_t **out, const vfs_backend_t **be) {
    (void)path;
    g_opendir_calls++;
    g_dir.pos = 0;
    *out = &g_dir;
    *be = &g_backend;
    return 0;
}
int vfs_stat(const char *path, vfs_stat_t *out) {
    (void)path;
    (void)out;
    return -1;
}

// === Toy tree ===================================================================

static value_t method_none(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    (void)argv;
    return val_none();
}

static const arg_decl_t load_args[] = {
    {.name = "image", .kind = V_STRING, .presentation_flags = VAL_PATH, .doc = "Image file"},
};
static const arg_decl_t resolve_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Object path"},
};
static const arg_decl_t copy_args[] = {
    {.name = "src", .kind = V_STRING, .presentation_flags = VAL_PATH,       .doc = "Source"},
    {.name = "dst", .kind = V_STRING, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Label" },
};
static const member_t tool_members[] = {
    {.kind = M_METHOD,
     .name = "load",
     .doc = "Load",
     .method = {.args = load_args, .nargs = 1, .result = V_NONE, .fn = method_none}   },
    {.kind = M_METHOD,
     .name = "resolve",
     .doc = "Resolve",
     .method = {.args = resolve_args, .nargs = 1, .result = V_NONE, .fn = method_none}},
    {.kind = M_METHOD,
     .name = "copy",
     .doc = "Copy",
     .method = {.args = copy_args, .nargs = 2, .result = V_NONE, .fn = method_none}   },
};
static const class_desc_t tool_class = {.name = "tool", .members = tool_members, .n_members = 3};

static void build_tree(void) {
    object_root_reset();
    object_attach(object_root(), object_new(&tool_class, NULL, "tool"));
}

static struct completion g_out;

static void complete(const char *line) {
    memset(&g_out, 0, sizeof(g_out));
    g_opendir_calls = 0;
    shell_complete(line, (int)strlen(line), &g_out);
}

static bool has(const char *item) {
    for (int i = 0; i < g_out.count; i++)
        if (strcmp(g_out.items[i], item) == 0)
            return true;
    return false;
}

// === Tests =====================================================================

TEST(test_val_path_argument_gets_files) {
    build_tree();
    complete("tool.load ");
    ASSERT_TRUE(g_opendir_calls == 1);
    ASSERT_TRUE(has("disk.img"));
    ASSERT_TRUE(has("roms/"));
    ASSERT_EQ_INT(COMP_KIND_VALUE, g_out.kinds[0]);
}

TEST(test_argument_named_path_without_flag_gets_none) {
    build_tree();
    complete("tool.resolve ");
    ASSERT_TRUE(g_opendir_calls == 0);
    ASSERT_TRUE(!has("disk.img"));
}

TEST(test_flag_not_name_decides_per_slot) {
    build_tree();
    complete("tool.copy d");
    ASSERT_TRUE(g_opendir_calls == 1);
    ASSERT_TRUE(has("disk.img"));
    // `dst` would have matched the old name heuristic; it is not VAL_PATH.
    complete("tool.copy disk.img ");
    ASSERT_TRUE(g_opendir_calls == 0);
    ASSERT_TRUE(!has("disk.img"));
}

TEST(test_command_word_completes_and_takes_its_methods_arguments) {
    build_tree();
    shell_command_clear_user();
    char err[200];
    ASSERT_TRUE(shell_command_define("ld", "tool.load", err, sizeof(err)) == 0);
    // A command is a word at the start of a line, with its method's doc.
    complete("l");
    ASSERT_TRUE(has("ld"));
    // Its arguments are the method's: `image` is VAL_PATH.
    complete("ld ");
    ASSERT_TRUE(g_opendir_calls == 1);
    ASSERT_TRUE(has("disk.img"));
    ASSERT_TRUE(g_out.has_context && strcmp(g_out.ctx_method, "tool.load") == 0);
    // A target that is not a method, or a root path as the name, is refused.
    ASSERT_TRUE(shell_command_define("tl", "tool", err, sizeof(err)) < 0);
    ASSERT_TRUE(shell_command_define("tool", "tool.load", err, sizeof(err)) < 0);
    shell_command_clear_user();
}

int main(void) {
    RUN(test_val_path_argument_gets_files);
    RUN(test_argument_named_path_without_flag_gets_none);
    RUN(test_flag_not_name_decides_per_slot);
    RUN(test_command_word_completes_and_takes_its_methods_arguments);
    return 0;
}
