// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gsvrom.h
// The GS generic declaration ROM, generated at runtime (proposal-nubus-
// runtime-vrom §3): the declarative records come from the declrom
// builder fed by the card kind's monitor table, and the 68K code blocks
// are fragments assembled by the core build (src/core/peripherals/
// nubus/vrom68k/, embedded via build/vrom68k/gsvrom_fragments.h).  The
// generic sibling card kinds call gsvrom_generate at card_init; the
// offer registry is never consulted.

#ifndef NUBUS_GSVROM_H
#define NUBUS_GSVROM_H

#include <stddef.h>
#include <stdint.h>

struct nubus_monitor;
struct declrom_builder;

// Card personalities with a built-in declaration ROM.
typedef enum gsvrom_personality {
    GSVROM_JMFB = 0, // 8•24 (JMFB) register model
    GSVROM_BOOGIE, // Display Card 24AC register model
    GSVROM_MDCGC, // 8•24 GC register model
    GSVROM_SE30, // SE/30 built-in video (framebuffer only)
} gsvrom_personality_t;

// The spliced 68K code blocks, one set per personality.
typedef enum gsvrom_frag_kind {
    GSVROM_FRAG_INIT = 0, // PrimaryInit sExecBlock
    GSVROM_FRAG_DRVR, // the DRVR sBlock
    GSVROM_FRAG_SINIT, // SecondaryInit sExecBlock (GC only)
} gsvrom_frag_kind_t;

// Returns the personality's assembled code fragment and its size, or
// NULL when the personality has no such block (e.g. SINIT off the GC).
// The pointer is static storage — never freed.
const uint8_t *gsvrom_frag(gsvrom_personality_t p, gsvrom_frag_kind_t k, size_t *out_size);

// Generate the personality's declaration-ROM image from the card
// kind's monitor list (one functional video sResource per monitor,
// spIDs from srsrc_sister, modes from the depth list — the monitors[]
// table is the single source of truth for mode geometry).  Returns a
// FINALISED builder whose bytes are the dense $0F image (install with
// declrom_install_builtin, then declrom_builder_free), or NULL on
// failure.
struct declrom_builder *gsvrom_generate(gsvrom_personality_t p, const struct nubus_monitor *monitors);

#endif // NUBUS_GSVROM_H
