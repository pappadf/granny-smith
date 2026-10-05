// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_object() alone, for the TEST_HARNESS=none suites that link one
// device's .c file with their own mocks (psc, rbv, tnt_gc, ...) and so do not
// get stub_system.c's copy.
//
// Returning NULL is safe and deliberate: object_attach(NULL, child) is a
// no-op, so a device that builds its object node at init gets a real,
// working node that simply has no parent.  A suite that wants to exercise
// the node calls its getters directly, which is what the node's members are.
struct object;

struct object *machine_object(void) {
    return NULL;
}
