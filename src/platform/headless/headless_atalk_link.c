// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// headless_atalk_link.c
// AppleTalk router links over UDP for the headless build: the transport
// behind appletalk.router.link_udp (platform_hooks.h, appletalk_link.h).
//
// One UDP socket per link, bound to `local` and connected to `remote`; one
// record per datagram.  UDP gives no delivery guarantee, which the records
// do not need on a loopback or LAN path: DDP is best effort anyway, the
// router resends HELLO until it is answered, and PINGs are periodic.
// Received records are drained from hl_pump_once, so they reach the router
// on the thread that runs the machine, between frame-units.

#include "headless.h"

#include "appletalk_link.h"
#include "platform_hooks.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// One link's socket.
typedef struct {
    int fd; // -1 when free
    atalk_link_t *link;
} udp_link_t;

static udp_link_t s_links[ATALK_LINK_MAX] = {[0 ... ATALK_LINK_MAX - 1] = {.fd = -1}};

// Resolve "host:port" (IPv4) into `out`; false with `err` set if it is not one.
static bool parse_addr(const char *spec, struct sockaddr_in *out, char *err, size_t err_len) {
    char host[128];
    const char *colon = spec ? strrchr(spec, ':') : NULL;
    if (!colon || colon == spec || (size_t)(colon - spec) >= sizeof(host)) {
        snprintf(err, err_len, "'%s' is not host:port", spec ? spec : "");
        return false;
    }
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = '\0';
    char *end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (!end || *end || port < 1 || port > 65535) {
        snprintf(err, err_len, "'%s': bad port", spec);
        return false;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        snprintf(err, err_len, "'%s': unknown host", host);
        return false;
    }
    memcpy(out, res->ai_addr, sizeof(*out));
    out->sin_port = htons((uint16_t)port);
    freeaddrinfo(res);
    return true;
}

// The transport's send: one record, one datagram.  A refused send (the other
// end is not listening yet) is not an error: the router retries what matters.
static void udp_send(void *ctx, const uint8_t *rec, size_t len, bool reliable) {
    (void)reliable;
    udp_link_t *u = ctx;
    if (u->fd >= 0)
        (void)send(u->fd, rec, len, 0);
}

// The transport's close: the router is done with the link.
static void udp_close(void *ctx) {
    udp_link_t *u = ctx;
    if (u->fd >= 0)
        close(u->fd);
    u->fd = -1;
    u->link = NULL;
}

static const atalk_link_transport_t k_udp_transport = {.send = udp_send, .close = udp_close};

int platform_atalk_link_udp(const char *local, const char *remote, bool friend_link, char *err, size_t err_len) {
    struct sockaddr_in la, ra;
    if (!parse_addr(local, &la, err, err_len) || !parse_addr(remote, &ra, err, err_len))
        return -1;
    udp_link_t *u = NULL;
    for (int i = 0; i < ATALK_LINK_MAX && !u; i++)
        if (s_links[i].fd < 0)
            u = &s_links[i];
    if (!u) {
        snprintf(err, err_len, "every link slot is in use");
        return -1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        snprintf(err, err_len, "socket: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&la, sizeof(la)) != 0 || connect(fd, (struct sockaddr *)&ra, sizeof(ra)) != 0) {
        snprintf(err, err_len, "%s: %s", local, strerror(errno));
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    u->fd = fd;
    u->link = atalk_link_open(&k_udp_transport, u, "socket", friend_link);
    if (!u->link) {
        close(fd);
        u->fd = -1;
        snprintf(err, err_len, "the router has no free link slot");
        return -1;
    }
    return 0;
}

void hl_atalk_links_poll(void) {
    uint8_t rec[ATALK_LINK_REC_MAX];
    for (int i = 0; i < ATALK_LINK_MAX; i++) {
        udp_link_t *u = &s_links[i];
        // Drain what has arrived; the router may close the link (and this
        // socket) while handling a record, so re-check each time
        for (int n = 0; n < 256 && u->fd >= 0 && u->link; n++) {
            ssize_t got = recv(u->fd, rec, sizeof(rec), 0);
            if (got <= 0)
                break; // nothing more now, or a refused send reported back
            atalk_link_receive(u->link, rec, (size_t)got);
        }
    }
}
