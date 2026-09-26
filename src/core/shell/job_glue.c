// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// job_glue.c -- what job.c needs from the shell and the scheduler (job.h,
// "Glue").  Kept apart so job.c links into a unit suite with stubs.

#include "job/job.h"

#include "scheduler.h"
#include "script.h"
#include "system.h"

int job_glue_run_source(const char *src, bool interactive) {
    return script_run_text(src, interactive);
}

bool job_glue_mode_waits(uint32_t client) {
    scheduler_t *s = system_scheduler();
    return s && scheduler_is_running(s) && scheduler_run_owner(s) == client && scheduler_mode_bounded(s);
}

uint32_t job_glue_mode_id(void) {
    return scheduler_mode_id(system_scheduler());
}

bool job_glue_stop_modes(uint32_t client) {
    scheduler_t *s = system_scheduler();
    return s && scheduler_stop_owned(s, client);
}

bool job_glue_stop_mode(uint32_t client, uint32_t mode_id) {
    scheduler_t *s = system_scheduler();
    if (!s || scheduler_mode_id(s) != mode_id)
        return false;
    return scheduler_stop_owned(s, client);
}
