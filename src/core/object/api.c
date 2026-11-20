// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// api.c
// Public entry point: object_eval. The former gs_inspect and gs_complete
// entry points were folded into the object model itself — schema is now
// reached via `<path>.meta.*` and tab-completion via
// `object_eval("meta.complete", [...])`.

#include "api.h"

#include "value_format.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_value.h"
#include "object.h"
#include "value.h"
#include "job/job.h" // EMU_THREAD_CHECK

// === JSON formatting ========================================================
//
// The document is value_format's VFMT_JSON_TAGGED rendering (value_format.h):
//   numeric / bool      → bare number / true / false (a VFLAG_HEX VK_UINT → "0x…" string)
//   strings             → quoted string with the standard escapes
//   bytes               → "0x..." hex string
//   enum                → {"enum": "<name>", "index": <idx>}
//   list / map          → JSON array / object, recursing
//   object              → {"object": "<class>", "name": "<name>", "path": …}
//   error               → {"error": "<message>"}
//   none                → null
// Every result, failures included, is a value_t rendered through that one
// encoder into a growable buffer; object_eval then copies it out whole or, when
// it does not fit, replaces it with an explicit {"error": ...} so no
// consumer parses a truncated document.

// Render `v` and copy it into out_buf. Returns false (out_buf then holding an
// error document naming both sizes) when the document exceeds out_size - 1.
static bool emit_json(const value_t *v, const char *path, char *out_buf, size_t out_size) {
    vbuf_t b = {0};
    value_format(v, VFMT_JSON_TAGGED, &b);
    const char *doc = b.p ? b.p : "null";
    size_t len = b.p ? b.len : 4;
    bool fits = len <= out_size - 1;
    if (fits) {
        memcpy(out_buf, doc, len + 1);
    } else {
        // The result is larger than the limit. A silently truncated payload
        // is worse than a failure -- the consumer would parse garbage (or,
        // for a string result, a shorter valid-looking document).
        value_t e = val_err("result of '%.64s' is %zu bytes, over the %zu-byte result limit", path, len, out_size - 1);
        vbuf_t eb = {0};
        value_format(&e, VFMT_JSON_TAGGED, &eb);
        size_t n = eb.len < out_size - 1 ? eb.len : out_size - 1;
        if (eb.p)
            memcpy(out_buf, eb.p, n);
        out_buf[n] = '\0';
        vbuf_free(&eb);
        value_free(&e);
    }
    vbuf_free(&b);
    return fits;
}

// === `args_json` =============================================================
//
// One JSON document read with the one reader (json_value_parse): an array
// of primitive values (numbers, strings, booleans, null) is positional; an
// object of them is named.  Nested containers are not argument shapes the
// methods declare; they are refused rather than guessed at.  Returns 0 on
// success and writes `*out_argv` (heap-allocated, caller frees with
// free_args), `*out_argc` and, for the object form, the parallel
// `*out_names`.

static void free_args(value_t *argv, int argc) {
    if (!argv)
        return;
    for (int i = 0; i < argc; i++)
        value_free(&argv[i]);
    free(argv);
}

// Free a parallel names array produced by the object form.
static void free_arg_names(char **names, int argc) {
    if (!names)
        return;
    for (int i = 0; i < argc; i++)
        free(names[i]);
    free(names);
}

// True for the kinds an argument may be: what a JSON scalar parses to.
static bool arg_is_scalar(const value_t *v) {
    return v->kind == VK_NONE || v->kind == VK_BOOL || v->kind == VK_INT || v->kind == VK_FLOAT || v->kind == VK_STRING;
}

static int json_parse_args(const char *json, value_t **out_argv, int *out_argc, char ***out_names) {
    *out_argv = NULL;
    *out_argc = 0;
    *out_names = NULL;
    if (!json || !*json)
        return 0;
    value_t doc;
    if (!json_value_parse(json, &doc, NULL, 0))
        return -1;
    bool named = doc.kind == VK_MAP;
    size_t n = named ? doc.map.len : doc.kind == VK_LIST ? doc.list.len : 0;
    if (!named && doc.kind != VK_LIST) {
        value_free(&doc);
        return -1;
    }
    value_t *argv = n ? (value_t *)calloc(n, sizeof(value_t)) : NULL;
    char **names = named && n ? (char **)calloc(n, sizeof(char *)) : NULL;
    if (n && (!argv || (named && !names))) {
        free(argv);
        free(names);
        value_free(&doc);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        value_t *v = named ? &doc.map.entries[i].val : &doc.list.items[i];
        if (!arg_is_scalar(v)) {
            free(argv);
            free(names);
            value_free(&doc);
            return -1;
        }
    }
    // Move the elements out of the document, which then frees empty
    for (size_t i = 0; i < n; i++) {
        if (named) {
            names[i] = doc.map.entries[i].key;
            doc.map.entries[i].key = NULL;
            argv[i] = doc.map.entries[i].val;
            doc.map.entries[i].val = val_none();
        } else {
            argv[i] = doc.list.items[i];
            doc.list.items[i] = val_none();
        }
    }
    value_free(&doc);
    *out_argv = argv;
    *out_argc = (int)n;
    *out_names = names;
    return 0;
}

// === Public entry points ====================================================

int object_eval(const char *path, const char *args_json, char *out_buf, size_t out_size) {
    // Thread-affinity guard (compiled out in release). See job/job.h.
    EMU_THREAD_CHECK("object_eval");

    if (!out_buf || out_size == 0)
        return -1;
    out_buf[0] = '\0';

    value_t *argv = NULL;
    int argc = 0;
    char **arg_names = NULL;
    value_t v;
    node_t n = {0};

    // Every failure is a VK_ERROR rendered by the same encoder as a result,
    // so a JS caller always sees the {"error": ...} shape (object-model.md),
    // never a bare string.
    if (!path || !*path) {
        v = val_err("empty path");
        path = "";
    } else if (!node_valid(n = object_resolve(object_root(), path))) {
        v = val_err("path '%s' did not resolve", path);
    } else if (args_json && *args_json && json_parse_args(args_json, &argv, &argc, &arg_names) < 0) {
        v = val_err("args_json must be a JSON array of primitives or an object of named arguments");
    } else if (arg_names && (!n.member || n.member->kind != MK_METHOD)) {
        // Method paths always dispatch via node_call. Attribute paths route
        // to node_set when args carry exactly one value, otherwise node_get.
        // Bare object/child nodes go through node_get (a VK_OBJECT reference).
        v = val_err("path '%s' is not a method — named arguments require one", path);
    } else if (n.member && n.member->kind == MK_METHOD && arg_names) {
        // Object form: bind every entry by name, no positionals.
        named_arg_t named[OBJ_BIND_MAX_ARGS];
        if (argc > OBJ_BIND_MAX_ARGS) {
            v = val_err("too many named arguments (limit %d)", OBJ_BIND_MAX_ARGS);
        } else {
            for (int i = 0; i < argc; i++) {
                named[i].name = arg_names[i];
                named[i].value = argv[i];
            }
            value_t bound[OBJ_BIND_MAX_ARGS];
            int bound_n = 0;
            value_t err = node_bind_args(n, 0, NULL, argc, named, bound, &bound_n);
            v = val_is_error(&err) ? err : node_call(n, bound_n, bound);
        }
    } else if (n.member && n.member->kind == MK_METHOD) {
        v = node_call(n, argc, argv);
    } else if (n.member && n.member->kind == MK_ATTR && argc == 1) {
        // node_set takes ownership of its value; pass a copy so the
        // outer free_args() can still walk argv.
        v = node_set(n, value_dup(&argv[0]));
    } else if (argc > 0) {
        v = val_err("path '%s' does not accept %d arg(s)", path, argc);
    } else {
        v = node_get(n);
    }

    int rc = val_is_error(&v) ? -1 : 0;
    if (!emit_json(&v, path, out_buf, out_size))
        rc = -1;
    value_free(&v);
    free_args(argv, argc);
    free_arg_names(arg_names, argc);
    return rc;
}
