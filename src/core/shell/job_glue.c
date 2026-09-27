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

// Whether a script's bare `scheduler.run` (no budget) holds the script
// until the machine stops.  It does: a script file, stdin and a daemon
// client mean "run until a breakpoint, an assertion or a stop, then go
// on" -- the integration suites are written that way.  The browser's
// terminal overrides this to false: a line typed there returns at once
// and the machine runs on, with Ctrl-C to stop it (job.h).
__attribute__((weak)) bool job_glue_unbounded_waits(uint32_t client) {
    (void)client;
    return true;
}

bool job_glue_mode_waits(uint32_t client) {
    scheduler_t *s = system_scheduler();
    if (!s || !scheduler_is_running(s) || scheduler_run_owner(s) != client)
        return false;
    return scheduler_mode_bounded(s) || job_glue_unbounded_waits(client);
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
