// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_applog.h
// The append log the AFP server's persistent stores share: the CNID catalog
// and the desktop database's icon and APPL stores (appletalk_server.md §4).
//
//   file    magic(4), then records
//   record  op(1) | len(2) | payload[len] | crc32(4) over op, len, payload
//
// A store changes its in-memory state, then appends a record describing the
// change; the record is flushed before the call returns.  Opening replays the
// records in order.  Every record carries its length, so an op the store does
// not know is skipped, not misread.  The first record cut short or failing
// its CRC ends the replay -- the normal crash case -- and the file is cut
// there, so later appends are not written behind the garbage.
//
// A log that holds more than AFP_APPLOG_COMPACT_FACTOR records per live one
// is compacted -- rewritten, through a temporary file and a rename, from what
// the store's dump callback emits -- checked on every append and at close.

#ifndef GS_NETWORK_AFP_APPLOG_H
#define GS_NETWORK_AFP_APPLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AFP_APPLOG_COMPACT_FACTOR 4

typedef struct afp_applog afp_applog_t;

// Apply one replayed record to the store.  Unknown ops are the store's to
// ignore.
typedef void (*afp_applog_replay_fn)(void *ctx, uint8_t op, const uint8_t *payload, uint16_t len);

// Emit the store's whole live state with afp_applog_emit, for a compaction.
// False when an emit fails.
typedef bool (*afp_applog_dump_fn)(void *ctx, afp_applog_t *log);

// Open the log at `path` and replay it into the store.  A missing file, or
// one whose magic is not `magic`, starts afresh: `*fresh` says so.  NULL only
// when out of memory; a log that cannot be written leaves the store volatile.
afp_applog_t *afp_applog_open(const char *path, uint32_t magic, afp_applog_replay_fn replay, afp_applog_dump_fn dump,
                              void *ctx, bool *fresh);

// Append a record and flush it; compact when the log holds too many records
// for the store's `live` entries.  False when the log is not being written.
bool afp_applog_append(afp_applog_t *log, uint8_t op, const void *payload, uint16_t len, size_t live);

// Write one record of a compaction, from inside the dump callback.
bool afp_applog_emit(afp_applog_t *log, uint8_t op, const void *payload, uint16_t len);

// Records in the file (for tests and diagnostics).
size_t afp_applog_records(const afp_applog_t *log);

// Compact when due, and close.
void afp_applog_close(afp_applog_t *log, size_t live);

#endif // GS_NETWORK_AFP_APPLOG_H
