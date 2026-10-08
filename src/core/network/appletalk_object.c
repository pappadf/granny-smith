// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_object.c
// The `appletalk` object-model tree: the stack's own nodes (stats, nbp), the
// AFP server's (afp, its volumes, sessions and stats) and the printer's.  It
// is glue over the public API in appletalk.h and holds no protocol state;
// the program-linking layers (adsp, ppc, aevt) install their own subtrees.

#include "appletalk.h"

#include "appletalk_adsp.h"
#include "appletalk_aevt.h"
#include "appletalk_internal.h"
#include "appletalk_ppc.h"
#include "object.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

static const class_desc_t atalk_class;
static const class_desc_t atalk_stats_class;
static const class_desc_t atalk_nbp_collection_class;
static const class_desc_t atalk_nbp_entry_class;
static const class_desc_t atalk_afp_class;
static const class_desc_t atalk_afp_stats_class;
static const class_desc_t atalk_volumes_collection_class;
static const class_desc_t atalk_volume_class;
static const class_desc_t atalk_sessions_collection_class;
static const class_desc_t atalk_session_class;
static const class_desc_t atalk_printer_class;
static const class_desc_t atalk_printer_stats_class;

// The tree's collection entries, made on first use by their caches and
// handed out by the collections' get() callbacks.
static object_cache_t g_nbp_objects;
static object_cache_t g_volume_objects;
static object_cache_t g_session_objects;

// The object tree: `appletalk` at the root, attached once and never taken
// down -- it lives as long as the process.  instance_data is unused (NULL)
// for the singleton nodes: their accessors call into the modules, which
// answer for whatever connection is plugged in.  Collection entries carry
// their slot index.
void atalk_install_objects(void) {
    struct object *atalk = object_new(&atalk_class, NULL, "appletalk");
    if (!atalk)
        return;
    object_set_order(atalk, 100);
    object_set_domain(atalk, OBJ_DOMAIN_NETWORK);
    object_attach(object_root(), atalk);

    struct object *stats = object_new(&atalk_stats_class, NULL, "stats");
    if (stats) {
        object_set_category(stats, M_CAT_ADVANCED);
        object_attach(atalk, stats);
    }
    struct object *nbp = object_new(&atalk_nbp_collection_class, NULL, "nbp");
    if (nbp) {
        object_set_category(nbp, M_CAT_ADVANCED);
        object_attach(atalk, nbp);
    }
    struct object *volumes = NULL, *sessions = NULL;
    struct object *afp = object_new(&atalk_afp_class, NULL, "afp");
    if (afp) {
        object_set_label(afp, "File Server");
        object_attach(atalk, afp);
        volumes = object_new(&atalk_volumes_collection_class, NULL, "volumes");
        if (volumes)
            object_attach(afp, volumes);
        sessions = object_new(&atalk_sessions_collection_class, NULL, "sessions");
        if (sessions) {
            object_set_category(sessions, M_CAT_ADVANCED);
            object_attach(afp, sessions);
        }
        struct object *afp_stats = object_new(&atalk_afp_stats_class, (void *)atalk_afp_get_stats(), "stats");
        if (afp_stats) {
            object_set_category(afp_stats, M_CAT_ADVANCED);
            object_attach(afp, afp_stats);
        }
    }
    struct object *printer = object_new(&atalk_printer_class, NULL, "printer");
    if (printer) {
        object_attach(atalk, printer);
        struct object *printer_stats =
            object_new(&atalk_printer_stats_class, (void *)atalk_printer_get_stats(), "stats");
        if (printer_stats) {
            object_set_category(printer_stats, M_CAT_ADVANCED);
            object_attach(printer, printer_stats);
        }
    }

    // Each program-linking layer owns its own subtree.
    atalk_adsp_install_objects(atalk);
    atalk_ppc_install_objects(atalk);
    atalk_aevt_install_objects(atalk);

    // Collection entry objects are made on first use by their caches and
    // handed out by the get() callbacks; they are never attached.
    g_volume_objects = (object_cache_t)OBJECT_CACHE(&atalk_volume_class, NULL);
    g_nbp_objects = (object_cache_t)OBJECT_CACHE(&atalk_nbp_entry_class, NULL);
    g_session_objects = (object_cache_t)OBJECT_CACHE(&atalk_session_class, NULL);
    object_cache_set_parent(&g_volume_objects, volumes);
    object_cache_set_parent(&g_nbp_objects, nbp);
    object_cache_set_parent(&g_session_objects, sessions);
}

// === Object-model class descriptors =========================================
//
// The tree published here:
//
//   appletalk            enabled / node_id / stats / nbp
//     afp                enabled / name / message / versions
//       volumes          add(name, path) -> the created volume, remove(name), count
//       sessions         one entry per live ASP session, count
//       stats            commands_served / bytes moved / errors
//     printer            enabled / name / capture / status / documents
//       stats            jobs / aborts / bytes / captures / last_capture
//     adsp               connections, stats        (appletalk_adsp.c)
//     ppc                ports, sessions, browse(), stats  (appletalk_ppc.c)
//     aevt               send(), events, inbox, stats      (appletalk_aevt.c)
//
// The design rules are: state is an attribute with a setter, methods are
// verbs, constructive methods return the object they made, and failures come
// back as V_ERROR carrying the real reason.

// Turn a subsystem call's error buffer into the V_ERROR a script will see.
static value_t atalk_err(const char *fallback, const char *buf) {
    return val_err("%s", (buf && *buf) ? buf : fallback);
}

// --- appletalk.stats -------------------------------------------------------

// The counters of whichever connection is plugged in (zeros while none is).
static DEF_GETTER(atalk_stats_get) {
    return obj_u64_at(atalk_get_stats(), m);
}

static const member_t atalk_stats_members[] = {
    OBJ_U64_FIELD_WITH(atalk_stats_t, llap_rx, "LLAP frames received", atalk_stats_get),
    OBJ_U64_FIELD_WITH(atalk_stats_t, llap_tx, "LLAP frames transmitted", atalk_stats_get),
    OBJ_U64_FIELD_WITH(atalk_stats_t, malformed, "Frames discarded as malformed, at any layer", atalk_stats_get),
    OBJ_U64_FIELD_WITH(atalk_stats_t, unhandled, "Well-formed frames nothing here serves", atalk_stats_get),
    OBJ_U64_FIELD_WITH(atalk_stats_t, tx_dropped, "Frames the stack gave up transmitting", atalk_stats_get),
    OBJ_U64_FIELD_WITH(atalk_stats_t, ddp_in, "DDP datagrams delivered inbound", atalk_stats_get),
    OBJ_U64_FIELD_WITH(atalk_stats_t, ddp_out, "DDP datagrams sent", atalk_stats_get),
    OBJ_U64_FIELD_WITH(atalk_stats_t, atp_requests, "ATP transactions this host originated", atalk_stats_get),
    OBJ_U64_FIELD_WITH(atalk_stats_t, atp_retries, "ATP request retransmissions", atalk_stats_get),
    OBJ_U64_FIELD_WITH(atalk_stats_t, nbp_packets, "NBP packets processed", atalk_stats_get),
};

static const class_desc_t atalk_stats_class = {
    .name = "atalk_stats",
    .members = atalk_stats_members,
    .n_members = ARRAY_LEN(atalk_stats_members),
};

// --- appletalk.nbp ---------------------------------------------------------

static int atalk_slot_of(struct object *self) {
    return object_entry_index(self);
}

// The NBP entry accessors re-read the registry each time: entries can be
// re-registered under a new name without the object identity changing.
static DEF_GETTER(atalk_nbp_attr_object) {
    atalk_nbp_info_t info;
    return val_str(atalk_nbp_entry_info(atalk_slot_of(self), &info) ? info.object : "");
}
static DEF_GETTER(atalk_nbp_attr_type) {
    atalk_nbp_info_t info;
    return val_str(atalk_nbp_entry_info(atalk_slot_of(self), &info) ? info.type : "");
}
static DEF_GETTER(atalk_nbp_attr_zone) {
    atalk_nbp_info_t info;
    return val_str(atalk_nbp_entry_info(atalk_slot_of(self), &info) ? info.zone : "");
}
static DEF_GETTER(atalk_nbp_attr_socket) {
    atalk_nbp_info_t info;
    return val_uint(1, atalk_nbp_entry_info(atalk_slot_of(self), &info) ? info.socket : 0);
}
static DEF_GETTER(atalk_nbp_attr_node) {
    atalk_nbp_info_t info;
    return val_uint(1, atalk_nbp_entry_info(atalk_slot_of(self), &info) ? info.node : 0);
}

static const member_t atalk_nbp_entry_members[] = {
    {.kind = M_ATTR,
     .name = "object",
     .doc = "NBP object name",
     .attr = {.type = V_STRING, .get = atalk_nbp_attr_object}                                                        },
    {.kind = M_ATTR, .name = "type", .doc = "NBP entity type", .attr = {.type = V_STRING, .get = atalk_nbp_attr_type}},
    {.kind = M_ATTR, .name = "zone", .doc = "NBP zone",        .attr = {.type = V_STRING, .get = atalk_nbp_attr_zone}},
    {.kind = M_ATTR,
     .name = "socket",
     .doc = "DDP socket the entity answers on",
     .attr = {.type = V_UINT, .width = 1, .get = atalk_nbp_attr_socket}                                              },
    {.kind = M_ATTR,
     .name = "node",
     .doc = "LLAP node the entity lives on",
     .attr = {.type = V_UINT, .width = 1, .get = atalk_nbp_attr_node}                                                },
};

static const class_desc_t atalk_nbp_entry_class = {
    .name = "atalk_nbp_entry",
    .members = atalk_nbp_entry_members,
    .n_members = ARRAY_LEN(atalk_nbp_entry_members),
};

static struct object *atalk_nbp_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= ATALK_NBP_MAX_ENTRIES || !atalk_nbp_entry_in_use(index))
        return NULL;
    return object_cache_at(&g_nbp_objects, index, NULL);
}
// Named lookup so `appletalk.nbp["Shared Folders"]` resolves.
static struct object *atalk_nbp_entry_lookup(struct object *self, const char *name) {
    (void)self;
    for (int i = 0; i < ATALK_NBP_MAX_ENTRIES && i < atalk_nbp_entry_max(); i++) {
        atalk_nbp_info_t info;
        if (atalk_nbp_entry_info(i, &info) && strcmp(info.object, name) == 0)
            return object_cache_at(&g_nbp_objects, i, NULL);
    }
    return NULL;
}

static const collection_desc_t atalk_nbp_collection_entries = {
    .entry = &atalk_nbp_entry_class,
    .by_index = {.get = atalk_nbp_get, .slots = ATALK_NBP_MAX_ENTRIES},
    .by_key = {.lookup = atalk_nbp_entry_lookup}
};

static const member_t atalk_nbp_collection_members[] = {
    OBJ_ENTRIES(&atalk_nbp_collection_entries, "Every entity this host advertises"),
};

static const class_desc_t atalk_nbp_collection_class = {
    .name = "atalk_nbp",
    .members = atalk_nbp_collection_members,
    .n_members = ARRAY_LEN(atalk_nbp_collection_members),
};

// --- appletalk.afp.volumes.[i] ---------------------------------------------

static DEF_GETTER(atalk_volume_attr_name) {
    const char *s = atalk_afp_volume_name(atalk_slot_of(self));
    return val_str(s ? s : "");
}
static DEF_GETTER(atalk_volume_attr_path) {
    const char *s = atalk_afp_volume_path(atalk_slot_of(self));
    return val_str(s ? s : "");
}
static DEF_GETTER(atalk_volume_attr_vol_id) {
    return val_uint(2, atalk_afp_volume_vol_id(atalk_slot_of(self)));
}
static DEF_GETTER(atalk_volume_attr_open_forks) {
    return val_uint(4, atalk_afp_volume_open_forks(atalk_slot_of(self)));
}
static DEF_GETTER(atalk_volume_attr_sessions_using) {
    return val_uint(4, atalk_afp_volume_sessions_using(atalk_slot_of(self)));
}
static DEF_GETTER(atalk_volume_attr_catalog_generation) {
    return val_uint(4, atalk_afp_volume_catalog_generation(atalk_slot_of(self)));
}
static DEF_GETTER(atalk_volume_attr_cnid_count) {
    return val_uint(4, atalk_afp_volume_cnid_count(atalk_slot_of(self)));
}

static DEF_METHOD(atalk_volume_method_remove) {
    const char *name = atalk_afp_volume_name(atalk_slot_of(self));
    if (!name)
        return val_err("volume already removed");
    char err[192];
    if (atalk_afp_volume_remove(name, err, sizeof(err)) != 0)
        return atalk_err("cannot remove the volume", err);
    return val_none();
}

static const member_t atalk_volume_members[] = {
    {.kind = M_ATTR,
     .name = "name",
     .doc = "AFP volume name as clients see it",
     .attr = {.type = V_STRING, .get = atalk_volume_attr_name}                        },
    {.kind = M_ATTR,
     .name = "path",
     .doc = "Host directory backing the volume",
     .attr = {.type = V_STRING, .get = atalk_volume_attr_path}                        },
    {.kind = M_ATTR,
     .name = "vol_id",
     .doc = "Wire volume identifier",
     .attr = {.type = V_UINT, .width = 2, .get = atalk_volume_attr_vol_id}            },
    {.kind = M_ATTR,
     .name = "open_forks",
     .doc = "Forks currently open on this volume",
     .attr = {.type = V_UINT, .width = 4, .get = atalk_volume_attr_open_forks}        },
    {.kind = M_ATTR,
     .name = "sessions_using",
     .doc = "Sessions that have this volume open",
     .attr = {.type = V_UINT, .width = 4, .get = atalk_volume_attr_sessions_using}    },
    {.kind = M_ATTR,
     .name = "catalog_generation",
     .doc = "CNID catalog generation; bumped by compaction and tombstone sweeps",
     .attr = {.type = V_UINT, .width = 4, .get = atalk_volume_attr_catalog_generation}},
    {.kind = M_ATTR,
     .name = "cnid_count",
     .doc = "Live entries in the CNID catalog",
     .attr = {.type = V_UINT, .width = 4, .get = atalk_volume_attr_cnid_count}        },
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Withdraw this AFP volume",
     .method = {.args = NULL,
                .nargs = 0,
                .result = V_NONE,
                .fn = atalk_volume_method_remove,
                .ui_flags = MM_DESTRUCTIVE | MM_MUTATE}                               },
};

static const class_desc_t atalk_volume_class = {
    .name = "atalk_volume",
    .members = atalk_volume_members,
    .n_members = ARRAY_LEN(atalk_volume_members),
};

// --- appletalk.afp.volumes -------------------------------------------------

static struct object *atalk_volumes_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= ATALK_AFP_MAX_VOLUMES || !atalk_afp_volume_in_use(index))
        return NULL;
    return object_cache_at(&g_volume_objects, index, NULL);
}
// Name lookup, so `appletalk.afp.volumes["Shared"].cnid_count` reads naturally.
static struct object *atalk_volumes_lookup(struct object *self, const char *name) {
    (void)self;
    int slot = atalk_afp_volume_find(name);
    if (slot < 0 || slot >= ATALK_AFP_MAX_VOLUMES)
        return NULL;
    return object_cache_at(&g_volume_objects, slot, NULL);
}

// Constructive methods return the object they made, so a script can chain
// straight into it.
static DEF_METHOD(atalk_volumes_method_add) {
    char err[192];
    int slot = atalk_afp_volume_add(argv[0].s, argv[1].s, err, sizeof(err));
    if (slot < 0)
        return atalk_err("cannot add the volume", err);
    if (slot >= ATALK_AFP_MAX_VOLUMES || !object_cache_at(&g_volume_objects, slot, NULL))
        return val_none();
    return val_obj(object_cache_at(&g_volume_objects, slot, NULL));
}

static DEF_METHOD(atalk_volumes_method_remove) {
    char err[192];
    if (atalk_afp_volume_remove(argv[0].s, err, sizeof(err)) != 0)
        return atalk_err("cannot remove the volume", err);
    return val_none();
}

static const arg_decl_t atalk_volumes_add_args[] = {
    {.name = "name",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "Volume name as clients see it (max 32 chars)"},
    {.name = "path",
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .validation_flags = OBJ_ARG_NONEMPTY,
     .doc = "Host directory to publish"},
};
static const arg_decl_t atalk_volumes_remove_args[] = {
    {.name = "name", .kind = V_STRING, .validation_flags = OBJ_ARG_NONEMPTY, .doc = "Volume name to withdraw"},
};

static const collection_desc_t atalk_volumes_collection_entries = {
    .entry = &atalk_volume_class,
    .by_index = {.get = atalk_volumes_get, .slots = ATALK_AFP_MAX_VOLUMES},
    .by_key = {.lookup = atalk_volumes_lookup}
};

static const member_t atalk_volumes_collection_members[] = {
    {.kind = M_METHOD,
     .name = "add",
     .doc = "Publish a host directory as an AFP volume; returns the new volume",
     .method = {.args = atalk_volumes_add_args,
                .nargs = 2,
                .result = V_OBJECT,
                .fn = atalk_volumes_method_add,
                .ui_flags = MM_MUTATE}                 },
    {.kind = M_METHOD,
     .name = "remove",
     .doc = "Withdraw an AFP volume by name",
     .method = {.args = atalk_volumes_remove_args,
                .nargs = 1,
                .result = V_NONE,
                .fn = atalk_volumes_method_remove,
                .ui_flags = MM_DESTRUCTIVE | MM_MUTATE}},
    OBJ_ENTRIES(&atalk_volumes_collection_entries, NULL),
};

static const class_desc_t atalk_volumes_collection_class = {
    .name = "atalk_volumes",
    .doc = "Host directories exported as AFP volumes",
    .members = atalk_volumes_collection_members,
    .n_members = ARRAY_LEN(atalk_volumes_collection_members),
};

// --- appletalk.afp.sessions ------------------------------------------------

static DEF_GETTER(atalk_session_attr_ref) {
    atalk_session_info_t info;
    return val_uint(2, atalk_asp_session_info(atalk_slot_of(self), &info) ? info.session_ref : 0);
}
static DEF_GETTER(atalk_session_attr_client_node) {
    atalk_session_info_t info;
    return val_uint(1, atalk_asp_session_info(atalk_slot_of(self), &info) ? info.client_node : 0);
}
static DEF_GETTER(atalk_session_attr_afp_version) {
    atalk_session_info_t info;
    return val_str(atalk_asp_session_info(atalk_slot_of(self), &info) ? info.afp_version : "");
}
static DEF_GETTER(atalk_session_attr_open_forks) {
    atalk_session_info_t info;
    return val_uint(4, atalk_asp_session_info(atalk_slot_of(self), &info) ? info.open_forks : 0);
}
static DEF_GETTER(atalk_session_attr_idle_ns) {
    atalk_session_info_t info;
    return val_uint(8, atalk_asp_session_info(atalk_slot_of(self), &info) ? info.idle_ns : 0);
}

static const member_t atalk_session_members[] = {
    {.kind = M_ATTR,
     .name = "session_ref",
     .doc = "ASP session reference",
     .attr = {.type = V_UINT, .width = 2, .get = atalk_session_attr_ref}        },
    {.kind = M_ATTR,
     .name = "client_node",
     .doc = "LLAP node of the workstation",
     .attr = {.type = V_UINT, .width = 1, .get = atalk_session_attr_client_node}},
    {.kind = M_ATTR,
     .name = "afp_version",
     .doc = "AFP version negotiated at login",
     .attr = {.type = V_STRING, .get = atalk_session_attr_afp_version}          },
    {.kind = M_ATTR,
     .name = "open_forks",
     .doc = "Forks this session holds open",
     .attr = {.type = V_UINT, .width = 4, .get = atalk_session_attr_open_forks} },
    {.kind = M_ATTR,
     .name = "idle_ns",
     .doc = "Emulated nanoseconds since the last packet from this client",
     .attr = {.type = V_UINT, .width = 8, .get = atalk_session_attr_idle_ns}    },
};

static const class_desc_t atalk_session_class = {
    .name = "atalk_session",
    .members = atalk_session_members,
    .n_members = ARRAY_LEN(atalk_session_members),
};

static struct object *atalk_sessions_get(struct object *self, int index) {
    (void)self;
    if (index < 0 || index >= ATALK_ASP_MAX_SESSIONS || !atalk_asp_session_in_use(index))
        return NULL;
    return object_cache_at(&g_session_objects, index, NULL);
}

static const collection_desc_t atalk_sessions_collection_entries = {
    .entry = &atalk_session_class, .by_index = {.get = atalk_sessions_get, .slots = ATALK_ASP_MAX_SESSIONS}
};

static const member_t atalk_sessions_collection_members[] = {
    OBJ_ENTRIES(&atalk_sessions_collection_entries, NULL),
};

static const class_desc_t atalk_sessions_collection_class = {
    .name = "atalk_sessions",
    .members = atalk_sessions_collection_members,
    .n_members = ARRAY_LEN(atalk_sessions_collection_members),
};

// --- appletalk.afp.stats ---------------------------------------------------

// The per-code error tally is a map rather than a fixed member list: only the
// codes that have actually occurred appear, so the tree stays small and the
// integration tests can assert on one key.
static DEF_GETTER(atalk_afp_stats_attr_errors_by_code) {
    value_map_builder_t *b = val_map_new();
    int32_t code = 0;
    uint64_t count = 0;
    for (int i = 0; atalk_afp_error_code_at(i, &code, &count) == 0; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%d", code);
        val_map_put(b, key, val_uint(8, count));
    }
    return val_map_finish(b);
}

static DEF_GETTER(atalk_afp_stats_attr_ok_by_command) {
    value_map_builder_t *b = val_map_new();
    const char *name = NULL;
    uint64_t count = 0;
    for (int i = 0; atalk_afp_ok_command_at(i, &name, &count) == 0; i++)
        val_map_put(b, name, val_uint(8, count));
    return val_map_finish(b);
}

static const member_t atalk_afp_stats_members[] = {
    OBJ_U64_FIELD(atalk_afp_stats_t, commands_served, "AFP commands dispatched"),
    OBJ_U64_FIELD(atalk_afp_stats_t, bytes_read, "Bytes served through FPRead"),
    OBJ_U64_FIELD(atalk_afp_stats_t, bytes_written, "Bytes accepted through FPWrite"),
    OBJ_U64_FIELD(atalk_afp_stats_t, errors, "Commands that returned a non-zero result"),
    OBJ_U64_FIELD(atalk_afp_stats_t, open_forks, "Forks currently open across all volumes"),
    {.kind = M_ATTR,
                                                                                .name = "errors_by_code",
                                                                                .doc = "Result code -> occurrence count, for the codes seen so far",
                                                                                .attr = {.type = V_MAP, .get = atalk_afp_stats_attr_errors_by_code}},
    {.kind = M_ATTR,
                                                                                .name = "ok_by_command",
                                                                                .doc = "Command name -> times it returned NoErr, for the commands that have",
                                                                                .attr = {.type = V_MAP, .get = atalk_afp_stats_attr_ok_by_command} },
};

static const class_desc_t atalk_afp_stats_class = {
    .name = "atalk_afp_stats",
    .members = atalk_afp_stats_members,
    .n_members = ARRAY_LEN(atalk_afp_stats_members),
};

// --- appletalk.afp ---------------------------------------------------------

static DEF_GETTER(atalk_afp_attr_enabled) {
    return val_bool(atalk_afp_get_enabled());
}
static DEF_SETTER(atalk_afp_attr_set_enabled) {
    char err[192];
    if (atalk_afp_set_enabled(in.b, err, sizeof(err)) != 0)
        return atalk_err("cannot change the AFP server state", err);
    return val_none();
}
static DEF_GETTER(atalk_afp_attr_name) {
    return val_str(atalk_afp_get_name());
}
static DEF_SETTER(atalk_afp_attr_set_name) {
    char err[192];
    if (atalk_afp_set_name(in.s, err, sizeof(err)) != 0) {
        value_free(&in);
        return atalk_err("cannot rename the AFP server", err);
    }
    value_free(&in);
    return val_none();
}
static DEF_GETTER(atalk_afp_attr_message) {
    return val_str(atalk_afp_get_message());
}
static DEF_SETTER(atalk_afp_attr_set_message) {
    char err[192];
    if (atalk_afp_set_message(in.s, err, sizeof(err)) != 0) {
        value_free(&in);
        return atalk_err("cannot set the server message", err);
    }
    value_free(&in);
    return val_none();
}
static DEF_GETTER(atalk_afp_attr_versions) {
    int count = 0;
    const char *const *versions = atalk_afp_versions(&count);
    value_t *items = (value_t *)calloc((size_t)(count > 0 ? count : 1), sizeof(value_t));
    if (!items)
        return val_err("out of memory");
    for (int i = 0; i < count; i++)
        items[i] = val_str(versions[i]);
    return val_list(items, (size_t)count);
}

static const member_t atalk_afp_members[] = {
    {.kind = M_ATTR,
     .name = "enabled",
     .doc = "Serve AFP and advertise the server over NBP",
     .attr = {.type = V_BOOL, .get = atalk_afp_attr_enabled, .set = atalk_afp_attr_set_enabled}  },
    {.kind = M_ATTR,
     .name = "name",
     .doc = "NBP object name; the setter re-registers the advertisement",
     .attr = {.type = V_STRING,
              .validation_flags = OBJ_ARG_NONEMPTY,
              .get = atalk_afp_attr_name,
              .set = atalk_afp_attr_set_name}                                                    },
    {.kind = M_ATTR,
     .name = "message",
     .doc = "Server message clients fetch with FPGetSrvrMsg",
     .attr = {.type = V_STRING, .get = atalk_afp_attr_message, .set = atalk_afp_attr_set_message}},
    {.kind = M_ATTR,
     .name = "versions",
     .doc = "AFP versions this server implements and advertises",
     .attr = {.type = V_LIST, .get = atalk_afp_attr_versions}                                    },
};

static const class_desc_t atalk_afp_class = {
    .name = "atalk_afp",
    .doc = "The host AFP file server: exported volumes, server name and message",
    .members = atalk_afp_members,
    .n_members = ARRAY_LEN(atalk_afp_members),
};

// --- appletalk.printer -----------------------------------------------------

static DEF_GETTER(atalk_printer_attr_enabled) {
    return val_bool(atalk_printer_get_enabled());
}
static DEF_SETTER(atalk_printer_attr_set_enabled) {
    char err[192];
    if (atalk_printer_set_enabled(in.b, err, sizeof(err)) != 0)
        return atalk_err("cannot change the printer state", err);
    return val_none();
}
static DEF_GETTER(atalk_printer_attr_name) {
    const char *n = atalk_printer_get_name();
    return val_str(n ? n : "");
}
static DEF_SETTER(atalk_printer_attr_set_name) {
    char err[192];
    if (atalk_printer_set_name(in.s, err, sizeof(err)) != 0) {
        value_free(&in);
        return atalk_err("cannot rename the printer", err);
    }
    value_free(&in);
    return val_none();
}
static DEF_GETTER(atalk_printer_attr_status) {
    return val_str(atalk_printer_get_status());
}
static DEF_GETTER(atalk_printer_attr_interpreter) {
    return val_bool(atalk_printer_has_interpreter());
}
static DEF_GETTER(atalk_printer_attr_capture) {
    return val_bool(atalk_printer_get_capture());
}
static DEF_SETTER(atalk_printer_attr_set_capture) {
    atalk_printer_set_capture(in.b);
    return val_none();
}
static DEF_GETTER(atalk_printer_attr_documents) {
    return val_int(atalk_printer_documents());
}
static DEF_GETTER(atalk_printer_attr_last_pages) {
    return val_int(atalk_printer_last_pages());
}
static DEF_GETTER(atalk_printer_attr_last_outcome) {
    return val_str(atalk_printer_last_outcome());
}
static DEF_GETTER(atalk_printer_attr_interpreter_jobs) {
    return val_int(atalk_printer_interpreter_jobs());
}
static DEF_GETTER(atalk_printer_attr_interpreter_permanent_jobs) {
    return val_int(atalk_printer_interpreter_permanent_jobs());
}
static DEF_METHOD(atalk_printer_method_restart) {
    char err[192];
    if (atalk_printer_restart(err, sizeof(err)) != 0)
        return atalk_err("cannot restart the printer", err);
    return val_bool(true);
}

static const member_t atalk_printer_stats_members[] = {
    OBJ_U64_FIELD(atalk_printer_stats_t, jobs, "Jobs that ran to their end"),
    OBJ_U64_FIELD(atalk_printer_stats_t, aborts, "Jobs cut off: timeout, too large, closed early"),
    OBJ_U64_FIELD(atalk_printer_stats_t, bytes, "PostScript bytes received"),
    OBJ_U64_FIELD(atalk_printer_stats_t, captures, "Captures handed to the host (appletalk.printer.capture)"),
    OBJ_U64_FIELD(atalk_printer_stats_t, last_capture, "Bytes in the last capture"),
};

static const class_desc_t atalk_printer_stats_class = {
    .name = "atalk_printer_stats",
    .members = atalk_printer_stats_members,
    .n_members = ARRAY_LEN(atalk_printer_stats_members),
};

static const member_t atalk_printer_members[] = {
    {.kind = M_ATTR,
     .name = "enabled",
     .doc = "Advertise the LaserWriter via NBP",
     .attr = {.type = V_BOOL, .get = atalk_printer_attr_enabled, .set = atalk_printer_attr_set_enabled}},
    {.kind = M_ATTR,
     .name = "name",
     .doc = "NBP entity name; the setter re-registers the advertisement",
     .attr = {.type = V_STRING,
              .validation_flags = OBJ_ARG_NONEMPTY,
              .get = atalk_printer_attr_name,
              .set = atalk_printer_attr_set_name}                                                      },
    {.kind = M_ATTR,
     .name = "status",
     .doc = "PAP status string as the workstation reads it",
     .attr = {.type = V_STRING, .get = atalk_printer_attr_status}                                      },
    {.kind = M_ATTR,
     .name = "interpreter",
     .doc = "True when the build links the PostScript interpreter (PLATEN=1)",
     .attr = {.type = V_BOOL, .get = atalk_printer_attr_interpreter}                                   },
    {.kind = M_ATTR,
     .name = "capture",
     .doc = "Also hand each job's PostScript to the host: a .ps beside the PDF, or a download",
     .attr = {.type = V_BOOL, .get = atalk_printer_attr_capture, .set = atalk_printer_attr_set_capture}},
    {.kind = M_ATTR,
     .name = "documents",
     .doc = "Documents the interpreter has handed to the platform",
     .attr = {.type = V_INT, .get = atalk_printer_attr_documents}                                      },
    {.kind = M_ATTR,
     .name = "last_pages",
     .doc = "Pages of the last finished job",
     .attr = {.type = V_INT, .get = atalk_printer_attr_last_pages}                                     },
    {.kind = M_ATTR,
     .name = "last_outcome",
     .doc = "Outcome of the last finished job: ok, error: <name> in <command>, budget",
     .attr = {.type = V_STRING, .get = atalk_printer_attr_last_outcome}                                },
    {.kind = M_ATTR,
     .name = "interpreter_jobs",
     .doc = "Jobs the printer has served since it was created (0 until its first job)",
     .attr = {.type = V_INT, .get = atalk_printer_attr_interpreter_jobs}                               },
    {.kind = M_ATTR,
     .name = "interpreter_permanent_jobs",
     .doc = "Of those, jobs whose changes exitserver made permanent (startjob is not counted)",
     .attr = {.type = V_INT, .get = atalk_printer_attr_interpreter_permanent_jobs}                     },
    {.kind = M_METHOD,
     .name = "restart",
     .doc = "Power-cycle the printer: a job in progress is cut off, and what jobs made permanent is lost",
     .method = {.args = NULL,
                .nargs = 0,
                .result = V_BOOL,
                .fn = atalk_printer_method_restart,
                .ui_flags = MM_DESTRUCTIVE | MM_MUTATE}                                                },
};

static const class_desc_t atalk_printer_class = {
    .name = "atalk_printer",
    .doc = "The emulated LaserWriter: status, captured documents, last job",
    .members = atalk_printer_members,
    .n_members = ARRAY_LEN(atalk_printer_members),
};

// --- appletalk root --------------------------------------------------------

static DEF_GETTER(atalk_attr_enabled) {
    return val_bool(atalk_get_enabled());
}
static DEF_SETTER(atalk_attr_set_enabled) {
    char err[192];
    if (atalk_set_enabled(in.b, err, sizeof(err)) != 0)
        return atalk_err("cannot change the link state", err);
    return val_none();
}
static DEF_GETTER(atalk_attr_node_id) {
    return val_uint(1, atalk_node_id());
}

static const member_t atalk_members[] = {
    {.kind = M_ATTR,
     .name = "enabled",
     .doc = "Attach the AppleTalk stack to the SCC link",
     .attr = {.type = V_BOOL, .get = atalk_attr_enabled, .set = atalk_attr_set_enabled}},
    {.kind = M_ATTR,
     .name = "node_id",
     .doc = "Current LLAP node ID (0 while the stack is detached)",
     .attr = {.type = V_UINT, .width = 1, .get = atalk_attr_node_id}                   },
};

static const class_desc_t atalk_class = {
    .name = "appletalk",
    .members = atalk_members,
    .n_members = ARRAY_LEN(atalk_members),
    .doc = "Simulated AppleTalk network: file server, printer, program linking",
};
