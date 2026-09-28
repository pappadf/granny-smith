// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unit tests for the shared half of an interrupt-controller node.
//
// The point of irq_controller.h is that five different parts -- OSS, RBV,
// PSC, AMIC, Grand Central -- answer the same four questions the same way,
// so a script that does not know which machine it is on can still ask who is
// shouting and at what level.  These tests pin that contract against a toy
// chip: the defaults, the per-chip override, and the level map's indexing.
// Each real chip's own half is exercised by its family's integration suite.

#include "irq_controller.h"
#include "object.h"
#include "test_assert.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>

// === A toy controller =======================================================

typedef struct toy {
    uint32_t pending, enabled, active;
    int ipl;
    int levels;
    uint32_t level_bits[4];
} toy_t;

static uint32_t toy_pending(void *ctx) {
    return ((toy_t *)ctx)->pending;
}
static uint32_t toy_enabled(void *ctx) {
    return ((toy_t *)ctx)->enabled;
}
static uint32_t toy_active(void *ctx) {
    return ((toy_t *)ctx)->active;
}
static int toy_ipl(void *ctx) {
    return ((toy_t *)ctx)->ipl;
}
static int toy_level_count(void *ctx) {
    return ((toy_t *)ctx)->levels;
}
static uint32_t toy_level(void *ctx, int i) {
    return ((toy_t *)ctx)->level_bits[i];
}

// The ordinary shape: no `active` override, so the shared getter derives it,
// and no level map.
static const irq_controller_ops_t toy_plain_ops = {
    .chip = "TOY",
    .pending = toy_pending,
    .enabled = toy_enabled,
    .ipl = toy_ipl,
};

// A chip whose line is NOT `pending & enabled` (Grand Central in mode 1 is
// the real one) and which routes sources to CPU levels starting at IPL 3
// (the PSC's L3-L6 file is the real one).
static const irq_controller_ops_t toy_full_ops = {
    .chip = "TOY-FULL",
    .pending = toy_pending,
    .enabled = toy_enabled,
    .active = toy_active,
    .ipl = toy_ipl,
    .level_count = toy_level_count,
    .level = toy_level,
    .level_base = 3,
};

static const member_t toy_plain_members[] = {IRQ_CONTROLLER_MEMBERS(&toy_plain_ops)};
static const member_t toy_full_members[] = {IRQ_CONTROLLER_MEMBERS(&toy_full_ops)};

static const class_desc_t toy_plain_class = {.name = "irq_controller",
                                             .members = toy_plain_members,
                                             .n_members = sizeof(toy_plain_members) / sizeof(toy_plain_members[0])};
static const class_desc_t toy_full_class = {.name = "irq_controller",
                                            .members = toy_full_members,
                                            .n_members = sizeof(toy_full_members) / sizeof(toy_full_members[0])};

// === Helpers ================================================================

static value_t get(struct object *o, const char *name) {
    const class_desc_t *cls = object_class(o);
    for (size_t i = 0; i < cls->n_members; i++) {
        if (strcmp(cls->members[i].name, name) == 0)
            return cls->members[i].attr.get(o, &cls->members[i]);
    }
    return val_err("no member '%s'", name);
}

static uint64_t get_uint(struct object *o, const char *name) {
    value_t v = get(o, name);
    ASSERT_EQ_INT(v.kind, V_UINT);
    uint64_t u = v.u;
    value_free(&v);
    return u;
}

// === Tests ==================================================================

// Five members, in a fixed order, on every controller: that fixed shape is
// the whole reason the macro exists.
TEST(test_shared_members_are_the_same_on_every_controller) {
    ASSERT_EQ_INT((int)(sizeof(toy_plain_members) / sizeof(toy_plain_members[0])), 6);
    static const char *const want[] = {"chip", "pending", "enabled", "active", "ipl", "levels"};
    for (int i = 0; i < 6; i++) {
        ASSERT_TRUE(strcmp(toy_plain_members[i].name, want[i]) == 0);
        ASSERT_EQ_INT(toy_plain_members[i].kind, M_ATTR);
        ASSERT_TRUE((toy_plain_members[i].flags & VAL_RO) != 0); // never writable
        ASSERT_TRUE(toy_full_members[i].attr.get == toy_plain_members[i].attr.get);
    }
}

TEST(test_active_defaults_to_pending_and_enabled) {
    toy_t toy = {.pending = 0x00F0u, .enabled = 0x0330u, .ipl = 4};
    struct object *o = object_new(&toy_plain_class, &toy, "toy");
    ASSERT_TRUE(o != NULL);

    value_t chip = get(o, "chip");
    ASSERT_EQ_INT(chip.kind, V_STRING);
    ASSERT_TRUE(strcmp(chip.s, "TOY") == 0);
    value_free(&chip);

    ASSERT_EQ_INT((int)get_uint(o, "pending"), 0x00F0);
    ASSERT_EQ_INT((int)get_uint(o, "enabled"), 0x0330);
    ASSERT_EQ_INT((int)get_uint(o, "active"), 0x0030); // the intersection
    ASSERT_EQ_INT((int)get_uint(o, "ipl"), 4);

    // No level map is a fact about the chip, not an error.
    value_t lv = get(o, "levels");
    ASSERT_EQ_INT(lv.kind, V_LIST);
    ASSERT_EQ_INT((int)lv.list.len, 0);
    value_free(&lv);

    object_delete(o);
}

// A chip whose hardware summary is not the intersection must be able to say
// so, or its node lies about which sources are driving the CPU line.
TEST(test_active_override_beats_the_default) {
    toy_t toy = {.pending = 0x00F0u, .enabled = 0x0330u, .active = 0x8001u, .ipl = 1};
    struct object *o = object_new(&toy_full_class, &toy, "toy");
    ASSERT_TRUE(o != NULL);
    ASSERT_EQ_INT((int)get_uint(o, "active"), 0x8001);
    object_delete(o);
}

// `levels` reports the CPU level, not the array index: a reader must not
// have to know that the PSC's index 0 is IPL 3 and the OSS's is IPL 1.
TEST(test_levels_report_cpu_levels_not_array_indices) {
    toy_t toy = {
        .levels = 4, .level_bits = {0x1u, 0x2u, 0x4u, 0x8u}
    };
    struct object *o = object_new(&toy_full_class, &toy, "toy");
    ASSERT_TRUE(o != NULL);

    value_t lv = get(o, "levels");
    ASSERT_EQ_INT(lv.kind, V_LIST);
    ASSERT_EQ_INT((int)lv.list.len, 4);
    for (int i = 0; i < 4; i++) {
        value_t *entry = &lv.list.items[i];
        ASSERT_EQ_INT(entry->kind, V_MAP);
        const value_t *ipl = value_map_get(entry, "ipl");
        const value_t *src = value_map_get(entry, "sources");
        ASSERT_TRUE(ipl != NULL && src != NULL);
        ASSERT_EQ_INT((int)ipl->u, 3 + i); // level_base = 3
        ASSERT_EQ_INT((int)src->u, 1 << i);
    }
    value_free(&lv);
    object_delete(o);
}

int main(void) {
    RUN(test_shared_members_are_the_same_on_every_controller);
    RUN(test_active_defaults_to_pending_and_enabled);
    RUN(test_levels_report_cpu_levels_not_array_indices);
    RUN(test_active_override_beats_the_default);
    printf("[PASS] All irq_controller tests passed\n");
    return 0;
}
