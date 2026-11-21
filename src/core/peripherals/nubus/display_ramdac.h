// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// display_ramdac.h
// The palette-port sequencer every RAMDAC in the tree shares: an entry index
// (one, or a write/read pair), a component phase, and the components of the
// entry being written.  Header-only.
//
// Every palette DAC modelled here -- the NuBus cards' (Mac II Video Card's
// Bt453, the 24AC's, JMFB's and the 8*24 GC's AC842/ACDC), the PCI cards'
// (Mach64 GX's RGB514 and the Rage Pro's integrated DAC, the Cirrus VGA DAC)
// and the built-in chips' (RBV's Bt450, DAFB's AC842, Control's RaDACal,
// PDM's Ariel II, CIVIC's Sebastian) -- runs the same protocol: write the
// entry index to an address register, then stream components, R, G, B (and
// alpha on Sebastian) to a data register; after the last component the index
// advances, so one address write loads or reads a run of entries.  An address
// write restarts the entry at its first component.
//
// Semantics, chosen once for all of them:
//
//  - Writes are LATCHED: components collect in `pending` and the entry is
//    stored only when its last component arrives (ramdac_seq_write returns
//    true).  This is the Brooktree run-write protocol the whole set follows
//    (holding registers, committed after blue), and the VGA DAC's.  No
//    reference page contradicts it for any of these chips: DAFB's
//    (dafb.md section 2.4.1), Ariel's (ariel.md sections 2.2-2.3), CIVIC's
//    (civic.md section 2.8), the 8*24 GC's, the 24AC's and Mach64's
//    describe the triplet and the advance, not a per-byte store, and Ariel
//    lists the mid-triplet address write as unestablished (its section 6).
//    A partial entry followed by an address write is therefore dropped,
//    never half-stored -- the renderer cannot see a mixed colour.  Several
//    ports used to store each byte as it arrived (Mac II Video Card,
//    Mach64, Cirrus, Control, Ariel, CIVIC); a guest that writes whole
//    entries sees no difference.
//  - Reads ADVANCE: each data read returns the next component of the entry
//    at the read index, and after the last component the read index moves
//    on, exactly as a write run does.  (DAFB and RBV used to step only the
//    phase, so a run-read returned its first entry over and over.)  Reads
//    come from the stored entry; a pending write's components are not
//    readable.  A `peek` reports what the next read returns and moves
//    nothing.
//  - One phase serves both directions, as one sub-counter does on the parts.
//    Most chips have one index: the write and read address registers set the
//    same index and a run in either direction moves it.  The VGA DAC (Cirrus)
//    has separate write and read indices (RAMDAC_SPLIT_INDEX).
//
// An all-zero ramdac_seq_t is the common part in its power-on state (three
// components, one index, entry 0), so a calloc'd or memset device needs no
// explicit set-up; ramdac_seq_init sets the other shapes.
//
// What stays per chip is outside the sequencer: inverted (active-low) ports
// (24AC), 32-bit data longs (JMFB), 6-bit read masks (Cirrus), banks
// (CIVIC), and where the committed entry is stored and who is told.

#ifndef DISPLAY_RAMDAC_H
#define DISPLAY_RAMDAC_H

// === Includes ===

#include <stdbool.h>
#include <stdint.h>

// === Constants ===

// ramdac_seq_t.flags: separate write and read indices (the VGA DAC).
#define RAMDAC_SPLIT_INDEX 0x01u

// ramdac_seq_t.flags: four components per entry, R, G, B, alpha (Sebastian).
#define RAMDAC_RGBA 0x02u

// Most components an entry carries (R, G, B, alpha).
#define RAMDAC_MAX_COMP 4u

// === Type Definitions ===

// One palette port's sequencer state; plain bytes, so it checkpoints as part
// of its owner's scalar block.
typedef struct ramdac_seq {
    uint8_t widx; // entry the next completed write stores
    uint8_t ridx; // entry the next read returns (== widx unless split)
    uint8_t phase; // component the next data access targets
    uint8_t flags; // RAMDAC_SPLIT_INDEX, RAMDAC_RGBA
    uint8_t pending[RAMDAC_MAX_COMP]; // components of the entry being written
} ramdac_seq_t;

// === Operations ===

// Puts the sequencer in its power-on state: index 0, first component.
static inline void ramdac_seq_init(ramdac_seq_t *s, uint8_t flags) {
    *s = (ramdac_seq_t){.flags = flags};
}

// Components per entry: 3, or 4 with RAMDAC_RGBA.
static inline uint8_t ramdac_seq_ncomp(const ramdac_seq_t *s) {
    return (s->flags & RAMDAC_RGBA) ? 4u : 3u;
}

// True when the write and read addresses are one index.
static inline bool ramdac_seq_shared(const ramdac_seq_t *s) {
    return !(s->flags & RAMDAC_SPLIT_INDEX);
}

// The write address register: selects the entry and restarts at its first
// component (dropping a partial entry).  Shared: the read index follows.
static inline void ramdac_seq_set_waddr(ramdac_seq_t *s, uint8_t idx) {
    s->widx = idx;
    if (ramdac_seq_shared(s))
        s->ridx = idx;
    s->phase = 0;
}

// The read address register: as ramdac_seq_set_waddr, for the read index.
static inline void ramdac_seq_set_raddr(ramdac_seq_t *s, uint8_t idx) {
    s->ridx = idx;
    if (ramdac_seq_shared(s))
        s->widx = idx;
    s->phase = 0;
}

// The (shared or write) index, as the address register reads back.
static inline uint8_t ramdac_seq_addr(const ramdac_seq_t *s) {
    return s->widx;
}

// One data-register write: latches `v` as the current component.  On the
// last one, returns true with the entry's index in *idx and its components
// in s->pending, and advances; the caller stores the entry.
static inline bool ramdac_seq_write(ramdac_seq_t *s, uint8_t v, uint8_t *idx) {
    // The mask keeps a phase restored from a damaged checkpoint in bounds
    s->pending[s->phase & (RAMDAC_MAX_COMP - 1u)] = v;
    if (++s->phase < ramdac_seq_ncomp(s))
        return false;
    *idx = s->widx++;
    if (ramdac_seq_shared(s))
        s->ridx = s->widx;
    s->phase = 0;
    return true;
}

// One data-register read: returns the entry index to read and, in *comp,
// which component of it; the caller fetches that byte.  Advances past the
// component (and past the entry after its last) unless `peek`.
static inline uint8_t ramdac_seq_read(ramdac_seq_t *s, bool peek, uint8_t *comp) {
    uint8_t idx = s->ridx;
    // An out-of-range phase (a damaged checkpoint) reads as the first component
    *comp = s->phase < ramdac_seq_ncomp(s) ? s->phase : 0;
    if (peek)
        return idx;
    if (++s->phase >= ramdac_seq_ncomp(s)) {
        s->phase = 0;
        s->ridx++;
        if (ramdac_seq_shared(s))
            s->widx = s->ridx;
    }
    return idx;
}

#endif // DISPLAY_RAMDAC_H
