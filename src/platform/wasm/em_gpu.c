// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_gpu.c
// The browser side of the WebGPU takeovers' transport (the Voodoo2's and
// the Rage 128's): the WASM overrides of the platform_gpu_* seam
// (platform_hooks.h), which reach the page as Module.onGpuAttach /
// onGpuDetach (bus/emulator.ts routes each to its card by the region's
// magic).  The translator on the
// raster pthread allocates a region in the wasm heap (control block +
// op ring + readback area, voodoo2_gpu_protocol.h) and asks the page to
// attach its GPU worker to it; from then on the two sides share memory
// and wake each other through Atomics — futex waits from this pthread,
// Atomics.waitAsync in the worker.  No message ever carries a frame.
//
// Availability is decided by the page: app/web2 requests a WebGPU
// adapter at startup and writes the answer into the mailbox's
// GPU_AVAILABLE control word before it reports ready -- page startup
// awaits it (an answer not in within 2 s counts as no adapter) -- so no
// boot and no catalog read can precede it, and the card's backend choice
// is honest at creation (regs.raster reports "thread" when the browser has
// no adapter).

#include "em.h"
#include "platform_hooks.h"

#include "system.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdint.h>

bool platform_gpu_available(void) {
    return mbx_load(get_mailbox(), GS_MBX_C_GPU_AVAILABLE) != 0;
}

bool platform_gpu_attach(void *ctrl, uint32_t bytes) {
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof Module.onGpuAttach === 'function') Module.onGpuAttach($0, $1); },
        (uint32_t)(uintptr_t)ctrl, bytes);
    // clang-format on
    return true;
}

void platform_gpu_detach(void *ctrl) {
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof Module.onGpuDetach === 'function') Module.onGpuDetach($0); },
        (uint32_t)(uintptr_t)ctrl);
    // clang-format on
}
