// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// peeler.c
// The buffer API: peel(), peel_path(), peel_detect() and the per-format
// archive functions, all conveniences over the structure-first API in
// archive.c, plus buffer/file-list lifecycle helpers.

#include "appledouble.h"
#include "internal.h"

#include <errno.h>

// ============================================================================
// Constants and Macros
// ============================================================================

// Maximum number of wrapper layers to peel before giving up (guards against
// degenerate or malicious inputs that detect as wrappers in a loop).
#define MAX_PEEL_DEPTH 32

// ============================================================================
// Wrapper Table — the buffer API's layer stripping
// ============================================================================

// A wrapper format's buffer transform: one layer in, its payload out.
typedef struct {
    const char *name;
    peel_buf_t (*peel_wrapper)(const uint8_t *src, size_t len, peel_err_t **err);
} wrapper_fn_t;

static const wrapper_fn_t g_wrappers[] = {
    {"hqx", peel_hqx},
    {"bin", peel_bin},
    {"gz",  peel_gz },
};

// ============================================================================
// Static Helpers
// ============================================================================

// Detect the format of a buffer through the bounded probe, as peel_open does.
static const peel_format_desc_t *detect_buffer(const uint8_t *src, size_t len) {
    peel_source_t *s = peel_source_memory(src, len, false);
    if (!s)
        return NULL;
    peel_probe_t p;
    const peel_format_desc_t *d = NULL;
    if (peel_probe_init(&p, s) == 0) {
        d = peel_identify(&p);
        peel_probe_free(&p);
    }
    peel_source_release(s);
    return d;
}

// The buffer transform of wrapper `name`.
static const wrapper_fn_t *wrapper_for(const char *name) {
    for (size_t i = 0; i < sizeof(g_wrappers) / sizeof(g_wrappers[0]); i++)
        if (strcmp(g_wrappers[i].name, name) == 0)
            return &g_wrappers[i];
    return NULL;
}

// Wrap a raw buffer as a single-file result with no metadata.
// If `owned_data` is non-NULL, ownership of that allocation is transferred
// into the result.  Otherwise the data at `src` is copied.
static peel_file_list_t wrap_single_file(const uint8_t *src, size_t len, uint8_t *owned_data, peel_err_t **err) {
    peel_file_t *files = calloc(1, sizeof(peel_file_t));
    if (!files) {
        *err = make_err("out of memory allocating single-file result");
        // Caller is responsible for freeing owned_data if we fail
        return (peel_file_list_t){0};
    }

    if (owned_data) {
        // Transfer ownership of the existing allocation
        files[0].data_fork = (peel_buf_t){.data = owned_data, .size = len, .owned = true};
    } else {
        // Copy the borrowed input into a fresh buffer
        files[0].data_fork = peel_buf_copy(src, len, err);
        if (*err) {
            free(files);
            return (peel_file_list_t){0};
        }
    }

    return (peel_file_list_t){.files = files, .count = 1};
}

// Extract every file of an archive through the structure-first API: open,
// then decode both forks of each entry that has a non-empty one.  Folders
// and empty files are not files of the result, as they never were.
static peel_file_list_t extract_archive(const char *format, const uint8_t *src, size_t len, peel_err_t **err) {
    *err = NULL;
    peel_source_t *s = peel_source_memory(src, len, false);
    if (!s) {
        *err = make_err("out of memory");
        return (peel_file_list_t){0};
    }
    peel_archive_t *a = peel_open_as(format, s, NULL, NULL, err);
    peel_source_release(s);
    if (!a)
        return (peel_file_list_t){0};

    int n = 0;
    for (int i = 0; i < peel_count(a); i++) {
        const peel_entry_t *e = peel_entry(a, i);
        if (!e->is_dir && (e->data_len > 0 || e->rsrc_len > 0))
            n++;
    }
    peel_file_list_t list = {0};
    if (n == 0) {
        peel_close(a);
        return list;
    }
    list.files = calloc((size_t)n, sizeof(peel_file_t));
    if (!list.files) {
        peel_close(a);
        *err = make_err("%s: out of memory for file list (%d files)", format, n);
        return (peel_file_list_t){0};
    }
    for (int i = 0; i < peel_count(a) && list.count < n; i++) {
        const peel_entry_t *e = peel_entry(a, i);
        if (e->is_dir || (e->data_len == 0 && e->rsrc_len == 0))
            continue;
        peel_file_t *f = &list.files[list.count++];
        size_t nl = strlen(e->path); // truncated to the file name's capacity, as it always was
        if (nl > sizeof(f->meta.name) - 1)
            nl = sizeof(f->meta.name) - 1;
        memcpy(f->meta.name, e->path, nl);
        f->meta.name[nl] = '\0';
        f->meta.mac_type = e->mac_type;
        f->meta.mac_creator = e->mac_creator;
        f->meta.finder_flags = e->finder_flags;
        f->data_fork = peel_read_fork(a, i, PEEL_FORK_DATA, err);
        if (!*err)
            f->resource_fork = peel_read_fork(a, i, PEEL_FORK_RSRC, err);
        if (*err) {
            peel_file_list_free(&list);
            peel_close(a);
            return (peel_file_list_t){0};
        }
    }
    peel_close(a);
    return list;
}

// ============================================================================
// Operations (Public API) — Format Detection
// ============================================================================

// Identify the outermost format without peeling.
const char *peel_detect(const uint8_t *src, size_t len) {
    const peel_format_desc_t *d = detect_buffer(src, len);
    return d ? d->name : NULL;
}

// ============================================================================
// Operations (Public API) — Per-Format Archive Sugar
// ============================================================================

peel_file_list_t peel_sit(const uint8_t *src, size_t len, peel_err_t **err) {
    return extract_archive("sit", src, len, err);
}

peel_file_list_t peel_cpt(const uint8_t *src, size_t len, peel_err_t **err) {
    return extract_archive("cpt", src, len, err);
}

peel_file_list_t peel_zip(const uint8_t *src, size_t len, peel_err_t **err) {
    return extract_archive("zip", src, len, err);
}

// ============================================================================
// Operations (Public API) — Main Entry Points
// ============================================================================

// Forward declaration for recursive peeling.
static peel_file_list_t peel_depth(const uint8_t *src, size_t len, int depth, peel_err_t **err);

// Recursively peel extracted files whose data forks contain recognized
// formats.  This handles archives-inside-archives (e.g. .sit containing
// a .sit.hqx file).
// architecture.md § "Recursive Peeling"
static peel_file_list_t recursive_peel_files(peel_file_list_t list, int depth, peel_err_t **err) {
    if (list.count == 0) {
        return list;
    }

    // Collect results in a new list.  Most files will pass through unchanged,
    // but some may expand into multiple files.
    int result_cap = list.count;
    int result_count = 0;
    peel_file_t *result = calloc((size_t)result_cap, sizeof(peel_file_t));
    if (!result) {
        *err = make_err("out of memory in recursive peel");
        peel_file_list_free(&list);
        return (peel_file_list_t){0};
    }

    for (int i = 0; i < list.count; i++) {
        peel_file_t *f = &list.files[i];

        // Check if this file's data fork is itself a recognized wrapper or
        // archive format.  Only peel further through WRAPPER formats to
        // avoid false positives on large binary files (e.g. disk images)
        // that may incidentally contain archive signatures.
        const peel_format_desc_t *fmt = NULL;
        if (f->data_fork.data && f->data_fork.size > 0) {
            fmt = detect_buffer(f->data_fork.data, f->data_fork.size);
            if (fmt && !fmt->is_wrapper) {
                fmt = NULL; // Only recurse through wrappers
            }
        }
        if (!fmt) {
            // Not a recognized format — keep as-is
            // Grow result if needed
            if (result_count >= result_cap) {
                result_cap = result_cap * 2;
                peel_file_t *tmp = realloc(result, (size_t)result_cap * sizeof(peel_file_t));
                if (!tmp) {
                    *err = make_err("out of memory growing recursive peel list");
                    goto fail;
                }
                result = tmp;
            }
            result[result_count++] = *f;
            // Clear the source so peel_file_list_free won't double-free
            memset(f, 0, sizeof(*f));
            continue;
        }

        // Recursively peel this file's data fork
        peel_err_t *sub_err = NULL;
        peel_file_list_t sub = peel_depth(f->data_fork.data, f->data_fork.size, depth + 1, &sub_err);
        if (sub_err) {
            // Recursive peel failed — keep the original file as-is
            peel_err_free(sub_err);
            if (result_count >= result_cap) {
                result_cap = result_cap * 2;
                peel_file_t *tmp = realloc(result, (size_t)result_cap * sizeof(peel_file_t));
                if (!tmp) {
                    *err = make_err("out of memory growing recursive peel list");
                    goto fail;
                }
                result = tmp;
            }
            result[result_count++] = *f;
            memset(f, 0, sizeof(*f));
            continue;
        }

        // Replace this file with the sub-results
        int need = result_count + sub.count;
        if (need > result_cap) {
            result_cap = need * 2;
            peel_file_t *tmp = realloc(result, (size_t)result_cap * sizeof(peel_file_t));
            if (!tmp) {
                *err = make_err("out of memory growing recursive peel list");
                peel_file_list_free(&sub);
                goto fail;
            }
            result = tmp;
        }
        for (int j = 0; j < sub.count; j++) {
            result[result_count++] = sub.files[j];
        }
        // Free the sub-list array (but not the individual file buffers,
        // which have been moved into result)
        free(sub.files);

        // Free the original file's buffers (replaced by sub-results)
        peel_free(&f->data_fork);
        peel_free(&f->resource_fork);
    }

    // Free the original list's file array (individual entries already freed/moved)
    free(list.files);
    return (peel_file_list_t){.files = result, .count = result_count};

fail:
    // Clean up partial results and original list
    for (int j = 0; j < result_count; j++) {
        peel_free(&result[j].data_fork);
        peel_free(&result[j].resource_fork);
    }
    free(result);
    peel_file_list_free(&list);
    return (peel_file_list_t){0};
}

// Detect all layers, peel wrappers, then extract the archive.
// architecture.md § "peel Implementation Sketch"
peel_file_list_t peel(const uint8_t *src, size_t len, peel_err_t **err) {
    return peel_depth(src, len, 0, err);
}

// Internal implementation with depth tracking for recursion limiting.
static peel_file_list_t peel_depth(const uint8_t *src, size_t len, int depth, peel_err_t **err) {
    *err = NULL;

    if (depth >= MAX_PEEL_DEPTH) {
        // Recursion limit reached — return data as a single unnamed file
        return wrap_single_file(src, len, NULL, err);
    }

    // `owned` holds the most recent intermediate buffer (heap-allocated by a
    // wrapper peeler).  NULL while we are still working from the caller's
    // original input pointer.
    uint8_t *owned = NULL;
    const uint8_t *cur = src;
    size_t cur_len = len;

    // Repeatedly strip wrapper layers until an archive or unknown data is found.
    for (int wrap_depth = 0; wrap_depth < MAX_PEEL_DEPTH; wrap_depth++) {
        const peel_format_desc_t *fmt = detect_buffer(cur, cur_len);
        if (!fmt) {
            break; // Nothing recognised — fall through to single-file wrap
        }

        if (fmt->is_wrapper) {
            // Peel one wrapper layer and replace the working buffer
            const wrapper_fn_t *w = wrapper_for(fmt->name);
            peel_buf_t decoded = w->peel_wrapper(cur, cur_len, err);
            if (*err) {
                free(owned);
                return (peel_file_list_t){0};
            }
            free(owned); // Release previous intermediate (NULL-safe)
            owned = decoded.data;
            cur = owned;
            cur_len = decoded.size;
            continue;
        }

        // Terminal format — extract files and return
        peel_file_list_t result = extract_archive(fmt->name, cur, cur_len, err);
        free(owned);
        if (*err) {
            return (peel_file_list_t){0};
        }
        // Recursively peel extracted files that contain nested archives
        return recursive_peel_files(result, depth, err);
    }

    // No archive found.  Wrap whatever we have as a single unnamed file.
    // Transfer ownership of `owned` if we peeled any wrappers.
    peel_file_list_t result = wrap_single_file(cur, cur_len, owned, err);
    if (*err) {
        // wrap_single_file failed; it did NOT take ownership on failure
        free(owned);
        return (peel_file_list_t){0};
    }
    // `owned` pointer is now inside the result — do not free it
    return result;
}

// Read a file from disk, then peel() its contents.
peel_file_list_t peel_path(const char *path, peel_err_t **err) {
    *err = NULL;

    // Slurp the entire file into memory
    peel_buf_t file_buf = peel_read_file(path, err);
    if (*err) {
        return (peel_file_list_t){0};
    }

    // Run the main peeling loop
    peel_file_list_t result = peel(file_buf.data, file_buf.size, err);

    // Release the input buffer regardless of success
    peel_free(&file_buf);
    return result;
}

// ============================================================================
// Operations (Public API) — Buffer Lifecycle
// ============================================================================

// Free the data inside a buffer (if owned) and zero the struct.
void peel_free(peel_buf_t *buf) {
    if (!buf) {
        return;
    }
    if (buf->owned) {
        free(buf->data);
    }
    memset(buf, 0, sizeof(*buf));
}

// Free every fork buffer in every file, then the file array itself.
void peel_file_list_free(peel_file_list_t *list) {
    if (!list) {
        return;
    }
    for (int i = 0; i < list->count; i++) {
        peel_free(&list->files[i].data_fork);
        peel_free(&list->files[i].resource_fork);
    }
    free(list->files);
    memset(list, 0, sizeof(*list));
}

// ============================================================================
// Operations (Public API) — Input Helpers
// ============================================================================

// Read an entire file into an owned buffer.
peel_buf_t peel_read_file(const char *path, peel_err_t **err) {
    *err = NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        *err = make_err("cannot open '%s': %s", path, strerror(errno));
        return (peel_buf_t){0};
    }

    // Determine file size by seeking to the end
    if (fseek(fp, 0, SEEK_END) != 0) {
        *err = make_err("cannot seek in '%s': %s", path, strerror(errno));
        fclose(fp);
        return (peel_buf_t){0};
    }

    long raw_size = ftell(fp);
    if (raw_size < 0) {
        *err = make_err("cannot determine size of '%s': %s", path, strerror(errno));
        fclose(fp);
        return (peel_buf_t){0};
    }
    // Bounded like every size an archive declares: this
    // loads the whole file, and files.archive.identify calls it on every probe.
    if ((uint64_t)raw_size > PEEL_MAX_INPUT) {
        *err = make_err("'%s' is %ld bytes, over the %u MiB limit", path, raw_size,
                        (unsigned)(PEEL_MAX_INPUT >> 20));
        fclose(fp);
        return (peel_buf_t){0};
    }
    size_t size = (size_t)raw_size;

    // Rewind and read the entire file
    rewind(fp);

    uint8_t *data = malloc(size);
    if (!data) {
        *err = make_err("out of memory reading '%s' (%zu bytes)", path, size);
        fclose(fp);
        return (peel_buf_t){0};
    }

    size_t nread = fread(data, 1, size, fp);
    fclose(fp);

    if (nread != size) {
        *err = make_err("short read on '%s': expected %zu bytes, got %zu", path, size, nread);
        free(data);
        return (peel_buf_t){0};
    }

    return (peel_buf_t){.data = data, .size = size, .owned = true};
}

// Copy caller-supplied bytes into a new owned buffer.
peel_buf_t peel_buf_copy(const void *src, size_t len, peel_err_t **err) {
    *err = NULL;

    if (len == 0 || !src) {
        return (peel_buf_t){0};
    }

    uint8_t *data = malloc(len);
    if (!data) {
        *err = make_err("out of memory (%zu bytes)", len);
        return (peel_buf_t){0};
    }
    memcpy(data, src, len);

    return (peel_buf_t){.data = data, .size = len, .owned = true};
}

// Create a non-owning view of caller data.  peel_free() on this buffer
// will not free the data pointer.
peel_buf_t peel_buf_wrap(const void *src, size_t len) {
    return (peel_buf_t){
        .data = (uint8_t *)(uintptr_t)src, // Cast away const — caller guarantees lifetime
        .size = len,
        .owned = false,
    };
}

int peel_build_sidecar(const peel_file_t *f, uint8_t **out, size_t *out_len) {
    if (!f || !out || !out_len)
        return -EINVAL;
    *out = NULL;
    *out_len = 0;
    // Finder info (FInfo + FXInfo): type, creator and flags big-endian at
    // the front, the rest zero.  Included only when something is set.
    uint8_t finder[AD_FINDER_INFO_SIZE] = {0};
    wr32be(finder, f->meta.mac_type);
    wr32be(finder + 4, f->meta.mac_creator);
    wr16be(finder + 8, f->meta.finder_flags);
    bool has_finder = f->meta.mac_type || f->meta.mac_creator || f->meta.finder_flags;
    if (!has_finder && f->resource_fork.size == 0)
        return 0; // a data-only file: nothing to preserve
    return ad_build_sidecar(f->resource_fork.data, f->resource_fork.size, has_finder ? finder : NULL, out, out_len);
}
