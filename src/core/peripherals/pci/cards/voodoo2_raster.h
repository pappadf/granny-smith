// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// voodoo2_raster.h
// The raster-backend seam (proposal-pci-3dfx-voodoo2 §3.6).
//
// The two triangle routes — host-setup and on-chip setup — converge on
// voodoo2_tri_t; everything downstream of it sits behind this vtable.
// The software walker in voodoo2.c is the DEFAULT and NORMATIVE backend:
// it defines the semantics, produces every golden, and runs every gate.
// Accelerated backends (SIMD, a rendering thread, WebGL2) are future
// work outside this PR; the seam exists so they can be installed without
// a refactor.  Two invariants keep any backend guest-invisible:
//
//   1. TIMING IS ANALYTIC.  Work completes synchronously at issue and
//      busy/idle is computed from bookkeeping, never from the cost of
//      rasterising — so the guest's instruction stream is
//      backend-independent by construction.  A triangle consumes zero
//      scheduled time whichever backend draws it.
//   2. THE CPU SHADOW IS AUTHORITATIVE FOR LFB READS.  A backend that
//      renders elsewhere must sync() — flush and read back the dirty
//      region — before an LFB read, screen.save, or a checkpoint.  The
//      software walker renders directly into the shadow, so its sync()
//      is a no-op.
//
// The FILL CONVENTION the walker implements is CHOSEN, NOT KNOWN: the
// Voodoo2 spec's own §7.2 defers the TRIANGLE walk to an SST-1
// Programming Guide nobody holds.  The convention (documented in
// docs/core/peripherals/pci/cards/voodoo2.md): vertices in 12.4, the
// sample point at the pixel's integer coordinate, half-open top-left
// edge inclusion with the winding taken from the command's area sign,
// and parameter iteration from vertex A's truncated position.  Goldens
// record what THIS rasteriser draws — regression anchors, not hardware
// conformance.

#ifndef VOODOO2_RASTER_H
#define VOODOO2_RASTER_H

#include <stdbool.h>
#include <stdint.h>

// One triangle, in the register file's own fixed-point formats, after
// both submission routes have converged (host setup latches these
// directly; on-chip setup computes the gradients from its vertices).
typedef struct voodoo2_tri {
    int32_t ax, ay, bx, by, cx, cy; // vertices, 12.4 two's complement
    int32_t r, g, b, a; // colour start values, 12.12 (sign-extended)
    int32_t drdx, dgdx, dbdx, dadx; // 12.12
    int32_t drdy, dgdy, dbdy, dady;
    int32_t z, dzdx, dzdy; // 20.12
    int64_t w, dwdx, dwdy; // FBI 1/W, 2.30 (sign-extended to 64)
    // Per-TMU texture parameters: S/W and T/W in 14.18, per-TMU 1/W in
    // 2.30 (sWtmu*).
    int64_t s[2], dsdx[2], dsdy[2];
    int64_t t[2], dtdx[2], dtdy[2];
    int64_t tw[2], dtwdx[2], dtwdy[2];
    bool area_sign; // triangleCMD bit 31: 1 = clockwise / negative area
} voodoo2_tri_t;

struct voodoo2;

// The backend vtable.  `ctx` is the owning card.  All hooks mandatory in
// a registered backend; the card installs the software walker at init.
typedef struct voodoo2_raster_backend {
    const char *name;
    // Rasterise one triangle through the full pixel pipeline.
    void (*triangle)(struct voodoo2 *v, const voodoo2_tri_t *tri);
    // FASTFILL: clear the clip rectangle per fbzMode's masks.
    void (*fastfill)(struct voodoo2 *v);
    // Make the CPU shadow authoritative (invariant 2).  Called before
    // any LFB read, screen capture, or checkpoint.
    void (*sync)(struct voodoo2 *v);
} voodoo2_raster_backend_t;

#endif // VOODOO2_RASTER_H
