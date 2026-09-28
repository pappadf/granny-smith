// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gs_out.c -- the output sink's router (gs_out.h): a job's output to the
// job, a served request's output to its answer, everything else to fd 1.

#include "gs_out.h"

#include "job/job.h"
#include "mailbox/mailbox.h"

void gs_out_route(const char *text, size_t len) {
    if (!len)
        return;
    if (job_output_append(text, len))
        return;
    if (gs_mailbox_output_append(text, len))
        return;
    fwrite(text, 1, len, stdout);
}
