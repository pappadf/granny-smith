// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// shell.c
// Interactive command shell for emulator debugging and control.

#include "shell.h"

#include "alias.h"
#include "cmd_complete.h"
#include "cmd_io.h"
#include "cmd_parse.h"
#include "cmd_types.h"
#include "expr.h"
#include "gs_thread.h"
#include "log.h"
#include "object.h"
#include "parse.h"
#include "shell_var.h"
#include "value.h"
#include "vfs.h"

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

// Volatile so the shared-memory pointer exposed to JS sees the real
// flag flip — avoids the optimiser caching the constant `0` it sees
// at init time and confusing the JS-side gsEval ready check.
static volatile int32_t shell_initialized = 0;

// Borrowed pointer into the shared-memory shell-initialized flag. JS
// polls this on first gsEval call before issuing the ccall — without
// it, calls fired during the boot window between Module-ready and the
// worker leaving shell_init() see the stale empty root class.
volatile int32_t *gs_shell_ready_ptr(void) {
    return &shell_initialized;
}

// Phase 5c — legacy command registry deleted. The typed object-model
// bridge is the sole dispatch surface; shell_dispatch falls straight
// through to the path-form parser.

/* --- tokenizer ----------------------------------------------------------- */
#define MAXTOK 32

// In-place tokenizer exposed via shell.h as `shell_tokenize` so the
// typed object-model bridge can split free-form spec strings (e.g.
// logpoint specs, log argv) without routing through `shell_dispatch`.
// `line` is mutated in place; returned argv pointers point inside it.
int tokenize(char *line, char *argv[], int max) {
    // Tokenizer with support for: \-escapes, ASCII and UTF-8 curly
    // quotes, and ' / " quoted strings. `${...}` substitution has
    // already been resolved by shell_var_expand before tokenisation,
    // so the tokenizer only sees the post-expansion source — except
    // inside single-quoted regions, where `${...}` survives verbatim
    // (the deferred-eval opt-out used by logpoint messages).
    int argc = 0;
    int esc = 0;
    enum { Q_NONE = 0, Q_DQUOTE, Q_SQUOTE, Q_CURLY } qstate = Q_NONE;

    char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (argc == max)
            return -1;

        argv[argc++] = p;
        char *dst = p;

        for (;;) {
            unsigned char c = (unsigned char)*p;
            if (c == '\0') {
                *dst = '\0';
                break;
            }

            if (esc) {
                *dst++ = *p++;
                esc = 0;
                continue;
            }

            // UTF-8 curly quotes (E2 80 9C/9D)
            if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80 &&
                ((unsigned char)p[2] == 0x9C || (unsigned char)p[2] == 0x9D)) {
                if (qstate == Q_NONE)
                    qstate = Q_CURLY;
                else if (qstate == Q_CURLY)
                    qstate = Q_NONE;
                else {
                    *dst++ = *p++;
                    continue;
                }
                p += 3;
                continue;
            }

            if (*p == '\\') {
                esc = 1;
                p++;
                continue;
            }

            if (*p == '"') {
                if (qstate == Q_NONE)
                    qstate = Q_DQUOTE;
                else if (qstate == Q_DQUOTE)
                    qstate = Q_NONE;
                else
                    *dst++ = *p;
                p++;
                continue;
            }

            if (*p == '\'') {
                if (qstate == Q_NONE)
                    qstate = Q_SQUOTE;
                else if (qstate == Q_SQUOTE)
                    qstate = Q_NONE;
                else
                    *dst++ = *p;
                p++;
                continue;
            }

            if (qstate == Q_NONE && isspace((unsigned char)*p)) {
                *dst = '\0';
                p++;
                break;
            }

            *dst++ = *p++;
        }
    }
    return argc;
}

// Phase 5c — legacy `help` / `echo` / `time` / `add` / `remove`
// shell built-ins retired. `echo` and other utilities are typed root
// methods now (root.c).

// Phase 5c — legacy filesystem commands (`ls`, `cd`, `mkdir`, `mv`,
// `cat`, `exists`, `size`, `rm`) and the registry-based execute_cmd
// retired. Typed object-model methods (vfs.ls, vfs.mkdir, vfs.cat,
// path_exists, path_size, …) replace them.

// Forward declaration — definition lives below in the shell-form
// grammar block. Returns 0 on success, -1 on error, 1 if unhandled.
static int try_path_dispatch(int argc, char **argv);

// Dispatch a command line. Phase 5c — the legacy registry is gone;
// everything routes through the typed path-form parser.
void dispatch_command(char *line, struct cmd_result *res) {
    memset(res, 0, sizeof(*res));
    res->type = RES_OK;

    if (!shell_initialized) {
        cmd_err(res, "shell not initialized");
        return;
    }

    char *expanded = shell_var_expand(line);
    if (!expanded) {
        cmd_err(res, "expansion failed");
        return;
    }

    char *argv[MAXTOK];
    int argc = tokenize(expanded, argv, MAXTOK);
    if (argc <= 0) {
        free(expanded);
        return;
    }

    int pd = try_path_dispatch(argc, argv);
    if (pd < 0)
        cmd_err(res, "command failed");
    else if (pd > 0)
        cmd_err(res, "unknown command: '%s'", argv[0]);
    free(expanded);
}

// === New shell-form grammar dispatch (proposal §4.1) =======================
//
// Falls between the legacy command lookup and the "unknown command"
// suggestion: if argv[0] resolves as a path against the object root,
// dispatch as one of:
//   - bare path (argc == 1)            → node_get + print
//   - path = value (argv[1] == "=")    → node_set with parsed argv[2]
//   - path arg arg arg (M_METHOD)      → node_call with parsed args
// Returns true if the line was handled (caller should not fall through
// to suggest_command), false otherwise.

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
    case V_BYTES:
        printf("0x");
        for (size_t i = 0; i < v->bytes.n; i++)
            printf("%02x", v->bytes.p[i]);
        break;
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
    }
}

// Returns 0 if handled successfully, -1 if handled with error, or 1 if
// not a path (caller continues with the unknown-command path).
static int try_path_dispatch(int argc, char **argv) {
    if (argc < 1)
        return 1;
    // Resolve argv[0] against the object root.  Anything that names a
    // valid node — root method, attached child object, attribute, or
    // dotted path — dispatches; everything else falls through to the
    // unknown-command path.
    node_t n = object_resolve(object_root(), argv[0]);
    if (!node_valid(n))
        return 1;

    // Setter form: `path = value`.
    if (argc >= 3 && strcmp(argv[1], "=") == 0) {
        if (!n.member || n.member->kind != M_ATTR) {
            fprintf(stderr, "'%s' is not a settable attribute\n", argv[0]);
            return -1;
        }
        value_t v = parse_literal_full(argv[2], NULL, 0);
        if (val_is_error(&v)) {
            // Fall back to treating the token as a string literal —
            // mirrors the method-call branch and lets attribute setters
            // accept opaque tokens (ISO timestamps, paths, identifiers
            // that aren't valid number/bool/enum literals).
            value_free(&v);
            v = val_str(argv[2]);
        }
        value_t result = node_set(n, v);
        if (val_is_error(&result)) {
            fprintf(stderr, "set %s: %s\n", argv[0], result.err ? result.err : "failed");
            value_free(&result);
            return -1;
        }
        value_free(&result);
        return 0;
    }

    // Method call form: `path arg arg arg ...`.
    if (n.member && n.member->kind == M_METHOD) {
        int call_argc = argc - 1;
        value_t *vals = call_argc > 0 ? (value_t *)calloc((size_t)call_argc, sizeof(value_t)) : NULL;
        for (int i = 0; i < call_argc; i++) {
            vals[i] = parse_literal_full(argv[i + 1], NULL, 0);
            if (val_is_error(&vals[i])) {
                // Fall back to treating the token as a string literal —
                // mirrors the way most legacy commands accept a bare
                // word as a path/name.
                value_free(&vals[i]);
                vals[i] = val_str(argv[i + 1]);
            }
        }
        value_t result = node_call(n, call_argc, vals);
        for (int i = 0; i < call_argc; i++)
            value_free(&vals[i]);
        free(vals);
        if (val_is_error(&result)) {
            fprintf(stderr, "%s: %s\n", argv[0], result.err ? result.err : "call failed");
            value_free(&result);
            return -1;
        }
        format_value_print(&result);
        value_free(&result);
        return 0;
    }

    // Bare path read.
    if (argc == 1) {
        value_t v = node_get(n);
        if (val_is_error(&v)) {
            // Print the error to stderr but return 0 — matches the
            // legacy `eval` semantics. Tests intentionally probe empty
            // indexed slots etc. ("scsi.devices[5]") and expect the
            // script to keep going. Use `assert (exists(path))` for
            // strict membership checks.
            fprintf(stderr, "%s: %s\n", argv[0], v.err ? v.err : "read failed");
            value_free(&v);
            return 0;
        }
        format_value_print(&v);
        value_free(&v);
        return 0;
    }

    // Anything else (path with extra tokens that isn't a setter or a
    // method call) is ambiguous — let the legacy code path handle it.
    return 1;
}

// Dispatch interactively and return integer result. Phase 5c — only
// the typed path-form parser remains.
uint64_t shell_dispatch(char *line) {
    if (!shell_initialized)
        return -1;

    gs_thread_assert_worker("shell_dispatch");

    char *expanded = shell_var_expand(line);
    if (!expanded) {
        fputs("expansion failed\n", stderr);
        return (uint64_t)-1;
    }

    char *argv[MAXTOK];
    int argc = tokenize(expanded, argv, MAXTOK);
    if (argc < 0) {
        fputs("too many arguments\n", stderr);
        free(expanded);
        return 0;
    }
    if (argc == 0) {
        free(expanded);
        return 0;
    }

    int pd = try_path_dispatch(argc, argv);
    if (pd > 0)
        fprintf(stderr, "Unknown command: '%s'\n", argv[0]);
    free(expanded);
    // pd == 0 → handled successfully
    // pd  < 0 → handled with error
    // pd  > 0 → unknown command; HARD error (was a no-op pre-phase-5e
    //          which masked test-script regressions like the legacy
    //          `fd create` line silently no-opping after 5c).
    return (pd != 0) ? (uint64_t)-1 : 0;
}

// Tab completion entry point
void shell_tab_complete(const char *line, int cursor_pos, struct completion *out) {
    shell_complete(line, cursor_pos, out);
}

/* --- shell init ---------------------------------------------------------- */
int shell_init(void) {
    if (shell_initialized)
        return 0;

    log_init();
    shell_var_init();

    // Install the top-level object-root methods (assert, echo, cp,
    // peeler, rom_probe, …) so JS callers (`gsEval`) and the typed
    // path-form parser can reach them.
    extern void root_install_class(void);
    root_install_class();

    // Register process-singleton namespace objects that exist
    // independently of any machine instance: rom, vrom, and machine
    // all carry pre-boot surfaces (rom.identify, vrom.load,
    // machine.boot, machine.profiles) that callers reach for *before*
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
    gs_thread_record_worker();

    shell_initialized = 1;
    return 0;
}
