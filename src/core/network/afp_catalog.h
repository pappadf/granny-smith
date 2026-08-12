// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_catalog.h
// Persistent per-volume CNID catalog for the AFP server
// (proposal-afp-server-completeness.md §4.2).
//
// AFP requires catalog node IDs that are unique per volume, stable across
// rename / move / server restart, never reused, and that cover files as well
// as directories (AFP_21_22 p. 25 Q&A; Files.p's FileID traps).  The catalog
// is the volume's identity map: every entry is (cnid, parent cnid, leaf name,
// is_dir), so a directory rename keeps every descendant's ID for free and a
// relative path is a walk of the parent chain.
//
// It is backed by an append-log at "<share>/.gs-afp/catalog.gsc":
//
//   header  'GSC1' | u32 generation | u32 next_cnid
//   record  u8 op | u32 cnid | u32 parent | u8 is_dir | pstr name | u32 crc
//
// Every mutation appends synchronously and is replayed in order at load;
// the log is compacted (rewritten as pure ADDs) once it grows past 4x the
// live-entry footprint, or at volume close.  A corrupt log is not fatal: the
// catalog rebuilds from a tree walk with fresh CNIDs and a bumped generation
// (aliases break, files do not).
//
// The module owns no AFP wire knowledge and no volume table, so the unit
// suite drives it directly against a temp directory.

#ifndef GS_NETWORK_AFP_CATALOG_H
#define GS_NETWORK_AFP_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The volume root's CNID and its notional parent (AFP fixes both).
#define AFP_CNID_ROOT        0x00000002u
#define AFP_CNID_ROOT_PARENT 0x00000001u

// HFS reserves everything below 16; a real server's first allocatable ID is
// 17, which is what clients expect to see (AFP_21_22 p. 25).
#define AFP_CNID_FIRST 17u

// Leaf-name and relative-path ceilings, matching the AFP wire limits.
#define AFP_CAT_MAX_NAME 256
#define AFP_CAT_MAX_PATH 768

typedef struct afp_catalog afp_catalog_t;

// One catalog entry as handed back to callers.  Each entry has its own
// storage, so holding two at once is safe — but any call that can grow the
// table (afp_catalog_add, and therefore afp_catalog_resolve_path with
// `adopt`) may invalidate an outstanding pointer, so copy the CNID you need
// before making one.
typedef struct {
    uint32_t cnid;
    uint32_t parent;
    bool is_dir;
    bool has_file_id; // an FPCreateID thread exists for this file
    const char *name; // leaf name ("" for the volume root)
} afp_cat_entry_t;

// Open (or create) the catalog for a volume rooted at `host_root`.  The
// control directory and log are created on demand; a missing, truncated or
// corrupt log yields an empty catalog with the generation bumped.  Returns
// NULL only on allocation failure.
afp_catalog_t *afp_catalog_open(const char *host_root);

// Flush, compact if the log has grown past its threshold, and free.
void afp_catalog_close(afp_catalog_t *cat);

// Monotonic generation.  Bumped by compaction and by any tombstone sweep;
// FPCatSearch's CatPosition and the FPEnumerate snapshot key off it.
uint32_t afp_catalog_generation(const afp_catalog_t *cat);

// Live (non-tombstoned) entry count, root included.
uint32_t afp_catalog_count(const afp_catalog_t *cat);

// Look up by CNID.  NULL when unknown or tombstoned.
const afp_cat_entry_t *afp_catalog_find(afp_catalog_t *cat, uint32_t cnid);

// Look up a child by (parent, leaf name).  NULL when absent.
const afp_cat_entry_t *afp_catalog_find_child(afp_catalog_t *cat, uint32_t parent, const char *name);

// Resolve a volume-relative path ("" = root, "a/b/c") to an entry, adopting
// any missing component along the way when `adopt` is true (the lazy-adoption
// policy of §4.2: anything the server touches that has no entry gets one).
// `is_dir` describes the final component; intermediate components are always
// adopted as directories.  NULL when the path is malformed or absent.
const afp_cat_entry_t *afp_catalog_resolve_path(afp_catalog_t *cat, const char *rel_path, bool adopt, bool is_dir);

// Reconstruct an entry's volume-relative path by walking the parent chain.
// False on overflow or a broken chain.
bool afp_catalog_path(afp_catalog_t *cat, uint32_t cnid, char *out, size_t cap);

// Create an entry under `parent`.  Returns the new entry, or the existing one
// if (parent, name) is already present.  NULL on allocation failure.
const afp_cat_entry_t *afp_catalog_add(afp_catalog_t *cat, uint32_t parent, const char *name, bool is_dir);

// Rename in place (same parent).  False when `cnid` is unknown.
bool afp_catalog_rename(afp_catalog_t *cat, uint32_t cnid, const char *new_name);

// Move to a new parent, optionally renaming (pass NULL to keep the name).
bool afp_catalog_move(afp_catalog_t *cat, uint32_t cnid, uint32_t new_parent, const char *new_name);

// Tombstone an entry (and, for a directory, everything beneath it).  The CNID
// is never reused.  Bumps the generation.
bool afp_catalog_remove(afp_catalog_t *cat, uint32_t cnid);

// FPCreateID / FPDeleteID: the file-ID thread flag.  A CNID is always a valid
// FileNumber; only an entry with a thread resolves through FPResolveID.
bool afp_catalog_set_file_id(afp_catalog_t *cat, uint32_t cnid, bool present);

// Iterate entries in ascending CNID order, starting strictly after `after`
// (pass 0 to start at the beginning).  Returns NULL at the end.  Tombstoned
// entries are skipped.  This is FPCatSearch's cursor.
const afp_cat_entry_t *afp_catalog_next(afp_catalog_t *cat, uint32_t after);

// Drop every entry whose host path no longer exists, bumping the generation
// if anything was swept.  Runs before an FPCatSearch that starts fresh.
// Returns the number of entries tombstoned.
uint32_t afp_catalog_sweep(afp_catalog_t *cat);

#endif // GS_NETWORK_AFP_CATALOG_H
