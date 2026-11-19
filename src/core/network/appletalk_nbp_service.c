// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// appletalk_nbp_service.c
// The advertised name of a service the host offers on the network (the
// LaserWriter, the ImageWriter, the AFP server, the program-linking port):
// one description literal, one name check and one "publish before store"
// rename, over atalk_nbp_name_check and atalk_nbp_publish (appletalk.c).

#include "appletalk.h"
#include "appletalk_internal.h"

#include <stdio.h>

int atalk_nbp_publish_host(atalk_nbp_entry_t **entry, const char *object, const char *type, uint8_t socket) {
    // Every host service is in the local zone, on the host's own node.
    atalk_nbp_service_desc_t desc = {
        .object = object, .type = type, .zone = "*", .socket = socket, .node = ATALK_HOST_NODE, .net = 0};
    return atalk_nbp_publish(entry, &desc);
}

int atalk_nbp_rename(atalk_nbp_entry_t **entry, const char *what, const char *name, const char *type, uint8_t socket,
                     bool published, char *err, size_t err_len) {
    char scratch[160];
    if (!err || !err_len) {
        err = scratch;
        err_len = sizeof(scratch);
    }
    err[0] = '\0';
    if (atalk_nbp_name_check(what, name, err, err_len) != 0)
        return -1;
    // Published under the new name first, stored by the caller after: a name
    // another entity holds leaves the service advertised, and named, as it was.
    if (published && atalk_nbp_publish_host(entry, name, type, socket) != 0) {
        snprintf(err, err_len, "the name '%s' is already taken on the network", name);
        return -1;
    }
    return 0;
}
