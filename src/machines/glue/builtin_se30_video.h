// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// builtin_se30_video.h
// SE/30 built-in video as a NuBus card living in slot $E.  See
// docs/reference/machines/glue/se30.md.  The card
// owns 64 KB VRAM at $FEE00000, a 32 KB declaration ROM at $FEFF8000
// (Apple's `builtin-se30-video-4f71ff1a.vrom` when offered, else the
// emulator's substitute),
// drives the slot-$E VBL pseudo-IRQ, and exposes a 512×342×1bpp
// `display_t` for system_display().
//
// The kind descriptor is registered explicitly in nubus.c's
// g_card_registry; machines refer to it by id "builtin_se30_video".

#ifndef NUBUS_CARDS_BUILTIN_SE30_VIDEO_H
#define NUBUS_CARDS_BUILTIN_SE30_VIDEO_H

#include "card.h"
#include <stdbool.h>

// Card-kind descriptor — exported as the single source of truth for the
// driver.  The bus controller calls ops->init once per BUILTIN slot.
extern const nubus_card_kind_t builtin_se30_video_kind;

// === SE/30-specific hooks the machine calls into ============================
//
// These are the only entry-points outside the nubus_card_ops_t vtable.
// They exist because some SE/30 behaviour straddles the GLUE chip and
// the slot-$E pseudo-card (notably the VIA1 PA6 buffer-select toggle and
// the saved/restored VRAM contents).  Future card drivers are expected
// to be self-contained — these hooks are SE/30-specific and live here so
// se30.c doesn't reach into the card's private state.

// Switch the on-screen framebuffer between the primary ($8040) and
// alternate ($0040) VRAM offsets.  Called from se30_via1_output when
// the OS toggles VIA1 PA6.  Sets display.fb_dirty so the renderer
// re-uploads.  No-op if `card` is NULL.
void builtin_se30_video_select_buffer(nubus_card_t *card, bool main_buf);

// Borrowed accessor for the card's VRAM buffer.  Used by se30_init to
// register the VRAM region on the bus map (the existing
// memory_map_host_region path) without dragging the card's private
// struct into se30.c.  Returns NULL if `card` is NULL.
uint8_t *builtin_se30_video_vram(nubus_card_t *card);

// Borrowed accessor for the card's VROM buffer (Apple's or the substitute
// declaration ROM bytes).  Same pattern as the VRAM accessor.
uint8_t *builtin_se30_video_vrom(nubus_card_t *card);

#endif // NUBUS_CARDS_BUILTIN_SE30_VIDEO_H
