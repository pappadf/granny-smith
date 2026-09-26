// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint_images.c
// Image-list checkpoint serialisation — see checkpoint_images.h.

#include "checkpoint_images.h"

#include "checkpoint_machine.h"
#include "image.h"
#include "log.h"
#include "storage.h"
#include "system.h" // MAX_IMAGES -- the real bound on the restored list

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

LOG_USE_CATEGORY_NAME("setup");

// Save the image list into a checkpoint stream.  Layout:
//   uint32_t count
//   for each image i:
//     uint32_t name_len, name_bytes...
//     int8_t   writable
//     uint64_t raw_size
//     uint32_t instance_len, instance_bytes...
//     <storage-specific blob via image_checkpoint>
void mac_checkpoint_save_images(config_t *cfg, checkpoint_t *cp) {
    uint32_t count = (uint32_t)cfg->n_images;
    system_write_checkpoint_data(cp, &count, sizeof(count));
    for (uint32_t i = 0; i < count; ++i)
        image_checkpoint(cfg->images[i], cp);
}

// Restore the image list from a checkpoint stream and attach each image
// back onto cfg->images.  Mirrors save layout; fails loudly via
// checkpoint_set_error on partial reads / failed image opens so the
// caller's restore loop sees a marked-error checkpoint.
image_t *mac_checkpoint_restore_one_image(checkpoint_t *cp, image_geometry_t geom) {
    // Bounded and terminated by the reader rather than by the writer's
    // promise: `name` goes on to access(), image_open_with_geometry() and
    // printf("%s"), so a file that omits the NUL used to read off the end of
    // the allocation, and an unbounded length drove the malloc.
    char *name = checkpoint_read_string(cp, CHECKPOINT_MAX_PATH, "image path");
    char writable = 0;
    system_read_checkpoint_data(cp, &writable, sizeof(writable));
    uint64_t raw_size = 0;
    system_read_checkpoint_data(cp, &raw_size, sizeof(raw_size));
    char *instance_path = checkpoint_read_string(cp, CHECKPOINT_MAX_PATH, "image instance path");

    image_t *img = NULL;
    if (name) {
        // Consolidated checkpoints carry the raw image bytes inline; quick
        // checkpoints reference the on-disk file and just need it reopened.  The
        // base is reopened with `geom` so a non-512 device (the ProFile) restores
        // at its real block size rather than the default 512.
        bool consolidated = checkpoint_get_kind(cp) == CHECKPOINT_KIND_CONSOLIDATED;
        if (raw_size > 0 && consolidated && access(name, F_OK) != 0) {
            // A consolidated checkpoint carries every block inline and the
            // restore below writes them all into a fresh delta, so the base
            // file's CONTENT is never read through — it only has to exist so
            // the opener can probe the geometry.  Materialise it only when
            // nothing is there: this used to run unconditionally, and
            // because it opens with "wb" it truncated the image at its
            // ORIGINAL path (the "checkpoint.load destroyed my disk image"
            // footgun — a suite that checkpointed with shared test media
            // attached silently zeroed that media on load, while the guest
            // kept working off the restored delta).  An existing file is now
            // left completely alone, whatever its size: a DiskCopy base is
            // legitimately 84 bytes larger than its logical raw_size, and a
            // genuinely mismatched geometry is still caught loudly by
            // storage_restore_from_checkpoint's own check.
            image_create_empty(name, (size_t)raw_size);
        }
        if (writable && consolidated) {
            img = image_create_with_geometry(name, checkpoint_machine_dir(), geom);
        } else if (writable && instance_path && instance_path[0]) {
            img = image_open_with_geometry(name, instance_path, geom);
        } else if (writable) {
            img = image_create_with_geometry(name, checkpoint_machine_dir(), geom);
        } else {
            img = image_open_readonly_with_geometry(name, geom);
        }
        if (!img) {
            printf("Error: image_open failed for %s while restoring checkpoint\n", name);
            checkpoint_set_error(cp);
        }
    }
    if (storage_restore_from_checkpoint(img ? img->storage : NULL, cp) != GS_SUCCESS) {
        printf("Error: storage_restore_from_checkpoint failed for %s\n", name ? name : "<unnamed>");
        checkpoint_set_error(cp);
    }
    free(name);
    free(instance_path);
    return img;
}

void mac_checkpoint_restore_images(config_t *cfg, checkpoint_t *cp) {
    // The count comes off disk and used to be the loop bound directly: a
    // corrupt 0xFFFFFFFF ran four billion iterations, each one re-entering the
    // restore, calling storage_restore_from_checkpoint and logging a line --
    // an effective hang plus a log flood rather than an error.
    uint32_t count = 0;
    if (!checkpoint_read_count(cp, &count, MAX_IMAGES, "images"))
        return;
    for (uint32_t i = 0; i < count; ++i) {
        // Stop at the first damaged entry rather than grinding through the
        // remainder against an already-failed stream.
        if (checkpoint_has_error(cp))
            return;
        // Generic image list is all flat 512-byte disks (block_size 0 ⇒ 512).
        image_t *img = mac_checkpoint_restore_one_image(cp, (image_geometry_t){0});
        if (img)
            add_image(cfg, img);
    }
}
