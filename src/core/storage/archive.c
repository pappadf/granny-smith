// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// archive.c
// Mac archive file handling: files.archive.identify and files.archive.extract.
// Archives are namespaces of the VFS (namespace.h), so identify is a bounded
// probe of the file and extract is a copy of its tree; the in-tree peeler
// library does the format work, and its name does not leak to users.

#include "archive.h"
#include "gs_out.h"

#include "io_leaf.h"
#include "io/io_worker.h"

#include "image_chunkmap.h"
#include "log.h"
#include "object.h"
#include "peeler.h"
#include "shell.h"
#include "source.h"
#include "storage_util.h"
#include "udif_writer.h"
#include "value.h"
#include "vfs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// Public API
// ============================================================================

const char *archive_identify_file(const char *path) {
    if (!path || !*path)
        return NULL;
    // Through the VFS, so an archive inside an image or another archive is
    // identified too; detection reads a bounded probe, never the whole file.
    int err = 0;
    gs_source_t *src = vfs_open_source(path, GS_FORK_DATA, &err);
    if (!src)
        return NULL;
    peel_probe_t p;
    const char *format = NULL;
    if (peel_probe_init(&p, src) == 0) {
        const peel_format_desc_t *d = peel_identify(&p);
        format = d ? d->name : NULL;
        peel_probe_free(&p);
    }
    gs_source_release(src);
    return format;
}

int archive_extract_file(const char *path, const char *out_dir) {
    if (!path)
        return -1;
    const char *dir = (out_dir && *out_dir) ? out_dir : ".";
    if (gs_mkdir_p(dir) != 0) {
        fprintf(stderr, "archive: cannot create output directory '%s': %s\n", dir, strerror(errno));
        return -1;
    }
    // The archive is a namespace (namespace.h): extracting it is copying its
    // tree out, the same walk files.cp does out of a disk image -- each file's
    // data fork under its name, its resource fork and Finder info in an
    // AppleDouble "._" sidecar.  Wrappers peel on the way (a .sit.hqx
    // extracts the .sit's files).
    uint64_t files = 0, bytes = 0;
    char err[400];
    int rc = shell_cp_contents(path, dir, &files, &bytes, err, sizeof(err));
    if (rc == -ECANCELED)
        return rc;
    if (rc != 0) {
        fprintf(stderr, "archive: failed to extract '%s': %s\n", path, err);
        return -1;
    }
    if (files == 0) {
        fprintf(stderr, "archive: no files extracted from '%s'\n", path);
        return -1;
    }
    gs_outf("Successfully extracted '%s' (%llu file%s)\n", path, (unsigned long long)files, files == 1 ? "" : "s");
    return 0;
}

// The extraction as an I/O job (io_leaf.h): the archive is decoded and its
// files written on the I/O worker; the answer comes when it is done.
static int work_extract(io_leaf_t *j) {
    int rc = archive_extract_file(j->a, j->b);
    if (rc == -ECANCELED)
        snprintf(j->err, sizeof j->err, "cancelled");
    else if (rc != 0)
        snprintf(j->err, sizeof j->err, "extraction of '%s' failed", j->a);
    return rc == 0 ? 0 : (rc == -ECANCELED ? -ECANCELED : -EIO);
}

// ============================================================================
// Import: one member, decoded straight into a UDIF
// ============================================================================
//
// A disk image inside a StuffIt / Compact Pro / BinHex / MacBinary archive is
// imported without extracting it: peeler's decode-through source fills a
// sink as it decodes, and this sink's write() is the UDIF writer's append,
// so the decoded disk streams into the compact image a 64 KB piece at a time.
// The sink keeps only the last piece, which is all the decode-through ever
// reads back here (one byte at the end of the fork drives the whole decode).

#define IMPORT_TAIL (64u * 1024u)

typedef struct {
    udif_writer_t *w;
    uint64_t expected; // the fork's declared length (PEEL_SIZE_UNKNOWN if not)
    uint64_t written;
    uint8_t tail[IMPORT_TAIL]; // the last bytes written, at tail_off
    uint64_t tail_off;
    size_t tail_len;
    int err; // the writer's error, or -ECANCELED
} import_sink_t;

static int64_t isink_read(peel_source_t *s, uint64_t off, void *buf, size_t len) {
    import_sink_t *k = s->ctx;
    if (off < k->tail_off || off >= k->tail_off + k->tail_len)
        return off >= k->written ? 0 : -EIO; // gone into the writer
    size_t at = (size_t)(off - k->tail_off);
    size_t n = k->tail_len - at < len ? k->tail_len - at : len;
    memcpy(buf, k->tail + at, n);
    return (int64_t)n;
}

static uint64_t isink_size(peel_source_t *s) {
    import_sink_t *k = s->ctx;
    return k->expected == PEEL_SIZE_UNKNOWN ? k->written : k->expected;
}

static const char *isink_key(peel_source_t *s) {
    (void)s;
    return "archive-import-sink";
}

static peel_tier_t isink_tier(peel_source_t *s) {
    (void)s;
    return PEEL_TIER_STREAM;
}

static void isink_close(peel_source_t *s) {
    (void)s; // the context belongs to the import
}

static const peel_source_ops_t isink_ops = {isink_read, isink_size, isink_key, isink_tier, isink_close, NULL};

static peel_source_t *isink_create(void *ctx, const char *key, uint64_t expected_len) {
    (void)key;
    import_sink_t *k = ctx;
    k->expected = expected_len;
    return peel_source_new(&isink_ops, k, NULL);
}

static int64_t isink_write(peel_source_t *s, uint64_t off, const void *buf, size_t len) {
    import_sink_t *k = s->ctx;
    if (off != k->written || k->err)
        return -1;
    if (io_check_cancelled()) {
        k->err = -ECANCELED;
        return -1;
    }
    int rc = udif_writer_append(k->w, buf, len);
    if (rc) {
        k->err = rc;
        return -1;
    }
    // Keep the newest IMPORT_TAIL bytes.
    const uint8_t *p = buf;
    if (len >= IMPORT_TAIL) {
        memcpy(k->tail, p + len - IMPORT_TAIL, IMPORT_TAIL);
        k->tail_len = IMPORT_TAIL;
    } else {
        if (k->tail_len + len > IMPORT_TAIL) {
            size_t drop = k->tail_len + len - IMPORT_TAIL;
            memmove(k->tail, k->tail + drop, k->tail_len - drop);
            k->tail_len -= drop;
        }
        memcpy(k->tail + k->tail_len, p, len);
        k->tail_len += len;
    }
    k->written += len;
    k->tail_off = k->written - k->tail_len;
    io_report_progress(k->written, k->expected == PEEL_SIZE_UNKNOWN ? 0 : k->expected);
    return (int64_t)len;
}

static void isink_commit(peel_source_t *s) {
    (void)s;
}

static const peel_sink_ops_t import_sink_ops = {isink_create, isink_write, isink_commit};

// True when `name` (an entry path) matches `member` exactly, case-blind, or
// by its last component.
static bool member_matches(const char *name, const char *member) {
    while (*member == '/')
        member++;
    if (strcmp(name, member) == 0 || strcasecmp(name, member) == 0)
        return true;
    const char *nb = strrchr(name, '/');
    const char *mb = strrchr(member, '/');
    return strcasecmp(nb ? nb + 1 : name, mb ? mb + 1 : member) == 0;
}

// Junk an archive carries besides its media.
static bool member_is_junk(const peel_entry_t *e) {
    const char *b = strrchr(e->path, '/');
    b = b ? b + 1 : e->path;
    return e->is_dir || b[0] == '.' || strncmp(e->path, "__MACOSX/", 9) == 0;
}

typedef struct {
    char member[512];
    char *want; // the member asked for, or NULL
    char *origin; // recorded in the image as gs-origin, or NULL
    udif_writer_stats_t st;
} import_job_t;

// The archive to take a member from: through wrapper layers whose payload
// is itself an archive (a .sit.hqx), each payload being the compressed --
// small -- side.  A new reference, or NULL with a message.
static peel_source_t *innermost_archive(peel_source_t *src, char *err, size_t cap) {
    peel_source_t *cur = peel_source_retain(src);
    for (int depth = 0; depth < 4; depth++) {
        peel_err_t *pe = NULL;
        peel_archive_t *a = peel_open(cur, NULL, NULL, &pe);
        if (!a) {
            snprintf(err, cap, "not an archive: %s", pe ? peel_err_msg(pe) : "unrecognised");
            peel_err_free(pe);
            peel_source_release(cur);
            return NULL;
        }
        if (!peel_is_wrapper(a) || peel_count(a) != 1) {
            peel_close(a);
            return cur;
        }
        peel_source_t *payload = peel_open_fork(a, 0, PEEL_FORK_DATA, &pe);
        peel_close(a);
        if (!payload) {
            peel_err_free(pe);
            return cur;
        }
        peel_probe_t p;
        bool inner = false;
        if (peel_probe_init(&p, payload) == 0) {
            const peel_format_desc_t *d = peel_identify(&p);
            inner = d != NULL;
            peel_probe_free(&p);
        }
        if (!inner) {
            peel_source_release(payload);
            return cur; // the wrapper's payload is the medium
        }
        peel_source_release(cur);
        cur = payload;
    }
    return cur;
}

static int work_archive_import(io_leaf_t *j) {
    import_job_t *u = j->ud;
    int e = 0;
    vfs_stat_t vst;
    if (vfs_stat(j->b, &vst) == 0) {
        snprintf(j->err, sizeof j->err, "'%s' exists (refuses to overwrite)", j->b);
        return -EEXIST;
    }
    gs_source_t *src = vfs_open_source(j->a, GS_FORK_DATA, &e);
    if (!src) {
        snprintf(j->err, sizeof j->err, "cannot open '%s'", j->a);
        return -ENOENT;
    }
    peel_source_t *arc = innermost_archive(src, j->err, sizeof j->err);
    gs_source_release(src);
    if (!arc)
        return -EINVAL;

    // Structure first (headers only), to choose the member.
    peel_err_t *pe = NULL;
    peel_archive_t *a = peel_open(arc, NULL, NULL, &pe);
    int pick = -1;
    uint64_t best = 0;
    for (int i = 0; a && i < peel_count(a); i++) {
        const peel_entry_t *en = peel_entry(a, i);
        if (u->want) {
            if (!en->is_dir && member_matches(en->path, u->want)) {
                pick = i;
                break;
            }
        } else if (!member_is_junk(en) && en->data_len > best) {
            best = en->data_len; // the largest file: a disk image dwarfs the rest
            pick = i;
        }
    }
    bool ndif = false;
    if (a && pick >= 0) {
        snprintf(u->member, sizeof u->member, "%s", peel_entry(a, pick)->path);
        // An NDIF image's block map is its resource fork, which a UDIF's
        // payload cannot carry: that one is extracted (forks kept) instead.
        if (peel_entry(a, pick)->rsrc_len > 0) {
            peel_source_t *rf = peel_open_fork(a, pick, PEEL_FORK_RSRC, &pe);
            ndif = rf && ndif_source_detect(rf);
            peel_source_release(rf);
        }
    }
    if (a)
        peel_close(a);
    peel_err_free(pe);
    if (ndif) {
        snprintf(j->err, sizeof j->err, "'%.200s' is a Disk Copy 6 (NDIF) image: extract it instead", u->member);
        peel_source_release(arc);
        return -ENOTSUP;
    }
    if (pick < 0) {
        snprintf(j->err, sizeof j->err, u->want ? "\"%s\" is not in the archive" : "the archive holds no file%s",
                 u->want ? u->want : "");
        peel_source_release(arc);
        return -ENOENT;
    }

    // Again with the sink that is the writer.
    import_sink_t *k = calloc(1, sizeof(*k));
    const char *base = strrchr(u->member, '/');
    udif_writer_opts_t o = {.level = 1, .source_name = base ? base + 1 : u->member, .origin = u->origin};
    gs_mkdir_parents(j->b);
    if (k)
        k->w = udif_writer_open(j->b, &o, j->err, sizeof j->err);
    if (!k || !k->w) {
        free(k);
        peel_source_release(arc);
        return -EIO;
    }
    a = peel_open(arc, &import_sink_ops, k, &pe);
    peel_source_t *fork = a ? peel_open_fork(a, pick, PEEL_FORK_DATA, &pe) : NULL;
    int rc = fork ? 0 : -EIO;
    uint64_t len = fork ? peel_source_size(fork) : 0;
    if (fork && peel_source_tier(fork) == PEEL_TIER_RANDOM) {
        // Stored: a view of the archive; copy it across.
        uint8_t *buf = malloc(1u << 20);
        rc = buf ? 0 : -ENOMEM;
        for (uint64_t at = 0; !rc && at < len;) {
            if (io_check_cancelled()) {
                rc = -ECANCELED;
                break;
            }
            size_t n = len - at < (1u << 20) ? (size_t)(len - at) : (1u << 20);
            if (peel_source_read_exact(fork, at, buf, n) != 0)
                rc = -EIO;
            else
                rc = udif_writer_append(k->w, buf, n);
            at += n;
            io_report_progress(at, len);
        }
        free(buf);
    } else if (fork && len > 0) {
        // Compressed: one byte at the end drives the whole decode through
        // the sink, a piece at a time.
        uint8_t last;
        if (peel_source_read(fork, len - 1, &last, 1) != 1)
            rc = k->err ? k->err : -EIO;
    }
    if (rc == 0 && k->err)
        rc = k->err;
    peel_source_release(fork);
    if (a)
        peel_close(a);
    peel_err_free(pe);
    peel_source_release(arc);
    if (rc == 0)
        rc = udif_writer_finish(k->w, &u->st);
    else
        udif_writer_abort(k->w);
    free(k);
    if (rc == -ECANCELED)
        snprintf(j->err, sizeof j->err, "cancelled");
    else if (rc != 0 && !j->err[0])
        snprintf(j->err, sizeof j->err, "importing '%.100s' from '%.100s' failed: %s", u->member, j->a, strerror(-rc));
    return rc;
}

static value_t answer_archive_import(io_leaf_t *j) {
    import_job_t *u = j->ud;
    value_map_builder_t *b = val_map_new();
    val_map_put(b, "member", val_str(u->member));
    val_map_put(b, "bytes_in", val_uint(8, u->st.bytes_in));
    val_map_put(b, "stored_bytes", val_uint(8, u->st.stored_bytes));
    val_map_put(b, "sectors", val_uint(8, u->st.sectors));
    return val_map_finish(b);
}

static void cleanup_archive_import(io_leaf_t *j) {
    import_job_t *u = j->ud;
    if (u) {
        free(u->want);
        free(u->origin);
    }
    free(u);
}

// ============================================================================
// Object-model class descriptor
// ============================================================================

// `files.archive.identify(path)` — return the format short name for a recognised
// archive ("sit" / "cpt" / "zip" / "hqx" / "bin" / "gz"), or empty string
// when the file is unreadable or not an archive. Empty is falsy under
// the predicate-truthy rule — same shape as floppy.identify.
static DEF_METHOD(archive_method_identify) {
    const char *format = archive_identify_file(argv[0].s);
    return val_str(format ? format : "");
}

// `files.archive.extract(path, [out_dir])` — extract a Mac archive into out_dir
// (defaults to the current working directory). Returns true on success.
static DEF_METHOD(archive_method_extract) {
    const char *path = argv[0].s;
    const char *out_dir = (argc >= 2 && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    io_leaf_t *j = io_leaf_new(path, out_dir);
    if (!j)
        return val_err("files.archive.extract: out of memory");
    j->work = work_extract;
    return io_leaf_dispatch(j, "files.archive.extract");
}

// `files.archive.import(path, dst, [member])` — decode one member of a Mac
// archive (the named one, or the largest file) straight into a compact UDIF
// at `dst`, without extracting anything.  Answers {member, bytes_in,
// stored_bytes, sectors}.
static DEF_METHOD(archive_method_import) {
    const char *member = (argc >= 3 && argv[2].kind == V_STRING && argv[2].s && *argv[2].s) ? argv[2].s : NULL;
    io_leaf_t *j = io_leaf_new(argv[0].s, argv[1].s);
    import_job_t *u = calloc(1, sizeof *u);
    if (!j || !u) {
        free(u);
        if (j)
            free(j->a), free(j->b), free(j);
        return val_err("files.archive.import: out of memory");
    }
    u->want = member ? strdup(member) : NULL;
    if (argc >= 4 && argv[3].kind == V_STRING && argv[3].s && *argv[3].s)
        u->origin = strdup(argv[3].s);
    j->ud = u;
    j->work = work_archive_import;
    j->answer = answer_archive_import;
    j->cleanup = cleanup_archive_import;
    return io_leaf_dispatch(j, "files.archive.import");
}

static const arg_decl_t archive_import_args[] = {
    ARG_PATH("path", "Archive file path"),
    ARG_PATH("dst", "The UDIF (.dmg) to write (must not exist)"),
    {.name = "member",
                                                          .kind = V_STRING,
                                                          .validation_flags = OBJ_ARG_OPTIONAL,
                                                          .doc = "The member to take (exact, case-blind, or by its last name component)",
                                                          .default_doc = "the largest file"},
    {.name = "origin",
                                                          .kind = V_STRING,
                                                          .validation_flags = OBJ_ARG_OPTIONAL,
                                                          .doc = "Where the archive came from (e.g. a URL), recorded in the image as is",
                                                          .default_doc = "none"            },
};

static const arg_decl_t archive_path_arg[] = {
    ARG_PATH("path", "Archive file path"),
};

static const arg_decl_t archive_extract_args[] = {
    ARG_PATH("path", "Archive file path"),
    {.name = "out_dir",
                                   .kind = V_STRING,
                                   .presentation_flags = VAL_PATH,
                                   .validation_flags = OBJ_ARG_OPTIONAL,
                                   .doc = "Output directory",
                                   .default_doc = "the current directory"},
};

static const member_t archive_members[] = {
    {.kind = M_METHOD,
     .name = "identify",
     .examples = EXAMPLES("files.archive.identify \"/opfs/downloads/app.sit\""),
     .doc = "Identify a Mac archive's format",
     .method = {.result_doc =
                    "\"sit\", \"cpt\", \"zip\", \"tar\", \"hqx\", \"bin\" or \"gz\"; empty when not an archive",
                .args = archive_path_arg,
                .nargs = 1,
                .result = V_STRING,
                .fn = archive_method_identify}},
    {.kind = M_METHOD,
     .name = "extract",
     .examples = EXAMPLES("files.archive.extract \"/opfs/downloads/app.sit\"",
     "files.archive.extract \"/opfs/downloads/app.sit\" \"/opfs/unpacked\""),
     .doc = "Extract a Mac archive into out_dir",
     .method =
         {.ui_flags = MM_IO, .args = archive_extract_args, .nargs = 2, .result = V_BOOL, .fn = archive_method_extract}},
    {.kind = M_METHOD,
     .name = "import",
     .examples =
         EXAMPLES("files.archive.import \"/opfs/upload/disk.sit\" \"/opfs/upload/disk.dmg.part\"",
     "files.archive.import \"/opfs/upload/cd.sit.hqx\" \"/opfs/upload/cd.dmg.part\" \"CD Image.toast\""),
     .doc = "Decode one member of an archive straight into a compact UDIF (.dmg), extracting nothing",
     .method = {.result_doc = "{member, bytes_in, stored_bytes, sectors}",
                .ui_flags = MM_IO,
                .args = archive_import_args,
                .nargs = 4,
                .result = V_MAP,
                .fn = archive_method_import}},
};

static const class_desc_t archive_class = {
    .name = "archive",
    .members = archive_members,
    .n_members = sizeof(archive_members) / sizeof(archive_members[0]),
    .doc = "Archive formats (StuffIt, Compact Pro, Zip, tar, BinHex, MacBinary, gzip): identify and extract",
};

// ============================================================================
// Lifecycle (process-singleton, idempotent)
// ============================================================================

static struct object *s_archive_object = NULL;

void archive_init(struct object *parent) {
    if (s_archive_object)
        return;
    s_archive_object = object_new(&archive_class, NULL, "archive");
    if (s_archive_object) {
        object_set_label(s_archive_object, "Archives");
        object_set_order(s_archive_object, 30);
        object_attach(parent ? parent : object_root(), s_archive_object);
    }
}

void archive_delete(void) {
    if (s_archive_object) {
        object_detach(s_archive_object);
        object_delete(s_archive_object);
        s_archive_object = NULL;
    }
}
