// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_fork.h
// Shared fork backing store, deny modes and byte-range locks for the AFP
// server (proposal-afp-server-completeness.md §5 WP-6/WP-7).
//
// A fork is opened many times by many sessions, so the bytes cannot live in a
// per-open private copy: the second opener would see a stale snapshot and the
// last close would silently win.  Here one *backing* object exists per
// (volume, relative path, fork), refcounted across every open of it:
//
//   data fork      the host file itself, opened once and shared
//   resource fork  a working file seeded from the AppleDouble sidecar and
//                  written back on flush / last close, streamed both ways so
//                  the fork's size never bounds memory
//
// Each open is a *handle* carrying its own access/deny modes and byte-range
// locks.  Deny evaluation follows Inside AppleTalk ch. 13 ("Synchronization
// rules"): a new open fails with DenyConflict when the requested access
// intersects the fork's cumulative deny mode, or when the requested deny
// intersects its cumulative access mode.

#ifndef GS_NETWORK_AFP_FORK_H
#define GS_NETWORK_AFP_FORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// FPOpenFork AccessMode bits (Inside AppleTalk ch. 13, FPOpenFork inputs).
#define AFP_ACCESS_READ  (1u << 0)
#define AFP_ACCESS_WRITE (1u << 1)
#define AFP_DENY_READ    (1u << 4)
#define AFP_DENY_WRITE   (1u << 5)

// A fork may hold at most this many byte-range locks before the server
// answers afpNoMoreLocks.
#define AFP_MAX_LOCKS_PER_FORK 256

typedef struct afp_fork afp_fork_t;

// Outcome of an open or a lock operation, mapped to AFP result codes by the
// caller so this module stays free of wire constants.
typedef enum {
    AFP_FORK_OK = 0,
    AFP_FORK_DENY_CONFLICT,
    AFP_FORK_ACCESS_DENIED,
    AFP_FORK_TOO_MANY,
    AFP_FORK_NO_MORE_LOCKS,
    AFP_FORK_LOCK_ERR,
    AFP_FORK_RANGE_OVERLAP,
    AFP_FORK_RANGE_NOT_LOCKED,
    AFP_FORK_IO_ERR,
} afp_fork_status_t;

// Release every backing and handle.  Called from the server's teardown.
void afp_fork_shutdown(void);

// Open a fork.  `host_path` is the data file's host path (the sidecar path is
// derived for resource forks) and `rel_path` its volume-relative path, kept
// for logging and for re-pointing after a rename.  On success `*out` holds the
// new handle.
afp_fork_status_t afp_fork_open(uint16_t vol_id, uint16_t session_id, const char *host_path, const char *rel_path,
                                bool is_resource, uint16_t access_mode, afp_fork_t **out);

// Look up an open handle by its wire reference number.
afp_fork_t *afp_fork_find(uint16_t ref);

// Handle accessors.
uint16_t afp_fork_ref(const afp_fork_t *fk);
uint16_t afp_fork_vol_id(const afp_fork_t *fk);
const char *afp_fork_rel_path(const afp_fork_t *fk);
const char *afp_fork_host_path(const afp_fork_t *fk);
bool afp_fork_is_resource(const afp_fork_t *fk);
uint16_t afp_fork_access_mode(const afp_fork_t *fk);

// Close one handle, dropping its locks and persisting the backing when this
// was its last reference.
void afp_fork_close(afp_fork_t *fk);

// Close every handle belonging to a volume / to a session.  Used by share
// removal, FPLogout, and session expiry.
void afp_fork_close_volume(uint16_t vol_id);
void afp_fork_close_session(uint16_t session_id);

// Live counters for the object model.
uint32_t afp_fork_count_volume(uint16_t vol_id);
uint32_t afp_fork_count_session(uint16_t session_id);
uint32_t afp_fork_count_total(void);

// True when any handle is open on `host_path` (either fork) — FPDelete and
// FPCreateFile(hard) use this to answer afpFileBusy.
bool afp_fork_path_busy(const char *host_path);

// AFP attribute bits (DAlreadyOpen / RAlreadyOpen) implied by the live opens
// on `host_path`.
uint16_t afp_fork_open_attrs(const char *host_path);

// Re-point every handle and backing from one host path to another, keeping
// open forks valid across FPRename / FPMoveAndRename / FPExchangeFiles.
void afp_fork_repoint(const char *old_host_path, const char *new_host_path, const char *new_rel_path);

// --- I/O -------------------------------------------------------------------

// Current fork length in bytes.
uint32_t afp_fork_length(afp_fork_t *fk);

// Read up to `count` bytes at `offset`.  `*out_read` receives the byte count.
// Fails with AFP_FORK_LOCK_ERR when the range is locked by another handle.
afp_fork_status_t afp_fork_read(afp_fork_t *fk, uint32_t offset, uint32_t count, uint8_t *buf, uint32_t *out_read);

// Write `count` bytes at `offset`.  Same foreign-lock rule as read.
afp_fork_status_t afp_fork_write(afp_fork_t *fk, uint32_t offset, const uint8_t *buf, uint32_t count,
                                 uint32_t *out_written);

// Truncate (or extend) to `length`.
afp_fork_status_t afp_fork_truncate(afp_fork_t *fk, uint32_t length);

// Flush this fork's backing to its permanent home.
void afp_fork_flush(afp_fork_t *fk);

// Flush every fork open on a volume — FPFlush.
void afp_fork_flush_volume(uint16_t vol_id);

// --- byte-range locks ------------------------------------------------------

// Lock or unlock [start, start+length).  `end_relative` measures `start` from
// end of fork; `length` of 0xFFFFFFFF means "to the end".  On success
// `*out_start` receives the resolved range start, which is the reply value.
afp_fork_status_t afp_fork_range_lock(afp_fork_t *fk, bool unlock, bool end_relative, int32_t start, uint32_t length,
                                      uint32_t *out_start);

#endif // GS_NETWORK_AFP_FORK_H
