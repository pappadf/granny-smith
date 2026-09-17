// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
//
// offer_registry.c
// See offer_registry.h.  One copy of the registry both ROM layers used to
// carry; identification stays behind the caller's `identify` callback.

#include "offer_registry.h"

#include "log.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("rom");

void offer_registry_add(offer_registry_t *r, const char *path, bool explicit_pick) {
    if (!r || !r->identify || !path || !*path)
        return;

    uint32_t crc = 0;
    size_t size = 0;
    const char *card_id = NULL;
    if (!r->identify(path, &crc, &size, &card_id) || !card_id)
        return; // not ours; the identifier logged which kind of stray it was

    // Idempotent by content: one entry per CRC.
    offer_entry_t *e = NULL;
    for (size_t i = 0; i < r->count; i++) {
        if (r->entries[i].crc == crc) {
            e = &r->entries[i];
            break;
        }
    }
    if (!e) {
        if (r->count == r->cap) {
            size_t cap = r->cap ? r->cap * 2 : 8;
            offer_entry_t *grown = realloc(r->entries, cap * sizeof(*grown));
            if (!grown)
                return;
            r->entries = grown;
            r->cap = cap;
        }
        e = &r->entries[r->count];
        memset(e, 0, sizeof(*e));
        r->count++;
    }
    char *dup = strdup(path);
    if (!dup) {
        // A fresh entry with no path is useless -- roll the append back.
        if (!e->path)
            r->count--;
        return;
    }
    free(e->path);
    e->path = dup;
    e->crc = crc;
    e->size = size;
    e->card_id = card_id;
    if (explicit_pick) {
        // Only one explicit pick at a time -- the latest wins.
        for (size_t i = 0; i < r->count; i++)
            r->entries[i].explicit_pick = false;
        e->explicit_pick = true;
    }
    LOG(2, "%s: '%s' provides card '%s' (crc $%08X)%s", r->tag, path, e->card_id, e->crc,
        explicit_pick ? " [explicit]" : "");
}

void offer_registry_clear(offer_registry_t *r) {
    if (!r)
        return;
    for (size_t i = 0; i < r->count; i++)
        free(r->entries[i].path);
    free(r->entries);
    r->entries = NULL;
    r->count = 0;
    r->cap = 0;
}

const char *offer_registry_find(const offer_registry_t *r, const char *card_id, int idx, size_t *out_size) {
    if (!r || !card_id)
        return NULL;
    // Pass 0: the explicit pick (at most one entry carries the flag).
    for (size_t i = 0; i < r->count; i++) {
        if (!r->entries[i].explicit_pick || strcmp(r->entries[i].card_id, card_id) != 0)
            continue;
        if (idx-- == 0) {
            if (out_size)
                *out_size = r->entries[i].size;
            return r->entries[i].path;
        }
    }
    // Passes 1..2: catalog order, preferred rows first.  One offer per CRC
    // (registry invariant), so each row yields at most one candidate.
    if (!r->catalog.row)
        return NULL;
    for (int want_preferred = 1; want_preferred >= 0; want_preferred--) {
        for (size_t row = 0; row < r->catalog.count; row++) {
            const char *row_card = NULL;
            uint32_t row_crc = 0;
            bool preferred = false;
            r->catalog.row(row, &row_card, &row_crc, &preferred);
            if (preferred != (bool)want_preferred || !row_card || strcmp(row_card, card_id) != 0)
                continue;
            for (size_t i = 0; i < r->count; i++) {
                if (r->entries[i].crc != row_crc || r->entries[i].explicit_pick)
                    continue; // the explicit entry was already yielded in pass 0
                if (idx-- == 0) {
                    if (out_size)
                        *out_size = r->entries[i].size;
                    return r->entries[i].path;
                }
            }
        }
    }
    return NULL;
}

bool offer_registry_info(const offer_registry_t *r, const char *path, uint32_t *out_crc, bool *out_explicit) {
    if (!r || !path)
        return false;
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->entries[i].path, path) != 0)
            continue;
        if (out_crc)
            *out_crc = r->entries[i].crc;
        if (out_explicit)
            *out_explicit = r->entries[i].explicit_pick;
        return true;
    }
    return false;
}

bool offer_registry_catalogued(const offer_registry_t *r, const char *card_id) {
    if (!r || !r->catalog.row || !card_id || !*card_id)
        return false;
    for (size_t row = 0; row < r->catalog.count; row++) {
        const char *row_card = NULL;
        uint32_t crc = 0;
        bool preferred = false;
        r->catalog.row(row, &row_card, &crc, &preferred);
        if (row_card && strcmp(row_card, card_id) == 0)
            return true;
    }
    return false;
}

bool offer_registry_resolvable(const offer_registry_t *r, const char *card_id) {
    return offer_registry_find(r, card_id, 0, NULL) != NULL;
}
