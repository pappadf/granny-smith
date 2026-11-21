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

// The list names every registered id, in registry order.
TEST(list_names_every_id) {
    value_t v = card_registry_list(reg(), "catalog.test_cards");
    ASSERT_TRUE(v.kind == VK_LIST && v.list.len == 2);
    ASSERT_TRUE(strcmp(v.list.items[0].s, "824gc") == 0);
    ASSERT_TRUE(strcmp(v.list.items[1].s, "mdc_8_24") == 0);
    value_free(&v);
}

// Whether the stand-in check below ever saw a ROM path.
static bool s_saw_rom;

// A stand-in entry check: refuses the card "bad", accepts the rest.
static bool stub_check(const void *slots, const char *model, const slot_opts_t *e, bool roms_final, char *why,
                       size_t why_len) {
    (void)slots;
    (void)model;
    (void)roms_final;
    s_saw_rom |= e->rom[0] != '\0';
    if (strcmp(e->card, "bad") == 0)
        return card_refuse(why, why_len, "card '%s' refused", e->card);
    return true;
}

// A restored table: good entries pass without their ROM path, the first bad
// one is named, and a malformed entry is refused before the check sees it.
TEST(slot_table_check) {
    slot_opts_t t[4];
    memset(t, 0, sizeof t);
    char why[64];
    s_saw_rom = false;
    t[1].slot = 1;
    strcpy(t[1].card, "824gc");
    strcpy(t[1].rom, "some/file.rom");
    ASSERT_TRUE(card_slot_table_check(t, 4, stub_check, NULL, "m", why, sizeof why) == -1);
    ASSERT_TRUE(!s_saw_rom);
    t[3].slot = 3;
    strcpy(t[3].card, "bad");
    ASSERT_TRUE(card_slot_table_check(t, 4, stub_check, NULL, "m", why, sizeof why) == 3);
    ASSERT_TRUE(strcmp(why, "card 'bad' refused") == 0);
    // An entry seating a card under another slot's number is malformed
    t[2].slot = 1;
    strcpy(t[2].card, "824gc");
    ASSERT_TRUE(card_slot_table_check(t, 4, stub_check, NULL, "m", why, sizeof why) == 2);
    ASSERT_TRUE(why[0] == '\0');
}

int main(void) {
    RUN(find_by_id);
    RUN(suggest_ignores_underscores);
    RUN(refuse_formats);
    RUN(list_names_every_id);
    RUN(slot_table_check);
    fprintf(stderr, "card_registry: all tests passed\n");
    return 0;
}
