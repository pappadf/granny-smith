// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// memory_class.h
// The machine.memory object node and its peek / poke children
// (memory_class.c), created and destroyed with the memory map.

#ifndef MEMORY_CLASS_H
#define MEMORY_CLASS_H

#include "memory.h"

struct object;

// Create machine.memory for `mem` with its peek and poke children, attached
// under the machine node.  Returns the memory node (NULL if it could not be
// made); the children come back through *peek_out / *poke_out.
struct object *memory_object_new(memory_map_t *mem, struct object **peek_out, struct object **poke_out);

// Detach and free the three nodes memory_object_new made (any may be NULL).
void memory_object_delete(struct object *obj, struct object *peek, struct object *poke);

#endif // MEMORY_CLASS_H
