// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// gs_lua.c -- the Lua state and the bridge from Lua to the object tree.
//
// Shape (docs/internals/core/lua/lua.md):
//   * A NODE PROXY is a userdata holding path text.  Indexing it appends a
//     segment and resolves the path: a child or a collection entry is a new
//     proxy, an attribute is read (node_get) and pushed as a Lua value, a
//     method is a callable proxy.  Assigning through it is node_set; calling
//     it is node_call.  Proxies hold text, not node_t, so a binding survives
//     machine.boot the way an alias does.
//   * VALUES cross in both directions through push_value / to_value: lists
//     are 1-based sequences, maps are tables whose insertion order `pairs`
//     keeps, bytes are strings, enums are their labels, VK_OBJECT is a proxy,
//     VK_ERROR is raised.
//   * The root's children are reachable as globals through _G's __index;
//     declaring a global that names one is refused.
//   * The embedded library gs_lua_lib.lua (gs.try, F"...", ...) runs when
//     the state is made.

#include "gs_lua.h"

#include "object.h"
#include "out.h"
#include "platform_hooks.h"
#include "value.h"
#include "value_format.h"
#include "job/job.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gs_lua_lib.h" // gs_lua_lib_lua[]: the embedded library (bin2c)

// --- State ------------------------------------------------------------------

static lua_State *g_L; // the one state; made on first use
static gs_lua_reporter_fn g_reporter; // where a failed chunk's report goes
static volatile int g_interrupt; // gs_lua_interrupt() was called

// Registry keys (their addresses): the `none` sentinel, the quit and
// interrupt signals, the map-order side table.
static const char K_NONE, K_QUIT, K_INTERRUPT, K_MAPORDER;

// Metatable names.
#define MT_NODE  "gs.node"
#define MT_ERROR "gs.error"
#define MT_BYTES "gs.bytes"
#define MT_MAP   "gs.map"

// The directory of the file each running chunk came from, innermost last:
// what a relative `require` resolves against.
#define DIR_STACK_MAX 32
static char *g_dirs[DIR_STACK_MAX];
static int g_ndirs;

// Node proxy: path text, and whether it names a method (callable).
typedef struct {
    bool method;
    char path[];
} gsl_node_t;

// Bytes argument (gs.bytes): a buffer the tree receives as VK_BYTES.
typedef struct {
    size_t n;
    uint8_t p[];
} gsl_bytes_t;

// --- Errors -----------------------------------------------------------------

// Push an error object: a table {text = ...} with the gs.error metatable.
static void push_error_object(lua_State *L, const char *text) {
    lua_createtable(L, 0, 2);
    lua_pushstring(L, text ? text : "error");
    lua_setfield(L, -2, "text");
    lua_pushstring(L, "error");
    lua_setfield(L, -2, "kind");
    luaL_setmetatable(L, MT_ERROR);
}

// Raise a tree error with the given text (never returns).
static int raise_text(lua_State *L, const char *text) {
    push_error_object(L, text);
    return lua_error(L);
}

// Raise a VK_ERROR, freeing it first (never returns).
static int raise_value(lua_State *L, value_t *err) {
    push_error_object(L, err->err);
    value_free(err);
    return lua_error(L);
}

// tostring(err) is its text.
static int error_tostring(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "text");
    if (lua_type(L, -1) != LUA_TSTRING)
        lua_pushstring(L, "error");
    return 1;
}

// --- Paths and proxies ------------------------------------------------------

static gsl_node_t *check_node(lua_State *L, int idx) {
    return (gsl_node_t *)luaL_checkudata(L, idx, MT_NODE);
}

static gsl_node_t *test_node(lua_State *L, int idx) {
    return (gsl_node_t *)luaL_testudata(L, idx, MT_NODE);
}

// Push a proxy for `path`.
static void push_node(lua_State *L, const char *path, bool method) {
    size_t n = strlen(path);
    gsl_node_t *nd = (gsl_node_t *)lua_newuserdatauv(L, sizeof(*nd) + n + 1, 0);
    nd->method = method;
    memcpy(nd->path, path, n + 1);
    luaL_setmetatable(L, MT_NODE);
}

// True when `s` is a bare identifier segment (what `path.name` can spell).
static bool is_identifier(const char *s) {
    if (!*s)
        return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

// Append the key at stack index `kidx` to `base` as one path segment:
// `.name`, `[N]` or `["key"]`.  False (with a message pushed) when the key
// cannot be a segment.
static bool child_path(lua_State *L, const char *base, int kidx, char *out, size_t cap) {
    int n;
    if (lua_type(L, kidx) == LUA_TSTRING) {
        const char *key = lua_tostring(L, kidx);
        if (strchr(key, '"'))
            goto bad;
        if (is_identifier(key))
            n = snprintf(out, cap, "%s%s%s", base, *base ? "." : "", key);
        else
            n = snprintf(out, cap, "%s[\"%s\"]", base, key);
    } else if (lua_isinteger(L, kidx)) {
        n = snprintf(out, cap, "%s[%lld]", base, (long long)lua_tointeger(L, kidx));
    } else {
        goto bad;
    }
    if (n < 0 || (size_t)n >= cap) {
        lua_pushfstring(L, "path too long under '%s'", base);
        return false;
    }
    return true;
bad:
    lua_pushfstring(L, "'%s' cannot be indexed with a %s", base, luaL_typename(L, kidx));
    return false;
}

static void push_value(lua_State *L, value_t *v);

// Resolve `path` and push what it names: a proxy for an object, a child or
// a method; the value for an attribute.  Raises when nothing is there.
static int push_path(lua_State *L, const char *path) {
    node_t nd = object_resolve(object_root(), path);
    if (!node_valid(nd)) {
        lua_pushfstring(L, "no such path '%s'", path);
        return raise_text(L, lua_tostring(L, -1));
    }
    if (nd.member == NULL) {
        push_node(L, path, false);
        return 1;
    }
    if (nd.member->kind == MK_CHILD) {
        // Validate the child now (an empty collection slot is an error at
        // the read, as it is in the terminal), then hand out the proxy.  An
        // index-less collection stays a proxy too, so `entries[id]` is a path
        // index; `#entries` and gs.list(entries) read it as a list.
        value_t v = node_get(nd);
        if (val_is_error(&v))
            return raise_value(L, &v);
        value_free(&v);
        push_node(L, path, false);
        return 1;
    }
    if (nd.member->kind == MK_METHOD) {
        push_node(L, path, true);
        return 1;
    }
    value_t v = node_get(nd);
    push_value(L, &v);
    return 1;
}

// proxy[key]: the child path, resolved.  A string key that is not a member
// may be a collection's key (`volumes["docs"]`): the bracket form is tried
// before giving up.
static int node_index(lua_State *L) {
    gsl_node_t *nd = check_node(L, 1);
    char path[OBJ_PATH_MAX];
    if (!child_path(L, nd->path, 2, path, sizeof path))
        return raise_text(L, lua_tostring(L, -1));
    if (lua_type(L, 2) == LUA_TSTRING && !node_valid(object_resolve(object_root(), path))) {
        char keyed[OBJ_PATH_MAX];
        int n = snprintf(keyed, sizeof keyed, "%s[\"%s\"]", nd->path, lua_tostring(L, 2));
        if (n > 0 && (size_t)n < sizeof keyed && node_valid(object_resolve(object_root(), keyed)))
            return push_path(L, keyed);
    }
    return push_path(L, path);
}

static bool to_value(lua_State *L, int idx, value_t *out);

// proxy[key] = v: node_set on the attribute.
static int node_newindex(lua_State *L) {
    gsl_node_t *nd = check_node(L, 1);
    char path[OBJ_PATH_MAX];
    if (!child_path(L, nd->path, 2, path, sizeof path))
        return raise_text(L, lua_tostring(L, -1));
    node_t n = object_resolve(object_root(), path);
    if (!node_valid(n)) {
        lua_pushfstring(L, "no such path '%s'", path);
        return raise_text(L, lua_tostring(L, -1));
    }
    if (n.member == NULL || n.member->kind != MK_ATTR) {
        lua_pushfstring(L, "'%s' is not an attribute", path);
        return raise_text(L, lua_tostring(L, -1));
    }
    value_t v;
    if (!to_value(L, 3, &v))
        return raise_text(L, lua_tostring(L, -1));
    value_t r = node_set(n, v); // consumes v
    if (val_is_error(&r))
        return raise_value(L, &r);
    value_free(&r);
    return 0;
}

// True when the table at `idx` carries named arguments: non-empty, every
// key a string, and not a map (a table from the tree, or one gs.map tagged,
// is a value).  An empty table or a sequence is a positional argument.
static bool table_is_named(lua_State *L, int idx) {
    idx = lua_absindex(L, idx);
    if (lua_getmetatable(L, idx)) {
        luaL_getmetatable(L, MT_MAP);
        bool is_map = lua_rawequal(L, -1, -2);
        lua_pop(L, 2);
        if (is_map)
            return false;
    }
    lua_pushnil(L);
    if (!lua_next(L, idx))
        return false;
    bool named = lua_type(L, -2) == LUA_TSTRING;
    lua_pop(L, 2);
    return named;
}

// proxy(...): node_call with positional arguments and, when the last
// argument is a table of string keys, those as the named arguments.
static int node_call_mm(lua_State *L) {
    gsl_node_t *nd = check_node(L, 1);
    if (!nd->method) {
        lua_pushfstring(L, "'%s' is not a method", nd->path);
        return raise_text(L, lua_tostring(L, -1));
    }
    node_t n = object_resolve(object_root(), nd->path);
    if (!node_valid(n) || !n.member || n.member->kind != MK_METHOD) {
        lua_pushfstring(L, "no such method '%s'", nd->path);
        return raise_text(L, lua_tostring(L, -1));
    }
    int top = lua_gettop(L);
    int last = top;
    bool has_named =
        top >= 2 && lua_type(L, top) == LUA_TTABLE && !luaL_testudata(L, top, MT_NODE) && table_is_named(L, top);
    if (has_named)
        last = top - 1;
    int pc = last - 1;
    if (pc > OBJ_BIND_MAX_ARGS)
        return raise_text(L, "too many arguments");
    value_t pos[OBJ_BIND_MAX_ARGS];
    named_arg_t named[OBJ_BIND_MAX_ARGS];
    int nn = 0;
    bool ok = true;
    int i;
    for (i = 0; i < pc; i++) {
        if (!to_value(L, 2 + i, &pos[i])) {
            ok = false;
            break;
        }
    }
    if (ok && has_named) {
        lua_pushnil(L);
        while (lua_next(L, top)) {
            if (lua_type(L, -2) != LUA_TSTRING) {
                lua_pop(L, 2);
                lua_pushstring(L, "named arguments need string keys");
                ok = false;
                break;
            }
            if (nn >= OBJ_BIND_MAX_ARGS) {
                lua_pop(L, 2);
                lua_pushstring(L, "too many named arguments");
                ok = false;
                break;
            }
            named[nn].name = lua_tostring(L, -2); // stays alive: the table holds the key
            if (!to_value(L, -1, &named[nn].value)) {
                lua_remove(L, -2); // the value; the message stays on top
                lua_remove(L, -2); // the key
                ok = false;
                break;
            }
            nn++;
            lua_pop(L, 1);
        }
    }
    value_t r;
    if (ok) {
        if (nn > 0) {
            value_t bound[OBJ_BIND_MAX_ARGS];
            int bc = 0;
            value_t e = node_bind_args(n, pc, pos, nn, named, bound, &bc);
            if (val_is_error(&e))
                r = e;
            else
                r = node_call(n, bc, bound);
        } else {
            r = node_call(n, pc, pos);
        }
    }
    for (int k = 0; k < i; k++)
        value_free(&pos[k]);
    for (int k = 0; k < nn; k++)
        value_free(&named[k].value);
    if (!ok)
        return raise_text(L, lua_tostring(L, -1));
    push_value(L, &r);
    return 1;
}

// #proxy: the length of what the path reads (a collection's entries, a
// list or string attribute).
static int node_len(lua_State *L) {
    gsl_node_t *nd = check_node(L, 1);
    node_t n = object_resolve(object_root(), nd->path);
    if (!node_valid(n)) {
        lua_pushfstring(L, "no such path '%s'", nd->path);
        return raise_text(L, lua_tostring(L, -1));
    }
    value_t v = node_get(n);
    if (val_is_error(&v))
        return raise_value(L, &v);
    lua_Integer len;
    if (v.kind == VK_LIST)
        len = (lua_Integer)v.list.len;
    else if (v.kind == VK_MAP)
        len = (lua_Integer)v.map.len;
    else if (v.kind == VK_STRING)
        len = (lua_Integer)(v.s ? strlen(v.s) : 0);
    else if (v.kind == VK_BYTES)
        len = (lua_Integer)v.bytes.n;
    else {
        value_free(&v);
        lua_pushfstring(L, "'%s' has no length", nd->path);
        return raise_text(L, lua_tostring(L, -1));
    }
    value_free(&v);
    lua_pushinteger(L, len);
    return 1;
}

// tostring(proxy): its path.
static int node_tostring(lua_State *L) {
    gsl_node_t *nd = check_node(L, 1);
    lua_pushstring(L, nd->path);
    return 1;
}

// Two proxies are equal when they name the same path.
static int node_eq(lua_State *L) {
    gsl_node_t *a = test_node(L, 1);
    gsl_node_t *b = test_node(L, 2);
    lua_pushboolean(L, a && b && strcmp(a->path, b->path) == 0);
    return 1;
}

// --- Values: tree -> Lua ------------------------------------------------------

// Push `v` without consuming it.  A VK_ERROR is pushed as an error object
// (the caller decides whether to raise).
static void push_value_ref(lua_State *L, const value_t *v) {
    switch (v->kind) {
    case VK_NONE:
        lua_pushnil(L);
        return;
    case VK_BOOL:
        lua_pushboolean(L, v->b);
        return;
    case VK_INT:
        lua_pushinteger(L, (lua_Integer)v->i);
        return;
    case VK_UINT:
        lua_pushinteger(L, (lua_Integer)v->u);
        return;
    case VK_FLOAT:
        lua_pushnumber(L, v->f);
        return;
    case VK_STRING:
        lua_pushstring(L, v->s ? v->s : "");
        return;
    case VK_BYTES:
        lua_pushlstring(L, (const char *)v->bytes.p, v->bytes.p ? v->bytes.n : 0);
        return;
    case VK_ENUM:
        if (v->enm.table && v->enm.idx >= 0 && (size_t)v->enm.idx < v->enm.n_table && v->enm.table[v->enm.idx])
            lua_pushstring(L, v->enm.table[v->enm.idx]);
        else
            lua_pushinteger(L, v->enm.idx);
        return;
    case VK_LIST: {
        lua_createtable(L, (int)v->list.len, 0);
        for (size_t i = 0; i < v->list.len; i++) {
            const value_t *it = &v->list.items[i];
            if (it->kind == VK_NONE)
                lua_rawgetp(L, LUA_REGISTRYINDEX, &K_NONE); // no holes in a sequence
            else
                push_value_ref(L, it);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        return;
    }
    case VK_MAP: {
        lua_createtable(L, 0, (int)v->map.len);
        int t = lua_gettop(L);
        lua_createtable(L, (int)v->map.len, 0); // the insertion order
        for (size_t i = 0; i < v->map.len; i++) {
            const struct value_entry *e = &v->map.entries[i];
            push_value_ref(L, &e->val);
            lua_setfield(L, t, e->key);
            lua_pushstring(L, e->key);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        // registry[K_MAPORDER][table] = order (weak keys)
        lua_rawgetp(L, LUA_REGISTRYINDEX, &K_MAPORDER);
        lua_pushvalue(L, t);
        lua_pushvalue(L, -3);
        lua_rawset(L, -3);
        lua_pop(L, 2); // the side table and the order
        luaL_setmetatable(L, MT_MAP);
        return;
    }
    case VK_OBJECT: {
        char path[OBJ_PATH_MAX];
        object_compute_path(v->obj, path, sizeof path);
        push_node(L, path, false);
        return;
    }
    case VK_ERROR:
        push_error_object(L, v->err);
        return;
    case VK_REF:
        push_node(L, v->ref ? v->ref : "", false);
        return;
    case VK_RANGE:
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, (lua_Integer)v->range.start);
        lua_setfield(L, -2, "start");
        lua_pushinteger(L, (lua_Integer)v->range.stop);
        lua_setfield(L, -2, "stop");
        lua_pushinteger(L, (lua_Integer)v->range.step);
        lua_setfield(L, -2, "step");
        return;
    }
    lua_pushnil(L);
}

// Push `v` and free it; a VK_ERROR is raised instead.
static void push_value(lua_State *L, value_t *v) {
    if (val_is_error(v)) {
        raise_value(L, v);
        return;
    }
    push_value_ref(L, v);
    value_free(v);
}

// pairs(map) walks the insertion order the tree gave the map.
static int map_next(lua_State *L) {
    // upvalue 1: the order table; upvalue 2: the map
    lua_Integer i = lua_tointeger(L, lua_upvalueindex(3)) + 1;
    lua_rawgeti(L, lua_upvalueindex(1), i);
    if (lua_isnil(L, -1))
        return 1;
    lua_pushinteger(L, i);
    lua_replace(L, lua_upvalueindex(3));
    lua_pushvalue(L, -1);
    lua_gettable(L, lua_upvalueindex(2));
    return 2;
}

static int map_pairs(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &K_MAPORDER);
    lua_pushvalue(L, 1);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        // No recorded order (the table was rebuilt): plain next.
        lua_getglobal(L, "next");
        lua_pushvalue(L, 1);
        lua_pushnil(L);
        return 3;
    }
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, map_next, 3);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

// --- Values: Lua -> tree ------------------------------------------------------

// Convert the Lua value at `idx` into an owned value_t.  False with a
// message pushed when it has no tree shape.
static bool to_value(lua_State *L, int idx, value_t *out) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        *out = val_none();
        return true;
    case LUA_TBOOLEAN:
        *out = val_bool(lua_toboolean(L, idx));
        return true;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx))
            *out = val_int((int64_t)lua_tointeger(L, idx));
        else
            *out = val_float(lua_tonumber(L, idx));
        return true;
    case LUA_TSTRING:
        *out = val_str(lua_tostring(L, idx));
        return true;
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(L, idx) == (void *)&K_NONE) {
            *out = val_none();
            return true;
        }
        break;
    case LUA_TUSERDATA: {
        gsl_node_t *nd = test_node(L, idx);
        if (nd) {
            node_t n = object_resolve(object_root(), nd->path);
            if (!node_valid(n)) {
                lua_pushfstring(L, "no such path '%s'", nd->path);
                return false;
            }
            if (n.member == NULL) {
                *out = val_obj(n.obj);
                return true;
            }
            if (n.member->kind == MK_METHOD) {
                lua_pushfstring(L, "'%s' is a method, not a value", nd->path);
                return false;
            }
            value_t v = node_get(n); // a child's object, or the attribute's value
            if (val_is_error(&v)) {
                lua_pushstring(L, v.err);
                value_free(&v);
                return false;
            }
            *out = v;
            return true;
        }
        gsl_bytes_t *b = (gsl_bytes_t *)luaL_testudata(L, idx, MT_BYTES);
        if (b) {
            *out = val_bytes(b->p, b->n);
            return true;
        }
        break;
    }
    case LUA_TTABLE: {
        lua_Integer len = (lua_Integer)lua_rawlen(L, idx);
        // Count the keys: a sequence has exactly len integer keys 1..len.
        lua_Integer nkeys = 0;
        bool all_strings = true;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            nkeys++;
            if (lua_type(L, -2) != LUA_TSTRING)
                all_strings = false;
            lua_pop(L, 1);
        }
        if (nkeys == len) {
            value_t *items = len ? (value_t *)calloc((size_t)len, sizeof(value_t)) : NULL;
            if (len && !items) {
                lua_pushstring(L, "out of memory");
                return false;
            }
            for (lua_Integer i = 1; i <= len; i++) {
                lua_rawgeti(L, idx, i);
                if (!to_value(L, -1, &items[i - 1])) {
                    lua_remove(L, -2);
                    for (lua_Integer k = 1; k < i; k++)
                        value_free(&items[k - 1]);
                    free(items);
                    return false;
                }
                lua_pop(L, 1);
            }
            *out = val_list(items, (size_t)len);
            return true;
        }
        if (!all_strings) {
            lua_pushstring(L, "a table argument must be a sequence or have string keys");
            return false;
        }
        value_map_builder_t *b = val_map_new();
        // Keep the tree's order when the table came from the tree.
        lua_rawgetp(L, LUA_REGISTRYINDEX, &K_MAPORDER);
        lua_pushvalue(L, idx);
        lua_rawget(L, -2);
        lua_remove(L, -2);
        if (lua_type(L, -1) == LUA_TTABLE) {
            int order = lua_gettop(L);
            lua_Integer n = (lua_Integer)lua_rawlen(L, order);
            for (lua_Integer i = 1; i <= n; i++) {
                lua_rawgeti(L, order, i); // the key
                lua_pushvalue(L, -1);
                lua_gettable(L, idx); // its value
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 2); // a key removed since: skip it
                    continue;
                }
                value_t v;
                if (!to_value(L, -1, &v)) {
                    // the message is on top: drop the value, the key and the order
                    lua_remove(L, -2);
                    lua_remove(L, -2);
                    lua_remove(L, -2);
                    value_t discard = val_map_finish(b);
                    value_free(&discard);
                    return false;
                }
                val_map_put(b, lua_tostring(L, -2), v);
                lua_pop(L, 2);
            }
            lua_pop(L, 1);
        } else {
            lua_pop(L, 1);
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                value_t v;
                if (!to_value(L, -1, &v)) {
                    lua_remove(L, -2); // the value
                    lua_remove(L, -2); // the key
                    value_t discard = val_map_finish(b);
                    value_free(&discard);
                    return false;
                }
                val_map_put(b, lua_tostring(L, -2), v);
                lua_pop(L, 1);
            }
        }
        *out = val_map_finish(b);
        return true;
    }
    default:
        break;
    }
    lua_pushfstring(L, "a %s cannot be passed to the object tree", luaL_typename(L, idx));
    return false;
}

// --- The gs module (C half) -----------------------------------------------------

// gs.bytes(int, width) -> the big-endian bytes; gs.bytes(string) -> its bytes.
static int gs_bytes(lua_State *L) {
    if (lua_type(L, 1) == LUA_TSTRING) {
        size_t n;
        const char *s = lua_tolstring(L, 1, &n);
        gsl_bytes_t *b = (gsl_bytes_t *)lua_newuserdatauv(L, sizeof(*b) + n, 0);
        b->n = n;
        memcpy(b->p, s, n);
        luaL_setmetatable(L, MT_BYTES);
        return 1;
    }
    lua_Integer v = luaL_checkinteger(L, 1);
    lua_Integer w = luaL_checkinteger(L, 2);
    luaL_argcheck(L, w >= 1 && w <= 8, 2, "width must be 1..8");
    gsl_bytes_t *b = (gsl_bytes_t *)lua_newuserdatauv(L, sizeof(*b) + (size_t)w, 0);
    b->n = (size_t)w;
    for (lua_Integer i = 0; i < w; i++)
        b->p[i] = (uint8_t)((uint64_t)v >> (8 * (w - 1 - i)));
    luaL_setmetatable(L, MT_BYTES);
    return 1;
}

static int bytes_tostring(lua_State *L) {
    gsl_bytes_t *b = (gsl_bytes_t *)luaL_checkudata(L, 1, MT_BYTES);
    luaL_Buffer buf;
    luaL_buffinit(L, &buf);
    for (size_t i = 0; i < b->n; i++) {
        char hex[4];
        snprintf(hex, sizeof hex, "%02x", b->p[i]);
        luaL_addstring(&buf, hex);
    }
    luaL_pushresult(&buf);
    return 1;
}

static int bytes_len(lua_State *L) {
    gsl_bytes_t *b = (gsl_bytes_t *)luaL_checkudata(L, 1, MT_BYTES);
    lua_pushinteger(L, (lua_Integer)b->n);
    return 1;
}

// gs.node(path) -> what the path names (a proxy, or an attribute's value).
static int gs_node(lua_State *L) {
    return push_path(L, luaL_checkstring(L, 1));
}

// gs.fmt(path) -> the attribute as plain text (what `${path}` spliced).
static int gs_fmt(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    node_t n = object_resolve(object_root(), path);
    if (!node_valid(n)) {
        lua_pushfstring(L, "no such path '%s'", path);
        return raise_text(L, lua_tostring(L, -1));
    }
    value_t v = node_get(n);
    if (val_is_error(&v))
        return raise_value(L, &v);
    vbuf_t b = {0};
    value_format(&v, VFMT_TEXT, &b);
    value_free(&v);
    lua_pushlstring(L, b.p ? b.p : "", b.len);
    vbuf_free(&b);
    return 1;
}

// gs.list(x) -> the list a collection proxy (or a list path) reads as; a
// table is returned as it is.
static int gs_list(lua_State *L) {
    if (lua_type(L, 1) == LUA_TTABLE) {
        lua_settop(L, 1);
        return 1;
    }
    gsl_node_t *nd = check_node(L, 1);
    node_t n = object_resolve(object_root(), nd->path);
    if (!node_valid(n)) {
        lua_pushfstring(L, "no such path '%s'", nd->path);
        return raise_text(L, lua_tostring(L, -1));
    }
    value_t v = node_get(n);
    push_value(L, &v);
    return 1;
}

// gs.map(t) -> t, tagged as a map value so a call passes it positionally
// instead of reading it as named arguments.
static int gs_map(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    luaL_setmetatable(L, MT_MAP);
    return 1;
}

// gs.path(proxy) -> its path text; gs.is_node(x) -> whether x is a proxy.
static int gs_path(lua_State *L) {
    lua_pushstring(L, check_node(L, 1)->path);
    return 1;
}

static int gs_is_node(lua_State *L) {
    lua_pushboolean(L, test_node(L, 1) != NULL);
    return 1;
}

// echo(...) / print(...): the arguments' text, space-separated, one line,
// through the output sink to the client whose script this is.
static int gs_print(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1)
            out_putc(' ');
        out_write(s, len);
        lua_pop(L, 1);
    }
    out_putc('\n');
    return 0;
}

// io.write(...): the arguments' text, nothing added.
static int gs_write(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_tolstring(L, i, &len);
        out_write(s, len);
        lua_pop(L, 1);
    }
    return 0;
}

// quit(): ask the platform to stop and unwind the script with the quit
// signal, which the runner reports as a clean end.
static int gs_quit(lua_State *L) {
    platform_quit();
    lua_rawgetp(L, LUA_REGISTRYINDEX, &K_QUIT);
    return lua_error(L);
}

// --- Globals: the root's children ------------------------------------------------

// _G[name] for a missing global: the root child of that name, if any.
static int globals_index(lua_State *L) {
    if (lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    const char *name = lua_tostring(L, 2);
    if (!is_identifier(name)) {
        lua_pushnil(L);
        return 1;
    }
    node_t n = object_resolve(object_root(), name);
    if (!node_valid(n)) {
        lua_pushnil(L);
        return 1;
    }
    return push_path(L, name);
}

// _G[name] = v: refused when the name is a root child.
static int globals_newindex(lua_State *L) {
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char *name = lua_tostring(L, 2);
        if (is_identifier(name)) {
            node_t n = object_resolve(object_root(), name);
            if (node_valid(n) && (n.member == NULL || n.member->kind == MK_CHILD))
                return luaL_error(L, "'%s' is an object of the tree; a global cannot shadow it", name);
        }
    }
    lua_rawset(L, 1);
    return 0;
}

// --- Files, directories, require ---------------------------------------------------

// The directory part of `path` ("" when none), malloc'd.
static char *dirname_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        return strdup("");
    size_t n = (size_t)(slash - path);
    char *r = (char *)malloc(n + 1);
    if (r) {
        memcpy(r, path, n);
        r[n] = '\0';
    }
    return r;
}

static bool push_dir(const char *file) {
    if (g_ndirs >= DIR_STACK_MAX)
        return false;
    g_dirs[g_ndirs] = dirname_of(file);
    if (!g_dirs[g_ndirs])
        return false;
    g_ndirs++;
    return true;
}

static void pop_dir(void) {
    if (g_ndirs > 0)
        free(g_dirs[--g_ndirs]);
}

// `rel` resolved against the running chunk's directory when relative.
static void resolve_path(const char *rel, char *out, size_t cap) {
    const char *dir = g_ndirs > 0 ? g_dirs[g_ndirs - 1] : "";
    if (rel[0] == '/' || !dir[0])
        snprintf(out, cap, "%s", rel);
    else
        snprintf(out, cap, "%s/%s", dir, rel);
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

// Load the file at `path` and run it with `nargs` arguments already on the
// stack (above the loaded chunk's place), with the directory stack
// maintained around the call.  Leaves `nres` results, or raises.
static void run_file_at(lua_State *L, const char *path, int nargs, int nres) {
    if (luaL_loadfilex(L, path, "t") != LUA_OK) {
        // the message is on top; the arguments are below it
        lua_insert(L, -(nargs + 1));
        lua_pop(L, nargs);
        lua_error(L);
        return;
    }
    lua_insert(L, -(nargs + 1)); // the chunk under its arguments
    if (!push_dir(path)) {
        lua_pop(L, nargs + 1);
        raise_text(L, "require: nesting too deep");
        return;
    }
    int rc = lua_pcall(L, nargs, nres, 0);
    pop_dir();
    if (rc != LUA_OK)
        lua_error(L);
}

// The loader `require` runs for a file the searcher found: upvalue-free,
// the path arrives as the second argument.
static int gs_loader(lua_State *L) {
    const char *path = luaL_checkstring(L, 2);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    run_file_at(L, path, 2, 1);
    return 1;
}

// The searcher: `require "lib.mac"` is lib/mac.lua beside the requiring
// file, else under the current directory.
static int gs_searcher(lua_State *L) {
    size_t n;
    const char *name = luaL_checklstring(L, 1, &n);
    char rel[OBJ_PATH_MAX];
    if (n + 5 > sizeof rel)
        return luaL_error(L, "module name too long");
    for (size_t i = 0; i < n; i++)
        rel[i] = name[i] == '.' ? '/' : name[i];
    memcpy(rel + n, ".lua", 5);
    char path[OBJ_PATH_MAX * 2];
    resolve_path(rel, path, sizeof path);
    if (file_exists(path)) {
        lua_pushcfunction(L, gs_loader);
        lua_pushstring(L, path);
        return 2;
    }
    if (strcmp(path, rel) != 0 && file_exists(rel)) {
        lua_pushcfunction(L, gs_loader);
        lua_pushstring(L, rel);
        return 2;
    }
    lua_pushfstring(L, "\n\tno file '%s'", path);
    return 1;
}

// dofile(path): relative to the running chunk's directory.
static int gs_dofile(lua_State *L) {
    const char *rel = luaL_checkstring(L, 1);
    char path[OBJ_PATH_MAX * 2];
    resolve_path(rel, path, sizeof path);
    lua_settop(L, 0);
    run_file_at(L, path, 0, LUA_MULTRET);
    return lua_gettop(L);
}

// --- Interrupts ---------------------------------------------------------------------

bool job_current_cancelled(void) __attribute__((weak));

// The count hook: unwind when the script was interrupted or its job cancelled.
static void count_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    bool cancelled = job_current_cancelled && job_current_cancelled();
    if (!g_interrupt && !cancelled)
        return;
    g_interrupt = 0;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &K_INTERRUPT);
    lua_error(L);
}

void gs_lua_interrupt(void) {
    g_interrupt = 1;
}

// --- Reporting ----------------------------------------------------------------------

static void default_reporter(const char *file, int line, const char *msg, const char *text) {
    (void)file;
    (void)line;
    (void)msg;
    fputs(text, stderr);
}

void gs_lua_set_reporter(gs_lua_reporter_fn fn) {
    g_reporter = fn;
}

// Report the error on top of the stack (and pop it).
static void report_error(lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    if (!msg)
        msg = "error";
    size_t n = strlen(msg);
    char *text = (char *)malloc(n + 2);
    if (text) {
        memcpy(text, msg, n);
        text[n] = '\n';
        text[n + 1] = '\0';
        // The message is the first line.
        char *nl = strchr(msg, '\n');
        char first[512];
        size_t fl = nl ? (size_t)(nl - msg) : n;
        if (fl >= sizeof first)
            fl = sizeof first - 1;
        memcpy(first, msg, fl);
        first[fl] = '\0';
        (g_reporter ? g_reporter : default_reporter)("", 0, first, text);
        free(text);
    }
    lua_pop(L, 1);
}

// The message handler of a top-level call: a quit or interrupt signal passes
// through; anything else becomes its text plus a traceback.
static int msgh(lua_State *L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &K_QUIT);
    if (lua_rawequal(L, 1, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    lua_pop(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &K_INTERRUPT);
    if (lua_rawequal(L, 1, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "script interrupted");
        return 1;
    }
    lua_pop(L, 1);
    const char *msg = luaL_tolstring(L, 1, NULL);
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// --- State construction ------------------------------------------------------------

static const luaL_Reg node_mt[] = {
    {"__index",    node_index   },
    {"__newindex", node_newindex},
    {"__call",     node_call_mm },
    {"__tostring", node_tostring},
    {"__eq",       node_eq      },
    {"__len",      node_len     },
    {NULL,         NULL         },
};

static const luaL_Reg gs_funcs[] = {
    {"bytes",   gs_bytes  },
    {"node",    gs_node   },
    {"fmt",     gs_fmt    },
    {"path",    gs_path   },
    {"list",    gs_list   },
    {"map",     gs_map    },
    {"is_node", gs_is_node},
    {NULL,      NULL      },
};

// Open the standard libraries a script may use: no io (host files go
// through `files`), os reduced to the clock and the environment.
static void open_libs(lua_State *L) {
    static const luaL_Reg libs[] = {
        {LUA_GNAME,       luaopen_base     },
        {LUA_LOADLIBNAME, luaopen_package  },
        {LUA_COLIBNAME,   luaopen_coroutine},
        {LUA_TABLIBNAME,  luaopen_table    },
        {LUA_STRLIBNAME,  luaopen_string   },
        {LUA_MATHLIBNAME, luaopen_math     },
        {LUA_UTF8LIBNAME, luaopen_utf8     },
        {LUA_DBLIBNAME,   luaopen_debug    },
        {LUA_OSLIBNAME,   luaopen_os       },
        {NULL,            NULL             },
    };
    for (const luaL_Reg *lib = libs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    // Lua's debug library moves to `luadebug`: `debug` is the tree's debugger.
    lua_getglobal(L, "debug");
    lua_setglobal(L, "luadebug");
    lua_pushnil(L);
    lua_setglobal(L, "debug");
    static const char *const os_drop[] = {"exit", "execute", "remove", "rename", "tmpname", "setlocale", NULL};
    lua_getglobal(L, "os");
    for (const char *const *p = os_drop; *p; p++) {
        lua_pushnil(L);
        lua_setfield(L, -2, *p);
    }
    lua_pop(L, 1);
    // io.write only.
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, gs_write);
    lua_setfield(L, -2, "write");
    lua_setglobal(L, "io");
    // require: the preload searcher and ours; no package.path, no C modules.
    lua_getglobal(L, "package");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "path");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "cpath");
    lua_getfield(L, -1, "searchers");
    lua_pushcfunction(L, gs_searcher);
    lua_rawseti(L, -2, 2);
    lua_pushnil(L);
    lua_rawseti(L, -2, 3);
    lua_pushnil(L);
    lua_rawseti(L, -2, 4);
    lua_pop(L, 2);
    lua_pushcfunction(L, gs_dofile);
    lua_setglobal(L, "dofile");
}

static lua_State *make_state(void) {
    lua_State *L = luaL_newstate();
    if (!L)
        return NULL;
    open_libs(L);
    // Metatables.
    luaL_newmetatable(L, MT_NODE);
    luaL_setfuncs(L, node_mt, 0);
    lua_pop(L, 1);
    luaL_newmetatable(L, MT_ERROR);
    lua_pushcfunction(L, error_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
    luaL_newmetatable(L, MT_BYTES);
    lua_pushcfunction(L, bytes_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, bytes_len);
    lua_setfield(L, -2, "__len");
    lua_pop(L, 1);
    luaL_newmetatable(L, MT_MAP);
    lua_pushcfunction(L, map_pairs);
    lua_setfield(L, -2, "__pairs");
    lua_pop(L, 1);
    // The map-order side table: weak keys.
    lua_createtable(L, 0, 0);
    lua_createtable(L, 0, 1);
    lua_pushstring(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &K_MAPORDER);
    // The sentinels.
    lua_pushlightuserdata(L, (void *)&K_NONE);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &K_NONE);
    lua_pushlightuserdata(L, (void *)&K_QUIT);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &K_QUIT);
    lua_pushlightuserdata(L, (void *)&K_INTERRUPT);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &K_INTERRUPT);
    // The gs module.
    luaL_newlib(L, gs_funcs);
    lua_pushlightuserdata(L, (void *)&K_NONE);
    lua_setfield(L, -2, "none");
    lua_pushlightuserdata(L, (void *)&K_QUIT);
    lua_setfield(L, -2, "quit_signal");
    lua_pushlightuserdata(L, (void *)&K_INTERRUPT);
    lua_setfield(L, -2, "interrupt_signal");
    lua_setglobal(L, "gs");
    // Globals of our own.
    lua_pushcfunction(L, gs_print);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, gs_print);
    lua_setglobal(L, "echo");
    lua_pushcfunction(L, gs_quit);
    lua_setglobal(L, "quit");
    lua_createtable(L, 0, 4);
    lua_pushstring(L, "tmp");
    lua_setfield(L, -2, "TMP_DIR");
    lua_setglobal(L, "vars");
    // The root's children as globals.
    lua_pushglobaltable(L);
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, globals_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, globals_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);
    // The embedded library.
    if (luaL_loadbufferx(L, (const char *)gs_lua_lib_lua, sizeof gs_lua_lib_lua, "=gs_lua_lib", "t") != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "gs_lua: the embedded library failed: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return NULL;
    }
    lua_sethook(L, count_hook, LUA_MASKCOUNT, 1000);
    return L;
}

static lua_State *state(void) {
    if (!g_L)
        g_L = make_state();
    return g_L;
}

void gs_lua_shutdown(void) {
    if (g_L) {
        lua_close(g_L);
        g_L = NULL;
    }
    while (g_ndirs > 0)
        pop_dir();
}

void gs_lua_var_set(const char *name, const char *value) {
    lua_State *L = state();
    if (!L)
        return;
    lua_getglobal(L, "vars");
    lua_pushstring(L, value ? value : "");
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

// --- Runners ------------------------------------------------------------------------

// Finish a top-level call: 0 ended, 1 quit, -1 failed (reported).
static int finish_call(lua_State *L, int rc) {
    if (rc == LUA_OK)
        return 0;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &K_QUIT);
    bool quit = lua_rawequal(L, -1, -2);
    lua_pop(L, 1);
    if (quit) {
        lua_pop(L, 1);
        return 1;
    }
    report_error(L);
    return -1;
}

int gs_lua_run_file(const char *path) {
    lua_State *L = state();
    if (!L)
        return -1;
    int base = lua_gettop(L);
    lua_pushcfunction(L, msgh);
    if (luaL_loadfilex(L, path, "t") != LUA_OK) {
        report_error(L);
        lua_settop(L, base);
        return -1;
    }
    if (!push_dir(path)) {
        lua_settop(L, base);
        return -1;
    }
    int rc = lua_pcall(L, 0, 0, base + 1);
    pop_dir();
    int r = finish_call(L, rc);
    lua_settop(L, base);
    return r;
}

int gs_lua_run_source(const char *src, bool interactive) {
    lua_State *L = state();
    if (!L)
        return -1;
    int base = lua_gettop(L);
    lua_pushcfunction(L, msgh);
    int status = LUA_ERRSYNTAX;
    if (interactive) {
        // An expression first, so its results print.
        size_t n = strlen(src);
        char *ret = (char *)malloc(n + 8);
        if (ret) {
            memcpy(ret, "return ", 7);
            memcpy(ret + 7, src, n + 1);
            status = luaL_loadbufferx(L, ret, n + 7, "=stdin", "t");
            free(ret);
            if (status != LUA_OK)
                lua_pop(L, 1);
        }
    }
    if (status != LUA_OK && luaL_loadbufferx(L, src, strlen(src), "=stdin", "t") != LUA_OK) {
        report_error(L);
        lua_settop(L, base);
        return -1;
    }
    int rc = lua_pcall(L, 0, LUA_MULTRET, base + 1);
    int r = finish_call(L, rc);
    if (r == 0 && interactive) {
        int nres = lua_gettop(L) - base - 1;
        for (int i = 0; i < nres; i++) {
            size_t len;
            const char *s = luaL_tolstring(L, base + 2 + i, &len);
            if (i > 0)
                out_putc('\t');
            out_write(s, len);
            lua_pop(L, 1);
        }
        if (nres > 0)
            out_putc('\n');
    }
    lua_settop(L, base);
    return r;
}
