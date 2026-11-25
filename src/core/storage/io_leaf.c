// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// io_leaf.c -- see io_leaf.h.

#include "io_leaf.h"

#include "image.h"
#include "value_format.h"
#include "io/io_worker.h"
#include "job/job.h"
#include "mailbox/mailbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

io_leaf_t *io_leaf_new(const char *a, const char *b) {
    io_leaf_t *j = (io_leaf_t *)calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->a = a ? strdup(a) : NULL;
    j->b = b ? strdup(b) : NULL;
    return j;
}

static void io_leaf_free(io_leaf_t *j) {
    if (j->cleanup)
        j->cleanup(j);
    free(j->a);
    free(j->b);
    free(j);
}

bool io_leaf_destination_attached(const char *dst) {
    return dst && image_path_is_open_writable(dst);
}

// The work, on either thread.  Its reason is always in j->err; the worker's
// own buffer (`err`) gets a copy.  io_leaf_dispatch's inline runs pass no
// buffer: copying j->err onto itself (an overlapping snprintf) emptied it,
// and the reason was lost to "failed".
static int io_leaf_run(void *ud, char *err, size_t cap) {
    io_leaf_t *j = (io_leaf_t *)ud;
    int rc = j->work(j);
    if (rc != 0 && !j->err[0])
        snprintf(j->err, sizeof j->err, "failed"); // a work that gave no reason
    if (rc != 0 && err && err != j->err)
        snprintf(err, cap, "%s", j->err);
    return rc;
}

// The success value as the JSON the answer carries: a string, a number, a
// boolean, a map or a list; anything else is `true`.  A map or a list is
// formatted as object_eval formats one; it and a string are held to the same
// result limit.
static void complete_with(uint32_t token, value_t *v) {
    // A script's call through the job seam takes the value itself.
    if (job_call_complete_value(token, v))
        return;
    if (v->kind == VK_MAP || v->kind == VK_LIST || (v->kind == VK_STRING && v->s)) {
        vbuf_t b = {0};
        if (v->kind == VK_STRING)
            vbuf_append_json_string(&b, v->s);
        else
            value_format(v, VFMT_JSON_TAGGED, &b);
        if (b.p && b.len < GS_MBX_RESULT_MAX) // the result's limit (mailbox.h), its NUL aside
            mailbox_result_complete(token, true, b.p);
        else {
            char err[128];
            snprintf(err, sizeof err, "result is %zu bytes, over the %u-byte result limit", b.len,
                     (unsigned)GS_MBX_RESULT_MAX);
            mailbox_result_complete_error(token, err);
        }
        vbuf_free(&b);
        return;
    }
    char json[64];
    if (v->kind == VK_UINT) {
        snprintf(json, sizeof json, "%llu", (unsigned long long)v->u);
    } else if (v->kind == VK_INT) {
        snprintf(json, sizeof json, "%lld", (long long)v->i);
    } else if (v->kind == VK_BOOL) {
        snprintf(json, sizeof json, "%s", v->b ? "true" : "false");
    } else {
        snprintf(json, sizeof json, "true");
    }
    mailbox_result_complete(token, true, json);
}

static void io_leaf_done(bool ok, double ms, const char *error, void *ud) {
    (void)ms;
    io_leaf_t *j = (io_leaf_t *)ud;
    if (ok) {
        value_t v = j->answer ? j->answer(j) : val_bool(true);
        complete_with(j->token, &v);
        value_free(&v);
    } else {
        mailbox_result_complete_error(j->token, error ? error : j->err);
    }
    io_leaf_free(j);
}

static void io_leaf_progress(uint64_t done, uint64_t total, void *ud) {
    io_leaf_t *j = (io_leaf_t *)ud;
    if (j->token)
        mailbox_result_progress(j->token, done, total);
}

value_t io_leaf_dispatch(io_leaf_t *j) {
    j->token = mailbox_result_defer();
    io_job_desc_t d = {
        .work = io_leaf_run,
        .work_ud = j,
        .done = io_leaf_done,
        .done_ud = j,
        .progress = io_leaf_progress,
        .observer_ud = j,
    };
    if (j->token) {
        // Provisional, of the declared kind (what the leaf returns now;
        // the completion carries the real answer or the error, to a
        // request and to a script's call alike).
        value_t provisional = j->answer ? j->answer(j) : val_bool(true);
        j->io_id = io_submit_job(&d);
        if (j->io_id) {
            mailbox_result_bind_io(j->token, j->io_id);
            return provisional;
        }
        // Deferred but no worker: work now and answer the deferral at once.
        int rc = io_run_inline(&d, NULL, 0);
        io_leaf_done(rc == 0, 0.0, rc == 0 ? NULL : j->err, j);
        return provisional;
    }
    // Nothing that can wait: the work runs now, and its failure reads as a
    // deferred one's does (the worker's reason, as is), so a script says the
    // same whether its call could suspend or not, with or without a worker.
    int rc = io_run_inline(&d, NULL, 0);
    value_t v = rc == 0 ? (j->answer ? j->answer(j) : val_bool(true)) : val_err("%s", j->err);
    io_leaf_free(j);
    return v;
}

// --- image export --------------------------------------------------------------

static int work_export_image(io_leaf_t *j) {
    return image_export_run((image_export_t *)j->ud, j->err, sizeof j->err);
}

static void cleanup_export_image(io_leaf_t *j) {
    image_export_end((image_export_t *)j->ud);
}

value_t io_leaf_export_image(struct image *img, const char *dest, const char *codec, const char *what) {
    if (io_leaf_destination_attached(dest))
        return val_err("%s: '%s' is attached to a device (E_BUSY)", what, dest);
    char err[256];
    image_export_t *e = image_export_begin(img, dest, err, sizeof err);
    if (!e)
        return val_err("%s: %s", what, err[0] ? err : "cannot export");
    if (image_export_set_codec(e, codec, err, sizeof err) != 0) {
        image_export_end(e);
        return val_err("%s: %s", what, err);
    }
    io_leaf_t *j = io_leaf_new(dest, NULL);
    if (!j) {
        image_export_end(e);
        return val_err("%s: out of memory", what);
    }
    j->ud = e;
    j->work = work_export_image;
    j->cleanup = cleanup_export_image;
    return io_leaf_dispatch(j);
}
