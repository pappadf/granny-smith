// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_complete.c
// Tab completion engine that walks the object tree at the cursor's
// path position. See proposal-module-object-model.md §4.6 — line-start
// suggests root children and pragmas, mid-path suggests members of the
// resolved-so-far object, method-arg position dispatches by arg_decl[i],
// and any cursor inside `$(...)`, `${...}`, or `"..."` returns nothing.

#include "cmd_complete.h"
#include "cmd_symbol.h"
#include "worker_thread.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include "../object/object.h"
#include "../object/value.h"

// Phase 5c — legacy command registry deleted; no more `cmd_head`. The
// completion code below skips the legacy branch entirely.

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
    if (!cand || out->count >= CMD_MAX_COMPLETIONS)
        return;
    size_t plen = prefix ? strlen(prefix) : 0;
    if (plen && strncasecmp(cand, prefix, plen) != 0)
        return;
    // Dedup against earlier matches in this completion set.
    for (int i = 0; i < out->count; i++) {
        if (out->items[i] && strcmp(out->items[i], cand) == 0)
            return;
    }
    out->items[out->count++] = cand;
}

// === Filesystem path completion =============================================

static void complete_paths(const char *prefix, struct completion *out) {
    const char *last_slash = strrchr(prefix, '/');
    char dir[256] = ".";
    const char *partial = prefix;

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

    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && out->count < CMD_MAX_COMPLETIONS) {
        if (ent->d_name[0] == '.' && partial[0] != '.')
            continue;
        const char *copy = pool_strdup(ent->d_name);
        if (!copy)
            break;
        push_match(out, copy, partial);
    }
    closedir(d);
}

// === Enum / bool helpers ====================================================

static void complete_enum(const char *const *enum_values, const char *partial, struct completion *out) {
    if (!enum_values)
        return;
    for (const char *const *ev = enum_values; *ev; ev++)
        push_match(out, *ev, partial);
}

static void complete_bool(const char *partial, struct completion *out) {
    static const char *bool_values[] = {"on", "off", "true", "false", NULL};
    for (const char **v = bool_values; *v; v++)
        push_match(out, *v, partial);
}

// === Depth-tracking state machine ===========================================
//
// Mirrors the §4.1.2 "balanced tokens" rule. `paren` counts `$(`/`(`/`)`
// inside expressions, `brace` counts `${...}` interpolation regions.
// `bracket` counts `[...]` subscripts at top level — the contents are
// numeric or another `$(...)`, neither of which is a tree path.

typedef struct {
    int paren;
    int brace;
    int bracket;
    bool in_string;
    bool escape_next;
} parse_state_t;

// Advance state machine by one character. Returns the number of input
// bytes consumed (1 normally; 2 for `$(` / `${` openers so the caller
// doesn't double-count).
static int advance(parse_state_t *st, const char *line, int i, int len) {
    unsigned char c = (unsigned char)line[i];
    if (st->in_string) {
        if (st->escape_next) {
            st->escape_next = false;
            return 1;
        }
        if (c == '\\') {
            st->escape_next = true;
            return 1;
        }
        if (c == '$' && i + 1 < len && line[i + 1] == '{') {
            st->brace++;
            return 2;
        }
        if (c == '"')
            st->in_string = false;
        return 1;
    }
    if (st->brace > 0) {
        if (c == '"') {
            st->in_string = true;
            return 1;
        }
        if (c == '}') {
            st->brace--;
            return 1;
        }
        if (c == '$' && i + 1 < len && line[i + 1] == '(') {
            st->paren++;
            return 2;
        }
        if (c == '$' && i + 1 < len && line[i + 1] == '{') {
            st->brace++;
            return 2;
        }
        if (c == '(')
            st->paren++;
        else if (c == ')' && st->paren > 0)
            st->paren--;
        return 1;
    }
    if (st->paren > 0) {
        if (c == '"') {
            st->in_string = true;
            return 1;
        }
        if (c == '$' && i + 1 < len && line[i + 1] == '{') {
            st->brace++;
            return 2;
        }
        if (c == '$' && i + 1 < len && line[i + 1] == '(') {
            st->paren++;
            return 2;
        }
        if (c == '(')
            st->paren++;
        else if (c == ')')
            st->paren--;
        return 1;
    }
    // Depth zero, not in a string.
    if (c == '"') {
        st->in_string = true;
        return 1;
    }
    if (c == '$' && i + 1 < len && line[i + 1] == '(') {
        st->paren++;
        return 2;
    }
    if (c == '$' && i + 1 < len && line[i + 1] == '{') {
        st->brace++;
        return 2;
    }
    if (c == '[') {
        st->bracket++;
        return 1;
    }
    if (c == ']' && st->bracket > 0) {
        st->bracket--;
        return 1;
    }
    return 1;
}

// True if any non-shell context is currently open at this state.
static bool in_special_context(const parse_state_t *st) {
    return st->in_string || st->paren > 0 || st->brace > 0 || st->bracket > 0;
}

// === Path-segment completion (object tree) ==================================
//
// `head_buf` is the dotted path from the line start to the cursor's word,
// without the trailing identifier the user is typing. `tail` is that
// trailing identifier. Suggestions are members of the class at
// `head_buf` plus statically-attached children of the resolved object.

static void complete_class_members(const class_desc_t *cls, const char *tail, struct completion *out) {
    if (!cls)
        return;
    for (size_t i = 0; i < cls->n_members; i++) {
        const member_t *m = &cls->members[i];
        if (!m->name)
            continue;
        push_match(out, m->name, tail);
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
    push_match(acc->out, name, acc->tail);
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
    if (!o || !m || m->kind != M_CHILD || !m->child.indexed || !m->child.next)
        return;
    int idx = m->child.next(o, -1);
    while (idx >= 0 && out->count < CMD_MAX_COMPLETIONS) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%d", idx);
        const char *copy = pool_strdup(tmp);
        if (!copy)
            break;
        push_match(out, copy, tail);
        idx = m->child.next(o, idx);
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

    if (n.member && n.member->kind == M_CHILD && n.member->child.indexed && n.index < 0) {
        complete_indexed_children(n.obj, n.member, tail, &local);
    } else {
        // For an object node, the class is on the object itself; for a
        // child-member with index resolved, descend into the live child.
        struct object *target = n.obj;
        if (n.member && n.member->kind == M_CHILD) {
            if (n.member->child.indexed && n.member->child.get)
                target = n.member->child.get(n.obj, n.index);
            else if (n.member->child.lookup)
                target = n.member->child.lookup(n.obj, n.member->name);
        }
        if (target) {
            complete_class_members(object_class(target), tail, &local);
            complete_attached(target, tail, &local);
        }
    }

    // Prepend head + '.' (or just head if empty).
    for (int i = 0; i < local.count && out->count < CMD_MAX_COMPLETIONS; i++) {
        const char *cand = local.items[i];
        if (!cand)
            continue;
        char composed[256];
        if (head[0])
            snprintf(composed, sizeof(composed), "%s.%s", head, cand);
        else
            snprintf(composed, sizeof(composed), "%s", cand);
        const char *copy = pool_strdup(composed);
        if (!copy)
            break;
        // Push as raw — push_match would re-filter against `partial` here,
        // but we already filtered against `tail`, so use a direct append.
        if (out->count < CMD_MAX_COMPLETIONS)
            out->items[out->count++] = copy;
    }
}

// === Argument completion ====================================================
//
// At arg position we need (a) the resolved method-or-command at token 0
// and (b) the zero-based arg index we're filling. For root methods we
// dispatch via `arg_decl_t.kind`; for legacy commands we dispatch via
// `arg_spec` (kept here as the only remaining use of the old metadata).

static void complete_method_arg(const member_t *m, int arg_idx, const char *partial, struct completion *out) {
    if (!m || m->kind != M_METHOD)
        return;
    if (arg_idx < 0)
        return;
    // OBJ_ARG_REST trailing arg: dispatch the last declared arg for any
    // index past the declared list.
    int n = m->method.nargs;
    if (n <= 0)
        return;
    int idx = arg_idx;
    const arg_decl_t *last = &m->method.args[n - 1];
    if (idx >= n) {
        if (last->flags & OBJ_ARG_REST)
            idx = n - 1;
        else
            return;
    }
    const arg_decl_t *a = &m->method.args[idx];
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
    case V_STRING: {
        // Heuristic: arg names of "path" / "src" / "dst" / "out_dir" /
        // "file" → filesystem paths. Other strings get nothing — guessing
        // a tree path here would litter the menu with irrelevant names.
        const char *nm = a->name ? a->name : "";
        if (strstr(nm, "path") || strstr(nm, "src") || strstr(nm, "dst") || strstr(nm, "file") || strstr(nm, "dir"))
            complete_paths(partial, out);
        break;
    }
    default:
        break;
    }
}

// === Line-start (command-position) completion ================================

static void complete_root_members(const char *tail, struct completion *out) {
    struct object *root = object_root();
    if (!root)
        return;
    complete_class_members(object_class(root), tail, out);
    complete_attached(root, tail, out);
}

// === Word-boundary scan ======================================================
//
// At depth zero we treat unquoted whitespace as the only word break.
// Walks a fresh state machine from line[0] to cursor_pos, tracking the
// most recent word start at depth zero.

typedef struct {
    int word_start; // index of first char of current word (or cursor_pos)
    int word_count; // number of completed words preceding the current one
    bool inside_special; // true if cursor falls inside string/expr/bracket
    int first_word_start; // for extracting the first-word string
    int first_word_end; // exclusive
} cursor_info_t;

static cursor_info_t scan_to_cursor(const char *line, int cursor_pos) {
    cursor_info_t info = {.word_start = cursor_pos,
                          .word_count = 0,
                          .inside_special = false,
                          .first_word_start = -1,
                          .first_word_end = -1};
    parse_state_t st = {0};
    int len = cursor_pos;
    bool in_word = false;
    int cur_start = -1;
    for (int i = 0; i < len;) {
        unsigned char c = (unsigned char)line[i];
        if (in_special_context(&st)) {
            i += advance(&st, line, i, len);
            continue;
        }
        if (isspace(c)) {
            if (in_word) {
                if (info.first_word_start < 0) {
                    info.first_word_start = cur_start;
                    info.first_word_end = i;
                }
                info.word_count++;
                in_word = false;
                cur_start = -1;
            }
            i += advance(&st, line, i, len);
            continue;
        }
        if (!in_word) {
            in_word = true;
            cur_start = i;
        }
        i += advance(&st, line, i, len);
    }
    info.inside_special = in_special_context(&st);
    if (in_word)
        info.word_start = cur_start;
    else
        info.word_start = cursor_pos;
    return info;
}

// === Public entry point =====================================================

void shell_complete(const char *line, int cursor_pos, struct completion *out) {
    // Thread-affinity guard (compiled out in release). See worker_thread.h.
    worker_thread_assert("shell_complete");

    if (!line || !out)
        return;
    out->count = 0;
    pool_reset();

    int len = (int)strlen(line);
    if (cursor_pos < 0)
        cursor_pos = 0;
    if (cursor_pos > len)
        cursor_pos = len;

    cursor_info_t info = scan_to_cursor(line, cursor_pos);
    if (info.inside_special)
        return; // §4.6: empty inside $(...), ${...}, "..."

    // Extract the partial being completed.
    char partial[512];
    int plen = cursor_pos - info.word_start;
    if (plen < 0)
        plen = 0;
    if (plen >= (int)sizeof(partial))
        plen = (int)sizeof(partial) - 1;
    memcpy(partial, line + info.word_start, plen);
    partial[plen] = '\0';

    // Word 0: command position. Suggest root members + attached children,
    // filtered by the typed prefix. Mid-path partials (containing '.' or
    // '[') walk into the tree.
    if (info.word_count == 0) {
        bool dotted = (strchr(partial, '.') != NULL) || (strchr(partial, '[') != NULL);
        if (dotted)
            complete_path(partial, out);
        else
            complete_root_members(partial, out);
        return;
    }

    // Argument position. Pull the first word out as the command path.
    char first[256];
    int fwlen = info.first_word_end - info.first_word_start;
    if (fwlen < 0)
        fwlen = 0;
    if (fwlen >= (int)sizeof(first))
        fwlen = (int)sizeof(first) - 1;
    memcpy(first, line + info.first_word_start, fwlen);
    first[fwlen] = '\0';

    // Resolve as a tree path — root methods and dotted method paths
    // (`floppy.drives[0].insert`) both land here.
    node_t cmd_node = object_resolve(object_root(), first);
    if (node_valid(cmd_node) && cmd_node.member && cmd_node.member->kind == M_METHOD)
        complete_method_arg(cmd_node.member, info.word_count - 1, partial, out);
}
