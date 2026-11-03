// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_remote_object.c
// The `appletalk.remote` object-model subtree: the network's further file
// servers and their remote volumes (afp_remote.h), and the verb the page's
// providers answer requests with.  Glue over afp_remote.h and appletalk.h;
// it holds no state.
//
//   appletalk.remote
//     servers            the further servers' names
//     add_server(name), remove_server(name)
//     volumes            every remote volume: {server, name, uri, path}
//     add_volume(server, name, uri, cache), remove_volume(server, name)
//     requests           requests waiting for the page: {id, op, uri, dest}
//     complete(id, status, [message])
//     unpack_limit       containers up to this many bytes are shown as folders
//     issued / failed    request counters

#include "afp_remote.h"
#include "appletalk.h"
#include "object.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// A subsystem call's error buffer as the VK_ERROR a script sees.
static value_t remote_err(const char *fallback, const char *buf) {
    return val_err("%s", (buf && *buf) ? buf : fallback);
}

static DEF_GETTER(remote_attr_servers) {
    int max = atalk_afp_server_max();
    value_t *items = calloc((size_t)(max > 0 ? max : 1), sizeof(value_t));
    if (!items)
        return val_err("out of memory");
    size_t n = 0;
    for (int i = 1; i < max; i++) // the first server is appletalk.afp
        if (atalk_afp_server_name(i))
            items[n++] = val_str(atalk_afp_server_name(i));
    return val_list(items, n);
}

static DEF_GETTER(remote_attr_volumes) {
    int max = atalk_afp_server_max(), vmax = atalk_afp_volume_max();
    value_t *items = calloc((size_t)(max * vmax > 0 ? max * vmax : 1), sizeof(value_t));
    if (!items)
        return val_err("out of memory");
    size_t n = 0;
    for (int s = 0; s < max; s++)
        for (int v = 0; v < vmax; v++) {
            const char *name, *uri, *path;
            if (!afp_remote_volume_at(s, v, &name, &uri, &path) || !uri[0])
                continue;
            value_map_builder_t *b = val_map_new();
            val_map_put(b, "server", val_str(atalk_afp_server_name(s)));
            val_map_put(b, "name", val_str(name));
            val_map_put(b, "uri", val_str(uri));
            val_map_put(b, "path", val_str(path));
            items[n++] = val_map_finish(b);
        }
    return val_list(items, n);
}

static DEF_GETTER(remote_attr_requests) {
    size_t cap = 8, n = 0;
    value_t *items = calloc(cap, sizeof(value_t));
    if (!items)
        return val_err("out of memory");
    uint32_t id;
    const char *op, *uri, *dest;
    for (int i = 0; afp_remote_request_at(i, &id, &op, &uri, &dest); i++) {
        if (n == cap) {
            value_t *grown = realloc(items, cap * 2 * sizeof(value_t));
            if (!grown)
                break;
            items = grown;
            cap *= 2;
        }
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "id", val_uint(4, id));
        val_map_put(b, "op", val_str(op));
        val_map_put(b, "uri", val_str(uri));
        val_map_put(b, "dest", val_str(dest));
        items[n++] = val_map_finish(b);
    }
    return val_list(items, n);
}

static DEF_GETTER(remote_attr_unpack_limit) {
    return val_uint(8, afp_remote_unpack_limit());
}
static DEF_SETTER(remote_attr_set_unpack_limit) {
    afp_remote_set_unpack_limit(in.u);
    return val_none();
}
static DEF_GETTER(remote_attr_issued) {
    return val_uint(4, afp_remote_requests_issued());
}
static DEF_GETTER(remote_attr_failed) {
    return val_uint(4, afp_remote_requests_failed());
}

static DEF_METHOD(remote_method_add_server) {
    char err[192];
    if (atalk_afp_server_add(argv[0].s, err, sizeof err) < 0)
        return remote_err("cannot add the server", err);
    return val_bool(true);
}

static DEF_METHOD(remote_method_remove_server) {
    char err[192];
    if (atalk_afp_server_remove(argv[0].s, err, sizeof err) != 0)
        return remote_err("cannot remove the server", err);
    return val_bool(true);
}

static DEF_METHOD(remote_method_add_volume) {
    char err[192];
    if (afp_remote_volume_add(argv[0].s, argv[1].s, argv[2].s, argv[3].s, err, sizeof err) < 0)
        return remote_err("cannot add the volume", err);
    return val_bool(true);
}

static DEF_METHOD(remote_method_remove_volume) {
    char err[192];
    if (afp_remote_volume_remove(argv[0].s, argv[1].s, err, sizeof err) != 0)
        return remote_err("cannot remove the volume", err);
    return val_bool(true);
}

static DEF_METHOD(remote_method_complete) {
    const char *message = (argc > 2 && argv[2].kind == VK_STRING) ? argv[2].s : "";
    if (afp_remote_complete((uint32_t)argv[0].u, (int)argv[1].i, message) != 0)
        return val_err("no request %u is waiting", (unsigned)argv[0].u);
    return val_bool(true);
}

static const arg_decl_t remote_name_args[] = {
    {.name = "name",
     .kind = VK_STRING,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "Server name, as the Chooser shows it"},
};
static const arg_decl_t remote_add_volume_args[] = {
    {.name = "server", .kind = VK_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Server the volume is on"},
    {.name = "name", .kind = VK_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Volume name (max 27 chars)"},
    {.name = "uri", .kind = VK_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "The provider's URI of its root"},
    {.name = "cache",
     .kind = VK_STRING,
     .presentation_flags = VFLAG_PATH,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "Host folder that caches it"},
};
static const arg_decl_t remote_remove_volume_args[] = {
    {.name = "server", .kind = VK_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Server the volume is on"},
    {.name = "name",   .kind = VK_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Volume name"            },
};
static const arg_decl_t remote_complete_args[] = {
    {.name = "id", .kind = VK_UINT, .doc = "The request's id"},
    {.name = "status", .kind = VK_INT, .doc = "0: the result is at the request's dest; else it failed"},
    {.name = "message", .kind = VK_STRING, .validation_flags = OBJ_ARG_OPTIONAL, .doc = "Why it failed"},
};

static const member_t remote_members[] = {
    {.kind = MK_ATTR,
     .name = "servers",
     .doc = "Names of the file servers beside appletalk.afp",
     .attr = {.type = VK_LIST, .get = remote_attr_servers}                                                      },
    {.kind = MK_ATTR,
     .name = "volumes",
     .doc = "Remote volumes: server, name, uri and cache path",
     .attr = {.type = VK_LIST, .get = remote_attr_volumes}                                                      },
    {.kind = MK_ATTR,
     .name = "requests",
     .doc = "Requests waiting for the page's providers: id, op, uri, dest",
     .attr = {.type = VK_LIST, .get = remote_attr_requests}                                                     },
    {.kind = MK_ATTR,
     .name = "unpack_limit",
     .doc = "Archives and disk images up to this many bytes are shown as the folders they unpack to",
     .attr = {.type = VK_UINT, .width = 8, .get = remote_attr_unpack_limit, .set = remote_attr_set_unpack_limit}},
    {.kind = MK_ATTR,
     .name = "issued",
     .doc = "Requests issued since start",
     .attr = {.type = VK_UINT, .width = 4, .get = remote_attr_issued}                                           },
    {.kind = MK_ATTR,
     .name = "failed",
     .doc = "Requests that failed since start",
     .attr = {.type = VK_UINT, .width = 4, .get = remote_attr_failed}                                           },
    {.kind = MK_METHOD,
     .name = "add_server",
     .doc = "Add a file server to the network: its own Chooser entry, no volumes yet",
     .method = {.args = remote_name_args,
                .nargs = 1,
                .result = VK_BOOL,
                .fn = remote_method_add_server,
                .ui_flags = MM_MUTATE}                                                                          },
    {.kind = MK_METHOD,
     .name = "remove_server",
     .doc = "Withdraw a server added with add_server, with its volumes",
     .method = {.args = remote_name_args,
                .nargs = 1,
                .result = VK_BOOL,
                .fn = remote_method_remove_server,
                .ui_flags = MM_DESTRUCTIVE | MM_MUTATE}                                                         },
    {.kind = MK_METHOD,
     .name = "add_volume",
     .doc = "Publish a remote tree as a read-only volume of a server",
     .method = {.args = remote_add_volume_args,
                .nargs = 4,
                .result = VK_BOOL,
                .fn = remote_method_add_volume,
                .ui_flags = MM_MUTATE}                                                                          },
    {.kind = MK_METHOD,
     .name = "remove_volume",
     .doc = "Withdraw a volume of a server",
     .method = {.args = remote_remove_volume_args,
                .nargs = 2,
                .result = VK_BOOL,
                .fn = remote_method_remove_volume,
                .ui_flags = MM_DESTRUCTIVE | MM_MUTATE}                                                         },
    {.kind = MK_METHOD,
     .name = "complete",
     .doc = "Answer a request: its result is at its dest (status 0), or it failed",
     .method = {.args = remote_complete_args,
                .nargs = 3,
                .result = VK_BOOL,
                .fn = remote_method_complete,
                .ui_flags = MM_MUTATE}                                                                          },
};

static const class_desc_t remote_class = {
    .name = "atalk_remote",
    .doc = "Further file servers and their remote volumes (archive.org, web file indexes, host folders)",
    .members = remote_members,
    .n_members = ARRAY_LEN(remote_members),
};

void afp_remote_install_objects(struct object *parent) {
    struct object *remote = object_new(&remote_class, NULL, "remote");
    if (!remote)
        return;
    object_set_label(remote, "Remote File Servers");
    object_attach(parent, remote);
}
