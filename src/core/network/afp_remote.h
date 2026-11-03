// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_remote.h
// Remote volumes: an AFP volume that is a cache of a remote tree -- an
// archive.org collection, a host directory -- filled in as the guest walks
// it.  The volume is an ordinary host folder (the cache) served read-only;
// what is not there yet is asked for when a command needs it, and the
// command waits (ASP_RESULT_DEFERRED) until it has arrived:
//
//   - a folder is listed the first time it is entered or enumerated: the
//     listing's folders become empty folders, its files empty placeholders
//     that report the listed size, type and creator;
//   - a file's data is fetched when either of its forks is opened;
//   - a container -- an archive or a disk image small enough to unpack
//     (afp_remote_unpack_limit) -- is shown as a folder, and the first time it
//     is entered it is fetched and its tree copied out with every file's
//     forks (the VFS's namespaces open StuffIt, Compact Pro, zip, tar,
//     BinHex, MacBinary and HFS/MFS/ISO images).  One that will not open is
//     shown as the original file inside its folder.
//
// The core does not know archive.org.  A volume has a URI; every listing
// entry carries the URI of its own folder or file, and the core passes URIs
// back unread.  "dir:///host/path" is served here (a host directory, for
// tests and for a host-side mirror); every other scheme is a request to the
// platform: the event {"event":"afp_remote_request","id","op","uri","dest"},
// op "list" or "fetch", answered with afp_remote_complete once `dest` holds
// the listing or the file.  The page's provider (app/web2/src/bus/remoteAfp.ts)
// answers archive.org URIs.
//
// A listing is a text file, one entry per line, tab-separated:
//   kind  name  size  mtime  type  creator  uri
// kind "d" (folder) or "f" (file), size in bytes, mtime in Unix seconds,
// type and creator four characters or empty (the core then guesses them
// from the name's extension).  Lines starting with '#' are ignored.
//
// Listings and fetched data stay in the cache (under the volume's .gs-afp
// control directory), so a folder listed or a file fetched once is served
// locally from then on, also after a reload.

#ifndef AFP_REMOTE_H
#define AFP_REMOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

// Publish `uri` as the read-only volume `name` of the server `server`
// (atalk_afp_server_add), cached in the host directory `cache_dir`
// (created).  Returns the volume's slot in that server, or -1 with `err`.
int afp_remote_volume_add(const char *server, const char *name, const char *uri, const char *cache_dir, char *err,
                          size_t err_len);

// A request's outcome: `status` 0 when `dest` holds the result, else
// non-zero with `message` saying why.  -1 when no such request is pending.
int afp_remote_complete(uint32_t id, int status, const char *message);

// The pending requests the page answers, by index from 0 (false past the
// last): for a page that missed their events.
bool afp_remote_request_at(int index, uint32_t *id, const char **op, const char **uri, const char **dest);

// Volume `slot` of server `server` (any volume; `uri` is "" for a host
// folder), and withdrawing one by name.
bool afp_remote_volume_at(int server, int slot, const char **name, const char **uri, const char **path);
int afp_remote_volume_remove(const char *server, const char *name, char *err, size_t err_len);

// The `appletalk.remote` subtree (afp_remote_object.c), under `appletalk`.
struct object;
void afp_remote_install_objects(struct object *parent);

// Containers up to this size are unpacked into folders (default 64 MB; 0
// unpacks none).
uint64_t afp_remote_unpack_limit(void);
void afp_remote_set_unpack_limit(uint64_t bytes);

// Requests issued and failed since start, for the object model.
uint32_t afp_remote_requests_issued(void);
uint32_t afp_remote_requests_failed(void);

// --- For the AFP server (afp_internal.h) --------------------------------------

struct afp_remote_vol;
void afp_remote_vol_free(struct afp_remote_vol *rv);

// The dispatcher brackets each command: what it needed and could not have yet
// makes it deferred; a request that failed makes it fail (MiscErr).
void afp_remote_command_begin(void);
bool afp_remote_command_deferred(void);
bool afp_remote_command_failed(void);

// The hooks the handlers call are declared in afp_internal.h (they take a
// volume).

#endif // AFP_REMOTE_H
