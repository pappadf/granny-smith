// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Local stubs for the isolated lisa_profile unit suite.
//
// The ProFile opens its images through image_open_kind, which asks for the
// per-machine delta directory (checkpoint_machine_dir, via image_delta_dir).
// This suite exercises only the device handshake and the real base+delta
// block I/O, never checkpoint save/restore, so it stubs that one symbol
// rather than drag in checkpoint_machine.c.

#include "checkpoint_machine.h"

// No active per-machine directory under test: deltas land in the image
// layer's scratch root, which is what the suite wants.
const char *checkpoint_machine_dir(void) {
    return NULL;
}
