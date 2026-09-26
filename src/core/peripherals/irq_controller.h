// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// irq_controller.h
// One object-model shape for every interrupt controller.
//
// Before this, not one interrupt controller, DMA engine, memory controller
// or bus bridge exported an object node: there was `machine.via1.ifr` and
// nothing for the RBV, the OSS, the PSC, the AMIC or Grand Central, which is
// to say that an IRQ storm on a IIci, IIfx, Quadra AV or Power Macintosh
// could not be looked at from the shell at all.
//
// The shape is deliberately two-part:
//
//   - Four attributes every controller can answer -- `pending`, `enabled`,
//     `active` and `ipl` -- so a script that does not know which machine it
//     is on can still ask "who is shouting, and at what level".
//   - Whatever else that chip has, declared by the chip.  This half is not
//     optional: an IIfx storm is diagnosed from the OSS's per-source level
//     map and a 7500's from Grand Central's `events`/`levels`/`mask`/`latch`
//     pair of modes, and a purely generic node would reach neither.
//
// Mechanically that is one set of shared getters reached through a member's
// `user_data` (which carries the ops table), so `instance_data` stays the
// chip's own state pointer and a chip-specific getter is written exactly the
// way it would be written without this file.
//
// NOTE: do NOT give these nodes per-controller log
// categories.  find_category is an exact strcmp and an unknown name is
// created on demand, so a typo would silently make a new category.

#ifndef GS_IRQ_CONTROLLER_H
#define GS_IRQ_CONTROLLER_H

#include "object.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What every interrupt controller can be asked.  `ctx` is the object's
// instance_data -- the chip's own state pointer.
//
// `pending` and `enabled` share a bit numbering, whatever it is; `active` is
// derived from them by the shared getter, so a chip whose hardware summary
// is not simply `pending & enabled` (Grand Central in mode 1, whose latch is
// the summary) overrides it with `active`.
typedef struct irq_controller_ops {
    const char *chip; // the part, e.g. "OSS" / "RBV" / "Grand Central"
    uint32_t (*pending)(void *ctx); // request bits the chip is holding
    uint32_t (*enabled)(void *ctx); // enable/mask bits, same numbering
    uint32_t (*active)(void *ctx); // optional; default is pending & enabled
    int (*ipl)(void *ctx); // CPU IPL asserted now, 0 = none
    // Optional per-level view.  A chip that routes sources to CPU levels
    // answers `level_count` > 0 and fills `level(i)` with the source bits
    // routed to level i; `level_base` is the CPU IPL that index 0 means.
    int (*level_count)(void *ctx);
    uint32_t (*level)(void *ctx, int index);
    int level_base;
} irq_controller_ops_t;

// The shared getters.  Declared here only so the member macro below can name
// them; nothing outside a member table should call them.
value_t irq_ctrl_attr_chip(struct object *self, const member_t *m);
value_t irq_ctrl_attr_pending(struct object *self, const member_t *m);
value_t irq_ctrl_attr_enabled(struct object *self, const member_t *m);
value_t irq_ctrl_attr_active(struct object *self, const member_t *m);
value_t irq_ctrl_attr_ipl(struct object *self, const member_t *m);
value_t irq_ctrl_attr_levels(struct object *self, const member_t *m);

// The five shared members, for splicing into a chip's own member table:
//
//   static const member_t oss_members[] = {
//       IRQ_CONTROLLER_MEMBERS(&oss_irq_ops)
//       { ...the OSS's own... },
//   };
//
// OPS is a `const irq_controller_ops_t *`.  It rides in each member's
// user_data so instance_data stays the chip state pointer.
#define IRQ_CONTROLLER_ATTR(NAME, TYPE, PFLAGS, GETTER, OPS, DOC)                                                      \
    {                                                                                                                  \
        .kind = M_ATTR,                                                                                                \
        .name = (NAME),                                                                                                \
        .doc = (DOC),                                                                                                  \
        .flags = VAL_RO,                                                                                               \
        .attr = {.type = (TYPE), .presentation_flags = (PFLAGS), .get = (GETTER), .set = NULL, .user_data = (OPS)} \
},

#define IRQ_CONTROLLER_MEMBERS(OPS)                                                                                    \
    IRQ_CONTROLLER_ATTR("chip", V_STRING, 0, irq_ctrl_attr_chip, (OPS), "The part this node models")                   \
    IRQ_CONTROLLER_ATTR("pending", V_UINT, VAL_HEX | VAL_VOLATILE, irq_ctrl_attr_pending, (OPS),                       \
                        "Interrupt request bits the chip is holding")                                                  \
    IRQ_CONTROLLER_ATTR("enabled", V_UINT, VAL_HEX | VAL_VOLATILE, irq_ctrl_attr_enabled, (OPS),                       \
                        "Enable/mask bits, same bit numbering as pending")                                             \
    IRQ_CONTROLLER_ATTR("active", V_UINT, VAL_HEX | VAL_VOLATILE, irq_ctrl_attr_active, (OPS),                         \
                        "Requests that survive masking and drive the CPU line")                                        \
    IRQ_CONTROLLER_ATTR("ipl", V_UINT, VAL_VOLATILE, irq_ctrl_attr_ipl, (OPS),                                         \
                        "CPU interrupt level asserted now (0 = none)")                                                 \
    IRQ_CONTROLLER_ATTR("levels", V_LIST, VAL_VOLATILE, irq_ctrl_attr_levels, (OPS),                                   \
                        "Per-CPU-level source bits: [{ipl, sources}], empty if the chip has no level map")

#ifdef __cplusplus
}
#endif

#endif // GS_IRQ_CONTROLLER_H
