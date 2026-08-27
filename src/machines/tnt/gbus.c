// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// gbus.c
// The GBUS island: Board Register 1's ANS top byte (keyswitch, hardwired
// box identifier, TwoSuppliesH), Board Register 2 (the "Safe Server"
// environmental halfword), the Ethernet address PROM and the MP doorbell
// that shares its chip select, and the timebase-enable register that shares
// the LCD's.  See gbus.h for the address map and the bit tables.
//
// Register truth: Apple, "Network Server Hardware Developer Notes", 1996,
// §4.2.1 and §4.2.2 (Apple's own bit tables, reproduced in the comments
// below), plus the production ROM's own behaviour observed at ladder rungs
// S2-S4.
//
// Three properties of this island are worth stating once, because getting
// any of them wrong is silent rather than loud:
//
//   * Every bit is ACTIVE LOW unless its name ends in `H`.  A healthy
//     machine reads all-ones, not zero.
//   * Board Register 2 is POLLED and NEVER interrupts — Apple is explicit
//     that "Network Server does not implement an interrupt for these
//     functions."  So a static healthy value satisfies both the ROM and
//     AIX with no event plumbing whatsoever.
//   * Board Register 1 is the same register the Macintosh boards call
//     BoxID, at the same address, and the guest reads it BYTE-WISE with
//     little-endian bit numbering.  The production ROM reads bytes +0 and
//     +1 only; byte +1 is the ANS top byte this file supplies.

#include "tnt.h"

#include "log.h"
#include "object.h"
#include "ppc.h"
#include "value.h"

#include <stdio.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("gbus");

static inline tnt_gbus_t *gb(config_t *cfg) {
    return cfg && cfg->machine_context ? &tnt_st(cfg)->gbus : NULL;
}

// ============================================================
// Board Register 1 — the ANS top byte
// ============================================================
// Apple's table (§4.2.1), all active low unless noted:
//   b10 not connected      b13 Keyswitch ServiceL
//   b11 BoxId0 = 1         b14 Keyswitch LockedL
//   b12 BoxId1 = 0         b15 TwoSuppliesH
//
// BoxId0/BoxId1 are hardwired and live in the board descriptor's `boxid`
// (ans500.c), because they are a strap.  What this function adds is the
// LIVE half: the two keyswitch lines and the redundant-supply report.
//
// The keyswitch encoding follows straight from the active-low pair, and
// matches the firmware values the ROM's own `(key=service?` / `(key=locked?`
// words test — Locked = 1, Service = 2 — when read as ((reg >> 13) & 3):
//   Locked   ServiceL=1 LockedL=0  ->  0b01 = 1
//   Service  ServiceL=0 LockedL=1  ->  0b10 = 2
//   Normal   both high             ->  0b11 = 3
uint32_t tnt_gbus_boxid_bits(config_t *cfg) {
    const tnt_gbus_t *g = gb(cfg);
    if (!g)
        return 0;
    uint32_t bits = 0;
    switch (g->keyswitch) {
    case ANS_KEY_LOCKED:
        bits |= ANS_BREG1_SERVICE_L; // ServiceL idles high; LockedL pulled low
        break;
    case ANS_KEY_SERVICE:
        bits |= ANS_BREG1_LOCKED_L; // LockedL idles high; ServiceL pulled low
        break;
    default: // ANS_KEY_NORMAL: neither position asserted
        bits |= ANS_BREG1_SERVICE_L | ANS_BREG1_LOCKED_L;
        break;
    }
    if (tnt_board(cfg)->two_supplies)
        bits |= ANS_BREG1_TWO_PSU_H; // active HIGH — the one exception
    return bits;
}

// ============================================================
// Board Register 2 — the environmental halfword
// ============================================================
// The eight fault bits are active low, so the register value is the
// complement of the INJECTED fault set.  The low byte is the Macintosh
// boards' and nothing on this machine is documented to drive it, so it
// idles high like every other undriven line on the register.
static uint16_t breg2_value(config_t *cfg) {
    const tnt_gbus_t *g = gb(cfg);
    return (uint16_t)(0xFFFFu & ~(g ? g->env_faults : 0u));
}

// ============================================================
// Ethernet address PROM (+ the MP doorbell)
// ============================================================
// Sixteen cells on a $10 stride (gbus.h).  Apple's layout is reproduced
// here in the fill; the MAC below is a fixed emulator constant, chosen from
// Apple's own 08:00:07 OUI so a guest that prints it prints something
// plausible.  Determinism matters more than the exact value: it must not
// vary run to run, or every AIX ODM record keyed on it would too.
#define ANS_MAC0 0x08u
#define ANS_MAC1 0x00u
#define ANS_MAC2 0x07u
#define ANS_MAC3 0x11u
#define ANS_MAC4 0x22u
#define ANS_MAC5 0x33u

static void eprom_fill(tnt_gbus_t *g) {
    const uint8_t mac[6] = {ANS_MAC0, ANS_MAC1, ANS_MAC2, ANS_MAC3, ANS_MAC4, ANS_MAC5};
    uint8_t xsum = 0;
    for (int i = 0; i < 6; i++) {
        g->eprom[i] = mac[i]; // group ID high/mid/low, then sequencing address
        g->eprom[8 + i] = (uint8_t)~mac[i]; // the same six bytes inverted
        xsum ^= mac[i];
    }
    g->eprom[6] = 0xAAu; // $60: normal bit ordering
    g->eprom[7] = (uint8_t)~xsum; // $70: inverted-XOR checksum
    g->eprom[14] = 0x55u; // $E0: reverse bit ordering
    // The inverted copy's checksum: XOR over six inverted bytes equals the
    // XOR over the originals (an even number of inversions cancels), so the
    // two checksum cells necessarily agree.
    g->eprom[15] = (uint8_t)~xsum;
}

// Any access to the PROM's GBUS space is ALSO the interprocessor doorbell:
// Apple ties SecToPri_Int (EXT10) to the Ethernet ROM chip select, which is
// "a programmatic way for a second processor to interrupt the first".
//
// On a single-processor machine — which is the DOCUMENTED normal path, not
// a simplification ("the first code executed will determine whether the
// processor is the primary … or if the processor is the secondary it will
// enter a spin-wait for an interprocessor interrupt") — raising it would
// interrupt the only CPU with a message from nobody.  So it is counted and
// left unraised.  Building MP (proposal §11 follow-up 2) turns this into
// one tnt_gc_pulse_event(cfg, ANS_INT_SECTOPRI) call.
static void eprom_doorbell(config_t *cfg) {
    tnt_gbus_t *g = gb(cfg);
    if (!g)
        return;
    g->doorbell++;
    LOG(4, "Ethernet-PROM access #%u (the SecToPri doorbell; single-CPU: not raised)", g->doorbell);
}

// ============================================================
// Island dispatch
// ============================================================

uint8_t tnt_gbus_read8(config_t *cfg, uint32_t offset) {
    tnt_gbus_t *g = gb(cfg);
    if (!g)
        return 0;
    switch (offset & 0x1F000u) {
    case ANS_OFF_EPROM: {
        eprom_doorbell(cfg);
        // Cells on $10 centres; anything off-centre lands on no cell.
        if ((offset & 0xFu) != 0 || ((offset & 0xFFFu) >> 4) >= ANS_EPROM_CELLS) {
            LOG(2, "Ethernet PROM read off-centre +$%05X", offset);
            return 0xFF;
        }
        uint8_t v = g->eprom[(offset & 0xFFu) >> 4];
        LOG(4, "Ethernet PROM cell %u -> $%02X", (offset & 0xFFu) >> 4, v);
        return v;
    }
    case ANS_OFF_BREG2: {
        // Byte j of the little-endian halfword (the ROM reads +0 and +1).
        uint8_t b = (uint8_t)(breg2_value(cfg) >> (8 * (offset & 1u)));
        LOG(3, "Board Register 2 byte read +%u -> $%02X", offset & 1u, b);
        return b;
    }
    default:
        LOG(1, "byte read of unwired GBUS offset +$%05X", offset);
        return 0;
    }
}

void tnt_gbus_write8(config_t *cfg, uint32_t offset, uint8_t value) {
    switch (offset & 0x1F000u) {
    case ANS_OFF_EPROM:
        // The PROM is read-only silicon; the write still rings the doorbell.
        eprom_doorbell(cfg);
        LOG(2, "write to the read-only Ethernet PROM +$%05X = $%02X ignored", offset, value);
        return;
    case ANS_OFF_BREG2:
        // Every documented bit is an input pin.  A guest write drives
        // nothing; recording it keeps a surprise visible.
        LOG(2, "write to read-only Board Register 2 +$%05X = $%02X ignored", offset, value);
        return;
    default:
        LOG(1, "byte write of unwired GBUS offset +$%05X = $%02X", offset, value);
        return;
    }
}

// 32-bit access.  `value` at this boundary is the big-endian bus view, so
// the little-endian register value is recovered with TNT_LE32 exactly as
// grand_central.c does for BoxID and the interrupt block.
uint32_t tnt_gbus_read32(config_t *cfg, uint32_t offset) {
    switch (offset & 0x1F000u) {
    case ANS_OFF_BREG2:
        LOG(3, "Board Register 2 read -> $%04X", breg2_value(cfg));
        return TNT_LE32(breg2_value(cfg));
    case ANS_OFF_EPROM: {
        // A byte-wide cell answering a longword cycle drives lane 0 only,
        // which on this big-endian bus is the MOST significant byte.
        eprom_doorbell(cfg);
        tnt_gbus_t *g = gb(cfg);
        uint32_t cell = (offset & 0xFFu) >> 4;
        if (!g || (offset & 0xFu) != 0 || cell >= ANS_EPROM_CELLS)
            return 0;
        return (uint32_t)g->eprom[cell] << 24;
    }
    default:
        LOG(1, "long read of unwired GBUS offset +$%05X", offset);
        return 0;
    }
}

void tnt_gbus_write32(config_t *cfg, uint32_t offset, uint32_t value) {
    (void)cfg; // every GBUS register this file owns is an input pin
    LOG(1, "long write of unwired GBUS offset +$%05X = $%08X", offset, TNT_LE32(value));
}

// ============================================================
// GBUS device 3's non-LCD registers ($1C020 / $1C030)
// ============================================================
// lcd.c owns the two LCD ports and routes these two here, because they are
// board facts rather than display ones.

// $1C020 bit 15 — the timebase enable.  Apple gives the MP sequence:
// "write a 0 to this register … This disables the 604 timebase.  It would
// then write a '0' value to each processor's timebase.  It would then write
// a 1 to bit 15 … re-enabling the timebases", and hedges that it "may or
// may not be useful in two processor configurations".
//
// On a single-processor machine the gate has NO observable effect: there is
// no second timebase to align to, and the documented sequence writes the
// timebase explicitly afterwards anyway, so a model that freezes the
// counter and one that does not are indistinguishable to the guest.  This
// is therefore store-and-readback, with the disable logged so that a guest
// which really does drive the sequence shows up in the ladder rather than
// silently diverging.  Freezing for real is part of the MP follow-up.
void tnt_gbus_tben_write(config_t *cfg, uint16_t value) {
    tnt_gbus_t *g = gb(cfg);
    if (!g)
        return;
    if ((g->tb_enable & 0x8000u) && !(value & 0x8000u))
        LOG(1, "timebase enable cleared ($1C020 = $%04X); single-CPU: the counter keeps running", value);
    g->tb_enable = value;
}

uint16_t tnt_gbus_tben_read(config_t *cfg) {
    tnt_gbus_t *g = gb(cfg);
    return g ? g->tb_enable : 0;
}

// $1C030 — undocumented.  The production ROM writes $FFFF here exactly once,
// immediately after its `Testing Parity DIMMs` LCD progress message and
// immediately before it reports the sized memory, which makes a
// parity-error latch the obvious reading; Apple documents neither the
// register nor its bits.  Store-and-readback, logged, and recorded as an
// open item in the dossier so the ladder can settle it.
void tnt_gbus_misc_write(config_t *cfg, uint16_t value) {
    tnt_gbus_t *g = gb(cfg);
    if (!g)
        return;
    LOG(1, "GBUS device 3 +$30 (undocumented) = $%04X", value);
    g->misc = value;
}

uint16_t tnt_gbus_misc_read(config_t *cfg) {
    tnt_gbus_t *g = gb(cfg);
    return g ? g->misc : 0;
}

// ============================================================
// machine.board — the object node
// ============================================================
// The environmental bits are deliberately WRITABLE: they are the only way
// to exercise the "Safe Server" path at all, POST prints a published string
// for each of them (`Drive Fan Failed!`, `Temperature Warning!`, `Left Power
// Fail!`, …), and AIX's monitoring daemon is supposed to react.  An
// injected fault therefore has a documented expected output.

static const char *const keyswitch_names[] = {"locked", "service", "normal"};

static value_t board_attr_keyswitch(struct object *self, const member_t *m) {
    (void)m;
    tnt_gbus_t *g = gb((config_t *)object_data(self));
    int k = g ? g->keyswitch : ANS_KEY_LOCKED;
    return val_enum(k, keyswitch_names, 3);
}

static value_t board_attr_keyswitch_set(struct object *self, const member_t *m, value_t v) {
    (void)m;
    tnt_gbus_t *g = gb((config_t *)object_data(self));
    if (!g)
        return val_err("keyswitch: no machine");
    for (int i = 0; i < 3; i++) {
        if (v.kind == V_STRING && v.s && strcmp(v.s, keyswitch_names[i]) == 0) {
            g->keyswitch = (uint8_t)i;
            return val_none();
        }
    }
    return val_err("keyswitch: want one of locked, service, normal");
}

static value_t board_attr_rear_key(struct object *self, const member_t *m) {
    (void)m;
    tnt_gbus_t *g = gb((config_t *)object_data(self));
    return val_bool(g ? g->rear_locked != 0 : true);
}

static value_t board_attr_rear_key_set(struct object *self, const member_t *m, value_t v) {
    (void)m;
    tnt_gbus_t *g = gb((config_t *)object_data(self));
    if (!g)
        return val_err("rear_key_locked: no machine");
    g->rear_locked = val_as_bool(&v) ? 1u : 0u;
    // The rear key is a POWER-ON PRECONDITION, not something software reads:
    // "the rear keyswitch is in the locked position" is one of the states
    // the Theory of Operations requires before the DC path comes up, and the
    // data sheet states the rear keylock powers the system down when
    // unlocked.  A real machine simply does not run; an emulator that
    // silently ran anyway would hide the fact, so say so loudly.
    if (!g->rear_locked)
        LOG(1, "rear keyswitch UNLOCKED — a real Network Server powers down; the model keeps running");
    return val_none();
}

// One accessor pair per environmental bit, generated from the bit mask.
#define BOARD_ENV_ATTR(fn, MASK, NAME)                                                                                 \
    static value_t fn##_get(struct object *self, const member_t *m) {                                                  \
        (void)m;                                                                                                       \
        tnt_gbus_t *g = gb((config_t *)object_data(self));                                                             \
        return val_bool(g && (g->env_faults & (MASK)));                                                                \
    }                                                                                                                  \
    static value_t fn##_set(struct object *self, const member_t *m, value_t v) {                                       \
        (void)m;                                                                                                       \
        tnt_gbus_t *g = gb((config_t *)object_data(self));                                                             \
        if (!g)                                                                                                        \
            return val_err(NAME ": no machine");                                                                       \
        bool on = val_as_bool(&v);                                                                                     \
        if (on)                                                                                                        \
            g->env_faults |= (MASK);                                                                                   \
        else                                                                                                           \
            g->env_faults &= (uint16_t) ~(MASK);                                                                       \
        LOG(1, NAME " = %d (Board Register 2 now $%04X)", on ? 1 : 0, breg2_value((config_t *)object_data(self)));     \
        return val_none();                                                                                             \
    }

BOARD_ENV_ATTR(env_fan_drive, ANS_ENV_FAN_DRIVE, "fan_fail_drive")
BOARD_ENV_ATTR(env_fan_proc, ANS_ENV_FAN_PROC, "fan_fail_processor")
BOARD_ENV_ATTR(env_temp_fail, ANS_ENV_TEMP_FAIL, "temp_fail")
BOARD_ENV_ATTR(env_temp_warn, ANS_ENV_TEMP_WARN, "temp_warn")
BOARD_ENV_ATTR(env_psu_left, ANS_ENV_PSU_LEFT, "psu_left_fail")
BOARD_ENV_ATTR(env_psu_right, ANS_ENV_PSU_RIGHT, "psu_right_fail")
BOARD_ENV_ATTR(env_hot_left, ANS_ENV_HOT_LEFT, "psu_left_hot")
BOARD_ENV_ATTR(env_hot_right, ANS_ENV_HOT_RIGHT, "psu_right_hot")

static value_t board_attr_breg1(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_uint(4, cfg ? tnt_gc_boxid(cfg) : 0u);
}

static value_t board_attr_breg2(struct object *self, const member_t *m) {
    (void)m;
    return val_uint(2, breg2_value((config_t *)object_data(self)));
}

static value_t board_attr_two_supplies(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_bool(cfg && tnt_board(cfg)->two_supplies);
}

static value_t board_attr_parity(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_bool(cfg && tnt_board(cfg)->has_parity);
}

static value_t board_attr_l2_kb(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_uint(4, cfg ? tnt_board(cfg)->l2_kb : 0);
}

static value_t board_attr_bus_hz(struct object *self, const member_t *m) {
    (void)m;
    config_t *cfg = (config_t *)object_data(self);
    return val_uint(4, cfg ? tnt_board(cfg)->bus_hz : 0);
}

static value_t board_attr_doorbell(struct object *self, const member_t *m) {
    (void)m;
    tnt_gbus_t *g = gb((config_t *)object_data(self));
    return val_uint(4, g ? g->doorbell : 0);
}

#define BOARD_ENV_MEMBER(attr_name, fn, attr_doc)                                                                      \
    {                                                                                                                  \
        .kind = M_ATTR, .name = attr_name, .doc = attr_doc, .attr = {                                                  \
            .type = V_BOOL,                                                                                            \
            .get = fn##_get,                                                                                           \
            .set = fn##_set                                                                                            \
        }                                                                                                              \
    }

static const member_t tnt_board_members[] = {
    {.kind = M_ATTR,
     .name = "keyswitch",
     .doc = "Front three-position keyswitch: locked | service | normal",
     .attr = {.type = V_ENUM, .get = board_attr_keyswitch, .set = board_attr_keyswitch_set}},
    {.kind = M_ATTR,
     .name = "rear_key_locked",
     .doc = "Rear keyswitch locked — a power-on precondition, not software-visible",
     .attr = {.type = V_BOOL, .get = board_attr_rear_key, .set = board_attr_rear_key_set}},
    {.kind = M_ATTR,
     .name = "register1",
     .doc = "Board Register 1 ($F301A000) as software reads it",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = board_attr_breg1, .set = NULL}},
    {.kind = M_ATTR,
     .name = "register2",
     .doc = "Board Register 2 ($F301E000) — the environmental halfword, active low",
     .flags = VAL_RO | VAL_HEX,
     .attr = {.type = V_UINT, .get = board_attr_breg2, .set = NULL}},
    BOARD_ENV_MEMBER("fan_fail_drive", env_fan_drive, "Inject FanFailDrive (POST: 'Drive Fan Failed!')"),
    BOARD_ENV_MEMBER("fan_fail_processor", env_fan_proc, "Inject FanFailProcessor (POST: 'Processor Fan Failed')"),
    BOARD_ENV_MEMBER("temp_fail", env_temp_fail, "Inject TempFailProcessor (POST: 'Temperature Too Hot!')"),
    BOARD_ENV_MEMBER("temp_warn", env_temp_warn, "Inject TempWarnProcessor (POST: 'Temperature Warning!')"),
    BOARD_ENV_MEMBER("psu_left_fail", env_psu_left, "Inject FailPowSupplyLeft (POST: 'Left Power Fail!')"),
    BOARD_ENV_MEMBER("psu_right_fail", env_psu_right, "Inject FailPowSupplyRight (POST: 'Right Power Fail!')"),
    BOARD_ENV_MEMBER("psu_left_hot", env_hot_left, "Inject powSupplyHotLeft (POST: 'Left Power Hot!')"),
    BOARD_ENV_MEMBER("psu_right_hot", env_hot_right, "Inject powSupplyHotRight (POST: 'Right Power Hot!')"),
    {.kind = M_ATTR,
     .name = "two_supplies",
     .doc = "TwoSuppliesH — redundant power supplies fitted (the 700)",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = board_attr_two_supplies, .set = NULL}},
    {.kind = M_ATTR,
     .name = "parity",
     .doc = "Parity DRAM fitted (selects 60 ns rather than 70 ns timing)",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = board_attr_parity, .set = NULL}},
    {.kind = M_ATTR,
     .name = "l2_kb",
     .doc = "L2 cache DIMM size in KB (0 = no cache DIMM)",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = board_attr_l2_kb, .set = NULL}},
    {.kind = M_ATTR,
     .name = "bus_hz",
     .doc = "Processor bus clock, sourced from the CPU card",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = board_attr_bus_hz, .set = NULL}},
    {.kind = M_ATTR,
     .name = "doorbell",
     .doc = "Accesses to the Ethernet PROM space — the SecToPri_Int doorbell",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = board_attr_doorbell, .set = NULL}},
};

static const class_desc_t tnt_board_class = {
    .name = "board",
    .members = tnt_board_members,
    .n_members = sizeof(tnt_board_members) / sizeof(tnt_board_members[0]),
};

// ============================================================
// Lifecycle
// ============================================================

void tnt_gbus_reset(config_t *cfg) {
    tnt_gbus_t *g = gb(cfg);
    if (!g)
        return;
    // Power-on: timebases running, no injected faults, PROM re-derived.
    // The KEYSWITCHES survive — they are physical switch positions, not
    // register state, and a reset does not turn a key.
    g->tb_enable = 0x8000u;
    g->misc = 0;
    g->env_faults = 0;
    g->doorbell = 0;
    eprom_fill(g);
}

void tnt_gbus_init(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_gbus_t *g = &st->gbus;
    // Both keyswitches default to LOCKED, which is what the Theory of
    // Operations requires: the rear key locked is a power-on precondition,
    // and Locked is the front switch's normal running position.  Every
    // non-default is logged at construction (R9).
    g->keyswitch = ANS_KEY_LOCKED;
    g->rear_locked = 1;
    tnt_gbus_reset(cfg);

    st->board_object = object_new(&tnt_board_class, cfg, "board");
    if (st->board_object) {
        object_set_label(st->board_object, "Board");
        object_set_order(st->board_object, 120);
        object_attach(machine_object(), st->board_object);
    }
}

void tnt_gbus_teardown(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (st && st->board_object) {
        object_detach(st->board_object);
        object_delete(st->board_object);
        st->board_object = NULL;
    }
}
