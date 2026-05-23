// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// aux_syscalls.h
// A/UX (System V Release 2-based Apple Unix for 68k Macs) syscall name
// table.  The A/UX ABI passes the syscall number in D0 and triggers via
// `TRAP #0`; the annotator uses this lookup to label TRAP #0 sites with
// "; syscall <name>" when the preceding instruction loaded D0 with an
// immediate.

#pragma once

#ifndef GS_AUX_SYSCALLS_H
#define GS_AUX_SYSCALLS_H

#include <stdint.h>

// Look up an A/UX syscall name by number.  Returns NULL when the number
// is unknown (covers the standard SVR2 range plus the Mac-specific
// extensions A/UX added; non-SVR2 numbers and indirect calls show as
// NULL and the annotator falls back to "syscall #N").
const char *aux_syscall_name(uint32_t num);

#endif // GS_AUX_SYSCALLS_H
