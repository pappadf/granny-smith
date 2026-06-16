// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// symbols.c
// See symbols.h.  Lookup is linear because in practice a single CODE
// segment carries a few hundred symbols at most; the dominant cost is
// per-instruction so a tighter index would be premature optimisation.

#include "symbols.h"

#include <stdlib.h>
#include <string.h>

void re_symbols_init(re_symbols_t *t) {
    if (!t)
        return;
    t->items = NULL;
    t->count = 0;
    t->capacity = 0;
}

void re_symbols_free(re_symbols_t *t) {
    if (!t)
        return;
    for (size_t i = 0; i < t->count; i++)
        free(t->items[i].name);
    free(t->items);
    t->items = NULL;
    t->count = t->capacity = 0;
}

void re_symbols_add(re_symbols_t *t, int16_t code_id, uint32_t addr, const char *name, const char *source) {
    if (!t || !name)
        return;
    // De-dup by (code_id, addr, name) so the same entry from multiple passes
    // doesn't double-list.  Source-tag conflicts keep the first one wins
    // ordering: MacsBug names take priority because passes run in that order.
    for (size_t i = 0; i < t->count; i++) {
        if (t->items[i].code_id == code_id && t->items[i].addr == addr && strcmp(t->items[i].name, name) == 0)
            return;
    }
    if (t->count == t->capacity) {
        size_t new_cap = t->capacity ? t->capacity * 2 : 16;
        re_symbol_t *new_items = realloc(t->items, new_cap * sizeof(*new_items));
        if (!new_items)
            return;
        t->items = new_items;
        t->capacity = new_cap;
    }
    t->items[t->count].addr = addr;
    t->items[t->count].name = strdup(name);
    t->items[t->count].source = source;
    t->items[t->count].code_id = code_id;
    if (!t->items[t->count].name)
        return;
    t->count++;
}

const re_symbol_t *re_symbols_find(const re_symbols_t *t, int16_t code_id, uint32_t addr) {
    if (!t)
        return NULL;
    for (size_t i = 0; i < t->count; i++) {
        if (t->items[i].code_id == code_id && t->items[i].addr == addr)
            return &t->items[i];
    }
    return NULL;
}

// qsort comparator for symbols.txt: sort by (code_id ASC, addr ASC).
static int cmp_symbol(const void *a, const void *b) {
    const re_symbol_t *sa = a, *sb = b;
    if (sa->code_id != sb->code_id)
        return (int)sa->code_id - (int)sb->code_id;
    if (sa->addr < sb->addr)
        return -1;
    if (sa->addr > sb->addr)
        return 1;
    return 0;
}

void re_symbols_write_txt(const re_symbols_t *t, FILE *fp) {
    if (!t || !fp)
        return;
    fprintf(fp, "; symbols recovered by re.dump\n");
    fprintf(fp, "; format: <code_id> <hex_addr> <source> <name>\n");
    fprintf(fp, "; source = macsbug | jumptable | boundary\n");
    re_symbol_t *copy = NULL;
    if (t->count > 0) {
        copy = malloc(t->count * sizeof(*copy));
        if (copy) {
            memcpy(copy, t->items, t->count * sizeof(*copy));
            qsort(copy, t->count, sizeof(*copy), cmp_symbol);
        }
    }
    const re_symbol_t *src = copy ? copy : t->items;
    for (size_t i = 0; i < t->count; i++) {
        fprintf(fp, "CODE-%04d  $%08X  %-9s  %s\n", (int)src[i].code_id, src[i].addr,
                src[i].source ? src[i].source : "?", src[i].name ? src[i].name : "");
    }
    free(copy);
}
