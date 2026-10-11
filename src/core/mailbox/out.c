// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// out.c -- the output sink's router (out.h): a job's output to the
// job, a served request's output to its answer, everything else to fd 1.

#include "out.h"

#include "job/job.h"
#include "mailbox/mailbox.h"

#include <stdio.h>

void out_route(const char *text, size_t len) {
    if (!len)
        return;
    if (job_output_append(text, len))
        return;
    if (mailbox_output_append(text, len))
        return;
    fwrite(text, 1, len, stdout);
}
