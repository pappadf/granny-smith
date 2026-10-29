// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// afp_remote.c
// Remote volumes (afp_remote.h): a read-only AFP volume that is a host
// folder caching a remote tree, listed, fetched and unpacked as the guest
// walks it.  Part of the AFP server; afp_internal.h has what its files share.
//
// The cache's own state lives in the volume's control directory, invisible
// to the guest:
//   .gs-afp/remote/L<hash>.tsv  a folder's listing, as the provider sent it
//   .gs-afp/remote/D<hash>      a file's data is in, or a container unpacked
//   .gs-afp/remote/req-<id>     a request's result while it is on its way
//   .gs-afp/remote/unpack-<id>/ a container being copied out
// where <hash> is a hash of the volume-relative path.  Everything else -- the
// folders, the placeholders, the fetched files and their sidecars -- is the
// volume as the guest sees it.
//
// Commands never wait here.  A hook that lacks something asks for it and
// marks the command deferred; ASP holds the command (ASP_RESULT_DEFERRED) and
// runs it again when a request completes, or when the workstation retries.

#include "afp_internal.h"

#include "afp_meta.h"
#include "afp_remote.h"
#include "afp_wire.h"
#include "appletalk_asp.h"
#include "log.h"
#include "shell.h"
#include "storage_util.h"
#include "event/event.h"
#include "io/io_worker.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

LOG_USE_CATEGORY_NAME("afp");

// A request that has not completed in this long fails (host time): the page
// that was to answer it went away, or the provider hangs.
#define REMOTE_TIMEOUT_MS (10.0 * 60.0 * 1000.0)

// Requests in flight at once, over every remote volume
#define REMOTE_MAX_REQUESTS 32

// The largest listing read
#define REMOTE_MAX_LISTING (16u << 20)

#define REMOTE_CTL_DIR "remote"

// What a node is: a folder to list, a file to fetch, or a container (an
// archive or disk image shown as the folder it unpacks to).
enum { RN_DIR = 'd', RN_FILE = 'f', RN_CONTAINER = 'c' };

// Where a node is: not asked for, on its way, in the cache, or failed (the
// next command that needs it reports the failure and asks again).
enum { RS_NEW, RS_PENDING, RS_DONE, RS_FAILED };

// One remote folder, file or container of a volume.
typedef struct {
    char *rel; // volume-relative host path ("" for the root)
    char *uri; // the provider's name for it
    char *orig; // a container's file name, which a failed unpack shows
    uint64_t size; // listed size of a file or container
    int64_t mtime; // listed modification time (Unix), 0 when unknown
    uint8_t kind;
    uint8_t state;
    uint32_t req; // the request in flight
    double started_ms; // when it was asked for
    char err[128]; // why it failed
} remote_node_t;

// A remote volume's nodes, an open-addressed table keyed by `rel`.
typedef struct afp_remote_vol afp_remote_vol_t;
struct afp_remote_vol {
    remote_node_t **slots;
    size_t cap; // a power of two
    size_t count;
};

// What a request asks for
enum { RQ_LIST, RQ_FETCH, RQ_CONTAINER, RQ_UNPACK };

// One request in flight: its node, by volume and path, so a volume removed
// meanwhile leaves the answer with nowhere to go (it is dropped).
typedef struct {
    bool in_use;
    uint32_t id;
    uint8_t op;
    uint16_t vol_id;
    char rel[AFP_MAX_REL_PATH];
    char uri[1024];
    char dest[PATH_MAX]; // where the provider puts the result
    char tmp[PATH_MAX]; // an unpack's directory
    // An unpack's outcome, from the worker
    uint64_t files;
    char err[256];
} remote_req_t;

static remote_req_t g_requests[REMOTE_MAX_REQUESTS];
static uint32_t g_next_request_id = 1;
static uint32_t g_issued, g_failed;
static uint64_t g_unpack_limit = 64ull << 20;

// The command being served: what it lacked
static bool g_cmd_deferred, g_cmd_failed;

// ============================================================================
// Small helpers
// ============================================================================

static double remote_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// FNV-1a over a path: the cache file names of its listing and marker.
static uint64_t remote_hash(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 0x100000001b3ull;
    }
    return h;
}

// "<root>/.gs-afp/remote/<prefix><hash of rel><suffix>"
static bool remote_ctl_path(const vol_t *v, char prefix, const char *rel, const char *suffix, char *out, size_t cap) {
    char leaf[64];
    snprintf(leaf, sizeof leaf, "%s/%c%016" PRIx64 "%s", REMOTE_CTL_DIR, prefix, remote_hash(rel), suffix);
    return afp_meta_control_path(v->root, leaf, out, cap);
}

static bool remote_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// Copy a file's bytes.  0 or a negative errno.
static int remote_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in)
        return -errno;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        int e = -errno;
        fclose(in);
        return e;
    }
    char buf[64 * 1024];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            rc = -EIO;
            break;
        }
    if (ferror(in))
        rc = -EIO;
    fclose(in);
    if (fclose(out) != 0 && rc == 0)
        rc = -EIO;
    return rc;
}

// Give a host entry its listed date (a fetched file would otherwise show the
// time it arrived).
static void remote_set_mtime(const char *path, int64_t mtime) {
    if (mtime <= 0)
        return;
    struct utimbuf t = {.actime = (time_t)mtime, .modtime = (time_t)mtime};
    utime(path, &t);
}

// ============================================================================
// The node table
// ============================================================================

static void rnode_free(remote_node_t *n) {
    if (!n)
        return;
    free(n->rel);
    free(n->uri);
    free(n->orig);
    free(n);
}

void afp_remote_vol_free(struct afp_remote_vol *rv) {
    if (!rv)
        return;
    for (size_t i = 0; i < rv->cap; i++)
        rnode_free(rv->slots[i]);
    free(rv->slots);
    free(rv);
}

static remote_node_t *rnode_get(const afp_remote_vol_t *rv, const char *rel) {
    if (!rv || !rv->cap)
        return NULL;
    for (size_t i = remote_hash(rel) & (rv->cap - 1);; i = (i + 1) & (rv->cap - 1)) {
        remote_node_t *n = rv->slots[i];
        if (!n)
            return NULL;
        if (strcmp(n->rel, rel) == 0)
            return n;
    }
}

// Put `n` in the table (it owns it from now on).  False when out of memory.
static bool rnode_put(afp_remote_vol_t *rv, remote_node_t *n) {
    if ((rv->count + 1) * 2 > rv->cap) {
        size_t cap = rv->cap ? rv->cap * 2 : 64;
        remote_node_t **slots = calloc(cap, sizeof(*slots));
        if (!slots)
            return false;
        for (size_t i = 0; i < rv->cap; i++) {
            remote_node_t *m = rv->slots[i];
            if (!m)
                continue;
            size_t j = remote_hash(m->rel) & (cap - 1);
            while (slots[j])
                j = (j + 1) & (cap - 1);
            slots[j] = m;
        }
        free(rv->slots);
        rv->slots = slots;
        rv->cap = cap;
    }
    size_t j = remote_hash(n->rel) & (rv->cap - 1);
    while (rv->slots[j])
        j = (j + 1) & (rv->cap - 1);
    rv->slots[j] = n;
    rv->count++;
    return true;
}

static remote_node_t *rnode_new(afp_remote_vol_t *rv, const char *rel, const char *uri, uint8_t kind) {
    remote_node_t *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->rel = gs_strdup(rel);
    n->uri = gs_strdup(uri);
    n->kind = kind;
    if (!n->rel || !n->uri || !rnode_put(rv, n)) {
        rnode_free(n);
        return NULL;
    }
    return n;
}

struct afp_remote_vol *afp_remote_vol_new(const char *uri) {
    afp_remote_vol_t *rv = calloc(1, sizeof(*rv));
    if (!rv)
        return NULL;
    if (!rnode_new(rv, "", uri, RN_DIR)) {
        afp_remote_vol_free(rv);
        return NULL;
    }
    return rv;
}

// ============================================================================
// Names and kinds
// ============================================================================

// Endings of the files shown as folders: archives and disk images the VFS
// opens.  Longest first where one ends another.
static const char *const k_container_ext[] = {
    ".sit", ".sitx", ".cpt", ".zip",   ".tar", ".tgz", ".gz",  ".hqx",      ".bin", ".img", ".image",
    ".dsk", ".dc42", ".iso", ".toast", ".cdr", ".dmg", ".smi", ".diskcopy", ".hfs", ".sea",
};

// Endings stripped from a container's folder name, repeatedly: "X.sit.hqx"
// is "X", and archive.org's wrapped "X.img_.zip" is "X" too.
static bool strip_one_ext(char *name) {
    size_t len = strlen(name);
    while (len > 1 && name[len - 1] == '_') // archive.org's "X.sit_.zip"
        name[--len] = '\0';
    for (size_t i = 0; i < sizeof k_container_ext / sizeof *k_container_ext; i++) {
        size_t e = strlen(k_container_ext[i]);
        if (len > e && strcasecmp(name + len - e, k_container_ext[i]) == 0) {
            name[len - e] = '\0';
            return true;
        }
    }
    return false;
}

static bool is_container_name(const char *name) {
    size_t len = strlen(name);
    for (size_t i = 0; i < sizeof k_container_ext / sizeof *k_container_ext; i++) {
        size_t e = strlen(k_container_ext[i]);
        // A bare ".sea" is an application; only a wrapped one is a container.
        if (strcmp(k_container_ext[i], ".sea") == 0)
            continue;
        if (len > e && strcasecmp(name + len - e, k_container_ext[i]) == 0)
            return true;
    }
    return false;
}

// Type and creator a file of this name most likely has, for a listing that
// gives none: what the Finder needs to show the right icon and open it with
// the right application.
static const struct {
    const char *ext, *type, *creator;
} k_ext_types[] = {
    {".sit",   "SIT!", "SIT!"},
    {".sitx",  "SITX", "SIT!"},
    {".sea",   "APPL", "aust"},
    {".cpt",   "PACT", "CPCT"},
    {".hqx",   "TEXT", "BnHq"},
    {".bin",   "BINA", "????"},
    {".zip",   "ZIP ", "SITx"},
    {".gz",    "Gzip", "SITx"},
    {".tgz",   "Gzip", "SITx"},
    {".tar",   "TARF", "SITx"},
    {".img",   "dImg", "dCpy"},
    {".image", "dImg", "dCpy"},
    {".dsk",   "dImg", "dCpy"},
    {".dc42",  "dImg", "dCpy"},
    {".smi",   "APPL", "oneb"},
    {".iso",   "rodh", "ddsk"},
    {".toast", "CDr3", "Ddsk"},
    {".txt",   "TEXT", "ttxt"},
    {".text",  "TEXT", "ttxt"},
    {".me",    "TEXT", "ttxt"},
    {".doc",   "TEXT", "ttxt"},
    {".htm",   "TEXT", "MOSS"},
    {".html",  "TEXT", "MOSS"},
    {".pdf",   "PDF ", "CARO"},
    {".gif",   "GIFf", "ogle"},
    {".jpg",   "JPEG", "ogle"},
    {".jpeg",  "JPEG", "ogle"},
    {".png",   "PNGf", "ogle"},
    {".pict",  "PICT", "ogle"},
    {".pct",   "PICT", "ogle"},
    {".mov",   "MooV", "TVOD"},
    {".mp3",   "MPG3", "TVOD"},
    {".aif",   "AIFF", "TVOD"},
    {".aiff",  "AIFF", "TVOD"},
    {".wav",   "WAVE", "TVOD"},
    {".mid",   "Midi", "TVOD"},
};

static bool guess_type(const char *name, char type[4], char creator[4]) {
    size_t len = strlen(name);
    for (size_t i = 0; i < sizeof k_ext_types / sizeof *k_ext_types; i++) {
        size_t e = strlen(k_ext_types[i].ext);
        if (len > e && strcasecmp(name + len - e, k_ext_types[i].ext) == 0) {
            memcpy(type, k_ext_types[i].type, 4);
            memcpy(creator, k_ext_types[i].creator, 4);
            return true;
        }
    }
    return false;
}

// A listed name as a host name: no path separators, no control bytes, not one
// of the server's own.  False when nothing usable is left.
static bool sanitize_name(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; p++)
        out[o++] = (*p == '/' || *p == '\\' || *p < 0x20) ? '-' : (char)*p;
    out[o] = '\0';
    if (!o || strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
        return false;
    return !afp_meta_is_hidden(out);
}

// ============================================================================
// Listings
// ============================================================================

// Whether a listing names an entry `name` (case-insensitively, as the host
// and the Finder compare names).
static bool listing_names(const char *text, const char *name) {
    size_t nl = strlen(name);
    for (const char *line = text; line && *line;) {
        const char *tab = strchr(line, '\t');
        const char *eol = strchr(line, '\n');
        if (tab && (!eol || tab < eol) && line[0] != '#') {
            const char *end = strchr(tab + 1, '\t');
            if (end && (!eol || end < eol) && (size_t)(end - tab - 1) == nl && strncasecmp(tab + 1, name, nl) == 0)
                return true;
        }
        line = eol ? eol + 1 : NULL;
    }
    return false;
}

// Make `parent`'s children from its listing at `path`: the folders and
// containers as empty folders, the files as empty placeholders carrying the
// listed type, creator and date.  Entries already in the cache are kept.
static void listing_apply(vol_t *v, remote_node_t *parent, const char *path) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (read_file(path, REMOTE_MAX_LISTING, &buf, &len) != 0) {
        LOG(1, "AFP remote: cannot read the listing of '%s'", parent->rel);
        return;
    }
    char *text = realloc(buf, len + 1);
    if (!text) {
        free(buf);
        return;
    }
    text[len] = '\0';
    char *names = gs_strdup(text); // the listing as read, for name clashes
    if (!names) {
        free(text);
        return;
    }
    unsigned made = 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r')
            line[--ll] = '\0';
        if (!ll || line[0] == '#')
            continue;
        char *f[7] = {0};
        int nf = 0;
        for (char *p = line; nf < 7; nf++) {
            f[nf] = p;
            char *tab = strchr(p, '\t');
            if (!tab) {
                nf++;
                break;
            }
            *tab = '\0';
            p = tab + 1;
        }
        if (nf < 7 || (f[0][0] != RN_DIR && f[0][0] != RN_FILE) || !f[6][0])
            continue;
        char name[AFP_MAX_NAME * 3 + 1];
        if (!sanitize_name(f[1], name, sizeof name))
            continue;
        uint64_t size = strtoull(f[2], NULL, 10);
        int64_t mtime = strtoll(f[3], NULL, 10);
        uint8_t kind = (uint8_t)f[0][0];
        if (kind == RN_FILE && is_container_name(name) && size > 0 && size <= g_unpack_limit)
            kind = RN_CONTAINER;

        // A container's folder is named without its archive endings, unless
        // that leaves nothing or names another entry of the folder.  (The
        // folder itself may be in the cache already, from an earlier session.)
        char shown[sizeof name];
        snprintf(shown, sizeof shown, "%s", name);
        if (kind == RN_CONTAINER) {
            char stripped[sizeof name];
            snprintf(stripped, sizeof stripped, "%s", name);
            while (strip_one_ext(stripped)) {
            }
            char rel_try[AFP_MAX_REL_PATH];
            if (stripped[0] && afp_build_child_path(parent->rel, stripped, rel_try, sizeof rel_try) &&
                !rnode_get(v->remote, rel_try) && !listing_names(names, stripped))
                snprintf(shown, sizeof shown, "%s", stripped);
        }
        char rel[AFP_MAX_REL_PATH], host[PATH_MAX];
        if (!afp_build_child_path(parent->rel, shown, rel, sizeof rel) || !afp_host_path(v, rel, host, sizeof host))
            continue;
        if (rnode_get(v->remote, rel))
            continue; // a duplicate name (or one that folds to it on the host)
        remote_node_t *n = rnode_new(v->remote, rel, f[6], kind);
        if (!n)
            break;
        n->size = size;
        n->mtime = mtime;
        if (kind == RN_CONTAINER)
            n->orig = gs_strdup(name);

        struct stat st;
        bool present = stat(host, &st) == 0;
        if (kind == RN_FILE) {
            if (!present) {
                FILE *ph = fopen(host, "wb");
                if (ph)
                    fclose(ph);
                char type[4], creator[4];
                bool have = strlen(f[4]) == 4 && strlen(f[5]) == 4;
                if (have) {
                    memcpy(type, f[4], 4);
                    memcpy(creator, f[5], 4);
                } else
                    have = guess_type(name, type, creator);
                if (have) {
                    afp_meta_t meta = {0};
                    meta.has_finder = true;
                    memcpy(meta.finder, type, 4);
                    memcpy(meta.finder + 4, creator, 4);
                    afp_meta_update(host, &meta);
                }
                remote_set_mtime(host, mtime);
            }
        } else if (!present) {
            mkdir(host, 0755);
            remote_set_mtime(host, mtime);
        }
        // What is already in the cache, from an earlier session
        char marker[PATH_MAX];
        if (kind != RN_DIR && remote_ctl_path(v, 'D', rel, "", marker, sizeof marker) && remote_exists(marker))
            n->state = RS_DONE;
        else if (kind == RN_FILE && size == 0)
            n->state = RS_DONE; // nothing to fetch
        made++;
    }
    free(names);
    free(text);
    parent->state = RS_DONE;
    v->mutations++; // listings taken before this one are stale
    LOG(3, "AFP remote: '%s' listed, %u entries", parent->rel[0] ? parent->rel : "<root>", made);
}

// The node at `rel`, loading the cached listings of the folders above it
// that this session has not read yet: a client can name a folder by its ID
// without walking to it.
static remote_node_t *rnode_find(vol_t *v, const char *rel) {
    remote_node_t *n = rnode_get(v->remote, rel);
    if (n || !rel[0])
        return n;
    char parent_rel[AFP_MAX_REL_PATH];
    afp_extract_parent(rel, parent_rel, sizeof parent_rel);
    remote_node_t *p = rnode_find(v, parent_rel);
    if (!p || p->kind != RN_DIR || p->state == RS_DONE)
        return NULL;
    char cached[PATH_MAX];
    if (remote_ctl_path(v, 'L', p->rel, ".tsv", cached, sizeof cached) && remote_exists(cached)) {
        listing_apply(v, p, cached);
        return rnode_get(v->remote, rel);
    }
    return NULL;
}

// ============================================================================
// Requests
// ============================================================================

static remote_req_t *req_find(uint32_t id) {
    for (int i = 0; i < REMOTE_MAX_REQUESTS; i++)
        if (g_requests[i].in_use && g_requests[i].id == id)
            return &g_requests[i];
    return NULL;
}

static void req_release(remote_req_t *q) {
    memset(q, 0, sizeof(*q));
}

// A request the core serves itself, on the I/O worker: it always completes,
// and its slot must stay its own until it does.
static bool req_is_local(const remote_req_t *q) {
    return q->op == RQ_UNPACK || strncmp(q->uri, "dir://", 6) == 0;
}

static void rnode_fail(remote_node_t *n, const char *why) {
    n->state = RS_FAILED;
    n->req = 0;
    snprintf(n->err, sizeof n->err, "%s", why ? why : "failed");
    g_failed++;
    LOG(1, "AFP remote: '%s' failed: %s", n->rel[0] ? n->rel : "<root>", n->err);
}

static void remote_finish(remote_req_t *q, int status, const char *message);

// --- dir://, served here --------------------------------------------------------

// A dir:// request, on the I/O worker: the listing written, or the file
// copied, to the request's destination.
static int dir_work(void *ud, char *err, size_t err_cap) {
    remote_req_t *q = ud;
    const char *path = q->uri + strlen("dir://");
    if (q->op != RQ_LIST) {
        int rc = remote_copy_file(path, q->dest);
        if (rc)
            snprintf(err, err_cap, "cannot copy '%s': %s", path, strerror(-rc));
        return rc;
    }
    DIR *d = opendir(path);
    if (!d) {
        int e = errno;
        snprintf(err, err_cap, "cannot list '%s': %s", path, strerror(e));
        return -e;
    }
    FILE *out = fopen(q->dest, "w");
    if (!out) {
        int e = errno;
        closedir(d);
        snprintf(err, err_cap, "cannot write the listing: %s", strerror(e));
        return -e;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue; // dot files, sidecars and the control directory
        char child[PATH_MAX];
        struct stat st;
        if ((size_t)snprintf(child, sizeof child, "%s/%s", path, de->d_name) >= sizeof child || stat(child, &st) != 0)
            continue;
        bool dir = S_ISDIR(st.st_mode);
        char type[5] = "", creator[5] = "";
        afp_meta_t meta;
        if (!dir && afp_meta_load(child, &meta) && meta.has_finder) {
            memcpy(type, meta.finder, 4);
            memcpy(creator, meta.finder + 4, 4);
            if (memchr(type, 0, 4) || memchr(creator, 0, 4) || strpbrk(type, "\t\n") || strpbrk(creator, "\t\n"))
                type[0] = creator[0] = '\0';
        }
        fprintf(out, "%c\t%s\t%llu\t%lld\t%s\t%s\tdir://%s\n", dir ? 'd' : 'f', de->d_name,
                dir ? 0ull : (unsigned long long)st.st_size, (long long)st.st_mtime, type, creator, child);
    }
    closedir(d);
    if (fclose(out) != 0) {
        snprintf(err, err_cap, "cannot write the listing");
        return -EIO;
    }
    return 0;
}

static void dir_done(bool ok, double ms, const char *error, void *ud) {
    (void)ms;
    remote_req_t *q = ud;
    if (q->in_use)
        remote_finish(q, ok ? 0 : 1, error);
}

// --- Unpacking a container ------------------------------------------------------

// Names a disk or archive carries that are the Finder's or another system's
// bookkeeping, not content.
static bool unpack_skip(const char *name) {
    static const char *const k_skip[] = {"Desktop DB", "Desktop DF",      "Desktop",
                                         "Trash",      ".DS_Store",       "__MACOSX",
                                         ".Trashes",   "Temporary Items", "TheVolumeSettingsFolder",
                                         ".fseventsd", ".Spotlight-V100", "TheFindByContentFolder"};
    for (size_t i = 0; i < sizeof k_skip / sizeof *k_skip; i++)
        if (strcasecmp(name, k_skip[i]) == 0)
            return true;
    return false;
}

// Drop the bookkeeping entries of `dir` (with their sidecars), and return the
// one folder that is all that remains, or NULL: a disk or archive whose
// content is one folder is shown as that folder's content.
static char *unpack_prune(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return NULL;
    char only[PATH_MAX] = "";
    int visible = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char path[PATH_MAX];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, de->d_name) >= sizeof path)
            continue;
        const char *base = strncmp(de->d_name, "._", 2) == 0 ? de->d_name + 2 : de->d_name;
        if (unpack_skip(base) || !de->d_name[0]) {
            rm_tree(path);
            continue;
        }
        if (strncmp(de->d_name, "._", 2) == 0)
            continue; // a sidecar goes with its entry
        struct stat st;
        visible++;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(only, sizeof only, "%s", path);
        else
            only[0] = '\0';
    }
    closedir(d);
    return (visible == 1 && only[0]) ? gs_strdup(only) : NULL;
}

// The one file a directory holds, when it is itself a container small enough
// to unpack (an archive that holds just a disk image), or NULL.
static char *unpack_lone_container(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return NULL;
    char found[PATH_MAX] = "";
    int visible = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' && (!de->d_name[1] || de->d_name[1] == '.' || de->d_name[1] == '_'))
            continue; // ".", ".." and sidecars
        char path[PATH_MAX];
        struct stat st;
        visible++;
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, de->d_name) < sizeof path && stat(path, &st) == 0 &&
            S_ISREG(st.st_mode) && is_container_name(de->d_name) && (uint64_t)st.st_size <= g_unpack_limit)
            snprintf(found, sizeof found, "%s", path);
    }
    closedir(d);
    return (visible == 1 && found[0]) ? gs_strdup(found) : NULL;
}

// On the I/O worker: copy the container's tree out into the request's
// directory, with every file's forks (the VFS opens archives and images).
// What comes out of an archive as one disk image or archive is opened in
// turn (archive.org's "X.image_.sit_.hqx" is a disk in a StuffIt archive):
// the guest gets the files, not a file it would have to mount.
static int unpack_work(void *ud, char *err, size_t err_cap) {
    remote_req_t *q = ud;
    uint64_t files = 0;
    int rc = shell_cp_contents(q->dest, q->tmp, &files, NULL, err, err_cap);
    q->files = files;
    if (rc == 0 && files == 0) {
        snprintf(err, err_cap, "nothing in it");
        return -ENOENT;
    }
    if (rc != 0)
        return rc;
    for (int depth = 0; depth < 3; depth++) {
        char *inner = unpack_lone_container(q->tmp);
        if (!inner)
            break;
        // Out of the way, then opened into the emptied directory; put back
        // when it does not open.
        char blob[PATH_MAX], inner_sc[PATH_MAX], blob_sc[PATH_MAX];
        char name[AFP_MAX_NAME * 3 + 1];
        snprintf(name, sizeof name, "%s", afp_last_component(inner));
        bool sc = afp_meta_sidecar_path(inner, inner_sc, sizeof inner_sc) && remote_exists(inner_sc);
        if ((size_t)snprintf(blob, sizeof blob, "%s.inner", q->tmp) >= sizeof blob || rename(inner, blob) != 0) {
            free(inner);
            break;
        }
        if (sc && afp_meta_sidecar_path(blob, blob_sc, sizeof blob_sc))
            rename(inner_sc, blob_sc);
        rm_tree(q->tmp);
        uint64_t n = 0;
        char inner_err[128];
        if (shell_cp_contents(blob, q->tmp, &n, NULL, inner_err, sizeof inner_err) == 0 && n > 0) {
            q->files = n;
            if (sc)
                unlink(blob_sc);
            unlink(blob);
            free(inner);
            continue;
        }
        rm_tree(q->tmp);
        mkdir(q->tmp, 0755);
        rename(blob, inner);
        if (sc)
            rename(blob_sc, inner_sc);
        free(inner);
        break;
    }
    return 0;
}

static void unpack_done(bool ok, double ms, const char *error, void *ud) {
    (void)ms;
    remote_req_t *q = ud;
    if (!q->in_use)
        return;
    snprintf(q->err, sizeof q->err, "%s", error ? error : "");
    q->files = ok ? (q->files ? q->files : 1) : 0;
    remote_finish(q, 0, NULL);
}

// The container's tree, or failing that its file, into its folder.
static void unpack_place(vol_t *v, remote_node_t *n, remote_req_t *q) {
    char host[PATH_MAX];
    if (!afp_host_path(v, n->rel, host, sizeof host))
        return;
    mkdir(host, 0755);
    bool placed = false;
    if (q->files) {
        // Collapse a lone folder at the top (twice: an archive holding a
        // disk image's one folder)
        char *src = gs_strdup(q->tmp);
        for (int depth = 0; src && depth < 2; depth++) {
            char *only = unpack_prune(src);
            if (!only)
                break;
            free(src);
            src = only;
        }
        placed = src && afp_move_entries(src, host) == 0;
        free(src);
        if (!placed)
            LOG(1, "AFP remote: cannot move the contents of '%s' into place", n->rel);
    } else
        LOG(2, "AFP remote: '%s' does not unpack (%s); shown as its file", n->orig ? n->orig : n->rel, q->err);
    if (!placed) {
        // The original file inside the folder: the guest's own tools may
        // open what the core cannot.
        char file[PATH_MAX];
        if ((size_t)snprintf(file, sizeof file, "%s/%s", host, n->orig ? n->orig : "file") < sizeof file) {
            if (rename(q->dest, file) != 0)
                remote_copy_file(q->dest, file);
            char type[4], creator[4];
            if (guess_type(file, type, creator)) {
                afp_meta_t meta = {0};
                meta.has_finder = true;
                memcpy(meta.finder, type, 4);
                memcpy(meta.finder + 4, creator, 4);
                afp_meta_update(file, &meta);
            }
            remote_set_mtime(file, n->mtime);
        }
    }
    rm_tree(q->tmp);
    unlink(q->dest);
    remote_set_mtime(host, n->mtime);
}

// --- Issuing and finishing ---------------------------------------------------------

// Ask the page for `q`: the event its provider answers.
static void remote_emit(const remote_req_t *q) {
    char *uri = json_escape_dup(q->uri);
    char *dest = json_escape_dup(q->dest);
    char *json = uri && dest ? str_printf("{\"event\":\"afp_remote_request\",\"id\":%u,\"op\":\"%s\",\"uri\":\"%s\","
                                          "\"dest\":\"%s\"}",
                                          q->id, q->op == RQ_LIST ? "list" : "fetch", uri, dest)
                             : NULL;
    if (json)
        event_emit(GS_EVENT_NOTIFY, json);
    free(json);
    free(uri);
    free(dest);
}

// Ask for what `n` needs.  It may complete before this returns (dir:// with
// no I/O worker), so the caller looks at the node's state afterwards.
static void remote_issue(vol_t *v, remote_node_t *n) {
    remote_req_t *q = NULL;
    for (int i = 0; i < REMOTE_MAX_REQUESTS && !q; i++)
        if (!g_requests[i].in_use)
            q = &g_requests[i];
    if (!q)
        return; // every slot busy: asked again when one completes
    memset(q, 0, sizeof(*q));
    q->in_use = true;
    q->id = g_next_request_id++;
    if (!g_next_request_id)
        g_next_request_id = 1;
    q->op = n->kind == RN_DIR ? RQ_LIST : n->kind == RN_FILE ? RQ_FETCH : RQ_CONTAINER;
    q->vol_id = v->vol_id;
    snprintf(q->rel, sizeof q->rel, "%s", n->rel);
    snprintf(q->uri, sizeof q->uri, "%s", n->uri);
    char leaf[48];
    snprintf(leaf, sizeof leaf, "%s/req-%u", REMOTE_CTL_DIR, q->id);
    if (!afp_meta_control_path(v->root, leaf, q->dest, sizeof q->dest)) {
        req_release(q);
        rnode_fail(n, "cache path too long");
        return;
    }
    unlink(q->dest);
    n->state = RS_PENDING;
    n->req = q->id;
    n->started_ms = remote_now_ms();
    g_issued++;
    LOG(3, "AFP remote: request %u: %s %s", q->id, q->op == RQ_LIST ? "list" : "fetch", q->uri);
    if (strncmp(q->uri, "dir://", 6) == 0) {
        if (!io_submit_work(dir_work, q, dir_done, q)) {
            char err[256] = "";
            int rc = dir_work(q, err, sizeof err);
            remote_finish(q, rc, err);
        }
        return;
    }
    remote_emit(q);
}

// A request's answer: its node takes what arrived, and every command that was
// waiting runs again.
static void remote_finish(remote_req_t *q, int status, const char *message) {
    afp_server_t *srv = NULL;
    vol_t *v = afp_vol_by_id_any(q->vol_id, &srv);
    remote_node_t *n = v && v->remote ? rnode_get(v->remote, q->rel) : NULL;
    if (!n || n->req != q->id) {
        unlink(q->dest); // its volume went, or the request timed out
        if (q->tmp[0])
            rm_tree(q->tmp);
        req_release(q);
        return;
    }
    if (status != 0) {
        rnode_fail(n, message && *message ? message : "the request failed");
        unlink(q->dest);
        req_release(q);
        asp_retry_deferred();
        return;
    }
    char host[PATH_MAX];
    switch (q->op) {
    case RQ_LIST: {
        char cached[PATH_MAX];
        if (!remote_ctl_path(v, 'L', n->rel, ".tsv", cached, sizeof cached) || rename(q->dest, cached) != 0) {
            rnode_fail(n, "cannot keep the listing");
            break;
        }
        listing_apply(v, n, cached);
        break;
    }
    case RQ_FETCH: {
        char marker[PATH_MAX], src_sc[PATH_MAX], dst_sc[PATH_MAX];
        if (!afp_host_path(v, n->rel, host, sizeof host) || rename(q->dest, host) != 0) {
            rnode_fail(n, "cannot put the file in place");
            break;
        }
        // A provider that sent a sidecar (dir://) sent the real forks and
        // Finder info; otherwise the placeholder's stay.
        if (afp_meta_sidecar_path(q->dest, src_sc, sizeof src_sc) &&
            afp_meta_sidecar_path(host, dst_sc, sizeof dst_sc) && remote_exists(src_sc))
            rename(src_sc, dst_sc);
        remote_set_mtime(host, n->mtime);
        if (remote_ctl_path(v, 'D', n->rel, "", marker, sizeof marker)) {
            FILE *m = fopen(marker, "wb");
            if (m)
                fclose(m);
        }
        n->state = RS_DONE;
        n->req = 0;
        v->mutations++;
        break;
    }
    case RQ_CONTAINER: {
        // The container is here: copy its tree out, on the I/O worker.
        char leaf[48];
        snprintf(leaf, sizeof leaf, "%s/unpack-%u", REMOTE_CTL_DIR, q->id);
        if (!afp_meta_control_path(v->root, leaf, q->tmp, sizeof q->tmp)) {
            rnode_fail(n, "cache path too long");
            break;
        }
        rm_tree(q->tmp);
        q->op = RQ_UNPACK;
        if (!io_submit_work(unpack_work, q, unpack_done, q)) {
            char err[256] = "";
            int rc = unpack_work(q, err, sizeof err);
            unpack_done(rc == 0, 0, rc ? err : NULL, q);
        }
        return; // the node stays pending until the tree is out
    }
    case RQ_UNPACK: {
        unpack_place(v, n, q);
        char marker[PATH_MAX];
        if (remote_ctl_path(v, 'D', n->rel, "", marker, sizeof marker)) {
            FILE *m = fopen(marker, "wb");
            if (m)
                fclose(m);
        }
        n->state = RS_DONE;
        n->req = 0;
        v->mutations++;
        break;
    }
    }
    if (n->state == RS_PENDING && n->req == q->id)
        n->state = RS_FAILED; // a case above that did not finish it
    unlink(q->dest);
    req_release(q);
    asp_retry_deferred();
}

int afp_remote_complete(uint32_t id, int status, const char *message) {
    remote_req_t *q = req_find(id);
    if (!q || req_is_local(q))
        return -1; // not the page's to answer
    remote_finish(q, status, message);
    return 0;
}

bool afp_remote_request_at(int index, uint32_t *id, const char **op, const char **uri, const char **dest) {
    int seen = 0;
    for (int i = 0; i < REMOTE_MAX_REQUESTS; i++) {
        const remote_req_t *q = &g_requests[i];
        if (!q->in_use || req_is_local(q) || seen++ != index)
            continue;
        *id = q->id;
        *op = q->op == RQ_LIST ? "list" : "fetch";
        *uri = q->uri;
        *dest = q->dest;
        return true;
    }
    return false;
}

bool afp_remote_volume_at(int server, int slot, const char **name, const char **uri, const char **path) {
    afp_server_t *srv = afp_server_at(server);
    if (!srv || slot < 0 || slot >= AFP_MAX_VOLUMES || !srv->vols[slot].in_use)
        return false;
    vol_t *v = &srv->vols[slot];
    remote_node_t *root = v->remote ? rnode_get(v->remote, "") : NULL;
    *name = v->name;
    *uri = root ? root->uri : "";
    *path = v->root;
    return true;
}

int afp_remote_volume_remove(const char *server, const char *name, char *err, size_t err_len) {
    afp_server_t *srv = afp_server_at(atalk_afp_server_find(server));
    if (!srv) {
        snprintf(err, err_len, "no server named '%s'", server ? server : "");
        return -1;
    }
    afp_server_t *prev = afp_use_server(srv);
    int rc = atalk_afp_volume_remove(name, err, err_len);
    afp_use_server(prev);
    return rc;
}

// ============================================================================
// The hooks
// ============================================================================

void afp_remote_command_begin(void) {
    g_cmd_deferred = g_cmd_failed = false;
}

bool afp_remote_command_deferred(void) {
    return g_cmd_deferred;
}

bool afp_remote_command_failed(void) {
    return g_cmd_failed && !g_cmd_deferred;
}

// What a node's state means for the command that needs it.
static bool remote_ready(vol_t *v, remote_node_t *n) {
    if (n->state == RS_PENDING && remote_now_ms() - n->started_ms > REMOTE_TIMEOUT_MS) {
        remote_req_t *q = req_find(n->req);
        if (!q || !req_is_local(q)) {
            if (q)
                req_release(q); // a late answer finds no request
            rnode_fail(n, "no answer");
        }
    }
    switch (n->state) {
    case RS_DONE:
        return true;
    case RS_FAILED:
        // Reported once to the command that meets it; the next one asks again.
        g_cmd_failed = true;
        n->state = RS_NEW;
        return false;
    case RS_NEW:
        if (n->kind == RN_DIR) {
            char cached[PATH_MAX];
            if (remote_ctl_path(v, 'L', n->rel, ".tsv", cached, sizeof cached) && remote_exists(cached)) {
                listing_apply(v, n, cached);
                return true;
            }
        }
        remote_issue(v, n);
        if (n->state == RS_DONE)
            return true; // answered at once
        if (n->state == RS_FAILED) {
            g_cmd_failed = true;
            n->state = RS_NEW;
            return false;
        }
        g_cmd_deferred = true;
        return false;
    default:
        g_cmd_deferred = true;
        return false;
    }
}

bool afp_remote_need_dir(vol_t *vol, const char *rel) {
    if (!vol || !vol->remote)
        return true;
    remote_node_t *n = rnode_find(vol, rel ? rel : "");
    if (!n || n->kind == RN_FILE)
        return true; // a folder of an unpacked tree, or not a folder
    return remote_ready(vol, n);
}

bool afp_remote_need_file(vol_t *vol, const char *rel) {
    if (!vol || !vol->remote)
        return true;
    remote_node_t *n = rnode_find(vol, rel ? rel : "");
    if (!n || n->kind != RN_FILE)
        return true;
    return remote_ready(vol, n);
}

bool afp_remote_dir_unlisted(vol_t *vol, const char *rel) {
    if (!vol || !vol->remote)
        return false;
    remote_node_t *n = rnode_get(vol->remote, rel ? rel : "");
    return n && n->kind != RN_FILE && n->state != RS_DONE;
}

void afp_remote_stat_fix(vol_t *vol, const char *rel, struct stat *st) {
    if (!vol || !vol->remote || !S_ISREG(st->st_mode))
        return;
    remote_node_t *n = rnode_get(vol->remote, rel ? rel : "");
    if (n && n->kind == RN_FILE && n->state != RS_DONE)
        st->st_size = (off_t)n->size;
}

// ============================================================================
// Volumes and settings
// ============================================================================

int afp_remote_volume_add(const char *server, const char *name, const char *uri, const char *cache_dir, char *err,
                          size_t err_len) {
    if (err && err_len)
        err[0] = '\0';
    int idx = atalk_afp_server_find(server);
    afp_server_t *srv = afp_server_at(idx);
    if (!srv) {
        snprintf(err, err_len, "no server named '%s'", server ? server : "");
        return -1;
    }
    if (!uri || !*uri) {
        snprintf(err, err_len, "a remote volume needs a URI");
        return -1;
    }
    if (!cache_dir || !*cache_dir || mkdir_p(cache_dir) != 0) {
        snprintf(err, err_len, "cannot create the cache folder '%s'", cache_dir ? cache_dir : "");
        return -1;
    }
    afp_server_t *prev = afp_use_server(srv);
    int slot = atalk_afp_volume_add(name, cache_dir, err, err_len);
    if (slot >= 0) {
        vol_t *v = &srv->vols[slot];
        v->read_only = true;
        v->remote = afp_remote_vol_new(uri);
        char ctl[PATH_MAX];
        if (!v->remote || !afp_meta_control_path(v->root, REMOTE_CTL_DIR, ctl, sizeof ctl) || mkdir_p(ctl) != 0) {
            atalk_afp_volume_remove(name, NULL, 0);
            snprintf(err, err_len, "cannot set up the cache under '%s'", cache_dir);
            slot = -1;
        } else
            LOG(1, "AFP: remote volume '%s' on '%s' -> %s (cache %s)", name, server, uri, v->root);
    }
    afp_use_server(prev);
    return slot;
}

uint64_t afp_remote_unpack_limit(void) {
    return g_unpack_limit;
}

void afp_remote_set_unpack_limit(uint64_t bytes) {
    g_unpack_limit = bytes;
}

uint32_t afp_remote_requests_issued(void) {
    return g_issued;
}

uint32_t afp_remote_requests_failed(void) {
    return g_failed;
}
