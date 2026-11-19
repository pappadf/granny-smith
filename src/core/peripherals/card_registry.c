// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// card_registry.c
// The card-kind registry helpers NuBus and PCI share (see card_registry.h).

#include "card_registry.h"

#include <stdarg.h>
#include <stdio.h>
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

bool card_refuse(char *why, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, len, fmt, ap);
    va_end(ap);
    return false;
}
