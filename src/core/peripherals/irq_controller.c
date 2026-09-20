// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// irq_controller.c — the shared half of every interrupt-controller node.
// See irq_controller.h for why the shape is two-part.

#include "irq_controller.h"

#include <stdlib.h>

// The ops table rides in the member's user_data; instance_data is the chip.
static const irq_controller_ops_t *ops_of(const member_t *m) {
    return (const irq_controller_ops_t *)m->attr.user_data;
}

static value_t hexed(uint32_t v) {
    value_t out = val_uint(4, v);
    out.flags |= VAL_HEX;
    return out;
}

value_t irq_ctrl_attr_chip(struct object *self, const member_t *m) {
    (void)self;
    const irq_controller_ops_t *ops = ops_of(m);
    return val_str(ops && ops->chip ? ops->chip : "");
}

value_t irq_ctrl_attr_pending(struct object *self, const member_t *m) {
    const irq_controller_ops_t *ops = ops_of(m);
    if (!ops || !ops->pending)
        return val_err("irq controller: no pending accessor");
    return hexed(ops->pending(object_data(self)));
}

value_t irq_ctrl_attr_enabled(struct object *self, const member_t *m) {
    const irq_controller_ops_t *ops = ops_of(m);
    if (!ops || !ops->enabled)
        return val_err("irq controller: no enabled accessor");
    return hexed(ops->enabled(object_data(self)));
}

// `pending & enabled` is right for every chip whose line is a masked OR.
// Grand Central in mode 1 is not one -- its latch is the summary -- so a
// chip may answer for itself.
value_t irq_ctrl_attr_active(struct object *self, const member_t *m) {
    const irq_controller_ops_t *ops = ops_of(m);
    void *ctx = object_data(self);
    if (!ops)
        return val_err("irq controller: no ops");
    if (ops->active)
        return hexed(ops->active(ctx));
    if (!ops->pending || !ops->enabled)
        return val_err("irq controller: no active accessor");
    return hexed(ops->pending(ctx) & ops->enabled(ctx));
}

value_t irq_ctrl_attr_ipl(struct object *self, const member_t *m) {
    const irq_controller_ops_t *ops = ops_of(m);
    if (!ops || !ops->ipl)
        return val_err("irq controller: no ipl accessor");
    int ipl = ops->ipl(object_data(self));
    return val_uint(1, ipl < 0 ? 0 : (uint64_t)ipl);
}

// [{ipl, sources}] — one entry per CPU level the chip routes to, so the map
// reads the same on a chip whose index 0 means IPL 1 (OSS) and one whose
// index 0 means IPL 3 (PSC).  Empty list when the chip has no level map,
// which is a fact about the chip and not an error.
value_t irq_ctrl_attr_levels(struct object *self, const member_t *m) {
    const irq_controller_ops_t *ops = ops_of(m);
    void *ctx = object_data(self);
    if (!ops || !ops->level_count || !ops->level)
        return val_list(NULL, 0);
    int n = ops->level_count(ctx);
    if (n <= 0)
        return val_list(NULL, 0);
    value_t *items = (value_t *)calloc((size_t)n, sizeof(value_t));
    if (!items)
        return val_err("irq controller: out of memory");
    for (int i = 0; i < n; i++) {
        value_map_builder_t *b = val_map_new();
        val_map_put(b, "ipl", val_uint(1, (uint64_t)(ops->level_base + i)));
        val_map_put(b, "sources", hexed(ops->level(ctx, i)));
        items[i] = val_map_finish(b);
    }
    return val_list(items, (size_t)n);
}
