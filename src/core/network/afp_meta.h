// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_meta.h
// Per-file AFP metadata carried in the AppleDouble "._<name>" sidecar.
//
// The AFP server keeps every per-file truth a real AppleShare server would
// hold in its catalog — resource fork, Finder Info, the create/modify/backup
// dates, the locked/inhibit attribute bits and the Finder comment — beside
// the data file in an AppleDouble header (RFC 1740).  The sidecar moves and
// copies with the file, interoperates with `cp`, macOS and Netatalk, and
// survives an OPFS page reload with no extra machinery
// (proposal-afp-server-completeness.md §4.3).
//
// This module owns the mapping between AFP wire values and AppleDouble entry
// payloads.  It is I/O-complete (it opens the sidecar itself) but knows
// nothing about volumes, sessions or the object model, so the unit suite can
// drive it against a temp directory.

#ifndef GS_NETWORK_AFP_META_H
#define GS_NETWORK_AFP_META_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// AFP timestamps count seconds from 1904-01-01 GMT (appletalk_server.md §1.7);
// AppleDouble's DATES entry counts signed seconds from 2000-01-01 GMT.
#define AFP_META_EPOCH_DELTA 3029529600u

// "Never" in an AFP date field — what a real server reports for a backup date
// that has never been set (appletalk_server.md, FPCreateFile details).
#define AFP_DATE_NEVER 0x80000000u

// Finder Info is 16 B FInfo + 16 B FXInfo, the payload of AD entry 9.
#define AFP_META_FINDER_SIZE 32

// AFP caps a Finder comment at 199 bytes plus its length byte (AFP_21_22
// p. 55 "a Str199"); the desktop-database calls carry it as a Pascal string.
#define AFP_META_COMMENT_MAX 199

// AFP file/directory attribute bits (appletalk_server.md, File Attributes).
#define AFP_ATTR_INVISIBLE     (1u << 0)
#define AFP_ATTR_MULTIUSER     (1u << 1)
#define AFP_ATTR_SYSTEM        (1u << 2)
#define AFP_ATTR_DALREADYOPEN  (1u << 3)
#define AFP_ATTR_RALREADYOPEN  (1u << 4)
#define AFP_ATTR_WRITEINHIBIT  (1u << 5)
#define AFP_ATTR_BACKUPNEEDED  (1u << 6)
#define AFP_ATTR_RENAMEINHIBIT (1u << 7)
#define AFP_ATTR_DELETEINHIBIT (1u << 8)
#define AFP_ATTR_COPYPROTECT   (1u << 10)
#define AFP_ATTR_SETCLEAR      (1u << 15)

// The attribute bits this server persists.  DAlreadyOpen/RAlreadyOpen are
// derived from the live fork table and CopyProtect is not settable, so
// FPSet*Parms may not write either (appletalk_server.md FPSetFileParms).
#define AFP_ATTR_PERSISTED                                                                                             \
    (AFP_ATTR_INVISIBLE | AFP_ATTR_MULTIUSER | AFP_ATTR_SYSTEM | AFP_ATTR_WRITEINHIBIT | AFP_ATTR_BACKUPNEEDED |       \
     AFP_ATTR_RENAMEINHIBIT | AFP_ATTR_DELETEINHIBIT)

// One file's sidecar-resident metadata.  Every field carries a `has_` flag so
// a load can report "absent" distinctly from "zero" — a data-only file has no
// sidecar at all and must not grow one until something is actually set.
typedef struct {
    bool has_dates;
    uint32_t create_date; // AFP time (1904 epoch)
    uint32_t modify_date;
    uint32_t backup_date;
    uint32_t access_date;

    bool has_finder;
    uint8_t finder[AFP_META_FINDER_SIZE];

    bool has_attrs;
    uint16_t attrs; // AFP_ATTR_* (persisted subset)

    bool has_comment;
    uint8_t comment_len;
    char comment[AFP_META_COMMENT_MAX + 1];
} afp_meta_t;

// Write the "._<name>" sidecar path for `host_path` into `out`.  False if it
// would escape (empty basename, e.g. a volume root) or overflow.
bool afp_meta_sidecar_path(const char *host_path, char *out, size_t cap);

// Load a file's sidecar metadata.  A missing or unparsable sidecar is not an
// error: `out` is zeroed and every `has_` flag stays false.  Returns true when
// a sidecar was parsed, false when none exists.
bool afp_meta_load(const char *host_path, afp_meta_t *out);

// Load the resource fork from the sidecar (malloc'd; caller frees).  Yields
// NULL/0 when the file has no sidecar or no entry 2.
void afp_meta_load_rsrc(const char *host_path, uint8_t **rsrc, size_t *rsrc_len);

// Resource-fork length recorded in the sidecar (0 if none).  Reads only the
// entry table, so it does not pay for the fork bytes.
uint32_t afp_meta_rsrc_len(const char *host_path);

// Persist metadata + resource fork to the sidecar, or remove the sidecar when
// there is nothing left to store (no fork, no set metadata) so metadata-free
// files keep a clean stream.  Returns 0 or -errno.
int afp_meta_store(const char *host_path, const afp_meta_t *meta, const uint8_t *rsrc, size_t rsrc_len);

// Read-modify-write helper: load, apply `meta`, keep the existing resource
// fork.  This is what every FPSet*Parms path uses.
int afp_meta_update(const char *host_path, const afp_meta_t *meta);

// Streaming variants, so a multi-megabyte resource fork never has to exist in
// memory as a whole (proposal §5 WP-7).  `rsrc_src` is read from its current
// position for exactly `rsrc_len` bytes; pass NULL/0 for no fork.
int afp_meta_store_stream(const char *host_path, const afp_meta_t *meta, FILE *rsrc_src, size_t rsrc_len);

// Copy the sidecar's resource fork into `dst` at its current position.
// Returns the number of bytes copied (0 when there is no fork).
size_t afp_meta_copy_rsrc(const char *host_path, FILE *dst);

// True for host entries that back Mac metadata or server state and must stay
// invisible to AFP clients: AppleDouble sidecars ("._*"), legacy
// resource-fork siblings ("*.rsrc") and the ".gs-afp" control directory.
bool afp_meta_is_hidden(const char *name);

// Name of the per-volume control directory holding the CNID catalog and the
// desktop database (proposal §4.1).
#define AFP_CONTROL_DIR ".gs-afp"

// AFP <-> host time conversions, shared by every caller that touches dates.
uint32_t afp_meta_time_from_unix(int64_t unix_secs);
int64_t afp_meta_time_to_unix(uint32_t afp_secs);

#endif // GS_NETWORK_AFP_META_H
