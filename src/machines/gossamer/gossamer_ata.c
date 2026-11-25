// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gossamer_ata.c
// Heathrow's two ATA cells (core/peripherals/ata.c) on the beige G3: the
// register decode, the interrupt and DMA wiring, the feature-control gates,
// the ATAPI back end, media transit and the machine.ata object.
//
// Wiring (the Rev C device tree: ide@20000 and ide@21000, AAPL,interrupts
// `0d 02` and `0e 03`):
//   cell 0 at +$20000: INTRQ on source $0D, DBDMA channel 11 (source 2)
//   cell 1 at +$21000: INTRQ on source $0E, DBDMA channel 12 (source 3)
// Inside a cell the task-file registers sit on 16-byte centres (+$00 data
// .. +$70 status/command), alternate status / device control at +$160, and
// a 32-bit little-endian timing latch at +$200.
//
// Feature-control gates (Linux heathrow.h HRW_*): IDE0_ENABLE bit 5 and
// IDE0_RESET_N bit 6 for cell 0; BAY_IDE_ENABLE bit 3 and IDE1_RESET_N bit
// 23 for cell 1.  A disabled cell floats ($FF); RESET- low holds both its
// devices in reset, and its release posts their signatures.
//
// ATAPI devices are CD-ROMs on the "atapi" SCSI bus (machine.atapi), SCSI
// id = cell * 2 + device: a disc attached there appears on the ATA channel.

#include "gossamer.h"

#include "ata.h"
#include "checkpoint.h"
#include "dbdma.h"
#include "image.h"
#include "log.h"
#include "machine.h"
#include "object.h"
#include "object_order.h"
#include "regfile.h"
#include "scheduler.h"
#include "scsi.h"
#include "system.h"
#include "value.h"

#include <stdio.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("ata");

#define ATA_CELL_SIZE    0x1000u
#define ATA_ALTSTATUS    0x160u
#define ATA_TIMING       0x200u
#define FCR_IDE0_ENABLE  0x00000020u
#define FCR_IDE0_RESET_N 0x00000040u
#define FCR_IDE1_ENABLE  0x00000008u
#define FCR_IDE1_RESET_N 0x00800000u

static const int ata_irq_source[2] = {GOS_INT_ATA0, GOS_INT_ATA1};
static const int ata_dma_chan[2] = {GOS_DMA_ATA0, GOS_DMA_ATA1};

static void ata_irq(void *ctx, bool level) {
    gos_ata_ctx_t *c = (gos_ata_ctx_t *)ctx;
    gos_set_source(c->cfg, ata_irq_source[c->cell], level);
}

// DMA pacing.  The device side of a DMA command is ready the moment the
// command is written, and with nothing to slow it the whole transfer — and
// INTRQ, and the channel's completion interrupt — happened inside the
// guest's command-register store, before the ATA Manager had returned from
// issuing.  Under Mac OS 9.2.1 that ordering leaks the Manager's critical
// section once a session of back-to-back paging reads begins, and the next
// page fault is refused as a fault taken at interrupt time: the bus error
// that stopped every boot from an ATA disk at "Welcome to Mac OS".  The
// port therefore moves a burst per activation and a pump kicks the channel
// on a 10 us cadence while a DMA command is pending (the MESH scheme,
// scsi_mesh.c), so a transfer costs emulated time as it does on the wire.
#define ATA_DMA_BURST   4096
#define ATA_DMA_PUMP_NS 10000.0

static void ata_pump(gos_ata_ctx_t *c, event_callback_t self) {
    gossamer_state_t *st = gos_st(c->cfg);
    if (!st || !st->ata_ready || !ata_dma_pending(&st->ata[c->cell]))
        return; // the transfer is over: the pump stops with it
    if (st->dbdma && dbdma_active(st->dbdma, ata_dma_chan[c->cell]))
        dbdma_kick(st->dbdma, ata_dma_chan[c->cell]);
    if (ata_dma_pending(&st->ata[c->cell]))
        scheduler_new_cpu_event(c->cfg->scheduler, self, c, 0, 0, (uint64_t)ATA_DMA_PUMP_NS);
}

static void ata_pump0(void *source, uint64_t data) {
    (void)data;
    ata_pump((gos_ata_ctx_t *)source, &ata_pump0);
}

static void ata_pump1(void *source, uint64_t data) {
    (void)data;
    ata_pump((gos_ata_ctx_t *)source, &ata_pump1);
}

static const event_callback_t ata_pump_fn[2] = {ata_pump0, ata_pump1};

// The device has data (or room) for a DMA command: arm the cell's pump.
static void ata_kick(void *ctx) {
    gos_ata_ctx_t *c = (gos_ata_ctx_t *)ctx;
    event_callback_t fn = ata_pump_fn[c->cell];
    if (c->cfg->scheduler && !has_event(c->cfg->scheduler, fn))
        scheduler_new_cpu_event(c->cfg->scheduler, fn, c, 0, 0, (uint64_t)ATA_DMA_PUMP_NS);
}

// The cells follow the feature-control register.
void gos_ata_fcr_changed(config_t *cfg, uint32_t old, uint32_t fcr) {
    gossamer_state_t *st = gos_st(cfg);
    if (!st || !st->ata_ready)
        return;
    static const uint32_t en[2] = {FCR_IDE0_ENABLE, FCR_IDE1_ENABLE};
    static const uint32_t rst[2] = {FCR_IDE0_RESET_N, FCR_IDE1_RESET_N};
    for (int c = 0; c < 2; c++) {
        ata_set_enabled(&st->ata[c], (fcr & en[c]) && (fcr & rst[c]));
        if (!(old & rst[c]) && (fcr & rst[c]))
            ata_hard_reset(&st->ata[c]); // RESET- released
    }
}

// ---- register decode ---------------------------------------------------------

static ata_channel_t *cell_of(config_t *cfg, uint32_t off, uint32_t *rel) {
    gossamer_state_t *st = gos_st(cfg);
    if (!st || !st->ata_ready)
        return NULL;
    int c = (int)((off - GOS_HR_ATA0) / ATA_CELL_SIZE);
    if (c < 0 || c > 1)
        return NULL;
    *rel = (off - GOS_HR_ATA0) % ATA_CELL_SIZE;
    return &st->ata[c];
}

// A byte read of a cell: the guest's, or an inspection's (`peek`), which goes
// to the channel's ata_peek* and so changes nothing in it.
static uint8_t cell_read8(config_t *cfg, uint32_t off, bool peek) {
    uint32_t rel;
    ata_channel_t *ch = cell_of(cfg, off, &rel);
    if (!ch)
        return 0xFF;
    if (rel < 0x80u && (rel & 0xFu) == 0)
        return peek ? ata_peek(ch, (int)(rel >> 4)) : ata_read(ch, (int)(rel >> 4));
    if (rel == ATA_ALTSTATUS)
        return peek ? ata_peek_altstatus(ch) : ata_read_altstatus(ch);
    if (rel >= ATA_TIMING && rel < ATA_TIMING + 4u)
        return (uint8_t)(ch->timing >> (8u * (rel - ATA_TIMING)));
    if (!peek)
        LOG(2, "cell %d: read of unassigned +$%03X", ch->index, rel);
    return 0xFF;
}

uint8_t gos_ata_read8(config_t *cfg, uint32_t off) {
    return cell_read8(cfg, off, false);
}

uint8_t gos_ata_peek8(config_t *cfg, uint32_t off) {
    return cell_read8(cfg, off, true);
}

void gos_ata_write8(config_t *cfg, uint32_t off, uint8_t value) {
    uint32_t rel;
    ata_channel_t *ch = cell_of(cfg, off, &rel);
    if (!ch)
        return;
    if (rel < 0x80u && (rel & 0xFu) == 0) {
        ata_write(ch, (int)(rel >> 4), value);
        return;
    }
    if (rel == ATA_ALTSTATUS) {
        ata_write_devctl(ch, value);
        return;
    }
    if (rel >= ATA_TIMING && rel < ATA_TIMING + 4u) {
        le_lane8_set(&ch->timing, rel - ATA_TIMING, value);
        return;
    }
    LOG(2, "cell %d: write of unassigned +$%03X = $%02X", ch->index, rel, value);
}

// The data register is the cell's one 16-bit port: the halfword carries the
// first byte of the stream in its high (lower-addressed) byte.
static uint16_t cell_read16(config_t *cfg, uint32_t off, bool peek) {
    uint32_t rel;
    ata_channel_t *ch = cell_of(cfg, off, &rel);
    if (!ch)
        return 0xFFFF;
    if (rel == 0)
        return peek ? ata_peek_data16(ch) : ata_read_data16(ch);
    return (uint16_t)((cell_read8(cfg, off, peek) << 8) | cell_read8(cfg, off + 1, peek));
}

uint16_t gos_ata_read16(config_t *cfg, uint32_t off) {
    return cell_read16(cfg, off, false);
}

uint16_t gos_ata_peek16(config_t *cfg, uint32_t off) {
    return cell_read16(cfg, off, true);
}

void gos_ata_write16(config_t *cfg, uint32_t off, uint16_t value) {
    uint32_t rel;
    ata_channel_t *ch = cell_of(cfg, off, &rel);
    if (!ch)
        return;
    if (rel == 0) {
        ata_write_data16(ch, value);
        return;
    }
    gos_ata_write8(cfg, off, (uint8_t)(value >> 8));
    gos_ata_write8(cfg, off + 1, (uint8_t)value);
}

// Longword cycles: the timing latch is a little-endian register; anything
// else is the byte cell on lane 0 (the data port, two halfwords).
static uint32_t cell_read32(config_t *cfg, uint32_t off, bool peek) {
    uint32_t rel;
    ata_channel_t *ch = cell_of(cfg, off, &rel);
    if (!ch)
        return 0xFFFFFFFFu;
    if (rel == ATA_TIMING)
        return GOS_LE32(ch->timing);
    if (rel == 0) {
        if (peek)
            return ata_peek_data32(ch);
        uint32_t hi = ata_read_data16(ch);
        return (hi << 16) | ata_read_data16(ch);
    }
    return (uint32_t)cell_read8(cfg, off, peek) << 24;
}

uint32_t gos_ata_read32(config_t *cfg, uint32_t off) {
    return cell_read32(cfg, off, false);
}

uint32_t gos_ata_peek32(config_t *cfg, uint32_t off) {
    return cell_read32(cfg, off, true);
}

void gos_ata_write32(config_t *cfg, uint32_t off, uint32_t value) {
    uint32_t rel;
    ata_channel_t *ch = cell_of(cfg, off, &rel);
    if (!ch)
        return;
    if (rel == ATA_TIMING) {
        ch->timing = GOS_LE32(value);
        return;
    }
    if (rel == 0) {
        ata_write_data16(ch, (uint16_t)(value >> 16));
        ata_write_data16(ch, (uint16_t)value);
        return;
    }
    gos_ata_write8(cfg, off, (uint8_t)(value >> 24));
}

// ---- construction --------------------------------------------------------------

void gos_ata_init(config_t *cfg, checkpoint_t *cp) {
    gossamer_state_t *st = gos_st(cfg);
    st->atapi = scsi_init_named(cfg->scheduler, cp, config_images(cfg), "atapi");
    for (int c = 0; c < 2; c++) {
        ata_channel_t *ch = &st->ata[c];
        ata_channel_init(ch, c);
        gos_ata_ctx_t *ctx = &st->ata_ctx[c];
        *ctx = (gos_ata_ctx_t){.cfg = cfg, .cell = c};
        ata_set_irq(ch, ata_irq, ctx);
        ata_set_dma_kick(ch, ata_kick, ctx);
        scheduler_new_event_type(cfg->scheduler, c ? "ata1" : "ata0", ctx, "dma_pump", ata_pump_fn[c]);
        ata_set_atapi_bus(ch, st->atapi);
        if (cp)
            ata_checkpoint_restore(ch, cp, config_images(cfg));
        dbdma_port_t port = {.out = ata_dma_out, .in = ata_dma_in, .s_bits = NULL, .ctx = ch, .burst = ATA_DMA_BURST};
        dbdma_set_port(st->dbdma, ata_dma_chan[c], &port);
    }
    st->ata_ready = true;
    // The cells follow the current feature-control word (a restore leaves
    // the latched enables as they were).
    uint32_t fcr = st->hr.fcr;
    for (int c = 0; c < 2; c++) {
        bool en = c == 0 ? (fcr & FCR_IDE0_ENABLE) && (fcr & FCR_IDE0_RESET_N)
                         : (fcr & FCR_IDE1_ENABLE) && (fcr & FCR_IDE1_RESET_N);
        ata_set_enabled(&st->ata[c], en);
    }
}

void gos_ata_reset(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (!st || !st->ata_ready)
        return;
    for (int c = 0; c < 2; c++)
        ata_hard_reset(&st->ata[c]);
    gos_ata_fcr_changed(cfg, 0, st->hr.fcr);
}

void gos_ata_teardown(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (!st || !st->ata_ready)
        return;
    st->ata_ready = false;
    for (int c = 0; c < 2; c++)
        ata_channel_free(&st->ata[c]);
    if (st->atapi) {
        scsi_delete(st->atapi);
        st->atapi = NULL;
    }
}

void gos_ata_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    gossamer_state_t *st = gos_st(cfg);
    scsi_checkpoint(st->atapi, cp);
    for (int c = 0; c < 2; c++)
        ata_checkpoint_save(&st->ata[c], cp);
}

// ---- media: attach and the runtime verbs -------------------------------------
//
// MEDIA_BUS_ATA unit u = cell * 2 + device.  A hard disk is the ATA device
// itself; a CD-ROM is the SCSI device at id u on the ATAPI back end.

static bool unit_ok(int unit) {
    return unit >= 0 && unit < 4;
}

int gos_media_attach(config_t *cfg, const media_slot_t *slot) {
    if (slot->bus != MEDIA_BUS_ATA)
        return system_media_attach_std(cfg, slot);
    gossamer_state_t *st = gos_st(cfg);
    if (!st || !st->ata_ready || !unit_ok(slot->unit))
        return -1;
    if (slot->scsi_type == 2) // a CD-ROM: on the ATAPI back end
        return system_media_attach_scsi_bus(cfg, st->atapi, slot);
    ata_channel_t *ch = &st->ata[slot->unit >> 1];
    if (!ata_attach_hd(ch, slot->unit & 1, slot->img))
        return -1;
    config_add_image(cfg, slot->img);
    LOG(1, "ATA cell %d device %d: hard disk %s (%llu sectors)", slot->unit >> 1, slot->unit & 1,
        image_get_filename(slot->img) ? image_get_filename(slot->img) : "(unnamed)",
        (unsigned long long)(disk_size(slot->img) / 512u));
    return 0;
}

bool gos_media_present(config_t *cfg, media_bus_t bus, int unit) {
    if (bus != MEDIA_BUS_ATA)
        return system_media_present_std(cfg, bus, unit);
    gossamer_state_t *st = gos_st(cfg);
    if (!st || !st->ata_ready || !unit_ok(unit))
        return false;
    if (st->ata[unit >> 1].img[unit & 1])
        return true;
    return system_media_present_scsi_bus(st->atapi, unit);
}

image_t *gos_media_image(config_t *cfg, media_bus_t bus, int unit) {
    if (bus != MEDIA_BUS_ATA)
        return system_media_image_std(cfg, bus, unit);
    gossamer_state_t *st = gos_st(cfg);
    if (!st || !st->ata_ready || !unit_ok(unit))
        return NULL;
    if (st->ata[unit >> 1].img[unit & 1])
        return st->ata[unit >> 1].img[unit & 1];
    return system_media_image_scsi_bus(st->atapi, unit);
}

int gos_media_eject(config_t *cfg, media_bus_t bus, int unit) {
    if (bus != MEDIA_BUS_ATA)
        return system_media_eject_std(cfg, bus, unit);
    gossamer_state_t *st = gos_st(cfg);
    if (!st || !st->ata_ready || !unit_ok(unit))
        return -1;
    // A hard disk comes off the bus as a SCSI disk does (the host's detach,
    // not the guest's eject); a disc comes out of its ATAPI drive.  Either
    // way the image goes back to the table, which closes it.
    image_t *hd = st->ata[unit >> 1].img[unit & 1];
    if (hd) {
        ata_detach(&st->ata[unit >> 1], unit & 1);
        config_release_image(cfg, hd);
        return 0;
    }
    return system_media_eject_scsi_bus(st->atapi, unit);
}

// ---- machine.ata ---------------------------------------------------------------

static value_t ata_attach(struct object *self, const value_t *argv, bool cdrom) {
    config_t *cfg = (config_t *)object_data(self);
    int64_t unit = argv[1].i;
    if (!unit_ok((int)unit))
        return val_err("ata.attach_%s: unit must be 0..3 (cell * 2 + device)", cdrom ? "cdrom" : "hd");
    char err[160];
    media_bay_t bay = {.bus = MEDIA_BUS_ATA,
                       .unit = (int)unit,
                       .label = unit < 2 ? (unit & 1 ? "Primary ATA bus, Slave" : "Primary ATA bus, Master")
                                         : (unit & 1 ? "Secondary ATA bus, Slave" : "Secondary ATA bus, Master")};
    if (system_media_attach_path(cfg, &bay, cdrom, argv[0].s, err, sizeof err) != 0)
        return val_err("ata.attach_%s: %s", cdrom ? "cdrom" : "hd", err);
    return val_bool(true);
}

static value_t ata_method_attach_hd(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    return ata_attach(self, argv, false);
}

static value_t ata_method_attach_cdrom(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    return ata_attach(self, argv, true);
}

// ---- machine.ata.device[U] ------------------------------------------------------
//
// One entry per unit (U = cell * 2 + device), whatever it holds: its `type`,
// `present`, `eject()` and, while loaded, the `image` node (image_class.h).
// A disc in an ATAPI drive is the same image the ATAPI back end's
// machine.atapi.device[U].image shows.

static image_t *ata_node_image(void *dev, unsigned slot) {
    return gos_media_image((config_t *)dev, MEDIA_BUS_ATA, (int)slot);
}
static int ata_node_eject(void *dev, unsigned slot) {
    if (!gos_media_present((config_t *)dev, MEDIA_BUS_ATA, (int)slot))
        return IMAGE_EJECT_EMPTY;
    int rc = gos_media_eject((config_t *)dev, MEDIA_BUS_ATA, (int)slot);
    return rc == 0 ? IMAGE_EJECT_OK : rc == -2 ? IMAGE_EJECT_LOCKED : IMAGE_EJECT_EMPTY;
}
static const image_node_ops_t ata_image_ops = {.image = ata_node_image, .eject = ata_node_eject};

static const image_node_t *ata_dev_node(struct object *self) {
    return (const image_node_t *)object_data(self);
}

static const char *const ATA_DEV_TYPE_NAMES[3] = {"none", "hd", "atapi"};

static DEF_GETTER(ata_dev_attr_type) {
    const image_node_t *n = ata_dev_node(self);
    gossamer_state_t *st = gos_st((config_t *)n->dev);
    if (!st || !st->ata_ready)
        return val_enum(0, ATA_DEV_TYPE_NAMES, 3);
    ata_channel_t *ch = &st->ata[n->slot >> 1];
    ata_refresh_devices(ch);
    ata_dev_kind_t k = ata_device_kind(ch, n->slot & 1);
    return val_enum(k == ATA_DEV_HD ? 1 : k == ATA_DEV_ATAPI ? 2 : 0, ATA_DEV_TYPE_NAMES, 3);
}

static DEF_GETTER(ata_dev_attr_present) {
    const image_node_t *n = ata_dev_node(self);
    return val_bool(gos_media_present((config_t *)n->dev, MEDIA_BUS_ATA, (int)n->slot));
}

static DEF_METHOD(ata_dev_method_eject) {
    const image_node_t *n = ata_dev_node(self);
    switch (ata_node_eject(n->dev, n->slot)) {
    case IMAGE_EJECT_OK:
        return val_bool(true);
    case IMAGE_EJECT_LOCKED:
        return val_err("ata.device[%u].eject: the guest has locked the drive (PREVENT MEDIUM REMOVAL)", n->slot);
    default:
        return val_bool(false);
    }
}

static struct object *ata_dev_image_lookup(struct object *self, const char *name) {
    (void)name;
    const image_node_t *n = ata_dev_node(self);
    gossamer_state_t *st = gos_st((config_t *)n->dev);
    if (!st || n->slot >= 4 || !ata_node_image(n->dev, n->slot))
        return NULL;
    return st->ata_image_objects[n->slot];
}

static const member_t ata_device_members[] = {
    {.kind = MK_ATTR,
     .name = "type",
     .doc = "What the unit holds: hd (an ATA disk), atapi (a CD-ROM drive) or none",
     .attr = {.type = VK_ENUM, .get = ata_dev_attr_type, .set = NULL}},
    {.kind = MK_ATTR,
     .name = "present",
     .doc = "True when an image is loaded",
     .attr = {.type = VK_BOOL, .get = ata_dev_attr_present, .set = NULL}},
    {.kind = MK_METHOD,
     .name = "eject",
     .doc = "Eject the image (a hard disk comes off the bus; an ATAPI drive stays, empty)",
     .method = {.args = NULL, .nargs = 0, .result = VK_BOOL, .fn = ata_dev_method_eject}},
    {.kind = MK_CHILD,
     .name = "image",
     .doc = "The image in this unit (present only when loaded)",
     .label = "Image",
     .child = {.cls = &image_class, .lookup = ata_dev_image_lookup}},
};

static const class_desc_t ata_device_class = {
    .name = "ata_device",
    .members = ata_device_members,
    .n_members = sizeof(ata_device_members) / sizeof(ata_device_members[0]),
};

static struct object *ata_devices_get(struct object *self, int index) {
    gossamer_state_t *st = gos_st((config_t *)object_data(self));
    if (!st || index < 0 || index > 3)
        return NULL;
    return st->ata_device_objects[index];
}

static const collection_desc_t ata_devices_entries = {
    .entry = &ata_device_class, .by_index = {.get = ata_devices_get, .slots = 4}
};

static const member_t ata_devices_members[] = {
    OBJ_ENTRIES(&ata_devices_entries, NULL),
};

static const class_desc_t ata_devices_class = {
    .name = "ata_devices",
    .doc = "ATA units, by U = cell * 2 + device",
    .members = ata_devices_members,
    .n_members = 1,
};

static const arg_decl_t ata_attach_args[] = {
    {.name = "path", .kind = VK_STRING, .doc = "image path"              },
    {.name = "unit", .kind = VK_INT,    .doc = "cell * 2 + device (0..3)"},
};

static const member_t ata_members[] = {
    {.kind = MK_METHOD,
     .name = "attach_hd",
     .doc = "Attach a hard-disk image as an ATA disk at a unit",
     .method = {.args = ata_attach_args, .nargs = 2, .result = VK_BOOL, .fn = ata_method_attach_hd}   },
    {.kind = MK_METHOD,
     .name = "attach_cdrom",
     .doc = "Attach a CD-ROM image as an ATAPI drive at a unit",
     .method = {.args = ata_attach_args, .nargs = 2, .result = VK_BOOL, .fn = ata_method_attach_cdrom}},
};

static const class_desc_t ata_class = {
    .name = "ata",
    .members = ata_members,
    .n_members = sizeof(ata_members) / sizeof(ata_members[0]),
};

void gos_ata_attach_objects(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (!st || st->ata_object)
        return;
    st->ata_object = object_attach_new(machine_object(), &ata_class, cfg, "ata", "ATA", OBJ_ORDER_ATA);
    if (!st->ata_object)
        return;
    st->ata_devices_object = object_new(&ata_devices_class, cfg, "device");
    if (!st->ata_devices_object)
        return;
    object_set_label(st->ata_devices_object, "Devices");
    object_attach(st->ata_object, st->ata_devices_object);
    for (int u = 0; u < 4; u++) {
        st->ata_nodes[u] = (image_node_t){.ops = &ata_image_ops, .dev = cfg, .slot = (unsigned)u};
        st->ata_device_objects[u] = object_new(&ata_device_class, &st->ata_nodes[u], NULL);
        st->ata_image_objects[u] = object_new(&image_class, &st->ata_nodes[u], "image");
        object_set_logical_parent(st->ata_device_objects[u], st->ata_devices_object, NULL, u, NULL);
        object_set_logical_parent(st->ata_image_objects[u], st->ata_device_objects[u], "image", -1, NULL);
    }
}

void gos_ata_detach_objects(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (st && st->ata_object) {
        for (int u = 0; u < 4; u++) {
            if (st->ata_image_objects[u]) {
                object_delete(st->ata_image_objects[u]);
                st->ata_image_objects[u] = NULL;
            }
            if (st->ata_device_objects[u]) {
                object_delete(st->ata_device_objects[u]);
                st->ata_device_objects[u] = NULL;
            }
        }
        object_destroy(&st->ata_devices_object);
        object_destroy(&st->ata_object);
    }
}
