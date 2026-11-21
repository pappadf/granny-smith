// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// page_fill.h
// One page of the physical map, filled or emptied: the AoS page-table entry
// plus the four SoA fast-path arrays (memory.h).  The machine families used
// to keep a private copy each -- three byte-identical PowerPC ones and two
// 68K ones that disagreed on read-only pages -- plus open-coded "route the
// page to a device and zero the arrays" blocks.  They run at map-change
// time, not per access, and the families pass the fills as callbacks.
//
// Two fill flavours, because the user arrays have different owners:
//   page_fill_68k          the 68K families: supervisor and user arrays both
//                          get the identity entry (the 68030/040 MMU refills
//                          them under translation), on the TLB tracker.
//   page_fill_identity_sv  the PowerPC families: the supervisor arrays hold
//                          the eager physical identity view; the user arrays
//                          belong to the PPC MMU front end (ppc_mmu.c) and
//                          are only ever cleared here.
//
// Needs no 68K headers, so the PowerPC families include it too.

#ifndef GS_MACHINES_RUNTIME_PAGE_FILL_H
#define GS_MACHINES_RUNTIME_PAGE_FILL_H

#include "memory.h" // memory_interface_t

#include <stdbool.h>
#include <stdint.h>

// 68K: map page `p` to `host` for both privilege levels, on the TLB tracker.
// A read-only fill takes the write entries away, and with them the page's
// refused-write record (memory.h: cleared wherever the switch core would
// lose the entry), or the 68K MMU slow path would complete a store through
// the read entry -- into whatever the page now maps (the ROM image under an
// overlay).
void page_fill_68k(uint32_t p, uint8_t *host, bool writable);

// PowerPC: map page `p` to `host` in the supervisor arrays; zero the user
// arrays, which the PPC MMU front end refills.  Callers that change the
// physical map (an HMC / Hammerhead / Grackle remap) also invalidate the MMU
// caches.
void page_fill_identity_sv(uint32_t p, uint8_t *host, bool writable);

// Empty page `p`: no host memory and no device, so it reads as nothing is
// there (outside a bus-error range, how an empty RAM window fails a probe).
// A bank decode that moves clears the windows it leaves, or they would keep
// echoing the RAM that used to be mapped there.
void page_clear(uint32_t p);

// Route page `p` to a device (`iface` on `dev`, offsets relative to `base`)
// and take its fast-path entries away, so every access dispatches.
void page_route_device(uint32_t p, const memory_interface_t *iface, void *dev, uint32_t base);

#endif // GS_MACHINES_RUNTIME_PAGE_FILL_H
