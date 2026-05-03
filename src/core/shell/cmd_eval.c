// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// cmd_eval.c
// Phase 5c — legacy `eval <path>` shell command retired. JS callers
// invoke gs_eval() directly via the SAB bridge; integration scripts
// use bare path-form (`echo $(some.path)` works without `eval`).
