// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// archive.c
// Mac archive file handling. Wraps the third-party peeler library so the
// emulator-side surface (archive.identify, archive.extract) doesn't leak
// the library name to users. Moved here from the legacy peeler_shell.c.

#include "archive.h"

#include "log.h"
#include "object.h"
#include "peeler.h"
#include "value.h"

#include <errno.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// Extraction helpers (lifted from src/core/shell/peeler_shell.c)
// ============================================================================

typedef struct {
    const char *output_dir;
    int verbose;
    int file_count;
} archive_ctx_t;

// Recursively create the directory chain leading to `path` under
// ctx->output_dir. Last component is treated as a directory.
static int ensure_dir_exists(const archive_ctx_t *ctx, const char *path) {
    char *path_copy = strdup(path);
    if (!path_copy)
        return -1;

    char *dir = dirname(path_copy);
    char full_path[1024];

    if (snprintf(full_path, sizeof(full_path), "%s/%s", ctx->output_dir, dir) >= (int)sizeof(full_path)) {
        fprintf(stderr, "archive: path too long\n");
        free(path_copy);
        return -1;
    }
    free(path_copy);

    char *p = full_path;
    if (*p == '/')
        p++;

    while ((p = strchr(p, '/'))) {
        *p = '\0';
        if (mkdir(full_path, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "archive: cannot create directory '%s': %s\n", full_path, strerror(errno));
            *p = '/';
            return -1;
        }
        *p = '/';
        p++;
    }

    if (mkdir(full_path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "archive: cannot create directory '%s': %s\n", full_path, strerror(errno));
        return -1;
    }

    return 0;
}

// Write a single extracted file's data fork to disk under ctx->output_dir.
// Resource forks are deliberately discarded — the emulator's MEMFS doesn't
// model them, and the only callers want the data fork (disk images,
// system files unpacked from .sit / .hqx).
static int write_extracted_file(const archive_ctx_t *ctx, const peel_file_t *file) {
    const char *name = file->meta.name;
    if (!name[0])
        name = "untitled";

    if (ctx->verbose)
        printf("Extracting: %s\n", name);

    if (ensure_dir_exists(ctx, name) != 0)
        return -1;

    char full_path[1024];
    if (snprintf(full_path, sizeof(full_path), "%s/%s", ctx->output_dir, name) >= (int)sizeof(full_path)) {
        fprintf(stderr, "archive: path too long\n");
        return -1;
    }

    FILE *fp = fopen(full_path, "wb");
    if (!fp) {
        fprintf(stderr, "archive: cannot create file '%s': %s\n", full_path, strerror(errno));
        return -1;
    }

    if (file->data_fork.size > 0) {
        size_t written = fwrite(file->data_fork.data, 1, file->data_fork.size, fp);
        if (written != file->data_fork.size) {
            fprintf(stderr, "archive: write error: %s\n", strerror(errno));
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    return 0;
}

static int process_archive(archive_ctx_t *ctx, const char *filepath) {
    peel_err_t *err = NULL;
    peel_file_list_t list = peel_path(filepath, &err);

    if (err) {
        fprintf(stderr, "archive: failed to extract '%s': %s\n", filepath, peel_err_msg(err));
        peel_err_free(err);
        return -1;
    }

    if (list.count == 0) {
        fprintf(stderr, "archive: no files extracted from '%s'\n", filepath);
        peel_file_list_free(&list);
        return -1;
    }

    int status = 0;
    for (int i = 0; i < list.count; i++) {
        if (write_extracted_file(ctx, &list.files[i]) != 0) {
            status = -1;
            break;
        }
    }

    int count = list.count;
    peel_file_list_free(&list);

    if (status == 0)
        ctx->file_count += count;

    return status;
}

// ============================================================================
// Public API
// ============================================================================

const char *archive_identify_file(const char *path) {
    if (!path || !*path)
        return NULL;
    peel_err_t *err = NULL;
    peel_buf_t buf = peel_read_file(path, &err);
    if (err) {
        peel_err_free(err);
        return NULL;
    }
    const char *format = peel_detect(buf.data, buf.size);
    peel_free(&buf);
    return format;
}

int archive_extract_file(const char *path, const char *out_dir) {
    if (!path)
        return -1;
    archive_ctx_t ctx = {
        .output_dir = (out_dir && *out_dir) ? out_dir : ".",
        .verbose = 0,
        .file_count = 0,
    };
    if (mkdir(ctx.output_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "archive: cannot create output directory '%s': %s\n", ctx.output_dir, strerror(errno));
        return -1;
    }
    int rc = process_archive(&ctx, path);
    if (rc == 0)
        printf("Successfully extracted '%s' (%d file%s)\n", path, ctx.file_count, ctx.file_count == 1 ? "" : "s");
    return rc;
}

// ============================================================================
// Object-model class descriptor
// ============================================================================

// `archive.identify(path)` — return the format short name for a recognised
// Mac archive ("sit" / "cpt" / "hqx" / "bin" / "sea"), or empty string
// when the file is unreadable or not an archive. Empty is falsy under
// the predicate-truthy rule — same shape as floppy.identify.
static value_t archive_method_identify(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("archive.identify: expected (path)");
    const char *format = archive_identify_file(argv[0].s);
    return val_str(format ? format : "");
}

// `archive.extract(path, [out_dir])` — extract a Mac archive into out_dir
// (defaults to the current working directory). Returns true on success.
static value_t archive_method_extract(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("archive.extract: expected (path, [out_dir])");
    const char *path = argv[0].s;
    const char *out_dir = (argc >= 2 && argv[1].kind == V_STRING && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    return val_bool(archive_extract_file(path, out_dir) == 0);
}

static const arg_decl_t archive_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "Archive file path"},
};

static const arg_decl_t archive_extract_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Archive file path"},
    {.name = "out_dir", .kind = V_STRING, .flags = OBJ_ARG_OPTIONAL, .doc = "Output directory (default: cwd)"},
};

static const member_t archive_members[] = {
    {.kind = M_METHOD,
     .name = "identify",
     .doc = "Return the archive format (\"sit\" / \"cpt\" / \"hqx\" / \"bin\" / \"sea\") or empty if not an archive",
     .method = {.args = archive_path_arg, .nargs = 1, .result = V_STRING, .fn = archive_method_identify} },
    {.kind = M_METHOD,
     .name = "extract",
     .doc = "Extract a Mac archive into out_dir",
     .method = {.args = archive_extract_args, .nargs = 2, .result = V_BOOL, .fn = archive_method_extract}},
};

const class_desc_t archive_class = {
    .name = "archive",
    .members = archive_members,
    .n_members = sizeof(archive_members) / sizeof(archive_members[0]),
};

// ============================================================================
// Lifecycle (process-singleton, idempotent)
// ============================================================================

static struct object *s_archive_object = NULL;

void archive_init(void) {
    if (s_archive_object)
        return;
    s_archive_object = object_new(&archive_class, NULL, "archive");
    if (s_archive_object)
        object_attach(object_root(), s_archive_object);
}

void archive_delete(void) {
    if (s_archive_object) {
        object_detach(s_archive_object);
        object_delete(s_archive_object);
        s_archive_object = NULL;
    }
}
