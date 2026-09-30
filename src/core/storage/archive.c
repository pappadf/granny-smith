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

#include "log.h"
#include "object.h"
#include "peeler.h"
#include "shell.h"
#include "source.h"
#include "storage_util.h"
#include "value.h"
#include "vfs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
// Object-model class descriptor
// ============================================================================

// `files.archive.identify(path)` — return the format short name for a recognised
// archive ("sit" / "cpt" / "zip" / "hqx" / "bin" / "gz"), or empty string
// when the file is unreadable or not an archive. Empty is falsy under
// the predicate-truthy rule — same shape as floppy.identify.
static value_t archive_method_identify(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    (void)argc;
    const char *format = archive_identify_file(argv[0].s);
    return val_str(format ? format : "");
}

// `files.archive.extract(path, [out_dir])` — extract a Mac archive into out_dir
// (defaults to the current working directory). Returns true on success.
static value_t archive_method_extract(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = argv[0].s;
    const char *out_dir = (argc >= 2 && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    io_leaf_t *j = io_leaf_new(path, out_dir);
    if (!j)
        return val_err("files.archive.extract: out of memory");
    j->work = work_extract;
    return io_leaf_dispatch(j, "files.archive.extract");
}

static const arg_decl_t archive_path_arg[] = {
    {.name = "path", .kind = V_STRING, .presentation_flags = VAL_PATH, .doc = "Archive file path"},
};

static const arg_decl_t archive_extract_args[] = {
    {.name = "path", .kind = V_STRING, .presentation_flags = VAL_PATH, .doc = "Archive file path"},
    {.name = "out_dir",
     .kind = V_STRING,
     .presentation_flags = VAL_PATH,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Output directory; omitted: the current directory"},
};

static const member_t archive_members[] = {
    {.kind = M_METHOD,
     .name = "identify",
     .examples = (const char *const[]){"files.archive.identify \"/opfs/downloads/app.sit\"", NULL},
     .doc = "Identify a Mac archive's format",
     .method = {.result_doc = "\"sit\", \"cpt\", \"zip\", \"hqx\", \"bin\" or \"gz\"; empty when not an archive",
                .args = archive_path_arg,
                .nargs = 1,
                .result = V_STRING,
                .fn = archive_method_identify}                                                                        },
    {.kind = M_METHOD,
     .name = "extract",
     .examples = (const char *const[]){"files.archive.extract \"/opfs/downloads/app.sit\"",
                                       "files.archive.extract \"/opfs/downloads/app.sit\" \"/opfs/unpacked\"", NULL},
     .doc = "Extract a Mac archive into out_dir",
     .method =
         {.ui_flags = MM_IO, .args = archive_extract_args, .nargs = 2, .result = V_BOOL, .fn = archive_method_extract}},
};

static const class_desc_t archive_class = {
    .name = "archive",
    .members = archive_members,
    .n_members = sizeof(archive_members) / sizeof(archive_members[0]),
    .doc = "Archive formats (StuffIt, Compact Pro, Zip, BinHex, MacBinary, gzip): identify and extract",
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
