// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// checkpoint_images.c
// Image-list checkpoint serialisation — see checkpoint_images.h.

#include "checkpoint_images.h"

#include "checkpoint_machine.h"
#include "image.h"
#include "log.h"
#include "storage.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
void mac_checkpoint_restore_images(config_t *cfg, checkpoint_t *cp) {
    uint32_t count = 0;
    system_read_checkpoint_data(cp, &count, sizeof(count));
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = 0;
        system_read_checkpoint_data(cp, &len, sizeof(len));
        char *name = NULL;
        if (len > 0) {
            name = (char *)malloc(len);
            if (!name) {
                char tmp;
                for (uint32_t k = 0; k < len; ++k)
                    system_read_checkpoint_data(cp, &tmp, 1);
            } else {
                system_read_checkpoint_data(cp, name, len);
            }
        }
        char writable = 0;
        system_read_checkpoint_data(cp, &writable, sizeof(writable));
        uint64_t raw_size = 0;
        system_read_checkpoint_data(cp, &raw_size, sizeof(raw_size));
        uint32_t instance_len = 0;
        system_read_checkpoint_data(cp, &instance_len, sizeof(instance_len));
        char *instance_path = NULL;
        if (instance_len > 0) {
            instance_path = (char *)malloc(instance_len);
            if (instance_path) {
                system_read_checkpoint_data(cp, instance_path, instance_len);
            } else {
                char tmp;
                for (uint32_t k = 0; k < instance_len; ++k)
                    system_read_checkpoint_data(cp, &tmp, 1);
            }
        }

        image_t *img = NULL;
        if (name) {
            // Consolidated checkpoints carry the raw image bytes inline; quick
            // checkpoints reference the on-disk file and just need it reopened.
            bool consolidated = checkpoint_get_kind(cp) == CHECKPOINT_KIND_CONSOLIDATED;
            if (raw_size > 0 && consolidated)
                image_create_empty(name, (size_t)raw_size);
            if (writable && consolidated) {
                img = image_create(name, checkpoint_machine_dir());
            } else if (writable && instance_path && instance_path[0]) {
                img = image_open(name, instance_path);
            } else if (writable) {
                img = image_create(name, checkpoint_machine_dir());
            } else {
                img = image_open_readonly(name);
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
        if (img)
            add_image(cfg, img);
        free(name);
        free(instance_path);
    }
}
