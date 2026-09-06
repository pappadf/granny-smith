// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// voodoo2_gpu.h
// The WebGPU takeover's translator (proposal-voodoo2-webgpu-takeover):
// the raster pthread's GPU mode.  Private to voodoo2_raster.c, which
// owns the thread and hands every command here under the "webgpu"
// backend; this unit decides whether the GPU (engaged) or the normative
// walker (bring-up, fallbacks, device loss) draws it.  The wire
// protocol to the browser's GPU worker is voodoo2_gpu_protocol.h; the
// host transport is the gs_v2gpu_* seam in system.h.

#ifndef VOODOO2_GPU_H
#define VOODOO2_GPU_H

#include "voodoo2_raster.h"

struct v2_gpu;
typedef struct v2_gpu v2_gpu_t;

// Allocate the shared region and attach the browser's GPU worker to it
// (blocks up to a few seconds for the attach).  NULL when no transport
// exists (native builds, no WebGPU adapter, or the attach timed out) —
// the caller falls back to the plain thread backend.
v2_gpu_t *v2_gpu_create(v2_target_t *tgt);
// Disengage without readback, tell the worker to drop everything, free.
void v2_gpu_destroy(v2_gpu_t *g);
// Execute one command on the raster thread: translated to the GPU while
// engaged, run on the walker otherwise.
void v2_gpu_execute(v2_gpu_t *g, const v2_draw_state_t *st, v2_target_t *tgt, const v2_cmd_t *cmd);
// The queue ran dry: close the open draw range and publish it, so the
// GPU starts on what it has rather than waiting for the next command.
void v2_gpu_idle(v2_gpu_t *g);
// Is GPU mode engaged (the GPU presents the frames)?  Readable from any
// thread.
bool v2_gpu_engaged(const v2_gpu_t *g);
// One-line statistics, into `buf`.
const char *v2_gpu_stats(v2_gpu_t *g, char *buf, size_t n);

#endif // VOODOO2_GPU_H
