// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// status.h
// The project-wide status code: what a fallible operation returns when the
// caller needs to know more than "it failed".  STATUS_OK is zero and every
// failure is negative, so `if (rc < 0)` and `if (rc != STATUS_OK)` both read
// correctly.  New failure kinds are added here, as STATUS_E_*, rather than
// invented per module.

#ifndef STATUS_H
#define STATUS_H

// Result of a fallible operation
typedef enum {
    STATUS_OK = 0, // success
    STATUS_ERROR = -1, // unspecified failure (the cause, if any, was logged)
    STATUS_E_IO = -2, // host I/O failed (open/read/write/seek)
    STATUS_E_INVAL = -3, // invalid argument or malformed input
    STATUS_E_NOMEM = -4, // allocation failed
    STATUS_E_NOENT = -5, // the named thing does not exist
    STATUS_E_RANGE = -6, // a value or index is out of range
    STATUS_E_UNSUPPORTED = -7, // valid request this build/model does not support
} status_t;

#endif // STATUS_H
