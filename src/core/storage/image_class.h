// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// image_class.h
// The object-model node of an image: `image`, the same class under every
// drive that holds one -- machine.floppy.drive[N].image, a SCSI or ATAPI
// device's, machine.ata.device[U].image, the Lisa's machine.hd.image -- and
// each entry of files.images (docs/internals/core/storage/image_kind.md §3).
// What differs per drive is how to find its image and how to eject it: the
// drive hands both in as ops; every member is the class's.

#ifndef IMAGE_CLASS_H
#define IMAGE_CLASS_H

#include "object.h"

struct image;

// What a drive's eject answers (image_node_ops_t.eject).
#define IMAGE_EJECT_OK     0
#define IMAGE_EJECT_EMPTY  -1 // nothing to eject
#define IMAGE_EJECT_LOCKED -2 // the guest has locked the drive (PREVENT MEDIUM REMOVAL)

// How a drive's node finds and ejects its image.
typedef struct image_node_ops {
    struct image *(*image)(void *dev, unsigned slot); // the image held, or NULL
    // Take it out (the host's eject): IMAGE_EJECT_*.  NULL: the node is not a
    // drive's (files.images), and eject() says where to eject it instead.
    int (*eject)(void *dev, unsigned slot);
} image_node_ops_t;

// One node: the drive (`dev`, `slot`) and its ops; the object's data.
typedef struct image_node {
    const image_node_ops_t *ops;
    void *dev;
    unsigned slot;
} image_node_t;

// The image class (members in image_class.c).
extern const class_desc_t image_class;

// The image `self` (an image-class object) holds now, or NULL.
struct image *image_node_image(struct object *self);

#endif // IMAGE_CLASS_H
