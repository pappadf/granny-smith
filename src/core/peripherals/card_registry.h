// SPDX-License-Identifier: MIT
// Copyright (c) pappadf
// card_registry.h
// The pieces of a card-kind registry NuBus (nubus.c) and PCI (pci.c) share:
// find a kind by id, suggest the id a near-miss typo meant, and format a
// slot-check refusal.
//
// Both buses keep their kinds in an explicit, NULL-terminated array of
// pointers to a kind struct whose FIRST member is `const char *id`
// (nubus_card_kind_t, pci_card_kind_t), so one body serves both: a pointer
// to a struct converts to a pointer to its first member.  Each bus wraps
// these in its typed nubus_card_* / pci_card_* entry points.

#ifndef CARD_REGISTRY_H
#define CARD_REGISTRY_H

// === Includes ===

#include <stdbool.h>
#include <stddef.h>

// === Operations ===

// The kind in `registry` whose id is `id`, or NULL (also for a NULL or empty
// id).  `registry` is a NULL-terminated array of kind pointers, each kind
// starting with its `const char *id`.
const void *card_registry_find(const void *const *registry, const char *id);

// For an unknown-id error: the registered id that differs from `id` only by
// underscores ("8_24gc" for "824gc"), or NULL.
const char *card_registry_suggest(const void *const *registry, const char *id);

// Formats a slot-check refusal into `why` and returns false.
__attribute__((format(printf, 3, 4))) bool card_refuse(char *why, size_t len, const char *fmt, ...);

#endif // CARD_REGISTRY_H
