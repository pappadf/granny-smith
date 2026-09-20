// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_object() for the bespoke suites that link one device's .c file
// directly (psc, rbv, tnt_gc) rather than going through common.mk, and so do
// not get stub_system.c's copy.
//
// Returning NULL is safe and deliberate: object_attach(NULL, child) is a
// no-op, so a device that builds its object node at init gets a real,
// working node that simply has no parent.  A suite that wants to exercise
// the node calls its getters directly, which is what the node's members are.
struct object;

struct object *machine_object(void) {
    return NULL;
}
