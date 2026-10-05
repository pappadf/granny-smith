// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// drive_activity.c
// See drive_activity.h.

#include "drive_activity.h"

unsigned drive_activity_update(drive_activity_t *a, const uint64_t reads[DRIVE_KIND_COUNT],
                               const uint64_t writes[DRIVE_KIND_COUNT], double now_ms) {
    unsigned changed = 0;
    for (int k = 0; k < DRIVE_KIND_COUNT; k++) {
        bool rd = a->primed && reads[k] > a->reads[k];
        bool wr = a->primed && writes[k] > a->writes[k];
        a->reads[k] = reads[k];
        a->writes[k] = writes[k];
        if (wr)
            a->write_until_ms[k] = now_ms + DRIVE_LIGHT_MIN_MS;
        if (wr || rd)
            a->lit_until_ms[k] = now_ms + DRIVE_LIGHT_MIN_MS;
        // A write shows as "write" for its minimum time even while reads
        // follow; then the reads show.
        drive_light_t next = now_ms < a->write_until_ms[k] ? DRIVE_LIGHT_WRITE
                             : now_ms < a->lit_until_ms[k] ? DRIVE_LIGHT_READ
                                                           : DRIVE_LIGHT_IDLE;
        if (next != a->light[k]) {
            a->light[k] = next;
            changed |= 1u << k;
        }
    }
    a->primed = true;
    return changed;
}
