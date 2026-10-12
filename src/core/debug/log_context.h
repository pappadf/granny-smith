// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// log_context.h
// Connects the leaf logger (log.c) to the running machine: the instruction
// count and PC decorations, and the debug trace's capture of log lines.

#ifndef LOG_CONTEXT_H
#define LOG_CONTEXT_H

// Install the machine-aware hooks into the logger (log_set_context_hooks).
// Called once from core_init; idempotent.
void log_context_install(void);

#endif // LOG_CONTEXT_H
