// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gossamer_bmac.c
// Heathrow's Ethernet cell at +$11000 on the core BMAC model (bmac.c):
// the bus edge (16-bit little-endian registers on 16-byte centres), the
// feature-control bits that enable and reset it, its interrupt (source
// $2A, bank 2) and DBDMA channels 2/3, and machine.bmac.

#include "gossamer.h"

#include "bmac.h"
#include "checkpoint.h"
#include "log.h"
#include "machine.h"
#include "object.h"

#include <stdio.h>

LOG_USE_CATEGORY_NAME("heathrow");

// FCR bits 29-31 (Apple's kEnetEnabledBits / kResetEnetCell; Linux
// HRW_BMAC_IO_ENABLE / HRW_BMAC_RESET).
#define FCR_ENET_ENABLE 0x60000000u
#define FCR_ENET_RESET  0x80000000u

// The station address the cell's EEPROM holds: Apple's OUI and a fixed,
// recognisable tail, so every run of a row sees the same address.
static const uint8_t gos_bmac_mac[6] = {0x00, 0x05, 0x02, 0x47, 0x53, 0x01};

static void gos_bmac_irq(void *ctx, bool level) {
    gos_set_source((config_t *)ctx, GOS_INT_BMAC, level);
}

void gos_bmac_init(config_t *cfg, checkpoint_t *cp) {
    gossamer_state_t *st = gos_st(cfg);
    st->bmac = bmac_init(cp, gos_bmac_mac);
    if (!st->bmac)
        return;
    bmac_set_irq(st->bmac, gos_bmac_irq, cfg);
    bmac_attach_dbdma(st->bmac, st->dbdma, GOS_DMA_BMAC_TX, GOS_DMA_BMAC_RX);
}

void gos_bmac_reset(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (st->bmac)
        bmac_reset(st->bmac);
}

void gos_bmac_teardown(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (st && st->bmac) {
        bmac_delete(st->bmac);
        st->bmac = NULL;
    }
}

void gos_bmac_checkpoint_save(config_t *cfg, checkpoint_t *cp) {
    bmac_checkpoint(gos_st(cfg)->bmac, cp);
}

// The reset pulse (bit 31 rising) returns the cell to its power-on
// registers; the drivers enable it (bits 29-30) around the pulse.
void gos_bmac_fcr_changed(config_t *cfg, uint32_t old, uint32_t fcr) {
    gossamer_state_t *st = gos_st(cfg);
    if (!st->bmac)
        return;
    if ((fcr & FCR_ENET_RESET) && !(old & FCR_ENET_RESET)) {
        LOG(2, "BMAC cell reset");
        bmac_reset(st->bmac);
    }
    if ((fcr ^ old) & FCR_ENET_ENABLE)
        LOG(2, "BMAC %s", (fcr & FCR_ENET_ENABLE) == FCR_ENET_ENABLE ? "enabled" : "disabled");
}

// ---- Bus edge ------------------------------------------------------------------

// A halfword cycle: the guest's lhbrx/sthbrx composes the little-endian
// register value, so the big-endian bus view is its byte swap.
uint16_t gos_bmac_read16(config_t *cfg, uint32_t off) {
    uint16_t v = bmac_read(gos_st(cfg)->bmac, off);
    return (uint16_t)((v >> 8) | (v << 8));
}

void gos_bmac_write16(config_t *cfg, uint32_t off, uint16_t value) {
    bmac_write(gos_st(cfg)->bmac, off, (uint16_t)((value >> 8) | (value << 8)));
}

// A word cycle (Open Firmware's `rl@-flip` / `rl!-flip`): the register is
// the low half of the little-endian longword.
uint32_t gos_bmac_read32(config_t *cfg, uint32_t off) {
    uint16_t v = bmac_read(gos_st(cfg)->bmac, off);
    return ((uint32_t)(v & 0xFFu) << 24) | ((uint32_t)(v >> 8) << 16);
}

void gos_bmac_write32(config_t *cfg, uint32_t off, uint32_t value) {
    bmac_write(gos_st(cfg)->bmac, off, (uint16_t)(((value >> 24) & 0xFFu) | (((value >> 16) & 0xFFu) << 8)));
}

// A byte cycle: lane 0 of a register is its low byte.  (DBDMA's LOAD_QUAD
// of STATUS after each transmitted frame arrives this way; reading the
// register clears it whichever byte is asked for.)
uint8_t gos_bmac_read8(config_t *cfg, uint32_t off) {
    uint16_t v = bmac_read(gos_st(cfg)->bmac, off & ~1u);
    return (uint8_t)((off & 1u) ? v >> 8 : v);
}

void gos_bmac_write8(config_t *cfg, uint32_t off, uint8_t value) {
    LOG(2, "byte write to BMAC +$%03X = $%02X", off & 0xFFFu, value);
    bmac_write(gos_st(cfg)->bmac, off & ~1u, value);
}

// The same cycles as inspections: bmac_peek leaves a read-to-clear STATUS set.
uint16_t gos_bmac_peek16(config_t *cfg, uint32_t off) {
    uint16_t v = bmac_peek(gos_st(cfg)->bmac, off);
    return (uint16_t)((v >> 8) | (v << 8));
}

uint32_t gos_bmac_peek32(config_t *cfg, uint32_t off) {
    uint16_t v = bmac_peek(gos_st(cfg)->bmac, off);
    return ((uint32_t)(v & 0xFFu) << 24) | ((uint32_t)(v >> 8) << 16);
}

uint8_t gos_bmac_peek8(config_t *cfg, uint32_t off) {
    uint16_t v = bmac_peek(gos_st(cfg)->bmac, off & ~1u);
    return (uint8_t)((off & 1u) ? v >> 8 : v);
}

// ---- machine.bmac --------------------------------------------------------------

static bmac_t *obj_bmac(struct object *self) {
    gossamer_state_t *st = gos_st((config_t *)object_data(self));
    return st ? st->bmac : NULL;
}

static value_t bmac_attr_mac(struct object *self, const member_t *m) {
    (void)m;
    bmac_t *b = obj_bmac(self);
    if (!b)
        return val_err("bmac: no cell");
    uint8_t a[6];
    bmac_get_mac(b, a);
    char buf[24];
    snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
    return val_str(buf);
}

static value_t bmac_attr_link(struct object *self, const member_t *m) {
    (void)m;
    bmac_t *b = obj_bmac(self);
    return val_bool(b && bmac_link(b));
}

static value_t bmac_attr_link_set(struct object *self, const member_t *m, value_t v) {
    (void)m;
    bmac_t *b = obj_bmac(self);
    if (!b)
        return val_err("bmac: no cell");
    bmac_set_link(b, v.b);
    return val_bool(true);
}

static value_t bmac_attr_tx(struct object *self, const member_t *m) {
    (void)m;
    bmac_t *b = obj_bmac(self);
    return val_int(b ? (int64_t)bmac_tx_frames(b) : 0);
}

static value_t bmac_attr_rx(struct object *self, const member_t *m) {
    (void)m;
    bmac_t *b = obj_bmac(self);
    return val_int(b ? (int64_t)bmac_rx_frames(b) : 0);
}

// `last_tx`: the last frame the transmitter sent, as lower-case hex.
static value_t bmac_attr_last_tx(struct object *self, const member_t *m) {
    (void)m;
    bmac_t *b = obj_bmac(self);
    uint8_t f[1600];
    int n = b ? bmac_last_tx(b, f, (int)sizeof f) : 0;
    char hex[2 * sizeof f + 1];
    for (int i = 0; i < n; i++)
        snprintf(hex + 2 * i, 3, "%02x", f[i]);
    hex[2 * n] = '\0';
    return val_str(hex);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// `receive(hex)`: a frame (destination address first, no FCS) arrives on
// the wire.  Spaces between bytes are allowed.  True when the receive
// filter queued it for DMA.
static value_t bmac_method_receive(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    bmac_t *b = obj_bmac(self);
    if (!b)
        return val_err("bmac: no cell");
    uint8_t f[1600];
    int n = 0;
    for (const char *p = argv[0].s; *p;) {
        if (*p == ' ' || *p == ':') {
            p++;
            continue;
        }
        int hi = hex_nibble(p[0]), lo = p[1] ? hex_nibble(p[1]) : -1;
        if (hi < 0 || lo < 0)
            return val_err("bmac.receive: '%s' is not hex bytes", argv[0].s);
        if (n == (int)sizeof f)
            return val_err("bmac.receive: frame longer than %d bytes", (int)sizeof f);
        f[n++] = (uint8_t)(hi << 4 | lo);
        p += 2;
    }
    return val_bool(bmac_receive(b, f, n));
}

static const arg_decl_t bmac_receive_args[] = {
    {.name = "frame", .kind = VK_STRING, .doc = "the frame as hex bytes, destination address first, no FCS"},
};

static const member_t bmac_members[] = {
    {.kind = MK_ATTR,
     .name = "mac",
     .doc = "Station address in the cell's serial EEPROM",
     .attr = {.type = VK_STRING, .get = bmac_attr_mac, .set = NULL}                                 },
    {.kind = MK_ATTR,
     .name = "link",
     .doc = "Link state the transceiver reports (XCVRIF bit 8)",
     .attr = {.type = VK_BOOL, .get = bmac_attr_link, .set = bmac_attr_link_set}                    },
    {.kind = MK_ATTR,
     .name = "tx_frames",
     .doc = "Frames the transmitter has sent",
     .attr = {.type = VK_INT, .get = bmac_attr_tx, .set = NULL}                                     },
    {.kind = MK_ATTR,
     .name = "rx_frames",
     .doc = "Frames the receiver has handed to DMA",
     .attr = {.type = VK_INT, .get = bmac_attr_rx, .set = NULL}                                     },
    {.kind = MK_ATTR,
     .name = "last_tx",
     .doc = "The last frame transmitted (padded, no FCS), as hex",
     .attr = {.type = VK_STRING, .get = bmac_attr_last_tx, .set = NULL}                             },
    {.kind = MK_METHOD,
     .name = "receive",
     .doc = "Deliver a frame to the receiver as if from the wire; true when the address filter queued it",
     .method = {.args = bmac_receive_args, .nargs = 1, .result = VK_BOOL, .fn = bmac_method_receive}},
};

static const class_desc_t bmac_class = {
    .name = "bmac",
    .members = bmac_members,
    .n_members = sizeof(bmac_members) / sizeof(bmac_members[0]),
};

void gos_bmac_attach_objects(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (!st || st->bmac_object)
        return;
    st->bmac_object = object_new(&bmac_class, cfg, "bmac");
    if (st->bmac_object) {
        object_set_label(st->bmac_object, "BMAC Ethernet");
        object_set_order(st->bmac_object, 92);
        object_attach(machine_object(), st->bmac_object);
    }
}

void gos_bmac_detach_objects(config_t *cfg) {
    gossamer_state_t *st = gos_st(cfg);
    if (st && st->bmac_object) {
        object_detach(st->bmac_object);
        object_delete(st->bmac_object);
        st->bmac_object = NULL;
    }
}
