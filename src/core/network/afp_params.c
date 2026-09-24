// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_params.c
// The parameter-area codec: host paths from volume-relative ones, CNID glue,
// file and directory parameter blocks and the volume parameter block.
// Part of the AFP server; afp_internal.h has what its files share.

#include "afp_internal.h"
#include "macroman.h"

#include "afp_catalog.h"
#include "afp_desktop.h"
#include "afp_fork.h"
#include "afp_meta.h"
#include "afp_server.h"
#include "afp_wire.h"
#include "appledouble.h"
#include "appletalk.h"
#include "appletalk_asp.h"
#include "appletalk_internal.h"
#include "atalk_id.h"
#include "common.h"
#include "log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

LOG_USE_CATEGORY_NAME("afp");

// ============================================================================
// Path helpers
// ============================================================================

uint32_t afp_unix_time_to_afp(time_t t) {
    return afp_meta_time_from_unix((int64_t)t);
}

static bool afp_path_pop(char *path) {
    if (!path || !*path)
        return false;
    char *slash = strrchr(path, '/');
    if (!slash) {
        path[0] = '\0';
        return true;
    }
    *slash = '\0';
    return true;
}

// Append one element to a volume-relative path.  The element is checked here
// too, whatever its source: nothing may walk a path out of its volume.
static bool afp_append_component(char *path, size_t path_len, const char *component) {
    if (!path || !component || !afp_host_element(component, strlen(component)))
        return false;
    size_t curr = strlen(path);
    size_t comp_len = strlen(component);
    size_t needed = curr + (curr ? 1 : 0) + comp_len + 1;
    if (needed > path_len)
        return false;
    if (curr)
        path[curr++] = '/';
    memcpy(path + curr, component, comp_len + 1);
    return true;
}

// One element of a client's path or new name, as the host name it stands for
// (macroman_name_to_host: MacRoman to UTF-8, '/' to ':').  False for a name
// that cannot be one element -- "..", ".", or anything the conversion refuses
// (':' is illegal in a Mac name) -- and for the names the server keeps for
// itself (§4.1).
static bool afp_client_element(const uint8_t *bytes, size_t len, char *host, size_t cap) {
    if (len == 0 || !macroman_name_to_host(bytes, len, host, cap))
        return false;
    return afp_host_element(host, strlen(host)) && !afp_meta_is_hidden(host);
}

int afp_read_path(const uint8_t *in, int in_len, int pos, afp_path_t *out) {
    if (!in || !out || pos < 0 || pos + 2 > in_len)
        return -1;
    // 1 = short names, 2 = long names (Inside AppleTalk 13-10).  3 is AFP 3's
    // UTF-8; 0 is no 2.x path type at all.  The host has one name per file, so
    // both 2.x types resolve alike.
    uint8_t type = in[pos];
    if (type != 1 && type != 2)
        return -1;
    uint8_t len = in[pos + 1];
    if (pos + 2 + len > in_len)
        return -1; // claims more bytes than the request holds
    out->len = len;
    memcpy(out->bytes, in + pos + 2, len);
    return pos + 2 + len;
}

// --- names longer than a Mac name ---------------------------------------------
//
// A Mac name is at most 31 characters (HFS's Str31; AFP 2.x has no longer
// one).  A host name whose MacRoman form is longer goes out as its first bytes
// and "#<CNID in hex>", 31 in all.  Measured before choosing (10-network D-5,
// appletalk-afp-longname): System 6's Finder listed a 40-character name whole,
// then failed to copy the file to its disk -- "couldn't be written and was
// skipped (unknown error)" -- since HFS cannot create the name.  The CNID never
// changes, so the short form does not either, and afp_demangle maps it back.
#define AFP_MAC_NAME_MAX 31

int afp_client_name(const char *host_name, uint32_t cnid, uint8_t *out, size_t cap) {
    uint8_t full[255];
    int n = afp_mac_name(host_name, full, sizeof(full));
    if (n <= AFP_MAC_NAME_MAX) {
        n = n < (int)cap ? n : (int)cap;
        memcpy(out, full, (size_t)n);
        return n;
    }
    char suffix[12];
    int sl = snprintf(suffix, sizeof(suffix), "#%X", (unsigned)cnid);
    int keep = AFP_MAC_NAME_MAX - sl;
    if (cap < AFP_MAC_NAME_MAX)
        return -1;
    memcpy(out, full, (size_t)keep);
    memcpy(out + keep, suffix, (size_t)sl);
    return AFP_MAC_NAME_MAX;
}

// `host` (one element, below `dir_rel`) is the short form of a longer name:
// replace it with the name.  A name that exists as given is left alone, as is
// one that is not exactly what afp_client_name makes for its CNID there.
static void afp_demangle(vol_t *vol, const char *dir_rel, char *host, size_t cap) {
    const char *hash = strrchr(host, '#');
    if (!vol || !vol->catalog || !hash || !hash[1] || strspn(hash + 1, "0123456789ABCDEF") != strlen(hash + 1))
        return;
    char rel[AFP_MAX_REL_PATH];
    struct stat st;
    if (afp_build_child_path(dir_rel, host, rel, sizeof(rel)) && afp_stat_path(vol, rel, &st))
        return; // a real file of that name
    uint32_t cnid = (uint32_t)strtoul(hash + 1, NULL, 16);
    const afp_cat_entry_t *e = afp_catalog_find(vol->catalog, cnid);
    const afp_cat_entry_t *dir = (dir_rel && *dir_rel) ? afp_catalog_resolve_path(vol->catalog, dir_rel, false, true)
                                                       : afp_catalog_find(vol->catalog, AFP_CNID_ROOT);
    if (!e || !dir || e->parent != dir->cnid)
        return;
    char name[AFP_CAT_MAX_NAME];
    snprintf(name, sizeof(name), "%s", e->name);
    uint8_t mac[AFP_MAC_NAME_MAX];
    int n = afp_client_name(name, cnid, mac, sizeof(mac));
    char expect[AFP_MAC_NAME_MAX * 3 + 1];
    if (n > AFP_MAC_NAME_MAX - 1 && macroman_name_to_host(mac, (size_t)n, expect, sizeof(expect)) &&
        strcmp(expect, host) == 0)
        snprintf(host, cap, "%s", name);
}

// --- case -------------------------------------------------------------------
//
// AFP names are case-insensitive and diacritical-sensitive (Inside AppleTalk
// 13-9): two names are one when they match after Appendix D's Table D-2 maps
// lowercase to uppercase -- a-z and 13 MacRoman letters.  É is é, but é is
// not e.  The hosts are case-sensitive, so a lookup that misses exactly is
// tried again folded, and a new name that folds onto a sibling is refused
// (10-network D-7).
static const uint8_t k_d2_pairs[][2] = {
    {0x88, 0xCB},
    {0x8A, 0x80},
    {0x8B, 0xCC},
    {0x8C, 0x81},
    {0x8D, 0x82},
    {0x8E, 0x83},
    {0x96, 0x84},
    {0x9A, 0x85},
    {0x9B, 0xCD},
    {0x9F, 0x86},
    {0xBE, 0xAE},
    {0xBF, 0xAF},
    {0xCF, 0xCE},
};

uint8_t afp_fold(uint8_t c) {
    if (c >= 'a' && c <= 'z')
        return (uint8_t)(c - ('a' - 'A'));
    if (c >= 0x80)
        for (size_t i = 0; i < sizeof(k_d2_pairs) / sizeof(k_d2_pairs[0]); i++)
            if (k_d2_pairs[i][0] == c)
                return k_d2_pairs[i][1];
    return c;
}

int afp_fold_cmp(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    for (size_t i = 0; i < n; i++) {
        int d = (int)afp_fold(a[i]) - (int)afp_fold(b[i]);
        if (d)
            return d;
    }
    return alen < blen ? -1 : alen > blen ? 1 : 0;
}

int afp_name_fold_cmp(const char *host_a, const char *host_b) {
    uint8_t a[255], b[255];
    int an = afp_mac_name(host_a, a, sizeof(a));
    int bn = afp_mac_name(host_b, b, sizeof(b));
    return afp_fold_cmp(a, (size_t)an, b, (size_t)bn);
}

bool afp_name_fold_contains(const char *host_haystack, const char *host_needle) {
    uint8_t h[255], n[255];
    int hn = afp_mac_name(host_haystack, h, sizeof(h));
    int nn = afp_mac_name(host_needle, n, sizeof(n));
    for (int i = 0; i + nn <= hn; i++)
        if (afp_fold_cmp(h + i, (size_t)nn, n, (size_t)nn) == 0)
            return true;
    return false;
}

// The visible entries of `dir_rel` whose Mac name folds to `mac`, skipping
// `skip` (a name, or NULL): how many, and the first in `found`.
static int afp_fold_matches(vol_t *vol, const char *dir_rel, const uint8_t *mac, size_t len, const char *skip,
                            char *found, size_t cap) {
    char dir_full[PATH_MAX];
    if (!afp_host_path(vol, dir_rel, dir_full, sizeof(dir_full)))
        return 0;
    DIR *d = opendir(dir_full);
    if (!d)
        return 0;
    int matches = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!afp_host_element(ent->d_name, strlen(ent->d_name)) || (skip && strcmp(ent->d_name, skip) == 0))
            continue;
        uint8_t m[255];
        int n = macroman_name_from_host(ent->d_name, m, sizeof(m));
        if (n <= 0 || afp_meta_is_hidden(ent->d_name) || afp_fold_cmp(m, (size_t)n, mac, len) != 0)
            continue;
        if (matches++ == 0 && found)
            snprintf(found, cap, "%s", ent->d_name);
    }
    closedir(d);
    return matches;
}

// `host` (one element below `dir_rel`, decoded from the client's `mac`) names
// nothing on the host: if exactly one entry there has a name that folds to the
// same, it is that entry.  Two or more -- names differing in case alone, made
// on the host -- and the element stays as sent, so a lookup finds nothing: no
// rule chooses between two files the Mac cannot tell apart.
static void afp_fold_lookup(vol_t *vol, const char *dir_rel, const uint8_t *mac, size_t len, char *host, size_t cap) {
    char rel[AFP_MAX_REL_PATH], found[AFP_MAX_NAME * 3 + 1];
    struct stat st;
    if (!vol || !afp_build_child_path(dir_rel, host, rel, sizeof(rel)) || afp_stat_path(vol, rel, &st))
        return;
    if (afp_fold_matches(vol, dir_rel, mac, len, NULL, found, sizeof(found)) == 1)
        snprintf(host, cap, "%s", found);
}

bool afp_name_taken(vol_t *vol, const char *dir_rel, const char *name, const char *self_rel) {
    char rel[AFP_MAX_REL_PATH];
    struct stat st, self;
    if (!afp_build_child_path(dir_rel, name, rel, sizeof(rel)))
        return true;
    bool has_self = self_rel && afp_stat_path(vol, self_rel, &self);
    if (afp_stat_path(vol, rel, &st)) {
        // On a case-insensitive host, the object's own name in another case.
        if (!has_self || st.st_ino != self.st_ino || st.st_dev != self.st_dev)
            return true;
    }
    uint8_t mac[255];
    int n = afp_mac_name(name, mac, sizeof(mac));
    const char *self_leaf = NULL;
    char self_parent[AFP_MAX_REL_PATH];
    if (self_rel) {
        afp_extract_parent(self_rel, self_parent, sizeof(self_parent));
        if (strcmp(self_parent, dir_rel ? dir_rel : "") == 0)
            self_leaf = afp_last_component(self_rel);
    }
    // The exact name was dealt with above.
    char found[AFP_MAX_NAME * 3 + 1];
    int matches = afp_fold_matches(vol, dir_rel, mac, (size_t)n, name, found, sizeof(found));
    if (matches == 1 && self_leaf && strcmp(found, self_leaf) == 0)
        return false; // a rename that changes only case
    return matches > 0;
}

// Resolve `path` below `base_rel` (Inside AppleTalk 13-10): CNode names
// separated by NUL bytes; a single NUL before the first name is ignored, and
// each NUL beyond the first in a run climbs one level.  This used to copy the
// pathname into a C string -- so everything after the first NUL was lost and
// "sub\0file" named "sub" -- and split it on ':', '/' and '\\' instead, so a
// Mac name holding a '/' became two host path elements (10-network N-01).
bool afp_walk_path(vol_t *vol, const char *base_rel, const afp_path_t *path, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return false;
    out[0] = '\0';
    if (base_rel && *base_rel) {
        if (strlen(base_rel) >= out_len)
            return false;
        strcpy(out, base_rel);
    }
    int i = 0;
    while (path && i < path->len) {
        if (path->bytes[i] == 0) {
            int run = 0;
            while (i < path->len && path->bytes[i] == 0) {
                run++;
                i++;
            }
            for (int up = 1; up < run; up++)
                if (!*out || !afp_path_pop(out))
                    return false; // above the volume root
            continue;
        }
        int start = i;
        while (i < path->len && path->bytes[i] != 0)
            i++;
        char host[AFP_MAX_NAME * 3 + 1];
        if (!afp_client_element(path->bytes + start, (size_t)(i - start), host, sizeof(host)))
            return false;
        afp_demangle(vol, out, host, sizeof(host));
        afp_fold_lookup(vol, out, path->bytes + start, (size_t)(i - start), host, sizeof(host));
        if (!afp_append_component(out, out_len, host))
            return false;
    }
    return true;
}

bool afp_parse_leaf(const afp_path_t *path, char *out, size_t cap) {
    if (!path || memchr(path->bytes, 0, (size_t)path->len))
        return false; // a new name is one element: no separators
    return afp_client_element(path->bytes, (size_t)path->len, out, cap);
}

bool afp_name_visible(const char *host_name) {
    uint8_t mac[255];
    return !afp_meta_is_hidden(host_name) && macroman_name_from_host(host_name, mac, sizeof(mac)) > 0;
}

void afp_extract_parent(const char *rel_path, char *parent, size_t parent_len) {
    if (!parent || parent_len == 0)
        return;
    parent[0] = '\0';
    if (!rel_path || !*rel_path)
        return;
    snprintf(parent, parent_len, "%s", rel_path);
    afp_path_pop(parent);
}

const char *afp_last_component(const char *rel_path) {
    if (!rel_path || !*rel_path)
        return NULL;
    const char *slash = strrchr(rel_path, '/');
    return slash ? (slash + 1) : rel_path;
}

bool afp_build_child_path(const char *parent, const char *child, char *out, size_t out_len) {
    if (!out || out_len == 0 || !child)
        return false;
    out[0] = '\0';
    if (parent && *parent) {
        if (strlen(parent) >= out_len)
            return false;
        strcpy(out, parent);
    }
    return afp_append_component(out, out_len, child);
}

bool afp_host_path(const vol_t *vol, const char *rel, char *out, size_t out_len) {
    return vol && afp_host_join(vol->root, rel, out, out_len);
}

bool afp_stat_path(vol_t *vol, const char *rel, struct stat *st) {
    char full[PATH_MAX];
    if (!afp_host_path(vol, rel, full, sizeof(full)))
        return false;
    return stat(full, st) == 0;
}

// ============================================================================
// Catalog glue
// ============================================================================

// Volume-relative path of a directory CNID, adopting the root as needed.
// Returns false when the CNID is unknown.
bool afp_dir_rel_path(vol_t *vol, uint32_t dir_id, char *out, size_t cap) {
    if (!vol || !vol->catalog)
        return false;
    if (dir_id == 0 || dir_id == AFP_CNID_ROOT) {
        out[0] = '\0';
        return true;
    }
    const afp_cat_entry_t *e = afp_catalog_find(vol->catalog, dir_id);
    if (!e)
        return false;
    return afp_catalog_path(vol->catalog, dir_id, out, cap);
}

// Adopt (or find) the catalog entry for a volume-relative path, deciding
// file-vs-directory from the host filesystem.  This is the lazy-adoption
// policy: anything the server touches gets an entry (§4.2).
const afp_cat_entry_t *afp_entry_for(vol_t *vol, const char *rel_path) {
    if (!vol || !vol->catalog)
        return NULL;
    if (!rel_path || !*rel_path)
        return afp_catalog_find(vol->catalog, AFP_CNID_ROOT);
    struct stat st;
    bool is_dir = afp_stat_path(vol, rel_path, &st) && S_ISDIR(st.st_mode);
    return afp_catalog_resolve_path(vol->catalog, rel_path, true, is_dir);
}

// CNID of a path's parent directory (AFP_CNID_ROOT_PARENT for the root).
uint32_t afp_parent_cnid(vol_t *vol, const char *rel_path) {
    if (!vol || !rel_path || !*rel_path)
        return AFP_CNID_ROOT_PARENT;
    char parent[AFP_MAX_REL_PATH];
    afp_extract_parent(rel_path, parent, sizeof(parent));
    if (!parent[0])
        return AFP_CNID_ROOT;
    const afp_cat_entry_t *e = afp_entry_for(vol, parent);
    return e ? e->cnid : AFP_CNID_ROOT;
}

// CNID of a path itself (its FileNumber / Directory ID).
static uint32_t afp_cnid_of(vol_t *vol, const char *rel_path) {
    const afp_cat_entry_t *e = afp_entry_for(vol, rel_path);
    return e ? e->cnid : AFP_CNID_ROOT;
}

// Note a catalog-visible change so live FPEnumerate snapshots are rebuilt.
void afp_vol_touch(vol_t *vol) {
    if (vol)
        vol->mutations++;
}

uint16_t afp_count_offspring(const char *full_path) {
    if (!full_path)
        return 0;
    DIR *dir = opendir(full_path);
    if (!dir)
        return 0;
    uint32_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (!afp_name_visible(ent->d_name))
            continue; // sidecars, .gs-afp, and names no Mac name can hold
        if (++count >= UINT16_MAX)
            break;
    }
    closedir(dir);
    return (uint16_t)count;
}

// Directory access rights.  The server is guest-only, so every caller gets
// full Search/Read/Write in all four right bytes (user, owner, group,
// everyone).  Mapping host permissions here instead would hand the guest a
// user-rights byte of 0 and the AppleShare client greys out the volume.
// Real per-directory rights are the deferred WP-13 work.
#define AFP_ACCESS_RIGHTS_ALL 0x07070707u

// The AFP attribute word for one object: the persisted inhibit/visibility
// bits from its sidecar, plus the live DAlreadyOpen / RAlreadyOpen bits and a
// WriteInhibit implied by host permissions.
uint16_t afp_attributes_of(const char *host_path, const struct stat *st, const afp_meta_t *meta) {
    uint16_t attrs = 0;
    if (meta && meta->has_attrs)
        attrs |= (uint16_t)(meta->attrs & AFP_ATTR_PERSISTED);
    if (st && !(st->st_mode & S_IWUSR))
        attrs |= AFP_ATTR_WRITEINHIBIT;
    attrs |= afp_fork_open_attrs(host_path);
    return attrs;
}

// ============================================================================
// File/directory parameter areas
// ============================================================================

// Width of one bitmap field, keyed by bit.  0 marks an undefined bit.
int afp_param_field_width(bool is_dir, int bit) {
    switch (bit) {
    case 0:
        return 2; // Attributes
    case 1:
        return 4; // Parent Directory ID
    case 2:
        return 4; // Creation Date
    case 3:
        return 4; // Modification Date
    case 4:
        return 4; // Backup Date
    case 5:
        return 32; // Finder Info
    case 6:
        return 2; // Long Name offset
    case 7:
        return 2; // Short Name offset
    case 8:
        return 4; // File Number / Directory ID
    case 9:
        return is_dir ? 2 : 4; // Offspring Count / Data Fork Length
    case 10:
        return 4; // Owner ID / Resource Fork Length
    case 11:
        return is_dir ? 4 : 0; // Group ID
    case 12:
        return is_dir ? 4 : 0; // Access Rights
    case 13:
        return 6; // ProDOS Info
    default:
        return 0;
    }
}

int afp_fixed_param_len(bool is_dir, uint16_t bm) {
    int fixed = 0;
    for (int b = 0; b < 16; b++)
        if (bm & (1u << b))
            fixed += afp_param_field_width(is_dir, b);
    return fixed;
}

// Offset of one bitmap field inside a parameter area starting at `pbase`,
// or -1 when the bit is not selected.
static int afp_param_field_ptr(bool is_dir, uint16_t bm, int pbase, int target_bit) {
    if (!(bm & (1u << target_bit)))
        return -1;
    int off = 0;
    for (int b = 0; b < target_bit; b++)
        if (bm & (1u << b))
            off += afp_param_field_width(is_dir, b);
    return pbase + off;
}

// Reserve and zero the fixed part of a parameter area, remembering where the
// long/short name offsets go.  Returns the end position, or -1 on overflow.
static int afp_write_param_area(bool is_dir, uint16_t bm, uint8_t *out, int p, int out_max, int *pos_long_off,
                                int *pos_short_off) {
    if (pos_long_off)
        *pos_long_off = -1;
    if (pos_short_off)
        *pos_short_off = -1;
    for (int b = 0; b < 16; b++) {
        if (!(bm & (1u << b)))
            continue;
        int width = afp_param_field_width(is_dir, b);
        if (width == 0)
            continue;
        if (p + width > out_max)
            return -1;
        if (b == 6 && pos_long_off)
            *pos_long_off = p;
        if (b == 7 && pos_short_off)
            *pos_short_off = p;
        memset(out + p, 0, (size_t)width);
        p += width;
    }
    return p;
}

int afp_mac_name(const char *host_name, uint8_t *out, size_t cap) {
    // Every file name the server lists passed afp_name_visible, so the
    // conversion holds for those; a name that somehow does not (a volume or
    // server name a script set) goes out as its raw bytes rather than not at all.
    const char *h = host_name ? host_name : "";
    int n = macroman_name_from_host(h, out, cap);
    if (n >= 0)
        return n;
    size_t raw = strlen(h);
    n = (int)(raw > cap ? cap : raw);
    memcpy(out, h, (size_t)n);
    return n;
}

int afp_mac_text(const char *text, uint8_t *out, size_t cap) {
    const char *t = text ? text : "";
    int n = macroman_from_utf8(t, out, cap);
    if (n >= 0)
        return n;
    size_t raw = strlen(t);
    n = (int)(raw > cap ? cap : raw);
    memcpy(out, t, (size_t)n);
    return n;
}

static int afp_write_name_vars(uint8_t *out, int vpos, int out_max, int pbase, const char *host_name, uint32_t cnid,
                               uint16_t bm, int pos_long_off, int pos_short_off) {
    // The Mac name for the host name (10-network N-02: host names went out as
    // raw UTF-8, so "café" reached the Mac as "cafÃ©"), at most 31 characters
    // in both fields (D-5).
    uint8_t nm[AFP_MAC_NAME_MAX];
    int n = afp_client_name(host_name, cnid, nm, sizeof(nm));
    if (n < 0)
        return -1;
    uint8_t long_len = (uint8_t)n;
    uint8_t short_len = (uint8_t)n;
    if ((bm & (1u << 6))) {
        if (vpos + 1 + (int)long_len > out_max)
            return -1;
        if (pos_long_off >= 0)
            WR_BE16(out + pos_long_off, (uint16_t)(vpos - pbase));
        out[vpos++] = long_len;
        if (long_len) {
            memcpy(&out[vpos], nm, long_len);
            vpos += long_len;
        }
    }
    if ((bm & (1u << 7))) {
        if (vpos + 1 + (int)short_len > out_max)
            return -1;
        if (pos_short_off >= 0)
            WR_BE16(out + pos_short_off, (uint16_t)(vpos - pbase));
        out[vpos++] = short_len;
        if (short_len) {
            memcpy(&out[vpos], nm, short_len);
            vpos += short_len;
        }
    }
    return vpos;
}

// Fill a reserved parameter area with the object's real values.  Every field
// a client can ask for is served from the catalog (IDs) or the AppleDouble
// sidecar (dates, Finder Info, attributes) rather than being synthesised.
static bool afp_populate_param_area(bool is_dir, vol_t *vol, const char *rel_path, const struct stat *st, uint16_t bm,
                                    uint8_t *out, int pbase) {
    if (!st || !vol)
        return false;
    char full[PATH_MAX];
    if (!afp_host_path(vol, rel_path ? rel_path : "", full, sizeof(full)))
        return false;
    afp_meta_t meta;
    afp_meta_load(full, &meta);

    int ptr;
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 5)) >= 0)
        memcpy(out + ptr, meta.has_finder ? meta.finder : (const uint8_t[AFP_META_FINDER_SIZE]){0},
               AFP_META_FINDER_SIZE);
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 0)) >= 0)
        WR_BE16(out + ptr, afp_attributes_of(full, st, &meta));
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 1)) >= 0)
        WR_BE32(out + ptr, afp_parent_cnid(vol, rel_path));
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 2)) >= 0) {
        // The create date is server-owned metadata; st_ctime is an inode
        // change time and is not it.  Fall back to the modification time so a
        // file that never went through AFP still reports something sane.
        WR_BE32(out + ptr, meta.has_dates ? meta.create_date : afp_unix_time_to_afp(st->st_mtime));
    }
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 3)) >= 0)
        WR_BE32(out + ptr, afp_unix_time_to_afp(st->st_mtime)); // host is authoritative
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 4)) >= 0)
        WR_BE32(out + ptr, meta.has_dates ? meta.backup_date : AFP_DATE_NEVER);
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 8)) >= 0)
        WR_BE32(out + ptr, afp_cnid_of(vol, rel_path));
    if ((ptr = afp_param_field_ptr(is_dir, bm, pbase, 13)) >= 0)
        memset(out + ptr, 0, 6);

    if (!is_dir) {
        // An open fork's length is live: the sidecar catches up only on flush,
        // and the data fork may have grown since the stat (10-network N-14).
        uint32_t len;
        if ((ptr = afp_param_field_ptr(false, bm, pbase, 9)) >= 0)
            WR_BE32(out + ptr, afp_fork_live_length(full, false, &len) ? len : (uint32_t)st->st_size);
        if ((ptr = afp_param_field_ptr(false, bm, pbase, 10)) >= 0)
            WR_BE32(out + ptr, afp_fork_live_length(full, true, &len) ? len : afp_meta_rsrc_len(full));
    } else {
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 9)) >= 0)
            WR_BE16(out + ptr, afp_count_offspring(full));
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 10)) >= 0)
            WR_BE32(out + ptr, 0); // Owner ID — guest-only server
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 11)) >= 0)
            WR_BE32(out + ptr, 0); // Group ID
        if ((ptr = afp_param_field_ptr(true, bm, pbase, 12)) >= 0)
            WR_BE32(out + ptr, AFP_ACCESS_RIGHTS_ALL);
    }
    return true;
}

int afp_emit_params(bool is_dir, vol_t *vol, const char *rel, const struct stat *st, uint16_t bm, uint8_t *out,
                    int pbase, int out_max) {
    int long_off = -1, short_off = -1;
    int p = afp_write_param_area(is_dir, bm, out, pbase, out_max, &long_off, &short_off);
    if (p < 0 || !afp_populate_param_area(is_dir, vol, rel, st, bm, out, pbase))
        return -1;
    const char *name = (rel && rel[0]) ? afp_last_component(rel) : vol->name;
    uint32_t cnid = (rel && rel[0]) ? afp_cnid_of(vol, rel) : AFP_CNID_ROOT;
    p = afp_write_name_vars(out, p, out_max, pbase, name, cnid, bm, long_off, short_off);
    if (p < 0)
        return -1;
    if (p % 2) {
        if (p >= out_max)
            return -1;
        out[p++] = 0x00;
    }
    return p;
}

int afp_emit_record(bool is_dir, vol_t *vol, const char *rel, const struct stat *st, uint16_t bm, uint8_t *out, int w,
                    int out_max) {
    if (w + 2 > out_max)
        return -1;
    out[w + 1] = is_dir ? 0x80 : 0x00;
    int end = afp_emit_params(is_dir, vol, rel, st, bm, out, w + 2, out_max);
    if (end < 0 || end - w > 255)
        return -1; // StructLength is one byte
    out[w] = (uint8_t)(end - w);
    return end;
}

int afp_read_pstring(const uint8_t *in, int in_len, int pos, char *dst, size_t dst_len) {
    if (!in || !dst || dst_len == 0 || pos >= in_len)
        return -1;
    // Strict: a length that runs past the request, or past `dst`, is a bad
    // parameter -- not a shorter string.  (Clamping it made an FPDelete that
    // claimed 10 bytes but carried "abc" delete "abc": 10-network N-17.)
    uint8_t raw_len = in[pos++];
    if (pos + raw_len > in_len || (size_t)raw_len >= dst_len)
        return -1;
    if (raw_len > 0)
        memcpy(dst, &in[pos], raw_len);
    dst[raw_len] = '\0';
    return pos + raw_len;
}

// ============================================================================
// Volume parameter block (WP-1: honest sizes and dates)
// ============================================================================

// Free/total bytes for a share, from the host filesystem and clamped to what
// a 32-bit classic client can hold.  A statvfs failure falls back to a fixed
// pair so an OPFS quirk can never make the volume look full.
static void afp_volume_space(const vol_t *v, uint32_t *out_free, uint32_t *out_total) {
    uint64_t free_bytes = AFP_VOL_FALLBACK_FREE;
    uint64_t total_bytes = AFP_VOL_FALLBACK_TOTAL;
    struct statvfs vfs;
    if (statvfs(v->root, &vfs) == 0 && vfs.f_blocks > 0) {
        uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        total_bytes = (uint64_t)vfs.f_blocks * unit;
        free_bytes = (uint64_t)vfs.f_bavail * unit;
    } else {
        LOG(2, "AFP: statvfs('%s') unavailable — reporting the fallback volume size", v->root);
    }
    if (total_bytes > AFP_VOL_SIZE_CEILING)
        total_bytes = AFP_VOL_SIZE_CEILING;
    if (free_bytes > AFP_VOL_SIZE_CEILING)
        free_bytes = AFP_VOL_SIZE_CEILING;
    if (free_bytes > total_bytes)
        free_bytes = total_bytes;
    *out_free = (uint32_t)free_bytes;
    *out_total = (uint32_t)total_bytes;
}

// Volume Attributes word.  The capability bits are per-volume and only set
// when the corresponding code path exists (AFP_21_22 p. 18) — and only for a
// session that negotiated 2.1: in AFP 2.0 every bit above ReadOnly is
// "reserved, must be 0", and a 2.0 client that sees one set reads the word as
// something else entirely (measured: System 6's AppleShare 2.0.2 mounts the
// volume software-locked).
static uint16_t afp_volume_attributes(const vol_t *v, bool afp21) {
    uint16_t attr = 0;
    if (afp21 && v->catalog) {
        attr |= AFP_VOL_ATTR_FILEIDS; // FPCreateID/DeleteID/ResolveID
        attr |= AFP_VOL_ATTR_CATSEARCH; // FPCatSearch
    }
    if (access(v->root, W_OK) != 0)
        attr |= AFP_VOL_ATTR_READONLY;
    return attr;
}

int afp_write_vol_param_block(vol_t *v, uint16_t *bitmap_ptr, uint8_t *out, int out_max, bool afp21) {
    if (!v || out_max < 2)
        return 0;
    uint16_t bitmap = bitmap_ptr ? *bitmap_ptr : 0;
    int param_start = 2;
    int fixed_len = 0;
    static const int k_vol_widths[9] = {2, 2, 4, 4, 4, 2, 4, 4, 2};
    for (int b = 0; b < 9; b++)
        if (bitmap & (1u << b))
            fixed_len += k_vol_widths[b];

    uint8_t mac_name[255];
    size_t name_len = (size_t)afp_mac_name(v->name, mac_name, sizeof(mac_name));
    int var_len = (bitmap & 0x0100) ? 1 + (int)name_len : 0;
    int total_len = 2 + fixed_len + var_len;
    if (total_len > out_max) {
        // Drop the volume name rather than truncating the fixed block.
        if (bitmap & 0x0100) {
            bitmap &= (uint16_t)~0x0100;
            fixed_len -= 2;
            var_len = 0;
            total_len = 2 + fixed_len;
        }
        if (total_len > out_max)
            return 0;
    }
    WR_BE16(out, bitmap);

    struct stat root_st;
    bool have_root = stat(v->root, &root_st) == 0;
    uint32_t bytes_free = 0, bytes_total = 0;
    afp_volume_space(v, &bytes_free, &bytes_total);

    int p = param_start;
    int var_base = param_start + fixed_len;
    if (bitmap & 0x0001) {
        WR_BE16(&out[p], afp_volume_attributes(v, afp21));
        p += 2;
    }
    if (bitmap & 0x0002) {
        WR_BE16(&out[p], 0x0002); // fixed directory-ID signature
        p += 2;
    }
    if (bitmap & 0x0004) {
        WR_BE32(&out[p], have_root ? afp_unix_time_to_afp(root_st.st_ctime) : 0);
        p += 4;
    }
    if (bitmap & 0x0008) {
        WR_BE32(&out[p], have_root ? afp_unix_time_to_afp(root_st.st_mtime) : 0);
        p += 4;
    }
    if (bitmap & 0x0010) {
        WR_BE32(&out[p], v->backup_date ? v->backup_date : AFP_DATE_NEVER);
        p += 4;
    }
    if (bitmap & 0x0020) {
        WR_BE16(&out[p], v->vol_id);
        p += 2;
    }
    if (bitmap & 0x0040) {
        WR_BE32(&out[p], bytes_free);
        p += 4;
    }
    if (bitmap & 0x0080) {
        WR_BE32(&out[p], bytes_total);
        p += 4;
    }
    if (bitmap & 0x0100) {
        WR_BE16(&out[p], (uint16_t)(var_base - param_start));
        p += 2;
    }
    int vpos = var_base;
    if (bitmap & 0x0100) {
        if (vpos + 1 + (int)name_len > out_max)
            return vpos;
        out[vpos++] = (uint8_t)name_len;
        if (name_len) {
            memcpy(&out[vpos], mac_name, name_len);
            vpos += (int)name_len;
        }
    }
    if (bitmap_ptr)
        *bitmap_ptr = bitmap;
    return vpos;
}
