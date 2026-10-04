// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_config.h
// The machine-description tree and the boot document.
//
// catalog.profile publishes each model's configuration space as one tree --
// options, floppy positions, storage buses, expansion slots, the cards that
// fit them, the display devices and the monitors they take -- and the
// default configuration as a document that mirrors it node by node
// (catalog.default_config).  machine.boot takes such a document (config=) and,
// for every older caller, the named arguments it rewrites into one; both are
// normalised against the model's defaults, validated, and turned into the
// construction arguments (machine_build_opts_t) here.

#ifndef MACHINE_CONFIG_H
#define MACHINE_CONFIG_H

#include "machine.h"
#include "value.h"

#include <stdbool.h>
#include <stddef.h>

// A slot's id in the tree and the document: "nubus_c" (NuBus slot ID $C) or
// "pci_2" (logical PCI slot 2).
void machine_slot_id(bool pci, int slot, char *buf, size_t len);

// Put the machine-description tree's keys (options, floppies, storage, slots,
// cards, displays, monitors, defaults) into a profile map under construction.
void machine_config_put_tree(const hw_profile_t *p, value_map_builder_t *b);

// The model's default configuration, as the document machine.boot takes.
value_t machine_config_defaults(const hw_profile_t *p);

// Resolve everything a boot needs besides the ROM -- memory, options,
// floppies, storage, the startup device, the expansion cards and the display
// devices -- from the configuration document `config` (a V_MAP, or NULL when
// the caller gave none) and the named arguments in `legacy`, into `out`.
// Every check runs here, before the running machine is touched; V_ERROR
// names the node on a rejection.
value_t machine_config_resolve(const hw_profile_t *p, const value_t *config, const boot_config_t *legacy,
                               machine_build_opts_t *out);

// The device a storage entry names, as the attach verbs address it: the bus's
// media bus and the unit there.  False for a bus the model does not have.
bool machine_storage_media_bay(const hw_profile_t *p, const char *bus_id, int unit, media_bay_t *out);

// The storage bus `id` of the model, or NULL.
const storage_bus_decl_t *machine_storage_bus(const hw_profile_t *p, const char *id);

#endif // MACHINE_CONFIG_H
