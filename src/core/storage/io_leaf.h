// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// io_leaf.h -- a leaf whose work is proportional to a file: an I/O job.
//
// A copy, an export, a blank image, an archive's extraction cost the size
// of a file, not of the machine, so they run on the I/O worker
// (io/io_worker.h) and the leaf answers later (gs_result_defer /
// gs_result_complete, mailbox.h): the page's promise or the script's call
// settles when the work ends, progress reaches the client as EVT_PROGRESS
// while it runs, and a REQ_CANCEL of the request (or of the script) stops
// the work at its next chunk.  Without a worker, or when nothing is being
// served that can wait, the same work runs here and now with the same
// hooks bound.  meta.method_info reports such a method with `io: true`
// (MM_IO, object.h).

#ifndef GS_IO_LEAF_H
#define GS_IO_LEAF_H

#include "object/value.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct io_leaf io_leaf_t;
struct io_leaf {
    uint32_t token; // the deferral (0: answering now)
    uint32_t io_id; // the worker job (0: inline)
    char *a, *b; // paths
    bool flag;
    char size_str[64];
    uint32_t blocks;
    char err[256];
    void *ud; // the leaf's own
    int (*work)(io_leaf_t *j); // the work, on either thread: 0 or -errno, err filled
    value_t (*answer)(io_leaf_t *j); // the success value (NULL: true)
    void (*cleanup)(io_leaf_t *j); // optional: frees `ud`
};

// A job with the two paths copied (either may be NULL); NULL when out of
// memory.
io_leaf_t *io_leaf_new(const char *a, const char *b);

// Runs the leaf's work as an I/O job when it can answer later, else now.
// Consumes `j`.  `what` names the leaf in an error.
value_t io_leaf_dispatch(io_leaf_t *j, const char *what);

// A destination under a device is never written over (E_BUSY): the guest
// is using it.  (image_vfs.c applies the same predicate to descents.)
bool io_leaf_destination_attached(const char *dst);

// The export of a device's live image (base + delta) to a new file as an
// I/O job (image_export_begin/run/end, image.h); `what` names the leaf.
struct image;
value_t io_leaf_export_image(struct image *img, const char *dest, const char *what);

#endif // GS_IO_LEAF_H
