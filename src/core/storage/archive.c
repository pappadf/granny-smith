// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// archive.c
// Mac archive file handling. Wraps the third-party peeler library so the
// emulator-side surface (archive.identify, archive.extract) doesn't
// leak the library name to users.

#include "archive.h"

#include "appledouble.h"
#include "log.h"
#include "object.h"
#include "peeler.h"
#include "storage_util.h"
#include "value.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// Extraction helpers
// ============================================================================

typedef struct {
    const char *output_dir;
    int file_count;
} archive_ctx_t;

// Create the directories leading to entry `path` under ctx->output_dir.
static int ensure_dir_exists(const archive_ctx_t *ctx, const char *path) {
    char *full = gs_str_printf("%s/%s", ctx->output_dir, path);
    if (!full)
        return -1;
    int rc = gs_mkdir_parents(full);
    if (rc != 0)
        fprintf(stderr, "archive: cannot create the directories for '%s': %s\n", full, strerror(-rc));
    free(full);
    return rc != 0 ? -1 : 0;
}

// Synthesize the 32-byte Finder Info block (FInfo + FXInfo) from peeler's
// best-effort metadata — type, creator, Finder flags (big-endian), rest zero.
// Returns true if any field was set (i.e. worth persisting).
static bool build_finder_info(const peel_file_meta_t *m, uint8_t out[32]) {
    memset(out, 0, 32);
    out[0] = (uint8_t)(m->mac_type >> 24);
    out[1] = (uint8_t)(m->mac_type >> 16);
    out[2] = (uint8_t)(m->mac_type >> 8);
    out[3] = (uint8_t)m->mac_type;
    out[4] = (uint8_t)(m->mac_creator >> 24);
    out[5] = (uint8_t)(m->mac_creator >> 16);
    out[6] = (uint8_t)(m->mac_creator >> 8);
    out[7] = (uint8_t)m->mac_creator;
    out[8] = (uint8_t)(m->finder_flags >> 8);
    out[9] = (uint8_t)m->finder_flags;
    return m->mac_type || m->mac_creator || m->finder_flags;
}

// Write an AppleDouble "._<name>" header sidecar next to the extracted data
// file at `data_full_path`, carrying the resource fork (entry 2) and Finder
// Info (entry 9).  This keeps a Mac file lossless on the flat host FS — e.g. a
// StuffIt/MacBinary-wrapped NDIF disk image unpacks to a mountable pair — and
// interoperates with macOS/Netatalk (proposal-appledouble-support.md §Phase 3).
// A file with neither a resource fork nor Finder Info gets no sidecar.
// Returns 0 on success (including the no-sidecar case), -1 on write failure.
static int write_ad_sidecar(const char *data_full_path, const peel_file_t *file) {
    uint8_t finder[32];
    bool finder_set = build_finder_info(&file->meta, finder);
    if (file->resource_fork.size == 0 && !finder_set)
        return 0; // data-only file: nothing to preserve

    uint8_t *hdr = NULL;
    size_t hdr_len = 0;
    if (ad_build_sidecar(file->resource_fork.data, file->resource_fork.size, finder_set ? finder : NULL, &hdr,
                         &hdr_len) != 0)
        return 0;

    char sidecar[1024];
    const char *slash = strrchr(data_full_path, '/');
    int n = slash ? snprintf(sidecar, sizeof(sidecar), "%.*s._%s", (int)(slash - data_full_path + 1), data_full_path,
                             slash + 1)
                  : snprintf(sidecar, sizeof(sidecar), "._%s", data_full_path);
    if (n < 0 || n >= (int)sizeof(sidecar)) {
        free(hdr);
        fprintf(stderr, "archive: sidecar path too long\n");
        return -1;
    }

    FILE *fp = fopen(sidecar, "wb");
    if (!fp) {
        free(hdr);
        fprintf(stderr, "archive: cannot create '%s': %s\n", sidecar, strerror(errno));
        return -1;
    }
    size_t written = fwrite(hdr, 1, hdr_len, fp);
    int close_rc = fclose(fp);
    free(hdr);
    if (written != hdr_len || close_rc != 0) {
        remove(sidecar);
        fprintf(stderr, "archive: write error on sidecar '%s'\n", sidecar);
        return -1;
    }
    return 0;
}

// Write a single extracted file to disk under ctx->output_dir: the data fork
// under its name, and — when the file carries a resource fork and/or Finder
// Info — an AppleDouble "._<name>" sidecar beside it so the fork is preserved
// (see write_ad_sidecar).
static int write_extracted_file(const archive_ctx_t *ctx, const peel_file_t *file) {
    const char *name = file->meta.name;
    if (!name[0])
        name = "untitled";

    // The entry name comes from the archive.  Unchecked, "../x" -- or a Mac
    // name containing '/', legal on HFS -- was written outside output_dir,
    // with the directories created on the way (09-storage F-13).  peeler now
    // builds names that cannot do this; this is the boundary, so check anyway.
    if (!peel_path_is_confined(name)) {
        fprintf(stderr, "archive: refusing entry '%s': it would land outside '%s'\n", name, ctx->output_dir);
        return -1;
    }

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

    // Preserve the resource fork + Finder Info as a sibling AppleDouble sidecar.
    return write_ad_sidecar(full_path, file);
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
        .file_count = 0,
    };
    if (gs_mkdir_p(ctx.output_dir) != 0) {
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
    (void)argc;
    const char *format = archive_identify_file(argv[0].s);
    return val_str(format ? format : "");
}

// `archive.extract(path, [out_dir])` — extract a Mac archive into out_dir
// (defaults to the current working directory). Returns true on success.
static value_t archive_method_extract(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    const char *path = argv[0].s;
    const char *out_dir = (argc >= 2 && argv[1].s && *argv[1].s) ? argv[1].s : NULL;
    return val_bool(archive_extract_file(path, out_dir) == 0);
}

static const arg_decl_t archive_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "Archive file path"},
};

static const arg_decl_t archive_extract_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Archive file path"},
    {.name = "out_dir",
     .kind = V_STRING,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "Output directory (default: cwd)"},
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

static const class_desc_t archive_class = {
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
