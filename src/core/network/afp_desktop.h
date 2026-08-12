// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_desktop.h
// Persistent per-volume desktop database for the AFP server
// (proposal-afp-server-completeness.md §4.4).
//
// The Finder uses the AFP desktop database instead of a shared-hostile
// Desktop file (Inside AppleTalk ch. 13, "Desktop database"): it stores icon
// bitmaps keyed by (creator, type, icon type), APPL mappings from a creator to
// the applications that own it, and Get Info comments.
//
// Comments live in each file's AppleDouble sidecar (afp_meta.c), so they move
// with the file for free.  What has no per-file home lives here, under the
// volume's control directory:
//
//   <share>/.gs-afp/desktop.icons   append-log of icon records
//   <share>/.gs-afp/desktop.appl    append-log of APPL records
//
// APPL mappings are keyed by CNID, not path, so renaming an application
// through AFP does not orphan its mapping.  Both stores grow by realloc; the
// only hard bounds are per-record sanity limits.  Misses are the caller's
// business — the DTDBMgr contract is that every miss returns
// afpItemNotFound (see OS/HFS/Extensions/DTDBMgr.a).

#ifndef GS_NETWORK_AFP_DESKTOP_H
#define GS_NETWORK_AFP_DESKTOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Largest icon AFP defines is kLarge8BitIcon at 1 KB (Files.p icon types).
#define AFP_ICON_MAX_BYTES 1024u

typedef struct afp_desktop afp_desktop_t;

// One icon record as handed back by a lookup (borrowed; valid until the next
// mutation of the same store).
typedef struct {
    uint32_t creator;
    uint32_t file_type;
    uint8_t icon_type;
    uint32_t tag;
    uint16_t size;
    const uint8_t *bitmap;
} afp_icon_t;

// One APPL mapping.
typedef struct {
    uint32_t creator;
    uint32_t cnid; // the application's catalog node ID
    uint32_t tag;
} afp_appl_t;

// Open (or create) the desktop database for a volume rooted at `host_root`.
afp_desktop_t *afp_desktop_open(const char *host_root);

// Compact if warranted and free.
void afp_desktop_close(afp_desktop_t *dt);

// --- icons -----------------------------------------------------------------

// Insert or replace the icon for (creator, type, icon_type).  Returns 0, or
// -EINVAL when the bitmap exceeds AFP_ICON_MAX_BYTES.
int afp_desktop_put_icon(afp_desktop_t *dt, uint32_t creator, uint32_t file_type, uint8_t icon_type, uint32_t tag,
                         const uint8_t *bitmap, uint16_t size);

// Exact lookup by (creator, type, icon_type).  NULL when absent.
const afp_icon_t *afp_desktop_get_icon(afp_desktop_t *dt, uint32_t creator, uint32_t file_type, uint8_t icon_type);

// The `index`-th (1-based) icon belonging to `creator`, in insertion order —
// what FPGetIconInfo enumerates.  NULL past the end.
const afp_icon_t *afp_desktop_icon_at(afp_desktop_t *dt, uint32_t creator, uint16_t index);

// --- APPL mappings ---------------------------------------------------------

// Insert or replace the mapping for (creator, cnid).  Returns 0 or -ENOMEM.
int afp_desktop_put_appl(afp_desktop_t *dt, uint32_t creator, uint32_t cnid, uint32_t tag);

// Drop the mapping for (creator, cnid); or every mapping for `creator` when
// `cnid` is 0.  Returns the number removed.
int afp_desktop_remove_appl(afp_desktop_t *dt, uint32_t creator, uint32_t cnid);

// The `index`-th (1-based) mapping for `creator`.  NULL past the end.
const afp_appl_t *afp_desktop_appl_at(afp_desktop_t *dt, uint32_t creator, uint16_t index);

// Drop mappings whose CNID no longer resolves.  `alive` is called per CNID.
void afp_desktop_prune_appls(afp_desktop_t *dt, bool (*alive)(uint32_t cnid, void *ud), void *ud);

#endif // GS_NETWORK_AFP_DESKTOP_H
