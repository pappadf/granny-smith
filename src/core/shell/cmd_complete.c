// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_complete.c
// Tab completion engine.  The statement the cursor is in -- a line's own,
// or an inline block's after its `{` -- is found and classified the way the
// parser reads it (syntax.h).  At the head of a statement: keywords, root
// members and command words, or the members of a mid-path partial; at an
// argument of a command: candidates for the declared slot the word fills.
// Inside a string or a parenthesised argument, nothing.

#include "cmd_complete.h"
#include "commands.h"
#include "shell_var.h"
#include "syntax.h"
#include "worker_thread.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "../object/alias.h"
#include "../object/meta.h"
#include "../object/object.h"
#include "../object/value.h"
#include "../vfs/vfs.h"

// === Tiny per-call string pool ==============================================
//
// Most completion items point at static class-member or registry strings —
// those need no copy. Indexed-child entries (`drives.0`) and any other
// dynamically composed names need backing storage that outlives the
// completion call; the terminal copies the strings before the next press.
// One static buffer per call is enough for the small fan-outs we ship.

static char g_pool[2048];
static size_t g_pool_used;

static void pool_reset(void) {
    g_pool_used = 0;
}

static const char *pool_strdup(const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    if (g_pool_used + n > sizeof(g_pool))
        return NULL;
    char *out = g_pool + g_pool_used;
    memcpy(out, s, n);
    g_pool_used += n;
    return out;
}

// === Completion accumulator ==================================================

static void push_match(struct completion *out, const char *cand, const char *prefix) {
    if (!cand) {
        // pool_strdup returned NULL: a composed candidate was dropped because
        // the pool filled.  Say so rather than returning a quietly short set.
        out->truncated = true;
        return;
    }
    if (out->count >= CMD_MAX_COMPLETIONS) {
        out->truncated = true;
        return;
    }
    // strncmp, not strncasecmp: the completer is a VIEW of the resolver, and
    // the resolver matches case-sensitively (class_find_member uses strcmp,
    // and alias.c's comment records that member names are case-sensitive).
    // Typing `MACH<Tab>` used to offer `machine`, which then failed to
    // resolve -- and this function's own dedup below was already
    // case-sensitive, so it disagreed with itself.
    size_t plen = prefix ? strlen(prefix) : 0;
    if (plen && strncmp(cand, prefix, plen) != 0)
        return;
    // Dedup against earlier matches in this completion set.
    for (int i = 0; i < out->count; i++) {
        if (out->items[i] && strcmp(out->items[i], cand) == 0)
            return;
    }
    out->kinds[out->count] = (uint8_t)out->cur_kind;
    out->docs[out->count] = out->cur_doc;
    out->items[out->count++] = cand;
}

const char *comp_kind_name(comp_kind_t k) {
    switch (k) {
    case COMP_KIND_OBJECT:
        return "object";
    case COMP_KIND_COLLECTION:
        return "collection";
    case COMP_KIND_ATTR:
        return "attr";
    case COMP_KIND_METHOD:
        return "method";
    case COMP_KIND_ALIAS:
        return "alias";
    case COMP_KIND_KEYWORD:
        return "keyword";
    default:
        return "value";
    }
}

// Set the detail the next candidates get.
static void set_detail(struct completion *out, comp_kind_t kind, const char *doc) {
    out->cur_kind = kind;
    out->cur_doc = doc;
}

// === Filesystem path completion =============================================

static void complete_paths(const char *prefix, struct completion *out) {
    const char *last_slash = strrchr(prefix, '/');
    char dir[256] = ".";
    const char *partial = prefix;

    // Candidates are bare entry names: narrow the replace span to the
    // basename after the last '/' (out->start arrives at the word start).
    if (last_slash)
        out->start += (int)(last_slash - prefix) + 1;

    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - prefix);
        if (dir_len == 0) {
            dir[0] = '/';
            dir[1] = '\0';
        } else {
            if (dir_len >= sizeof(dir))
                dir_len = sizeof(dir) - 1;
            memcpy(dir, prefix, dir_len);
            dir[dir_len] = '\0';
        }
        partial = last_slash + 1;
    }

    // Route through the VFS so completion works uniformly on host paths,
    // image-vfs HFS directories, and the synthetic /rsrc/<TYPE> resource-fork
    // trees. Skip dotfiles unless the
    // user has typed at least one '.' (same UX as the old host-only path).
    vfs_dir_t *vd = NULL;
    const vfs_backend_t *be = NULL;
    if (vfs_opendir(dir, &vd, &be) < 0)
        return;
    vfs_dirent_t ent;
    set_detail(out, COMP_KIND_VALUE, NULL);
    while (out->count < CMD_MAX_COMPLETIONS) {
        int rc = be->readdir(vd, &ent);
        if (rc <= 0)
            break;
        if (ent.name[0] == '.' && partial[0] != '.')
            continue;
        // Skip non-matches before any stat so the per-entry cost below
        // is only paid for actual candidates.
        size_t plen = strlen(partial);
        if (plen && strncasecmp(ent.name, partial, plen) != 0)
            continue;
        // Directories complete to "name/" (bash-style) so the terminal
        // omits the trailing space and the next Tab descends into them.
        // Name-only backends (host readdir) leave has_stat false; one
        // vfs_stat per matching entry types those.
        bool is_dir = ent.has_stat && (ent.st.mode & VFS_MODE_DIR);
        if (!ent.has_stat) {
            char full[512];
            snprintf(full, sizeof(full), "%s%s%s", dir, (dir[0] == '/' && dir[1] == '\0') ? "" : "/", ent.name);
            vfs_stat_t st;
            if (vfs_stat(full, &st) == 0 && (st.mode & VFS_MODE_DIR))
                is_dir = true;
        }
        char buf[sizeof(ent.name) + 1];
        snprintf(buf, sizeof(buf), "%s%s", ent.name, is_dir ? "/" : "");
        const char *copy = pool_strdup(buf);
        if (!copy)
            break;
        push_match(out, copy, partial);
    }
    be->closedir(vd);
}

// === Enum / bool helpers ====================================================

static void complete_enum(const char *const *enum_values, const char *partial, struct completion *out) {
    if (!enum_values)
        return;
    set_detail(out, COMP_KIND_VALUE, NULL);
    for (const char *const *ev = enum_values; *ev; ev++)
        push_match(out, *ev, partial);
}

static void complete_bool(const char *partial, struct completion *out) {
    static const char *bool_values[] = {"on", "off", "true", "false", NULL};
    set_detail(out, COMP_KIND_VALUE, NULL);
    for (const char **v = bool_values; *v; v++)
        push_match(out, *v, partial);
}

// === Path-segment completion (object tree) ==================================
//
// `head_buf` is the dotted path from the line start to the cursor's word,
// without the trailing identifier the user is typing. `tail` is that
// trailing identifier. Suggestions are members of the class at
// `head_buf` plus statically-attached children of the resolved object.

// Push one member/child name; object-valued entries complete to "name."
// (bash's directory-slash idiom) so the terminal omits the trailing
// space and the next Tab lists the object's own members.
static void push_name_match(struct completion *out, const char *name, bool is_object, const char *tail) {
    if (!name)
        return;
    if (!is_object) {
        push_match(out, name, tail);
        return;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "%s.", name);
    push_match(out, pool_strdup(buf), tail);
}

// Detail kind of a child object: a collection container or a plain object.
static comp_kind_t object_kind(struct object *o) {
    return (o && class_collection(object_class(o))) ? COMP_KIND_COLLECTION : COMP_KIND_OBJECT;
}

static void complete_class_members(struct object *target, const char *tail, struct completion *out) {
    const class_desc_t *cls = object_class(target);
    if (!cls)
        return;
    for (size_t i = 0; i < cls->n_members; i++) {
        const member_t *m = &cls->members[i];
        if (!m->name)
            continue;
        comp_kind_t kind = m->kind == M_ATTR     ? COMP_KIND_ATTR
                           : m->kind == M_METHOD ? COMP_KIND_METHOD
                           : m->child.collection ? COMP_KIND_COLLECTION
                                                 : COMP_KIND_OBJECT;
        set_detail(out, kind, m->doc);
        push_name_match(out, m->name, m->kind == M_CHILD, tail);
    }
}

struct attached_acc {
    const char *tail;
    struct completion *out;
};

static void each_attached_cb(struct object *parent, struct object *child, void *ud) {
    (void)parent;
    struct attached_acc *acc = (struct attached_acc *)ud;
    const char *name = object_name(child);
    if (!name)
        return;
    // Attached children are objects: complete to "name.".
    set_detail(acc->out, object_kind(child), object_doc(child));
    push_name_match(acc->out, name, true, acc->tail);
}

static void complete_attached(struct object *o, const char *tail, struct completion *out) {
    if (!o)
        return;
    struct attached_acc acc = {.tail = tail, .out = out};
    object_each_attached(o, each_attached_cb, &acc);
}

// Indexed-child completion: `floppy.drives.<TAB>` should suggest live
// indices as bare integers ("0", "1"). Pool-allocates the name strings.
static void complete_indexed_children(struct object *o, const member_t *m, const char *tail, struct completion *out) {
    if (!o || !member_is_collection(m))
        return;
    set_detail(out, COMP_KIND_OBJECT, NULL);
    int idx = object_child_next(o, m, -1);
    while (idx >= 0 && out->count < CMD_MAX_COMPLETIONS) {
        // Indexed children resolve to objects: complete to "N.".
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%d.", idx);
        const char *copy = pool_strdup(tmp);
        if (!copy)
            break;
        push_match(out, copy, tail);
        idx = object_child_next(o, m, idx);
    }
}

// Walk a dotted/bracketed prefix and return the deepest resolvable node.
// `prefix` may be empty (root) or `cpu`, `cpu.pc`, `floppy.drives[0]`,
// `floppy.drives[0].`, etc. Trailing `.` is stripped by the caller.
static node_t resolve_prefix(const char *prefix) {
    struct object *root = object_root();
    node_t cur = (node_t){.obj = root, .member = NULL, .index = -1};
    if (!prefix || !*prefix)
        return cur;
    // object_resolve enforces a strict grammar; fall back to walking
    // segments by hand so a partial `cpu.` (segment ending in `.`) still
    // resolves to the cpu object node.
    node_t r = object_resolve(root, prefix);
    if (node_valid(r))
        return r;
    return (node_t){0};
}

// Path completion for the partial token just before the cursor.
// `partial` is, e.g., "cpu.p", "floppy.drives", "memory.peek.b", or
// "" (line start). Splits into head (everything up to the last '.')
// and tail (the unfinished identifier), resolves the head, and emits
// matching member / child names with the head re-prepended so the
// terminal performs the right textual replace.
static void complete_path(const char *partial, struct completion *out) {
    if (!partial)
        partial = "";

    // Split at the rightmost separator that is not inside `[...]`.
    int split = -1;
    int depth = 0;
    for (int i = 0; partial[i]; i++) {
        if (partial[i] == '[')
            depth++;
        else if (partial[i] == ']' && depth > 0)
            depth--;
        else if (depth == 0 && partial[i] == '.')
            split = i;
    }

    char head[256];
    const char *tail;
    if (split < 0) {
        head[0] = '\0';
        tail = partial;
    } else {
        size_t hlen = (size_t)split;
        if (hlen >= sizeof(head))
            hlen = sizeof(head) - 1;
        memcpy(head, partial, hlen);
        head[hlen] = '\0';
        tail = partial + split + 1;
    }

    node_t n = resolve_prefix(head);
    if (!node_valid(n))
        return;

    // Collect raw match names against the resolved class first, then
    // prepend the head + '.' to each so the terminal replaces the whole
    // partial with the correctly-prefixed candidate.
    struct completion local = {0};

    if (member_is_collection(n.member) && n.index < 0) {
        complete_indexed_children(n.obj, n.member, tail, &local);
    } else {
        // For an object node, the class is on the object itself; for a
        // child-member with index resolved, descend into the live child.
        struct object *target = n.obj;
        if (n.member && n.member->kind == M_CHILD)
            target = n.member->child.collection ? object_entry_at(n.obj, n.member, n.index)
                                                : object_named_child(n.obj, n.member);
        if (target) {
            complete_class_members(target, tail, &local);
            complete_attached(target, tail, &local);
        }
    }

    // Prepend head + '.' (or just head if empty).
    for (int i = 0; i < local.count && out->count < CMD_MAX_COMPLETIONS; i++) {
        const char *cand = local.items[i];
        if (!cand)
            continue;
        char composed[512];
        if (head[0])
            snprintf(composed, sizeof(composed), "%s.%s", head, cand);
        else
            snprintf(composed, sizeof(composed), "%s", cand);
        const char *copy = pool_strdup(composed);
        if (!copy)
            break;
        // Push as raw — push_match would re-filter against `partial` here,
        // but we already filtered against `tail`, so use a direct append.
        if (out->count < CMD_MAX_COMPLETIONS) {
            out->kinds[out->count] = local.kinds[i];
            out->docs[out->count] = local.docs[i];
            out->items[out->count++] = copy;
        }
    }
}

// === Commands =================================================================
//
// Command words (commands.h) at the head of a statement: those whose target
// is a method now, and which no root member or `def` function shadows.

struct command_acc {
    const char *partial;
    struct completion *out;
};

static bool command_cb(const char *name, const char *target, bool builtin, void *ud) {
    (void)target;
    (void)builtin;
    struct command_acc *acc = (struct command_acc *)ud;
    node_t n;
    if (shell_head_resolve(name, strlen(name), &n, NULL, NULL, 0) != SHELL_HEAD_COMMAND)
        return true;
    // The name is the iteration's copy: keep it in the per-call pool.
    const char *copy = pool_strdup(name);
    if (!copy)
        return false;
    set_detail(acc->out, COMP_KIND_METHOD, n.member->doc);
    push_match(acc->out, copy, acc->partial);
    return acc->out->count < CMD_MAX_COMPLETIONS;
}

static void complete_commands(const char *partial, struct completion *out) {
    struct command_acc acc = {.partial = partial, .out = out};
    shell_command_each(command_cb, &acc);
}

// === Argument completion ====================================================
//
// At an argument position: the resolved method at the head of the statement
// and the declared slot the cursor's word fills (script_arg_slot).  The
// slot's kind picks the candidates.

static void complete_method_arg(const member_t *m, int slot, const char *partial, struct completion *out) {
    if (!m || m->kind != M_METHOD || m->method.nargs <= 0)
        return;
    int n = m->method.nargs;
    const arg_decl_t *args = m->method.args;
    int fixed_n = (args[n - 1].validation_flags & OBJ_ARG_REST) ? n - 1 : n;

    // Named-argument value: `name=partial` → complete the value against the
    // named slot's kind (enum/bool — other kinds have no closed value set).
    const char *eq = strchr(partial, '=');
    if (eq && eq != partial) {
        int i = script_arg_slot(m, 0, partial, (size_t)(eq - partial));
        if (i < 0 || i >= fixed_n)
            return;
        const char *nm = args[i].name;
        char buf[160];
        set_detail(out, COMP_KIND_VALUE, args[i].doc);
        if (args[i].kind == V_ENUM && args[i].enum_values) {
            for (const char *const *ev = args[i].enum_values; *ev; ev++) {
                snprintf(buf, sizeof(buf), "%s=%s", nm, *ev);
                push_match(out, pool_strdup(buf), partial);
            }
        } else if (args[i].kind == V_BOOL) {
            static const char *bool_vals[] = {"true", "false", NULL};
            for (const char *const *bv = bool_vals; *bv; bv++) {
                snprintf(buf, sizeof(buf), "%s=%s", nm, *bv);
                push_match(out, pool_strdup(buf), partial);
            }
        }
        return;
    }

    if (slot < 0)
        return;
    const arg_decl_t *a = &args[slot];
    switch (a->kind) {
    case V_BOOL:
        complete_bool(partial, out);
        break;
    case V_ENUM:
        complete_enum(a->enum_values, partial, out);
        break;
    case V_OBJECT:
        complete_path(partial, out);
        break;
    case V_STRING:
        // A string declared VAL_PATH names a filesystem path. Other strings
        // get nothing — guessing here would litter the menu.
        if (a->presentation_flags & VAL_PATH)
            complete_paths(partial, out);
        break;
    default:
        break;
    }

    // After the positional candidates, offer `name=` for the declared
    // arguments this slot (or a later one) could still fill by name.
    for (int i = slot; i < fixed_n; i++) {
        if (!args[i].name)
            continue;
        char nbuf[96];
        snprintf(nbuf, sizeof(nbuf), "%s=", args[i].name);
        set_detail(out, COMP_KIND_ATTR, args[i].doc);
        push_match(out, pool_strdup(nbuf), partial);
    }
}

// === Line-start (command-position) completion ================================

static void complete_root_members(const char *tail, struct completion *out) {
    struct object *root = object_root();
    if (!root)
        return;
    complete_class_members(root, tail, out);
    complete_attached(root, tail, out);
}

// === Binding-name completion (`$par` → `$pc`, `$pram_...`) =================

typedef struct {
    struct completion *out;
    const char *prefix; // without the leading '$'
} binding_complete_ctx_t;

static bool binding_complete_var_cb(const char *name, const value_t *v, void *ud) {
    (void)v;
    binding_complete_ctx_t *cc = (binding_complete_ctx_t *)ud;
    if (!name || (cc->prefix[0] && strncasecmp(name, cc->prefix, strlen(cc->prefix)) != 0))
        return true;
    char buf[128];
    snprintf(buf, sizeof(buf), "$%s", name);
    push_match(cc->out, pool_strdup(buf), NULL);
    return true;
}

static bool binding_complete_alias_cb(const char *name, const char *path, alias_kind_t kind, void *ud) {
    (void)path;
    (void)kind;
    binding_complete_ctx_t *cc = (binding_complete_ctx_t *)ud;
    if (cc->prefix[0] && strncasecmp(name, cc->prefix, strlen(cc->prefix)) != 0)
        return true;
    char buf[128];
    snprintf(buf, sizeof(buf), "$%s", name ? name : "");
    push_match(cc->out, pool_strdup(buf), NULL);
    return true;
}

static void complete_bindings(const char *prefix, struct completion *out) {
    binding_complete_ctx_t cc = {.out = out, .prefix = prefix};
    set_detail(out, COMP_KIND_ALIAS, NULL);
    // Scope bindings first (let / --var), then the alias table. The
    // alias walk is prefix-filtered in its callback so the ~500-entry
    // mac-global table doesn't flood a bare `$`.
    shell_var_each(binding_complete_var_cb, &cc);
    alias_each(binding_complete_alias_cb, &cc);
}

// === Statement position =====================================================

// True when `word` (length n) is a `name=value` argument: an identifier
// followed by '='.
static bool is_named_word(const char *word, size_t n) {
    if (n == 0 || !(isalpha((unsigned char)word[0]) || word[0] == '_'))
        return false;
    for (size_t i = 1; i < n; i++) {
        if (word[i] == '=')
            return true;
        if (!(isalnum((unsigned char)word[i]) || word[i] == '_'))
            return false;
    }
    return false;
}

// The partial word [s, e) as a NUL-terminated copy (truncated to fit).
static void copy_word(char *buf, size_t size, const char *s, const char *e) {
    size_t n = (size_t)(e - s);
    if (n >= size)
        n = size - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
}

// The head of a statement: `$` bindings, a mid-path partial's members, or
// the statement keywords, root members and command words.
static void complete_head(const char *partial, struct completion *out) {
    if (partial[0] == '$') {
        complete_bindings(partial + 1, out);
        return;
    }
    if (strpbrk(partial, ".[")) {
        complete_path(partial, out);
        return;
    }
    for (size_t i = 0; i < object_keyword_count(); i++) {
        if (!object_keyword_is_statement(i))
            continue;
        set_detail(out, COMP_KIND_KEYWORD, NULL);
        push_match(out, object_keyword(i), partial);
    }
    complete_root_members(partial, out);
    complete_commands(partial, out);
}

// The arguments of the command statement `st`, whose head ends at
// `head_end`, with the cursor at `cur`.
static void complete_arguments(const char *line, const script_stmt_t *st, const char *head_end, const char *cur,
                               struct completion *out) {
    // Walk the argument words before the cursor's, counting positionals
    // (a `name=` word names its own slot).
    int pos = 0;
    const char *p = head_end;
    while (p < cur && isspace((unsigned char)*p))
        p++;
    const char *word = cur;
    while (p < cur) {
        bool open = false;
        const char *e = script_arg_end(p, cur, &open);
        if (e >= cur) {
            if (open)
                return; // inside a string or a parenthesised expression
            word = p;
            break;
        }
        if (!is_named_word(p, (size_t)(e - p)))
            pos++;
        p = e;
        while (p < cur && isspace((unsigned char)*p))
            p++;
    }
    out->start = (int)(word - line);
    char partial[512];
    copy_word(partial, sizeof(partial), word, cur);
    if (partial[0] == '$') {
        complete_bindings(partial + 1, out);
        return;
    }

    // The head: a tree path, or a bare word read the way the interpreter
    // reads it -- a command stands for the method it names; a `def`
    // function has no declared arguments.
    if (*st->head == '$')
        return;
    char head[256];
    copy_word(head, sizeof(head), st->head, head_end);
    node_t cmd = object_resolve(object_root(), head);
    if (!node_valid(cmd) && !strpbrk(head, ".[") &&
        shell_word_resolve(head, strlen(head), &cmd, NULL, NULL, 0) != SHELL_HEAD_COMMAND)
        cmd = (node_t){0};
    if (!node_valid(cmd) || !cmd.member || cmd.member->kind != M_METHOD)
        return;
    const member_t *m = cmd.member;
    const char *eq = is_named_word(partial, strlen(partial)) ? strchr(partial, '=') : NULL;
    int slot = eq ? script_arg_slot(m, 0, partial, (size_t)(eq - partial)) : script_arg_slot(m, pos, NULL, 0);

    // The argument context: the method's full path and the declared slot.
    char path[200];
    object_compute_path(cmd.obj, path, sizeof(path));
    snprintf(out->ctx_method, sizeof(out->ctx_method), "%s%s%s", path, path[0] ? "." : "", m->name);
    out->has_context = true;
    out->ctx_arg_index = slot;
    out->ctx_arg_name = slot >= 0 ? m->method.args[slot].name : NULL;
    complete_method_arg(m, slot, partial, out);
}

// === Public entry point =====================================================

void shell_complete(const char *line, int cursor_pos, struct completion *out) {
    // Thread-affinity guard (compiled out in release). See worker_thread.h.
    worker_thread_assert("shell_complete");

    if (!line || !out)
        return;
    out->count = 0;
    out->has_context = false;
    out->ctx_method[0] = '\0';
    out->ctx_arg_index = -1;
    out->ctx_arg_name = NULL;
    out->truncated = false;
    set_detail(out, COMP_KIND_VALUE, NULL);
    pool_reset();

    int len = (int)strlen(line);
    if (cursor_pos < 0)
        cursor_pos = 0;
    if (cursor_pos > len)
        cursor_pos = len;
    const char *cur = line + cursor_pos;
    out->start = cursor_pos;
    out->end = cursor_pos;

    // The source line holding the cursor, up to the cursor, split the way
    // the parser reads it: the statement the cursor is in is the line's
    // own, or an inline block's once past its `{`.
    const char *ls = cur;
    while (ls > line && ls[-1] != '\n')
        ls--;
    script_line_t ln;
    script_line_split(ls, cur, &ln);
    if (ln.comment || ln.close)
        return; // in a comment, or past an inline block's `}`
    const script_stmt_t *st = ln.open ? &ln.body : &ln.stmt;
    if (st->kind == SCRIPT_STMT_EMPTY) {
        // Nothing typed yet at a statement position.
        if (cur > ls && !isspace((unsigned char)cur[-1]) && cur[-1] != '{')
            return;
        complete_head("", out);
        return;
    }

    // The head word: where the cursor still is, it is completed as a head.
    const char *s = st->start;
    if (*s != '$' && !isalpha((unsigned char)*s) && *s != '_')
        return; // an expression statement
    const char *he = script_path_end(s, cur);
    if (!he)
        return; // inside an index
    if (he + 1 == cur && *he == '.')
        he = cur; // `machine.cpu.` lists the members
    if (he == cur) {
        out->start = (int)(s - line);
        char partial[512];
        copy_word(partial, sizeof(partial), s, cur);
        complete_head(partial, out);
        return;
    }
    // Past the head only a command takes argument-mode words.
    if (st->kind != SCRIPT_STMT_COMMAND || !isspace((unsigned char)*he))
        return;
    complete_arguments(line, st, he, cur, out);
}
