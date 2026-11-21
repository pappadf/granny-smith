// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// page_fill.c
// Filling and emptying one page of the physical map, shared by every machine
// family.  See the header.

#include "page_fill.h"

// Zero the four SoA entries of page `p`: every access takes the slow path
static void page_soa_zero(uint32_t p) {
    if (g_supervisor_read)
        g_supervisor_read[p] = 0;
    if (g_supervisor_write)
        g_supervisor_write[p] = 0;
    if (g_user_read)
        g_user_read[p] = 0;
    if (g_user_write)
        g_user_write[p] = 0;
}

// 68K fill: identity entries for both privilege levels (see the header)
void page_fill_68k(uint32_t p, uint8_t *host, bool writable) {
    if (p >= g_page_count)
        return;
    memory_page_set_host(p, host, writable);
    uintptr_t adjusted = MEM_RAM_ENTRY((uintptr_t)host - ((uintptr_t)p << MEM_PAGE_SHIFT));
    if (g_supervisor_read)
        g_supervisor_read[p] = adjusted;
    if (g_user_read)
        g_user_read[p] = adjusted;
    if (writable) {
        // 0 on a predecoded code page (memory.h)
        uintptr_t wadj = memory_write_fill(p, host, adjusted, MEM_WT_SUPER | MEM_WT_USER);
        if (g_supervisor_write)
            g_supervisor_write[p] = wadj;
        if (g_user_write)
            g_user_write[p] = wadj;
    } else {
        if (g_supervisor_write)
            g_supervisor_write[p] = 0;
        if (g_user_write)
            g_user_write[p] = 0;
        memory_write_refused_clear(p, MEM_WT_SUPER | MEM_WT_USER); // read-only now on the switch core too
    }
    // A direct fast-path entry must be on the TLB tracker like every other
    // one, or the next mmu_invalidate_tlb -- the guest enabling the PMMU --
    // zeroes the tracked pages and leaves this identity mapping standing
    // under translation.  That was a latent bug the ROM overlay exposed after
    // a reset: the overlay's low pages stayed mapped to physical 0 with the
    // MMU on, so a IIci's vectors went to Bank A (the frame buffer) instead
    // of Bank B, the desktop fill wrote $AAAAAAAA over them, and the next
    // A-trap double-faulted.  A fresh boot hid it only because its memory
    // test overflows the tracker first, which forces a full clear.
    tlb_track_page(p);
    memory_logpoint_guard_page(p);
}

// PowerPC fill: supervisor identity entries, user arrays zeroed
void page_fill_identity_sv(uint32_t p, uint8_t *host, bool writable) {
    if (p >= g_page_count)
        return;
    memory_page_set_host(p, host, writable);
    uintptr_t adjusted = MEM_RAM_ENTRY((uintptr_t)host - ((uintptr_t)p << MEM_PAGE_SHIFT));
    if (g_supervisor_read)
        g_supervisor_read[p] = adjusted;
    if (g_supervisor_write) // write entry refused on a predecoded code page (memory.h)
        g_supervisor_write[p] = writable ? memory_write_fill(p, host, adjusted, MEM_WT_SUPER) : 0;
    if (g_user_read)
        g_user_read[p] = 0;
    if (g_user_write)
        g_user_write[p] = 0;
    memory_logpoint_guard_page(p);
}

// Empty one page: no host memory, no device, no fast-path entries
void page_clear(uint32_t p) {
    if (p >= g_page_count)
        return;
    memory_page_clear(p);
    page_soa_zero(p);
}

// Route one page to a device; every access dispatches
void page_route_device(uint32_t p, const memory_interface_t *iface, void *dev, uint32_t base) {
    if (p >= g_page_count)
        return;
    memory_page_set_device(p, iface, dev, base);
    page_soa_zero(p);
}
