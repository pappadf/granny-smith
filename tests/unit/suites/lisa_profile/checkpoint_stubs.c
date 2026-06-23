// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Local stubs for the isolated lisa_profile unit suite.
//
// The ProFile device now references the checkpoint/image-restore machinery —
// the per-machine delta directory (checkpoint_machine_dir, via pro_delta_dir)
// and the geometry-aware single-image restore (mac_checkpoint_restore_one_image,
// via lisa_profile_init).  This suite exercises only the device handshake and
// the real base+delta block I/O, never checkpoint save/restore, so we stub the
// two symbols the shared isolated-harness stubs don't provide rather than drag
// in checkpoint_machine.c / checkpoint_images.c.

#include "checkpoint_images.h"
#include "checkpoint_machine.h"

// No active per-machine directory under test → deltas land adjacent to the base
// (image_create's NULL-delta_dir derivation), which is what the suite wants.
const char *checkpoint_machine_dir(void) {
    return NULL;
}

// Never reached: the suite always inits the ProFile with a NULL checkpoint, so
// lisa_profile_init takes no restore path.  Present only to satisfy the linker.
image_t *mac_checkpoint_restore_one_image(checkpoint_t *cp, image_geometry_t geom) {
    (void)cp;
    (void)geom;
    return NULL;
}
