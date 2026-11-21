// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// card_registry.c
// The card-kind registry helpers NuBus and PCI share (see card_registry.h).

#include "card_registry.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Static Helpers
// ============================================================================

// The id of registry entry `kind`: its first member.
static const char *kind_id(const void *kind) {
    return *(const char *const *)kind;
}

// Compare two ids ignoring underscores — "8_24gc" for "824gc", "mdc_824"
// for "mdc_8_24": a typo that close deserves a suggestion.
static bool ids_match_sans_underscores(const char *a, const char *b) {
    while (*a == '_')
        a++;
    while (*b == '_')
        b++;
    while (*a && *b) {
        if (*a != *b)
            return false;
        a++;
        b++;
        while (*a == '_')
            a++;
        while (*b == '_')
            b++;
    }
    return *a == '\0' && *b == '\0';
}

// ============================================================================
// Operations (Public API)
// ============================================================================

const void *card_registry_find(const void *const *registry, const char *id) {
    // No kind has an empty id, so "" is refused like NULL on both buses
    if (!registry || !id || !*id)
        return NULL;
    for (const void *const *p = registry; *p; p++) {
        if (strcmp(kind_id(*p), id) == 0)
            return *p;
    }
    return NULL;
}

const char *card_registry_suggest(const void *const *registry, const char *id) {
    if (!registry || !id || !*id)
        return NULL;
    for (const void *const *p = registry; *p; p++) {
        if (ids_match_sans_underscores(kind_id(*p), id))
            return kind_id(*p);
    }
    return NULL;
}

value_t card_registry_list(const void *const *registry, const char *what) {
    size_t n = 0;
    for (const void *const *p = registry; p && *p; p++)
        n++;
    if (n == 0)
        return val_list(NULL, 0);
    value_t *items = (value_t *)calloc(n, sizeof(value_t));
    if (!items)
        return val_err("%s: out of memory", what);
    size_t i = 0;
    for (const void *const *p = registry; *p; p++)
        items[i++] = val_str(kind_id(*p));
    return val_list(items, n);
}

int card_slot_table_check(slot_opts_t *table, int n_slots, card_entry_check_fn check, const void *slots,
                          const char *model, char *why, size_t why_len) {
    for (int n = 0; n < n_slots; n++) {
        slot_opts_t *e = &table[n];
        // An entry whose strings or counts are out of range is refused before reading it
        if (!slot_opts_sanitize(e, n)) {
            if (why_len)
                why[0] = '\0';
            return n;
        }
        if (!e->card[0])
            continue;
        // The ROM path is a boot-time input; a restore's ROM comes from its block
        slot_opts_t probe = *e;
        probe.rom[0] = '\0';
        if (!check(slots, model, &probe, false, why, why_len))
            return n;
    }
    return -1;
}

bool card_refuse(char *why, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, len, fmt, ap);
    va_end(ap);
    return false;
}
