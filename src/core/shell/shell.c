// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell.c
// Interactive command shell for emulator debugging and control.

#include "shell.h"

#include "alias.h"
#include "cmd_complete.h"
#include "debug.h"
#include "expr.h"
#include "log.h"
#include "meta.h"
#include "object.h"
#include "parse.h"
#include "script.h"
#include "shell_var.h"
#include "system.h"
#include "value.h"
#include "vfs.h"
#include "worker_thread.h"

#include <inttypes.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Internal guard for shell_dispatch / dispatch_command. Volatile because
// shell_init runs on the worker pthread while assertions in tests may
// read the flag from another thread.
static volatile int32_t shell_initialized = 0;

// Run a free-form line through the v2 script interpreter (REPL
// semantics). Used by the Shell class's `run` method.
bool shell_internal_dispatch_command(char *line, char *err_buf, size_t err_size) {
    if (!shell_initialized) {
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "shell not initialized");
        return false;
    }
    if (script_run_line(line) != 0) {
        if (err_buf && err_size)
            snprintf(err_buf, err_size, "command failed");
        return false;
    }
    return true;
}

// === Value printing (the REPL formatting surface, §5) ======================

// Print one scalar value inline (no trailing newline). Used as the
// rhs in the object-attribute table dump and inside list expansion.
// Mirrors the per-kind formatting in format_value_print but without
// a trailing newline so it composes into a `name = value` line.
static void format_scalar_inline(const value_t *v) {
    if (!v)
        return;
    switch (v->kind) {
    case V_NONE:
        // empty inline representation
        break;
    case V_BOOL:
        printf("%s", v->b ? "true" : "false");
        break;
    case V_INT:
        if (v->flags & VAL_HEX)
            printf("0x%" PRIx64, (uint64_t)v->i);
        else
            printf("%" PRId64, v->i);
        break;
    case V_UINT:
        if (v->flags & VAL_HEX)
            printf("0x%" PRIx64, v->u);
        else
            printf("%" PRIu64, v->u);
        break;
    case V_FLOAT:
        printf("%g", v->f);
        break;
    case V_STRING:
        printf("\"%s\"", v->s ? v->s : "");
        break;
    case V_BYTES: {
        // Cap inline rendering so a 1 MiB bytes attribute doesn't print
        // 2 MiB of hex. Past `bytes_cap` we annotate with "...N more".
        size_t bytes_cap = 64;
        size_t shown = v->bytes.n > bytes_cap ? bytes_cap : v->bytes.n;
        printf("0x");
        for (size_t i = 0; i < shown; i++)
            printf("%02x", v->bytes.p[i]);
        if (v->bytes.n > bytes_cap)
            printf(" ...%zu more", v->bytes.n - bytes_cap);
        break;
    }
    case V_ENUM:
        if (v->enm.table && (size_t)v->enm.idx < v->enm.n_table && v->enm.table[v->enm.idx])
            printf("\"%s\"", v->enm.table[v->enm.idx]);
        else
            printf("enum:%d", v->enm.idx);
        break;
    case V_LIST:
        printf("<list:%zu>", v->list.len);
        break;
    case V_OBJECT: {
        const class_desc_t *cc = v->obj ? object_class(v->obj) : NULL;
        printf("<%s>", cc && cc->name ? cc->name : "object");
        break;
    }
    case V_ERROR:
        printf("<error: %s>", v->err ? v->err : "");
        break;
    case V_REF:
        printf("%s", v->ref ? v->ref : "");
        break;
    case V_RANGE:
        printf("%lld..%lld", (long long)v->range.start, (long long)v->range.stop);
        break;
    }
}

// Print an object as a multi-line `name = value` table. Walks the
// class's member table and reads each attribute through its getter;
// child objects show as `<class:name>` placeholders (drill in via
// the path to see their state); methods are skipped.
static void format_object_table(struct object *o) {
    if (!o)
        return;
    const class_desc_t *cls = object_class(o);
    if (!cls || !cls->members || cls->n_members == 0) {
        const char *cls_name = (cls && cls->name) ? cls->name : "object";
        const char *o_name = object_name(o);
        printf("<%s:%s>\n", cls_name, o_name ? o_name : "");
        return;
    }
    // Pass 1: compute the longest member name so we can right-pad.
    int width = 0;
    for (size_t i = 0; i < cls->n_members; i++) {
        const member_t *mb = &cls->members[i];
        if (!mb->name)
            continue;
        if (mb->kind == M_METHOD)
            continue;
        int len = (int)strlen(mb->name);
        if (len > width)
            width = len;
    }
    if (width > 24)
        width = 24; // cap so very long attr names don't blow the layout
    // Pass 2: print attrs first, then children.
    for (size_t i = 0; i < cls->n_members; i++) {
        const member_t *mb = &cls->members[i];
        if (!mb->name)
            continue;
        if (mb->kind != M_ATTR)
            continue;
        if (!mb->attr.get)
            continue;
        value_t v = mb->attr.get(o, mb);
        printf("%-*s = ", width, mb->name);
        format_scalar_inline(&v);
        printf("\n");
        value_free(&v);
    }
    for (size_t i = 0; i < cls->n_members; i++) {
        const member_t *mb = &cls->members[i];
        if (!mb->name || mb->kind != M_CHILD)
            continue;
        const char *child_cls = (mb->child.cls && mb->child.cls->name) ? mb->child.cls->name : "object";
        printf("%-*s : <%s%s>\n", width, mb->name, child_cls, mb->child.indexed ? "[]" : "");
    }
}

// Render one cell of the object-list table into buf. Scalar kinds only;
// structured kinds render as compact placeholders.
static void format_cell(const value_t *v, char *buf, size_t buf_size) {
    switch (v->kind) {
    case V_NONE:
        snprintf(buf, buf_size, "-");
        break;
    case V_BOOL:
        snprintf(buf, buf_size, "%s", v->b ? "true" : "false");
        break;
    case V_INT:
        snprintf(buf, buf_size, (v->flags & VAL_HEX) ? "0x%llx" : "%lld",
                 (v->flags & VAL_HEX) ? (long long)(uint64_t)v->i : (long long)v->i);
        break;
    case V_UINT:
        snprintf(buf, buf_size, (v->flags & VAL_HEX) ? "0x%llx" : "%llu", (unsigned long long)v->u);
        break;
    case V_FLOAT:
        snprintf(buf, buf_size, "%g", v->f);
        break;
    case V_STRING:
        snprintf(buf, buf_size, "%s", v->s ? v->s : "");
        break;
    case V_ENUM:
        if (v->enm.table && (size_t)v->enm.idx < v->enm.n_table && v->enm.table[v->enm.idx])
            snprintf(buf, buf_size, "%s", v->enm.table[v->enm.idx]);
        else
            snprintf(buf, buf_size, "enum:%d", v->enm.idx);
        break;
    default:
        snprintf(buf, buf_size, "<%d>", (int)v->kind);
        break;
    }
}

#define TABLE_MAX_COLS 12
#define TABLE_CELL_MAX 48

// A V_LIST whose elements are all objects of one class prints as a
// table — columns from the class's attributes (§5). This is what lets
// `debug.breakpoints.entries` at the prompt render the same table the
// retired `list` methods used to print. Returns false when the list
// isn't table-shaped (caller falls back to inline list rendering).
static bool try_print_object_table(const value_t *v) {
    if (v->kind != V_LIST || v->list.len == 0)
        return false;
    const class_desc_t *cls = NULL;
    for (size_t i = 0; i < v->list.len; i++) {
        const value_t *e = &v->list.items[i];
        if (e->kind != V_OBJECT || !e->obj)
            return false;
        const class_desc_t *c = object_class(e->obj);
        if (!c || (cls && c != cls))
            return false;
        cls = c;
    }
    if (!cls || !cls->members)
        return false;
    // Collect attribute columns.
    int cols[TABLE_MAX_COLS];
    int n_cols = 0;
    for (size_t i = 0; i < cls->n_members && n_cols < TABLE_MAX_COLS; i++) {
        const member_t *mb = &cls->members[i];
        if (mb->kind == M_ATTR && mb->name && mb->attr.get)
            cols[n_cols++] = (int)i;
    }
    if (n_cols == 0)
        return false;
    // Width pass.
    int width[TABLE_MAX_COLS];
    for (int c = 0; c < n_cols; c++)
        width[c] = (int)strlen(cls->members[cols[c]].name);
    char cell[TABLE_CELL_MAX];
    for (size_t i = 0; i < v->list.len; i++) {
        for (int c = 0; c < n_cols; c++) {
            const member_t *mb = &cls->members[cols[c]];
            value_t cv = mb->attr.get(v->list.items[i].obj, mb);
            format_cell(&cv, cell, sizeof(cell));
            value_free(&cv);
            int len = (int)strlen(cell);
            if (len > width[c])
                width[c] = len;
        }
    }
    // Header + rows.
    for (int c = 0; c < n_cols; c++)
        printf("%-*s%s", width[c], cls->members[cols[c]].name, c + 1 < n_cols ? "  " : "\n");
    for (size_t i = 0; i < v->list.len; i++) {
        for (int c = 0; c < n_cols; c++) {
            const member_t *mb = &cls->members[cols[c]];
            value_t cv = mb->attr.get(v->list.items[i].obj, mb);
            format_cell(&cv, cell, sizeof(cell));
            value_free(&cv);
            printf("%-*s%s", width[c], cell, c + 1 < n_cols ? "  " : "\n");
        }
    }
    return true;
}

static void format_value_print(const value_t *v) {
    if (!v)
        return;
    switch (v->kind) {
    case V_NONE:
        break;
    case V_BOOL:
        printf("%s\n", v->b ? "true" : "false");
        break;
    case V_INT:
        printf("%" PRId64 "\n", v->i);
        break;
    case V_UINT:
        if (v->flags & VAL_HEX)
            printf("0x%" PRIx64 "\n", v->u);
        else
            printf("%" PRIu64 "\n", v->u);
        break;
    case V_FLOAT:
        printf("%g\n", v->f);
        break;
    case V_STRING:
        printf("%s\n", v->s ? v->s : "");
        break;
    case V_BYTES:
        printf("0x");
        for (size_t i = 0; i < v->bytes.n; i++)
            printf("%02x", v->bytes.p[i]);
        printf("\n");
        break;
    case V_ENUM:
        if (v->enm.table && (size_t)v->enm.idx < v->enm.n_table && v->enm.table[v->enm.idx])
            printf("%s\n", v->enm.table[v->enm.idx]);
        else
            printf("enum:%d\n", v->enm.idx);
        break;
    case V_LIST:
        // A list of same-class objects renders as an attribute table.
        if (try_print_object_table(v))
            break;
        // Expand list elements inline: [item1, item2, ...]. Strings are
        // quoted, ints/uints printed in their natural base, objects use
        // <class:name>, nested lists recurse via the size form.
        printf("[");
        for (size_t i = 0; i < v->list.len; i++) {
            const value_t *e = &v->list.items[i];
            if (i)
                printf(", ");
            switch (e->kind) {
            case V_NONE:
                printf("null");
                break;
            case V_BOOL:
                printf("%s", e->b ? "true" : "false");
                break;
            case V_INT:
                printf("%" PRId64, e->i);
                break;
            case V_UINT:
                if (e->flags & VAL_HEX)
                    printf("0x%" PRIx64, e->u);
                else
                    printf("%" PRIu64, e->u);
                break;
            case V_FLOAT:
                printf("%g", e->f);
                break;
            case V_STRING:
                printf("\"%s\"", e->s ? e->s : "");
                break;
            case V_BYTES:
                printf("<bytes:%zu>", e->bytes.n);
                break;
            case V_LIST:
                printf("<list:%zu>", e->list.len);
                break;
            case V_ENUM:
                if (e->enm.table && (size_t)e->enm.idx < e->enm.n_table && e->enm.table[e->enm.idx])
                    printf("\"%s\"", e->enm.table[e->enm.idx]);
                else
                    printf("enum:%d", e->enm.idx);
                break;
            case V_OBJECT: {
                const class_desc_t *cc = e->obj ? object_class(e->obj) : NULL;
                printf("<%s:%s>", cc && cc->name ? cc->name : "object",
                       e->obj && object_name(e->obj) ? object_name(e->obj) : "");
                break;
            }
            case V_ERROR:
                printf("<error>");
                break;
            case V_REF:
                printf("%s", e->ref ? e->ref : "");
                break;
            case V_RANGE:
                printf("%lld..%lld", (long long)e->range.start, (long long)e->range.stop);
                break;
            }
        }
        printf("]\n");
        break;
    case V_OBJECT:
        // Bare-read of an object prints its attributes as a
        // `name = value` table. Methods are skipped; child nodes show
        // as placeholders (drill in by typing the dotted path).
        format_object_table(v->obj);
        break;
    case V_ERROR:
        fprintf(stderr, "%s\n", v->err ? v->err : "(error)");
        break;
    case V_REF:
        printf("%s\n", v->ref ? v->ref : "");
        break;
    case V_RANGE:
        printf("%lld..%lld\n", (long long)v->range.start, (long long)v->range.stop);
        break;
    }
}

// Public print surface for the script interpreter (REPL result
// printing, §5).
void shell_print_value(const value_t *v) {
    format_value_print(v);
}

// Dispatch interactively and return integer result. The line runs
// through the v2 script interpreter with REPL semantics (results
// print). Errors return -1 so scripted drivers surface failures.
uint64_t shell_dispatch(char *line) {
    if (!shell_initialized)
        return -1;

    worker_thread_assert("shell_dispatch");

    if (!line)
        return 0;
    return script_run_line(line) == 0 ? 0 : (uint64_t)-1;
}

// Tab completion entry point
void shell_tab_complete(const char *line, int cursor_pos, struct completion *out) {
    shell_complete(line, cursor_pos, out);
}

// Compose the current shell prompt: `<disasm> > ` when a machine is up
// (matching the headless REPL and the legacy WASM `build_prompt_text`),
// `gs> ` otherwise. Centralised here so the Shell class's `prompt`
// attribute, the headless REPL's `print_prompt`, and any future
// consumer share a single source of truth.
void shell_build_prompt(char *buf, size_t buf_size) {
    if (!buf || buf_size == 0)
        return;
    buf[0] = '\0';
    if (!system_is_initialized()) {
        snprintf(buf, buf_size, "gs> ");
        return;
    }
    // Reserve 3 bytes for " > " suffix + 1 for terminator.
    debugger_disasm_pc(buf, buf_size > 3 ? buf_size - 3 : 1);
    size_t used = strnlen(buf, buf_size - 1);
    if (used == 0) {
        snprintf(buf, buf_size, "gs> ");
        return;
    }
    if (used >= buf_size - 3)
        used = buf_size - 4;
    buf[used++] = ' ';
    buf[used++] = '>';
    buf[used++] = ' ';
    buf[used] = '\0';
}

// Provider wired into the Meta class so `meta.complete(line, cursor)`
// runs the shell's full tab-completion engine. The Meta layer treats a
// missing provider as "completion service not available" (empty list);
// shell_init registers this wrapper so the bridge has the engine wired
// once the worker is ready.
static value_t shell_meta_complete_provider(const char *line, int cursor) {
    struct completion comp;
    memset(&comp, 0, sizeof(comp));
    shell_complete(line ? line : "", cursor, &comp);
    if (comp.count <= 0)
        return val_list(NULL, 0);
    value_t *items = (value_t *)calloc((size_t)comp.count, sizeof(value_t));
    if (!items)
        return val_err("meta.complete: out of memory");
    for (int i = 0; i < comp.count; i++)
        items[i] = val_str(comp.items[i] ? comp.items[i] : "");
    return val_list(items, (size_t)comp.count);
}

/* --- shell init ---------------------------------------------------------- */
int shell_init(void) {
    if (shell_initialized)
        return 0;

    log_init();
    shell_var_init();

    // Wire the Meta class's `complete(line, cursor)` method to the
    // shell's tab-completion engine. Done early so any `gs_eval` that
    // lands during init (vanishingly unlikely but cheap to guarantee)
    // sees a live provider.
    meta_set_complete_provider(shell_meta_complete_provider);

    // Install the top-level object-root methods (assert, echo, cp,
    // peeler, rom_probe, …) so JS callers (`gsEval`) and the typed
    // path-form parser can reach them.
    extern void root_install_class(void);
    root_install_class();

    // Register process-singleton namespace objects that exist
    // independently of any machine instance: rom, vrom, and machine
    // all carry pre-boot surfaces (rom.identify, vrom.load,
    // machine.boot, machine.profile) that callers reach for *before*
    // a machine has been created. The WASM URL-media boot path is the
    // canonical case — drag-drop a Plus ROM, ask rom.identify for the
    // compatible models, then call machine.boot with the answer.
    // Hooking these up in system_create was wrong: the WASM platform
    // doesn't run system_create at startup, so the path-form would
    // fail to resolve until the legacy `rom load` had already booted
    // a machine.
    extern void rom_init(void);
    extern void vrom_init(void);
    extern void machine_init(void);
    extern void checkpoint_init(void);
    extern void archive_init(void);
    extern void mouse_class_register(void);
    extern void keyboard_class_register(void);
    extern void screen_class_register(void);
    extern void vfs_class_register(void);
    extern void find_class_register(void);
    extern void scsi_class_register(void);
    rom_init();
    vrom_init();
    machine_init();
    checkpoint_init();
    archive_init();
    mouse_class_register();
    keyboard_class_register();
    screen_class_register();
    vfs_class_register();
    find_class_register();
    scsi_class_register();

    // Install the cfg-scoped namespace stubs (storage, shell, mouse,
    // keyboard, screen, vfs, find) with a NULL cfg so their pre-boot
    // surfaces resolve — particularly storage.cp and storage.find_media,
    // which the URL-media auto-boot path uses *before* machine.boot to
    // copy the downloaded ROM into OPFS and to scan extracted archives
    // for floppy images. system_create will later re-install with the
    // real cfg (root_install handles the cfg-change uninstall +
    // reinstall internally).
    extern void root_install(struct config * cfg);
    root_install(NULL);

    // Latch the worker pthread for the thread-affinity guard. From now
    // on (under MODE=debug/sanitize) any call into shell_dispatch() or
    // gs_eval() from a different thread aborts with GS_ASSERTF.
    worker_thread_record();

    shell_initialized = 1;
    return 0;
}
