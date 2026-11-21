// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// status.h
// The repo's numeric error convention is negative errno: 0 is success and a
// failure is -EIO, -ENOMEM, -EINVAL, ..., always spelled by its <errno.h>
// name (the numbers differ between hosts: EIO is 5 on Linux, 29 under
// Emscripten).  status_t is the typed spelling of that convention used by
// the object model, aliases and the storage engine: each STATUS_E_* *is* the
// errno of the same meaning, so a status that crosses into code reading
// errno (strerror, a -EIO test) keeps its meaning without a bridge.
// STATUS_ERROR (-1) is the unspecified failure the rest of the repo spells
// `return -1`; it is not an errno and is never handed to strerror.
//
// Libraries with their own numbering (codec.h's CODEC_ERR_*) translate at a
// named bridge (codec_errno) before their codes meet this one.

#ifndef STATUS_H
#define STATUS_H

#include <errno.h>

// Result of a fallible operation
typedef enum {
    STATUS_OK = 0, // success
    STATUS_ERROR = -1, // unspecified failure (the cause, if any, was logged)
    STATUS_E_IO = -EIO, // host I/O failed (open/read/write/seek), or the medium is unreadable
    STATUS_E_INVAL = -EINVAL, // invalid argument or malformed input
    STATUS_E_NOMEM = -ENOMEM, // allocation failed
    STATUS_E_NOENT = -ENOENT, // the named thing does not exist
    STATUS_E_RANGE = -ERANGE, // a value or index is out of range
    STATUS_E_UNSUPPORTED = -ENOTSUP, // valid request this build/model does not support
} status_t;

#endif // STATUS_H
