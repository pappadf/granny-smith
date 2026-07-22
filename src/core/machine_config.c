// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// machine_config.c
// Storage and object-model surface for the built-from record
// (proposal-named-args-boot-config §4.2).

#include "machine_config.h"

#include "json_encode.h"
#include "object.h"
#include "value.h"

#include <stdio.h>
#include <string.h>

// Process-global record — one live machine at a time, like the machine
// object itself.
static machine_config_record_t s_record;

const machine_config_record_t *machine_config_record(void) {
    return &s_record;
}

machine_config_record_t *machine_config_record_mut(void) {
    return &s_record;
}

void machine_config_reset_vroms(void) {
    memset(s_record.vroms, 0, sizeof(s_record.vroms));
    s_record.n_vroms = 0;
}

void machine_config_note_vrom(const char *card_id, const char *path, uint32_t crc, bool explicit_pick) {
    if (s_record.n_vroms >= MC_MAX_VROMS)
        return;
    machine_config_vrom_t *e = &s_record.vroms[s_record.n_vroms++];
    snprintf(e->card_id, sizeof(e->card_id), "%s", card_id ? card_id : "");
    snprintf(e->path, sizeof(e->path), "%s", path ? path : "");
    e->crc = crc;
    e->explicit_pick = explicit_pick;
}

void machine_config_note_rom(const char *path, uint32_t crc) {
    if (path && *path)
        snprintf(s_record.rom, sizeof(s_record.rom), "%s", path);
    s_record.rom_crc = crc;
}

// === machine.config object ==================================================

static value_t cfg_str(const char *s) {
    return val_str(s ? s : "");
}

static value_t cfg_attr_model(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return cfg_str(s_record.model);
}
static value_t cfg_attr_ram(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_uint(4, s_record.ram_kb);
}
static value_t cfg_attr_rom(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return cfg_str(s_record.rom);
}
static value_t cfg_attr_rom_crc(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    value_t v = val_uint(4, s_record.rom_crc);
    v.flags |= VAL_HEX;
    return v;
}
static value_t cfg_attr_rom2(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return cfg_str(s_record.rom2);
}
static value_t cfg_attr_vrom(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return cfg_str(s_record.vrom);
}
static value_t cfg_attr_video_card(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return cfg_str(s_record.video_card);
}
static value_t cfg_attr_video_sense(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_int(s_record.video_sense);
}
static value_t cfg_attr_video_mode(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return cfg_str(s_record.video_mode);
}
static value_t cfg_attr_created(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return cfg_str(s_record.created);
}
static value_t cfg_attr_valid(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    return val_bool(s_record.valid);
}

// `machine.config.vroms` — JSON array of the resolved declaration-ROM
// picks: [{card_id, path, crc, explicit}, ...] in load order.
static value_t cfg_attr_vroms(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    json_builder_t *b = json_builder_new();
    if (!b)
        return val_err("machine.config.vroms: out of memory");
    json_open_arr(b);
    for (int i = 0; i < s_record.n_vroms; i++) {
        const machine_config_vrom_t *e = &s_record.vroms[i];
        json_open_obj(b);
        json_key(b, "card_id");
        json_str(b, e->card_id);
        json_key(b, "path");
        json_str(b, e->path);
        json_key(b, "crc");
        char hex[16];
        snprintf(hex, sizeof(hex), "0x%08x", e->crc);
        json_str(b, hex);
        json_key(b, "explicit");
        json_bool(b, e->explicit_pick);
        json_close_obj(b);
    }
    json_close_arr(b);
    return json_finish(b);
}

static const member_t config_members[] = {
    {.kind = M_ATTR,
     .name = "valid",
     .doc = "True once a machine was built through the boot document",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = cfg_attr_valid, .set = NULL}       },
    {.kind = M_ATTR,
     .name = "model",
     .doc = "Model id the machine was built from",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = cfg_attr_model, .set = NULL}     },
    {.kind = M_ATTR,
     .name = "ram",
     .doc = "RAM in KB the machine was built with",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = cfg_attr_ram, .set = NULL}         },
    {.kind = M_ATTR,
     .name = "rom",
     .doc = "ROM file path staged at boot (updated by rom.load)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = cfg_attr_rom, .set = NULL}       },
    {.kind = M_ATTR,
     .name = "rom_crc",
     .doc = "Content checksum of the installed ROM",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = cfg_attr_rom_crc, .set = NULL}     },
    {.kind = M_ATTR,
     .name = "rom2",
     .doc = "Lisa second ROM chip path (empty = single-file ROM)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = cfg_attr_rom2, .set = NULL}      },
    {.kind = M_ATTR,
     .name = "vrom",
     .doc = "Explicit vrom= pick (empty = auto-resolved from offers)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = cfg_attr_vrom, .set = NULL}      },
    {.kind = M_ATTR,
     .name = "vroms",
     .doc = "JSON array of resolved declaration-ROM picks per card",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = cfg_attr_vroms, .set = NULL}     },
    {.kind = M_ATTR,
     .name = "video_card",
     .doc = "Wildcard-socket card id from the boot document",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = cfg_attr_video_card, .set = NULL}},
    {.kind = M_ATTR,
     .name = "video_sense",
     .doc = "Monitor sense from the boot document (-1 = unset)",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = cfg_attr_video_sense, .set = NULL}  },
    {.kind = M_ATTR,
     .name = "video_mode",
     .doc = "Video-mode id from the boot document (empty = card default)",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = cfg_attr_video_mode, .set = NULL}},
    {.kind = M_ATTR,
     .name = "created",
     .doc = "Boot timestamp (ISO8601 UTC), stamped by the emulator",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = cfg_attr_created, .set = NULL}   },
};

static const class_desc_t config_class = {
    .name = "config",
    .members = config_members,
    .n_members = sizeof(config_members) / sizeof(config_members[0]),
};

static struct object *s_config_object = NULL;

void machine_config_object_init(struct object *machine_obj) {
    if (s_config_object || !machine_obj)
        return;
    s_config_object = object_new(&config_class, NULL, "config");
    if (s_config_object) {
        object_set_label(s_config_object, "Built-from configuration");
        object_set_order(s_config_object, 96);
        object_attach(machine_obj, s_config_object);
    }
}
