// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// report.h -- a failed script's report.  The script engine (gs_lua.c) hands
// every failure here (gs_lua_set_reporter): `text` is the report as printed
// (newline-terminated, possibly several lines: the message and a
// traceback).  Inside a job it becomes an `error` annotation at this point
// of the job's record stream: file, line (0 when none), message and the
// text's lines.  It goes to stderr only when no record can carry it whole
// (outside a job, past the output cut, or as a `truncated` record's full
// text).

#ifndef GS_SHELL_REPORT_H
#define GS_SHELL_REPORT_H

void script_report_error(const char *file, int line, const char *msg, const char *text);

#endif // GS_SHELL_REPORT_H
