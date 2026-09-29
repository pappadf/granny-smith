// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// lint.h
// The member-doc lint behind shell.lint_members (lint.c).

#ifndef GS_OBJECT_LINT_H
#define GS_OBJECT_LINT_H

#include "value.h"

// Walk the live tree and return a V_LIST of `<task or ->\t<path>: <rule>`
// lines, one per documentation gap (the rules are listed in lint.c).
value_t object_lint_members(void);

#endif // GS_OBJECT_LINT_H
