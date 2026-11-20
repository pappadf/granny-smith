// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// display.c
// The checkpoint block of a display descriptor's head (display.h), shared by
// every producer that saves one, so the fixed-width form is written once.

#include "display.h"

#include "checkpoint.h"

void display_head_save(checkpoint_t *cp, const display_t *d) {
    // Fixed widths, not a raw struct prefix: the prefix carried a bare
    // pixel_format_t, whose size is implementation-defined (see display.h).
    display_head_t head = display_head_of(d);
    system_write_checkpoint_data(cp, &head, sizeof head, "display.head");
}

void display_head_restore(checkpoint_t *cp, display_t *d) {
    display_head_t head;
    system_read_checkpoint_data(cp, &head, sizeof head, "display.head");
    display_head_apply(d, &head);
}
