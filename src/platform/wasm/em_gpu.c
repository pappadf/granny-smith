// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_gpu.c
// The browser side of the Voodoo2 WebGPU takeover's transport: the WASM
// overrides of the gs_v2gpu_* seam (system.h).  The translator on the
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

#include "system.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <limits.h>
#include <stdint.h>

bool gs_v2gpu_available(void) {
    return mbx_load(get_gs_mailbox(), GS_MBX_C_GPU_AVAILABLE) != 0;
}

bool gs_v2gpu_attach(void *ctrl, uint32_t bytes) {
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof Module.onVoodooGpuAttach === 'function') Module.onVoodooGpuAttach($0, $1); },
        (uint32_t)(uintptr_t)ctrl, bytes);
    // clang-format on
    return true;
}

void gs_v2gpu_detach(void *ctrl) {
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof Module.onVoodooGpuDetach === 'function') Module.onVoodooGpuDetach($0); },
        (uint32_t)(uintptr_t)ctrl);
    // clang-format on
}

int gs_v2gpu_wait(volatile uint32_t *addr, uint32_t expected, uint32_t timeout_ms) {
    return emscripten_futex_wait(addr, expected, (double)timeout_ms);
}

void gs_v2gpu_notify(volatile uint32_t *addr) {
    emscripten_futex_wake(addr, INT_MAX);
}
