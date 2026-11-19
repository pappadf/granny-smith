// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// card_registry.c: the registry helpers NuBus and PCI share.
//
// Runs them over a registry of a stand-in kind laid out as both buses' are:
// `const char *id` first, anything after it.

#include "card_registry.h"
#include "test_assert.h"

#include <stdio.h>
#include <string.h>

// A kind as the buses declare one: the id leads.
typedef struct stub_kind {
    const char *id;
    int tag;
} stub_kind_t;

static const stub_kind_t s_824gc = {.id = "824gc", .tag = 1};
static const stub_kind_t s_mdc = {.id = "mdc_8_24", .tag = 2};
static const stub_kind_t *const s_registry[] = {&s_824gc, &s_mdc, NULL};

// The registry as the helpers take it.
static const void *const *reg(void) {
    return (const void *const *)s_registry;
}

// Exact ids find their kind; anything else, NULL and "" included, finds none.
TEST(find_by_id) {
    ASSERT_TRUE(card_registry_find(reg(), "824gc") == &s_824gc);
    ASSERT_TRUE(card_registry_find(reg(), "mdc_8_24") == &s_mdc);
    ASSERT_TRUE(card_registry_find(reg(), "mdc_824") == NULL);
    ASSERT_TRUE(card_registry_find(reg(), "") == NULL);
    ASSERT_TRUE(card_registry_find(reg(), NULL) == NULL);
}

// A near miss by underscores alone earns the id it meant.
TEST(suggest_ignores_underscores) {
    ASSERT_TRUE(strcmp(card_registry_suggest(reg(), "8_24gc"), "824gc") == 0);
    ASSERT_TRUE(strcmp(card_registry_suggest(reg(), "mdc_824"), "mdc_8_24") == 0);
    ASSERT_TRUE(strcmp(card_registry_suggest(reg(), "_mdc8_24_"), "mdc_8_24") == 0);
    ASSERT_TRUE(card_registry_suggest(reg(), "mdc_8_25") == NULL);
    ASSERT_TRUE(card_registry_suggest(reg(), "") == NULL);
}

// A refusal formats its reason and says no.
TEST(refuse_formats) {
    char why[32];
    ASSERT_TRUE(!card_refuse(why, sizeof why, "slot %d: %s", 3, "no"));
    ASSERT_TRUE(strcmp(why, "slot 3: no") == 0);
}

int main(void) {
    RUN(find_by_id);
    RUN(suggest_ignores_underscores);
    RUN(refuse_formats);
    fprintf(stderr, "card_registry: all tests passed\n");
    return 0;
}
