// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// grand_central.c
// Grand Central (343S1125) — the I/O controller behind almost every
// non-video device: a 128 KB window at the base of Bandit 1's PCI I/O
// space ($F3000000) holding the interrupt controller, eleven DBDMA
// engines, and the apertures of every legacy I/O cell.
//
// The chip is LITTLE-ENDIAN behind a big-endian bus with no byte-lane
// swapper: 32-bit registers (the interrupt block, DBDMA, BoxID) are
// reached with lwbrx/stwbrx by the guest and swapped at this model's edge
// (TNT_LE32); byte-wide device cells need no swapping and sit on $10
// centres (VIA: $200) so each occupies its own aligned longword slot —
// they are byte-access only, and wider access logs and reads open bus.
//
// Populated here: the interrupt block (+$20..$2C), the DBDMA channel
// windows (+$8000+n*$100 — the engine itself is dbdma.c), the VIA1/Cuda
// window (+$16000), BoxID (+$1A000), the banked NVRAM (+$1D000 port /
// +$1F000 data window), AWACS (+$14000) and the RaDACal RAMDAC (+$1B000 —
// control.c).  An aperture with no model behind it logs and reads open bus.
//
// Register truth: the shipping ROM's Open Firmware device tree and 68k
// DecoderInfo tables, the ROM's own NanoKernel interrupt handler, and
// OSF/Apple MkLinux DR3 (powermac_pci.h, whose window offsets match entry
// for entry).  Interrupt semantics: see the interrupt-block comments.

#include "tnt.h"

#include "checkpoint.h"
#include "dbdma.h"
#include "irq_controller.h"
#include "log.h"
#include "machine.h"
#include "object.h"
#include "of_nvram.h"
#include "pci.h"
#include "ppc.h"
#include "scc.h"
#include "scsi_53c96.h"
#include "via.h"

#include <stdio.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("gc");

// Island offsets (relative to $F3000000)
#define OFF_INTS      0x00020u // +$20 Events / +$24 Mask / +$28 Clear / +$2C Levels
#define OFF_DBDMA     0x08000u // channels 0-10 at +$8000+n*$100 (dbdma.c)
#define OFF_DBDMA_END (OFF_DBDMA + 0x100u * DBDMA_CHANNELS_GRAND_CENTRAL)
#define OFF_SCSI0     0x10000u // 53C94, external bus
#define OFF_MACE      0x11000u // MACE Ethernet
#define OFF_SCCLEG    0x12000u // SCC legacy aperture
#define OFF_ESCC      0x13000u // ESCC: channel B at +0, channel A at +$20
#define OFF_AWACS     0x14000u // AWACS codec + sound control
#define OFF_SWIM3     0x15000u // SWIM3 floppy: 16 regs on $10 centres (swim3.c)
#define OFF_VIA       0x16000u // VIA1/Cuda: 16 byte regs on $200 centres (8 KB)
#define OFF_MESH      0x18000u // MESH, internal bus
#define OFF_EPROM     0x19000u // Ethernet address PROM (+ the ANS MP doorbell)
#define OFF_BOXID     0x1A000u // machine-identification register (LE)
#define OFF_RADACAL   0x1B000u // RAMDAC colormap bank
#define OFF_LCDGB     0x1C000u // GBUS device 3: ANS front-panel LCD (lcd.c)
#define OFF_BREG2     0x1E000u // ANS Board Register 2: environment (gbus.c)
#define OFF_NVPORT    0x1D000u // NVRAM bank-select port
#define OFF_NVDATA    0x1F000u // NVRAM data window: byte j at +j*$10

// Interrupt-register offsets within the block
#define INT_EVENTS 0x20u
#define INT_MASK   0x24u
#define INT_CLEAR  0x28u
#define INT_LEVELS 0x2Cu

// The NanoKernel's per-interrupt acknowledge: a write of $80000000 to
// InterruptClear is a MODE acknowledge, not a source clear — it must not
// clear pending device bits or the guest loses interrupts.
#define INT_MODE_ACK 0x80000000u

// ============================================================
// BoxID ($F301A000, little-endian bit numbering — the guest reads lwbrx)
// ============================================================
// Bit map, pinned empirically at ladder rungs T4/T6 (the community's
// "bits 11-12 model code" reading is dead — both identification halves
// decoded from the ROM):
//   0-5   PCI slot power/present pins (empty slots: 0)
//   6-7   SCC RTS-A/B readback
//   8     factory-test strap; POST tests it (modeled pulled high = normal;
//         set sends the boot into the ROM's serial test monitor)
//   9     microphone sense
//   10    Ethernet 10BT link
//   11    SET = 8500 (the 68k identification routine at ROM $FFC14844)
//   12    unread by either identification
//   13    SET = 7500 (Open Firmware's model decode splits the 7500/8500
//         class — selected by Hammerhead +$20 bit 31 — on this bit)
//   14    MESH / fast-SCSI present (all three TNT boards)
//   15    unused, pulled high
// The per-model composite values live in the board descriptors (pm7500.c
// etc.); bits 0-5 are added here from the live slot population, because
// they are not a board constant — they are the power/present pins of the
// machine's PCI sockets, and an empty socket reads 0.

// The BoxID as software sees it: the board's straps plus one presence bit
// per POPULATED socket (bit n-1 for slot n, in the machine's declared slot
// order).  The builtin VCI entry is not a socket and contributes nothing.
uint32_t tnt_gc_boxid(config_t *cfg) {
    uint32_t id = tnt_board(cfg)->boxid;
    for (const pci_slot_decl_t *d = cfg->machine->pci_slots; d && d->slot; d++) {
        if (d->kind != PCI_SLOT_SOCKET || d->slot < 1 || d->slot > 6)
            continue;
        if (pci_slot_device(cfg->pci, d->slot))
            id |= 1u << (d->slot - 1);
    }
    // On a Network Server this same register is Board Register 1, and its
    // top byte is live board state rather than straps: the two active-low
    // keyswitch lines and TwoSuppliesH (gbus.c).  Bits 14 and 15 are NOT
    // the Macintosh boards' "MESH present" / idle-high pair there.
    if (tnt_board(cfg)->has_gbus)
        id |= tnt_gbus_boxid_bits(cfg);
    return id;
}

// ============================================================
// Interrupt fabric
// ============================================================

void tnt_gc_recompute(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_gc_t *gc = &st->gc;
    // Clear-mode 1 (the NanoKernel's scheme, selected by the $80000000
    // acknowledge): the CPU line is an OUTPUT LATCH, held PER SOURCE — a
    // source's bit is set by any CHANGE of it while enabled (assertion or
    // deassertion; see tnt_gc_set_source) or by an enabled event edge,
    // cleared by the acknowledge, re-asserted only by the NEXT change.
    // The handler classifies from Levels & Mask; a level a guest leaves
    // unserviced does not re-fire until its next change, and the kernel's
    // rfi does not land straight back in the handler.
    //
    // The mask gates the OUTPUT and not just the latching, because a guest
    // that masks a source has to be able to quiet it: AIX polls the
    // fast/wide SCSI controllers and leaves their externals masked, and a
    // latch the mask could not reach held the line up for a source the
    // guest had deliberately turned off.
    // Mode 0 (power-on; the MkLinux scheme): combinational
    // ((events | levels) & mask), with Events cleared by explicit W1C.
    bool line;
    if (gc->int_mode1)
        line = (gc->int_latch & gc->int_mask) != 0;
    else
        line = ((gc->int_events | gc->int_levels) & gc->int_mask) != 0;
    ppc_set_ext_irq(cfg->ppc, line);
}

// An enabled source edge sets the mode-1 output latch.
static void gc_edge(tnt_gc_t *gc, uint32_t bit) {
    gc->int_events |= bit;
    if (gc->int_mask & bit)
        gc->int_latch |= bit;
}

void tnt_gc_set_source(config_t *cfg, int n, bool level) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    uint32_t bit = 1u << n;
    bool was = (gc->int_levels & bit) != 0;
    if (level) {
        gc->int_levels |= bit;
        // Events latch source EDGES; a still-asserted source re-latches
        // only on its next assertion edge.
        if (!was)
            gc_edge(gc, bit);
    } else {
        gc->int_levels &= ~bit;
        // DEASSERTION of an enabled source latches too: mode 1 is an
        // interrupt-on-CHANGE scheme (same law as AMIC INTMODE=1, amic.c).
        // The deassert interrupt is how the NanoKernel learns a source went
        // away — ExtIntHandlerTNT re-reads Levels, finds them quiet, and
        // stores 68k IPL 0 through EmuIntLevelPtr.  Nothing else in the
        // kernel/emulator contract lowers the posted IPL (the emulator's
        // delivery path never touches it, and the TNT handler stores the
        // IplValue without the $8000 reprioritize flag), so without this
        // the 68k emulator redelivers the stale level forever and the
        // boot's base context never runs again after its first unmask.
        if (was && (gc->int_mask & bit))
            gc->int_latch |= bit;
    }
    if (was != level)
        LOG(3, "source %d %s (events=$%08X levels=$%08X mask=$%08X latch=$%08X)", n, level ? "asserted" : "cleared",
            gc->int_events, gc->int_levels, gc->int_mask, gc->int_latch);
    tnt_gc_recompute(cfg);
}

void tnt_gc_pulse_event(config_t *cfg, int n) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    gc_edge(gc, 1u << n);
    tnt_gc_recompute(cfg);
}

// The interrupt block is 32-bit little-endian; `value` here is the
// little-endian register value (the dispatcher swaps at the bus edge).
static uint32_t int_read(config_t *cfg, uint32_t offset) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    LOG(4, "int read +$%02X (events=$%08X levels=$%08X mask=$%08X latch=$%08X)", offset, gc->int_events, gc->int_levels,
        gc->int_mask, gc->int_latch);
    switch (offset) {
    case INT_EVENTS:
        return gc->int_events;
    case INT_MASK:
        return gc->int_mask;
    case INT_CLEAR:
        return 0; // write-only
    case INT_LEVELS:
        return gc->int_levels; // live, never latched
    default:
        LOG(2, "interrupt-block read of unwired +$%02X", offset);
        return 0;
    }
}

static void int_write(config_t *cfg, uint32_t offset, uint32_t value) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    switch (offset) {
    case INT_EVENTS:
    case INT_CLEAR:
        // W1C into Events — except the mode-acknowledge bit, which clears
        // no device bits (see INT_MODE_ACK above).  A Clear write also
        // selects the clear mode from bit 31: the NanoKernel writes
        // $80000000 on every interrupt (mode 1), MkLinux writes the event
        // bits themselves (mode 0).
        gc->int_events &= ~(value & ~INT_MODE_ACK);
        if (offset == INT_CLEAR) {
            gc->int_mode1 = (value & INT_MODE_ACK) != 0;
            if (gc->int_mode1)
                gc->int_latch = 0; // the acknowledge drops the line
        }
        LOG(3, "clear $%08X -> events $%08X (mode %d)", value, gc->int_events, gc->int_mode1);
        // The DBDMA channels (sources 0-10) hold their completion request
        // as a level until acknowledged here (tnt_dbdma_irq): the clear
        // deasserts it, which in mode 1 is itself the change the
        // NanoKernel needs to lower the posted IPL again.
        for (int n = 0; n < DBDMA_CHANNELS_GRAND_CENTRAL; n++) {
            uint32_t bit = 1u << n;
            if ((value & bit) && (gc->int_levels & bit))
                tnt_gc_set_source(cfg, n, false);
        }
        break;
    case INT_MASK: {
        // Enabling a source whose event or level is already pending
        // counts as an edge for the mode-1 latch (the enable is the first
        // moment the controller may assert for it).
        uint32_t newly = value & ~gc->int_mask;
        gc->int_mask = value;
        gc->int_latch |= (gc->int_events | gc->int_levels) & newly;
        LOG(3, "mask = $%08X (pc=%08X)", value, ppc_get_pc(cfg->ppc));
        break;
    }
    case INT_LEVELS:
        LOG(2, "write to read-only Levels ($%08X) ignored", value);
        break;
    default:
        LOG(2, "interrupt-block write of unwired +$%02X = $%08X", offset, value);
        break;
    }
    tnt_gc_recompute(cfg);
}

// ============================================================
// NVRAM (banked two-aperture model)
// ============================================================
// Port at +$1D000 selects a 32-byte bank (bank = byte offset / 32); the
// data window at +$1F000 exposes the bank's 32 bytes on $10 centres.
// 8 KB total = 256 banks.  POST logs into it before anything else works,
// so it is live from reset; the contents survive machine reset (it is
// non-volatile) and are checkpointed with the family blob.

// The data window decodes FIVE address bits for the cell — (offset >> 4) &
// $1F — and ignores the rest, so the window WRAPS every $200 bytes rather
// than ending.  POST leans on that: its NVRAM pattern test writes 4-byte
// values at cells $1D..$1F with its plain "+$10 per byte" accessor, so the
// last byte lands at +$200 and must alias cell 0 of the same bank (as the
// part wires it) for the read-back to match.  A model that dropped it
// failed every warm boot's "MainLBU NVRAM" test.
static uint32_t nvram_cell(uint32_t offset) {
    return (offset >> 4) & 0x1Fu;
}

static uint8_t nvram_read(config_t *cfg, uint32_t offset) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    if ((offset & 0xFu) != 0) {
        LOG(2, "NVRAM data read off-centre +$%03X", offset);
        return 0xFF;
    }
    uint32_t idx = ((uint32_t)gc->nvram_bank * 32u + nvram_cell(offset)) % TNT_NVRAM_SIZE;
    if (idx >= 0x1300u && idx < 0x1400u)
        LOG(4, "XPRAM read nv[$%04X] -> $%02X", idx, gc->nvram[idx]);
    return gc->nvram[idx];
}

static void nvram_write(config_t *cfg, uint32_t offset, uint8_t value) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    if ((offset & 0xFu) != 0) {
        LOG(2, "NVRAM data write off-centre +$%03X = $%02X", offset, value);
        return;
    }
    uint32_t idx = ((uint32_t)gc->nvram_bank * 32u + nvram_cell(offset)) % TNT_NVRAM_SIZE;
    if (idx >= 0x1300u && idx < 0x1400u)
        LOG(4, "XPRAM write nv[$%04X] = $%02X (pc=%08X)", idx, value, ppc_get_pc(cfg->ppc));
    else
        LOG(3, "NVRAM write nv[$%04X] = $%02X (pc=%08X)", idx, value, ppc_get_pc(cfg->ppc));
    gc->nvram[idx] = value;
}

// ============================================================
// Island dispatch
// ============================================================

// === Object node: machine.gc ================================================
//
// Grand Central is the whole interrupt controller of a 7500/8500/9500 and an
// ANS, and it is the chip whose two clear modes make an IRQ storm here
// specifically hard to read: in mode 0 the line follows
// ((events | levels) & mask), in mode 1 it follows (latch & mask) alone, and
// which mode you are in is invisible from the register values.  So the four
// raw registers and the mode are all first-class here, and `active`
// overrides the generic `pending & enabled` to answer for the mode actually
// selected.

static tnt_gc_t *gc_obj(void *ctx) {
    return &tnt_st((config_t *)ctx)->gc;
}

static uint32_t gc_obj_pending(void *ctx) {
    const tnt_gc_t *gc = gc_obj(ctx);
    return gc->int_events | gc->int_levels;
}
static uint32_t gc_obj_enabled(void *ctx) {
    return gc_obj(ctx)->int_mask;
}
static uint32_t gc_obj_active(void *ctx) {
    const tnt_gc_t *gc = gc_obj(ctx);
    return gc->int_mode1 ? (gc->int_latch & gc->int_mask) : ((gc->int_events | gc->int_levels) & gc->int_mask);
}
// A PowerPC has one external-interrupt pin, not an IPL: 1 = asserted.
static int gc_obj_ipl(void *ctx) {
    return gc_obj_active(ctx) ? 1 : 0;
}

static const irq_controller_ops_t gc_irq_ops = {
    .chip = "Grand Central",
    .pending = gc_obj_pending,
    .enabled = gc_obj_enabled,
    .active = gc_obj_active,
    .ipl = gc_obj_ipl,
};

#define GC_U32_ATTR(NAME, EXPR)                                                                                        \
    static value_t gc_attr_##NAME(struct object *self, const member_t *m) {                                            \
        (void)m;                                                                                                       \
        const tnt_gc_t *gc = gc_obj(object_data(self));                                                                \
        value_t v = val_uint(4, (EXPR));                                                                               \
        v.flags |= VFLAG_HEX;                                                                                          \
        return v;                                                                                                      \
    }

GC_U32_ATTR(events, gc->int_events)
GC_U32_ATTR(levels, gc->int_levels)
GC_U32_ATTR(mask, gc->int_mask)
GC_U32_ATTR(latch, gc->int_latch)

static DEF_GETTER(gc_attr_clear_mode) {
    return val_uint(1, gc_obj(object_data(self))->int_mode1 ? 1u : 0u);
}

static const member_t gc_members[] = {
    IRQ_CONTROLLER_MEMBERS(&gc_irq_ops){.kind = MK_ATTR,
                                        .name = "events",
                                        .doc = "Edge-latched source rising edges (write-1-to-clear in mode 0)",
                                        .attr = {.type = VK_UINT,
                                                 .presentation_flags = VFLAG_HEX | VFLAG_VOLATILE,
                                                 .get = gc_attr_events,
                                                 .set = NULL}                                                                                          },
    {.kind = MK_ATTR,
                                        .name = "source_levels",
                                        .doc = "Live source picture, never latched",
                                        .attr = {.type = VK_UINT, .presentation_flags = VFLAG_HEX | VFLAG_VOLATILE, .get = gc_attr_levels, .set = NULL}},
    {.kind = MK_ATTR,
                                        .name = "mask",
                                        .doc = "Per-source enables",
                                        .attr = {.type = VK_UINT, .presentation_flags = VFLAG_HEX, .get = gc_attr_mask, .set = NULL}                   },
    {.kind = MK_ATTR,
                                        .name = "latch",
                                        .doc = "Mode-1 per-source output latch",
                                        .attr = {.type = VK_UINT, .presentation_flags = VFLAG_HEX | VFLAG_VOLATILE, .get = gc_attr_latch, .set = NULL} },
    {.kind = MK_ATTR,
                                        .name = "clear_mode",
                                        .doc = "0 = power-on ((events|levels) & mask); 1 = NanoKernel acknowledge (latch & mask)",
                                        .attr = {.type = VK_UINT, .get = gc_attr_clear_mode, .set = NULL}                                              },
};

static const class_desc_t gc_class = {
    .name = "irq_controller",
    .doc = "Grand Central, the PCI Power Mac I/O controller: interrupt state",
    .members = gc_members,
    .n_members = sizeof(gc_members) / sizeof(gc_members[0]),
};

// ============================================================
// machine.nvram — the 8 KB non-volatile store as the test lever
// ============================================================
// The banked store read flat: byte i is bank i/32, cell i%32.  The same
// shape as machine.rtc.pram on the 68k machines (peek/poke/dump/snapshot/
// restore), so a row can pin what lives here — the Mac OS XPRAM image at
// +$1300 (the display depth, the startup device) and Open Firmware's
// environment in the top bank — without driving a control panel.
static uint8_t *nvram_store(struct object *self) {
    tnt_state_t *st = tnt_st((config_t *)object_data(self));
    return st ? st->gc.nvram : NULL;
}

static DEF_METHOD(nvram_method_peek) {
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    uint64_t addr = argv[0].u;
    if (addr >= TNT_NVRAM_SIZE)
        return val_err("nvram.peek: offset 0x%llX is outside the %u-byte store", (unsigned long long)addr,
                       TNT_NVRAM_SIZE);
    return val_uint(1, nv[addr]);
}

static DEF_METHOD(nvram_method_poke) {
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    uint64_t addr = argv[0].u;
    const value_t *bytes = &argv[1];
    if (bytes->kind != VK_BYTES || !bytes->bytes.p)
        return val_err("nvram.poke: bytes argument must be VK_BYTES (use the :N width suffix, e.g. 0x80:1)");
    size_t n = bytes->bytes.n;
    if (n == 0)
        return val_err("nvram.poke: bytes argument is empty");
    if (addr >= TNT_NVRAM_SIZE || addr + n > TNT_NVRAM_SIZE)
        return val_err("nvram.poke: write of %zu bytes at 0x%llX would overflow the %u-byte store", n,
                       (unsigned long long)addr, TNT_NVRAM_SIZE);
    memcpy(nv + addr, bytes->bytes.p, n);
    return val_none();
}

static DEF_METHOD(nvram_method_dump) {
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    uint64_t addr = argv[0].u;
    uint64_t n = argv[1].u;
    if (addr >= TNT_NVRAM_SIZE || n == 0 || addr + n > TNT_NVRAM_SIZE)
        return val_err("nvram.dump: read of %llu bytes at 0x%llX would overflow the %u-byte store",
                       (unsigned long long)n, (unsigned long long)addr, TNT_NVRAM_SIZE);
    return val_bytes(nv + addr, (size_t)n);
}

static DEF_METHOD(nvram_method_snapshot) {
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    return val_bytes(nv, TNT_NVRAM_SIZE);
}

// Whole-store restore from a snapshot: how a row seeds one boot's formatted
// store into another.
static DEF_METHOD(nvram_method_restore) {
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    const value_t *bytes = &argv[0];
    if (bytes->kind != VK_BYTES || bytes->bytes.n != TNT_NVRAM_SIZE || !bytes->bytes.p)
        return val_err("nvram.restore: expected VK_BYTES of length %u (got len=%zu)", TNT_NVRAM_SIZE,
                       bytes->kind == VK_BYTES ? bytes->bytes.n : 0);
    memcpy(nv, bytes->bytes.p, TNT_NVRAM_SIZE);
    return val_none();
}

// `machine.nvram.clear()` — the battery pull, on every TNT board (the
// Network Server's `machine.board.clear_nvram()` is the same call).
static DEF_METHOD(nvram_method_clear) {
    config_t *cfg = (config_t *)object_data(self);
    if (!cfg || !tnt_st(cfg))
        return val_err("nvram not available");
    tnt_nvram_clear(cfg);
    return val_bool(true);
}

// The named fields: what a row or the frontend actually wants to pin,
// written into the store the way the firmware or Mac OS itself writes it.

static const arg_decl_t nvram_getenv_args[] = {
    {.name = "name", .kind = VK_STRING, .doc = "Open Firmware variable, e.g. \"boot-device\""},
};
static const arg_decl_t nvram_setenv_args[] = {
    {.name = "name",  .kind = VK_STRING, .doc = "Open Firmware variable"                 },
    {.name = "value", .kind = VK_STRING, .doc = "true/false, a hex number, or the string"},
};

// getenv(name): the variable as printenv shows it.
static DEF_METHOD(nvram_method_getenv) {
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    char buf[OF_NVRAM_OF_SIZE + 1];
    if (of_nvram_getenv(nv, argv[0].s, buf, sizeof(buf)) == OF_VAR_NONE)
        return val_err("nvram.getenv: no variable '%s' (or no valid Open Firmware partition)", argv[0].s);
    return val_str(buf);
}

// setenv(name, value): what Open Firmware's setenv writes.
static DEF_METHOD(nvram_method_setenv) {
    uint8_t *nv = nvram_store(self);
    if (!nv)
        return val_err("nvram not available");
    const char *err = of_nvram_setenv(nv, argv[0].s, argv[1].s);
    if (err)
        return val_err("nvram.setenv %s: %s", argv[0].s, err);
    return val_none();
}

// startup_disk: Mac OS's default startup device as a SCSI ID; -1 = none.
static DEF_GETTER(nvram_attr_startup_disk) {
    uint8_t *nv = nvram_store(self);
    return val_int(nv ? of_nvram_startup_scsi(nv) : -1);
}

static DEF_SETTER(nvram_attr_startup_disk_set) {
    uint8_t *nv = nvram_store(self);
    bool ok = true;
    int64_t id = val_as_i64(&in, &ok);
    value_free(&in);
    if (!nv)
        return val_err("nvram not available");
    if (!ok || id < -1 || id > 6)
        return val_err("nvram.startup_disk: a SCSI ID 0..6, or -1 for no default");
    of_nvram_set_startup_scsi(nv, (int)id, &of_nvram_defaults_tnt);
    return val_none();
}

// depth: the built-in Control video's saved depth, in bits per pixel.  The
// driver keeps one Name Registry record, 'gprf', whose data is {0, display
// mode, depth index 0/1/2 = 8/16/32 bpp, monitor code, 0...}; it reads it
// at boot and rewrites it when the depth changes.  0 = no record yet.
static uint8_t *gprf_data(uint8_t *nv) {
    return of_nvram_nr_find(nv, "gprf");
}

static DEF_GETTER(nvram_attr_depth) {
    uint8_t *nv = nvram_store(self);
    uint8_t *d = nv ? gprf_data(nv) : NULL;
    return val_uint(1, (d && d[2] <= 2) ? (8u << d[2]) : 0);
}

// The record a first save writes for each monitor sense (Control's own
// mode number and monitor code, measured per strap).
static void gprf_mode_for_sense(uint8_t sense, uint8_t *mode, uint8_t *code) {
    switch (sense) {
    case 0x0:
        *mode = 0x12;
        *code = 0x08;
        break; // 21" two-page, 1152x870
    case 0x1:
        *mode = 0x07;
        *code = 0x05;
        break; // portrait, 640x870
    case 0x2:
        *mode = 0x02;
        *code = 0x02;
        break; // 12", 512x384
    case 0x7:
        *mode = 0x06;
        *code = 0x00;
        break; // no monitor
    default:
        *mode = 0x06;
        *code = 0x03;
        break; // 13"/14" hi-res, 640x480
    }
}

static DEF_SETTER(nvram_attr_depth_set) {
    config_t *cfg = (config_t *)object_data(self);
    tnt_state_t *st = tnt_st(cfg);
    bool ok = true;
    uint64_t bpp = val_as_u64(&in, &ok);
    value_free(&in);
    if (!st)
        return val_err("nvram not available");
    if (tnt_board(cfg)->kind != TNT_BOARD_MAC)
        return val_err("nvram.depth: this board has no built-in Control video");
    uint8_t idx = bpp == 8 ? 0 : bpp == 16 ? 1 : bpp == 32 ? 2 : 0xFF;
    if (!ok || idx == 0xFF)
        return val_err("nvram.depth: 8, 16 or 32");
    uint8_t *d = gprf_data(st->gc.nvram);
    if (d) {
        d[2] = idx;
        return val_none();
    }
    // No record yet: write the one the driver's first save would, for the
    // monitor on the port.  Location: PCI bus 0 through one bridge, Chaos
    // device $0B -- the Control node's path.
    static const uint8_t control_location[6] = {0x11, 0x40, 0x00, 0x00, 0x10, 0x0B};
    uint8_t mode = 0, code = 0;
    gprf_mode_for_sense((uint8_t)(~st->control.mon_grounded & 7u), &mode, &code);
    const uint8_t data[OF_NVRAM_NR_DATA] = {0, mode, idx, code, 0, 0, 0, 0};
    if (!of_nvram_nr_add(st->gc.nvram, control_location, "gprf", data))
        return val_err("nvram.depth: the Name Registry area is full");
    return val_none();
}

static DEF_GETTER(nvram_attr_size) {
    return val_uint(4, TNT_NVRAM_SIZE);
}

static const arg_decl_t nvram_peek_args[] = {
    {.name = "addr", .kind = VK_UINT, .presentation_flags = VFLAG_HEX, .doc = "byte offset (0..$1FFF)"},
};
static const arg_decl_t nvram_poke_args[] = {
    {.name = "addr", .kind = VK_UINT, .presentation_flags = VFLAG_HEX, .doc = "byte offset (0..$1FFF)"},
    {.name = "bytes", .kind = VK_BYTES, .doc = "1..N bytes to write (use the :N integer-width suffix)"},
};
static const arg_decl_t nvram_dump_args[] = {
    {.name = "addr", .kind = VK_UINT, .presentation_flags = VFLAG_HEX, .doc = "byte offset (0..$1FFF)"},
    {.name = "n", .kind = VK_UINT, .doc = "byte count"},
};
static const arg_decl_t nvram_restore_args[] = {
    {.name = "bytes", .kind = VK_BYTES, .doc = "8192-byte buffer (typically from nvram.snapshot)"},
};

static const member_t nvram_members[] = {
    {.kind = MK_ATTR,
     .name = "size",
     .doc = "Store size in bytes (256 banks of 32)",
     .attr = {.type = VK_UINT, .get = nvram_attr_size, .set = NULL}                                   },
    {.kind = MK_METHOD,
     .name = "peek",
     .doc = "Read one byte at a flat offset (the Mac OS XPRAM image is at $1300 + PRAM address)",
     .method = {.args = nvram_peek_args, .nargs = 1, .result = VK_UINT, .fn = nvram_method_peek}      },
    {.kind = MK_METHOD,
     .name = "poke",
     .doc = "Write 1..N bytes at a flat offset",
     .method = {.args = nvram_poke_args, .nargs = 2, .result = VK_NONE, .fn = nvram_method_poke}      },
    {.kind = MK_METHOD,
     .name = "dump",
     .doc = "Read N bytes starting at a flat offset",
     .method = {.args = nvram_dump_args, .nargs = 2, .result = VK_BYTES, .fn = nvram_method_dump}     },
    {.kind = MK_METHOD,
     .name = "snapshot",
     .doc = "Read the whole 8 KB store",
     .method = {.args = NULL, .nargs = 0, .result = VK_BYTES, .fn = nvram_method_snapshot}            },
    {.kind = MK_METHOD,
     .name = "restore",
     .doc = "Write the whole store from a snapshot",
     .method = {.args = nvram_restore_args, .nargs = 1, .result = VK_NONE, .fn = nvram_method_restore}},
    {.kind = MK_METHOD,
     .name = "clear",
     .doc = "Pull the battery: back to the store a new board carries (blank on the Network Server)",
     .method = {.args = NULL, .nargs = 0, .result = VK_BOOL, .fn = nvram_method_clear}                },
    {.kind = MK_METHOD,
     .name = "getenv",
     .doc = "Read an Open Firmware variable, as printenv shows it",
     .method = {.args = nvram_getenv_args, .nargs = 1, .result = VK_STRING, .fn = nvram_method_getenv}},
    {.kind = MK_METHOD,
     .name = "setenv",
     .doc = "Set an Open Firmware variable, as setenv does (repacks, re-checksums)",
     .method = {.args = nvram_setenv_args, .nargs = 2, .result = VK_NONE, .fn = nvram_method_setenv}  },
    {.kind = MK_ATTR,
     .name = "startup_disk",
     .doc = "Mac OS's default startup device as a SCSI ID (XPRAM $78-$7B); -1 = none",
     .attr = {.type = VK_INT, .get = nvram_attr_startup_disk, .set = nvram_attr_startup_disk_set}     },
    {.kind = MK_ATTR,
     .name = "depth",
     .doc = "Built-in video's saved depth in bpp (8/16/32; 0 = not saved yet), read at boot",
     .attr = {.type = VK_UINT, .get = nvram_attr_depth, .set = nvram_attr_depth_set}                  },
};

static const class_desc_t nvram_class = {
    .name = "nvram",
    .doc = "Grand Central's NVRAM: read, write, snapshot, clear",
    .members = nvram_members,
    .n_members = sizeof(nvram_members) / sizeof(nvram_members[0]),
};

void tnt_gc_attach_object(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (!st || st->gc_object)
        return;
    st->gc_object = object_new(&gc_class, cfg, "gc");
    if (!st->gc_object)
        return;
    object_set_order(st->gc_object, 45);
    object_attach(machine_object(), st->gc_object);
    // The non-volatile store beside it: machine.nvram.
    st->nvram_object = object_new(&nvram_class, cfg, "nvram");
    if (st->nvram_object) {
        object_set_label(st->nvram_object, "NVRAM");
        object_set_order(st->nvram_object, 46);
        object_attach(machine_object(), st->nvram_object);
    }
}

void tnt_gc_detach_object(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (st && st->nvram_object) {
        object_detach(st->nvram_object);
        object_delete(st->nvram_object);
        st->nvram_object = NULL;
    }
    if (st && st->gc_object) {
        object_detach(st->gc_object);
        object_delete(st->gc_object);
        st->gc_object = NULL;
    }
}

void tnt_gc_init(config_t *cfg) {
    tnt_gc_t *gc = &tnt_st(cfg)->gc;
    // Power-on: everything masked, nothing latched.  NVRAM contents are
    // deliberately NOT touched — the store is non-volatile (tnt.c zeroes
    // it once at machine construction).
    gc->int_events = 0;
    gc->int_mask = 0;
    gc->int_levels = 0;
    // The mode-1 output latch and the clear-mode selector, which this said
    // "nothing latched" about while leaving both standing.  With int_mask zero
    // the line is quiet either way, so nothing fired immediately -- but the
    // moment post-reset firmware writes its first mask, stale pre-reset latch
    // bits inside it assert the CPU line for sources that never re-asserted.
    // And a clear mode surviving a reset means a machine restarted out of
    // MkLinux boots the ROM in mode 1 instead of the power-on mode 0.
    gc->int_latch = 0;
    gc->int_mode1 = false;
    gc->nvram_bank = 0;
}

// ============================================================
// Grand Central's PCI presence — device 16 on Bandit 1
// ============================================================
// Apple's own device tree calls it /gc@10, and Open Firmware does issue a
// command + BAR write at IDSEL 16 during probe-slots.  What that write
// lands on was an open question until the diagnostic utility's bridge
// test named the vendor; the header below is the generic type-0 one with
// Grand Central's ids.
//
// Note also what Grand Central is NOT: its 128 KB island at $F3000000 is
// reached by PASS-THROUGH MEMORY cycles, not through a BAR (ANS developer
// note §4.2.1), so tnt.c maps it at a fixed address and no BAR is
// declared here.
static const char *gc_pci_name(const pci_device_t *dev) {
    (void)dev;
    return "Grand Central";
}

// The identity, recovered from the Network Server Diagnostic Utility's
// PCI bridge test (`CheckPCI`): it reads config dword 0 of IDSEL 16 on
// Bandit 1 and reports "Grand Central not found" unless the vendor is
// Apple ($106B); the device id is Apple's Grand Central number ($0002 --
// Bandit is $0001, O'Hare $0007), the class the "unassigned" $FF0000 the
// part reports.  No BARs (see above); the command register latches.
static const pci_config_decl_t gc_decl = {
    .vendor_id = 0x106Bu,
    .device_id = 0x0002u,
    .revision = 0x02u,
    .class_code = 0xFF0000u,
    .header_type = 0x00u,
    .command_writable = PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_MASTER,
};

static const pci_device_ops_t gc_pci_ops = {
    .name = gc_pci_name,
};

void tnt_gc_pci_attach(config_t *cfg, pci_bus_t *bus, checkpoint_t *cp) {
    tnt_state_t *st = tnt_st(cfg);
    st->gc_dev.ops = &gc_pci_ops;
    st->gc_dev.decl = &gc_decl;
    st->gc_dev.priv = cfg;
    pci_cfg_reset(&st->gc_dev);
    pci_bus_add_device(bus, &st->gc_dev, 16);
    pci_device_part(cfg, cp, &st->gc_dev, "pci.gc");
}

// Map an ESCC-aperture offset (+$13000: B ctl +$00 / B data +$10 /
// A ctl +$20 / A data +$30) onto the SCC cell's classic address pins
// (A/B on address bit 1, D/C on bit 2 — the legacy-aperture layout the
// shared model decodes natively).
static uint32_t escc_pins(uint32_t off) {
    uint32_t ab = (off >> 5) & 1u; // A channel at +$20
    uint32_t dc = (off >> 4) & 1u; // data register at +$10
    return (ab << 1) | (dc << 2);
}

// --- The ESCC's four DBDMA channels ------------------------------------------
//
// Grand Central gives each SCC channel a transmit and a receive DBDMA
// channel (4/5 for A, 6/7 for B).  An OUTPUT command hands the port a run
// of bytes, which go to the channel's data register one at a time -- the
// same path as a CPU store to it, so local loopback (WR14 LOOP) and the
// cross-channel cable behave as they do under PIO; an INPUT command asks
// for what the channel has received, which the port serves from the
// receive queue and kicks again whenever a data register is written.
// (Bytes arriving from the host side -- `machine.scc.a.receive` -- reach
// an INPUT program at its next kick; the diagnostic utility's DMA tests
// are loopbacks and never wait on that.)  Fitted to the Network Server
// Diagnostic Utility's serial DMA tests (SccDMA: channel 4 OUTPUT of an
// 80-byte pattern, channel 5 INPUT of the loopback, byte-for-byte compare).

#define SCC_DMA_TX(ch) (4u + 2u * (ch))
#define SCC_DMA_RX(ch) (5u + 2u * (ch))

static uint32_t scc_data_pins(unsigned ch) {
    // The cell's address pins: A/B on bit 1 (A = 1), D/C on bit 2 -- so
    // channel 0 (A) data is 6, channel 1 (B) data is 4 (escc_pins).
    return (ch == 0 ? 2u : 0u) | 4u;
}

static void scc_dma_kick_rx(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    if (!st->dbdma)
        return;
    for (unsigned ch = 0; ch < 2; ch++)
        if (dbdma_active(st->dbdma, SCC_DMA_RX(ch)))
            dbdma_kick(st->dbdma, SCC_DMA_RX(ch));
}

static int scc_port_out(void *ctx, const uint8_t *buf, int len) {
    tnt_scc_dma_ctx_t *c = (tnt_scc_dma_ctx_t *)ctx;
    const memory_interface_t *mi = scc_get_memory_interface(c->cfg->scc);
    for (int n = 0; n < len; n++)
        mi->write_uint8(c->cfg->scc, scc_data_pins(c->ch), buf[n]);
    scc_dma_kick_rx(c->cfg);
    return len;
}

static int scc_port_in(void *ctx, uint8_t *buf, int len) {
    tnt_scc_dma_ctx_t *c = (tnt_scc_dma_ctx_t *)ctx;
    const memory_interface_t *mi = scc_get_memory_interface(c->cfg->scc);
    int n = 0;
    while (n < len && scc_channel_rx_pending(c->cfg->scc, c->ch) > 0)
        buf[n++] = mi->read_uint8(c->cfg->scc, scc_data_pins(c->ch));
    return n;
}

void tnt_scc_dma_init(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    for (unsigned ch = 0; ch < 2; ch++) {
        st->scc_dma_ctx[ch].cfg = cfg;
        st->scc_dma_ctx[ch].ch = ch;
        dbdma_port_t tx = {.out = scc_port_out, .in = NULL, .s_bits = NULL, .ctx = &st->scc_dma_ctx[ch]};
        dbdma_port_t rx = {.out = NULL, .in = scc_port_in, .s_bits = NULL, .ctx = &st->scc_dma_ctx[ch]};
        dbdma_set_port(st->dbdma, SCC_DMA_TX(ch), &tx);
        dbdma_set_port(st->dbdma, SCC_DMA_RX(ch), &rx);
    }
}

// One island byte: the guest's read, or an inspection (`peek`) routed to each
// chip's side-effect-free peek.
static uint8_t gc_access8(config_t *cfg, uint32_t offset, bool peek) {
    uint32_t block = offset & 0x1F000u;
    switch (block) {
    case OFF_VIA:
    case OFF_VIA + 0x1000: // 16 regs at stride $200 span the 8 KB window
        return memory_iface_read8(via_get_memory_interface(cfg->via1), cfg->via1, offset - OFF_VIA, peek);
    case OFF_SCCLEG:
        // Legacy aperture: +0 bCtl / +2 aCtl / +4 bData / +6 aData — the
        // low offset bits carry the chip's A/B and D/C pins directly.
        return memory_iface_read8(scc_get_memory_interface(cfg->scc), cfg->scc, offset - OFF_SCCLEG, peek);
    case OFF_ESCC:
        return memory_iface_read8(scc_get_memory_interface(cfg->scc), cfg->scc, escc_pins(offset - OFF_ESCC), peek);
    case OFF_NVPORT:
        return tnt_st(cfg)->gc.nvram_bank;
    case OFF_NVDATA:
        return nvram_read(cfg, offset - OFF_NVDATA);
    case OFF_BOXID: {
        // Byte j of the little-endian register (the ROM reads it both as
        // lwbrx and byte-wise).  On the Network Servers this is Board
        // Register 1 and the ROM reads bytes +0 and +1 only.
        uint8_t b = (uint8_t)(tnt_gc_boxid(cfg) >> (8 * (offset & 3u)));
        LOG(3, "BoxID byte read +%u -> $%02X", offset & 3u, b);
        return b;
    }
    case OFF_LCDGB: // GBUS device 3 — LCD + timebase enable (ANS only)
        if (tnt_board(cfg)->has_gbus)
            return tnt_lcd_read8(cfg, offset - OFF_LCDGB);
        break;
    case OFF_EPROM: // Ethernet address PROM + MP doorbell (ANS only)
    case OFF_BREG2: // Board Register 2 — the environmental halfword
        if (tnt_board(cfg)->has_gbus)
            return peek ? tnt_gbus_peek8(cfg, offset) : tnt_gbus_read8(cfg, offset);
        break;
    case OFF_RADACAL:
        return peek ? tnt_control_rad_peek(cfg, offset - OFF_RADACAL) : tnt_control_rad_read(cfg, offset - OFF_RADACAL);
    case OFF_SCSI0:
        // 53C94: sixteen byte-wide registers on $10 centres.
        {
            unsigned reg = ((offset - OFF_SCSI0) >> 4) & 0xFu;
            return peek ? scsi_53c96_peek(tnt_st(cfg)->scsi96, reg) : scsi_53c96_read(tnt_st(cfg)->scsi96, reg);
        }
    case OFF_SWIM3:
        // SWIM3: sixteen byte-wide registers on $10 centres (swim3.c).
        return peek ? tnt_swim3_peek(cfg, offset - OFF_SWIM3) : tnt_swim3_read(cfg, offset - OFF_SWIM3);
    case OFF_MESH:
        // Absent on the Network Servers (board delta #4): the aperture
        // decodes nothing, so it falls through to the open-bus log.
        if (tnt_board(cfg)->has_mesh)
            return peek ? mesh_peek(tnt_st(cfg)->mesh, offset - OFF_MESH)
                        : mesh_read(tnt_st(cfg)->mesh, offset - OFF_MESH);
        LOG(1, "byte read of the absent MESH aperture +$%05X", offset);
        return 0;
    default:
        break;
    }
    LOG(1, "byte read of unwired island offset +$%05X", offset);
    return 0;
}

uint8_t tnt_gc_read8(config_t *cfg, uint32_t offset) {
    return gc_access8(cfg, offset, false);
}

uint8_t tnt_gc_peek8(config_t *cfg, uint32_t offset) {
    return gc_access8(cfg, offset, true);
}

void tnt_gc_write8(config_t *cfg, uint32_t offset, uint8_t value) {
    uint32_t block = offset & 0x1F000u;
    switch (block) {
    case OFF_VIA:
    case OFF_VIA + 0x1000:
        via_get_memory_interface(cfg->via1)->write_uint8(cfg->via1, offset - OFF_VIA, value);
        return;
    case OFF_SCCLEG:
        scc_get_memory_interface(cfg->scc)->write_uint8(cfg->scc, offset - OFF_SCCLEG, value);
        return;
    case OFF_ESCC:
        scc_get_memory_interface(cfg->scc)->write_uint8(cfg->scc, escc_pins(offset - OFF_ESCC), value);
        scc_dma_kick_rx(cfg);
        return;
    case OFF_NVPORT:
        // The bank-select port is ONE byte-wide cell on the $10 centre.
        // Load-bearing: the ROM's XPRam trap path selects the bank with
        // a 16-bit write of the byte-swapped bank number (ROM $FFC5831E,
        // `move.w` of bank<<8) — the bus splits it into the +$1D000 byte
        // (the bank) and a +$1D001 byte ($00) that lands on NO cell.  A
        // model that latches the off-centre byte clobbers the bank back
        // to 0 and every trap-path PRAM read serves bank 0 — the T12
        // "XPRAM $77 reads 0" wall.
        if (((offset - OFF_NVPORT) & 0xFu) == 0)
            tnt_st(cfg)->gc.nvram_bank = value;
        else
            LOG(3, "NVRAM bank-port off-centre byte +$%03X = $%02X ignored", offset - OFF_NVPORT, value);
        return;
    case OFF_NVDATA:
        nvram_write(cfg, offset - OFF_NVDATA, value);
        return;
    case OFF_RADACAL:
        tnt_control_rad_write(cfg, offset - OFF_RADACAL, value);
        return;
    case OFF_LCDGB: // GBUS device 3 — the write-only LCD ports (ANS only)
        if (tnt_board(cfg)->has_gbus) {
            tnt_lcd_write8(cfg, offset - OFF_LCDGB, value);
            return;
        }
        break;
    case OFF_EPROM:
    case OFF_BREG2:
        if (tnt_board(cfg)->has_gbus) {
            tnt_gbus_write8(cfg, offset, value);
            return;
        }
        break;
    case OFF_SCSI0:
        scsi_53c96_write(tnt_st(cfg)->scsi96, ((offset - OFF_SCSI0) >> 4) & 0xFu, value);
        return;
    case OFF_SWIM3:
        tnt_swim3_write(cfg, offset - OFF_SWIM3, value);
        return;
    case OFF_MESH:
        if (tnt_board(cfg)->has_mesh)
            mesh_write(tnt_st(cfg)->mesh, offset - OFF_MESH, value);
        else
            LOG(1, "byte write of the absent MESH aperture +$%05X = $%02X", offset, value);
        return;
    default:
        break;
    }
    LOG(1, "byte write of unwired island offset +$%05X = $%02X", offset, value);
}

// 32-bit access: the LE register blocks.  `value` at this boundary is the
// big-endian bus view; TNT_LE32 recovers the little-endian register value
// the guest composed with stwbrx (and vice versa on reads).
static uint32_t gc_access32(config_t *cfg, uint32_t offset, bool peek) {
    if (offset >= OFF_INTS && offset < OFF_INTS + 0x10u)
        return TNT_LE32(int_read(cfg, offset));
    if (offset >= OFF_DBDMA && offset < OFF_DBDMA_END) {
        // DBDMA channel n at +$8000+n*$100; registers are LE longwords.
        int chan = (int)((offset - OFF_DBDMA) >> 8);
        return TNT_LE32(dbdma_reg_read(tnt_st(cfg)->dbdma, chan, offset & 0xFFu));
    }
    if ((offset & 0x1F000u) == OFF_AWACS)
        return TNT_LE32(tnt_awacs_read32(cfg, offset - OFF_AWACS));
    if ((offset & 0x1F000u) == OFF_BOXID) {
        LOG(3, "BoxID read -> $%08X", tnt_gc_boxid(cfg));
        return TNT_LE32(tnt_gc_boxid(cfg));
    }
    // The Network Server's GBUS blocks: Board Register 2 (a little-endian
    // halfword like BoxID) and the Ethernet PROM's byte cells.
    if (tnt_board(cfg)->has_gbus && ((offset & 0x1F000u) == OFF_BREG2 || (offset & 0x1F000u) == OFF_EPROM))
        return peek ? tnt_gbus_peek32(cfg, offset) : tnt_gbus_read32(cfg, offset);
    // The NVRAM data window's byte cells answering a longword cycle.  The
    // production ANS ROM reads them this way; a byte-wide cell on this
    // big-endian bus drives lane 0, which is the MOST significant byte of
    // the longword.  Gated on the board because no Macintosh TNT ROM has
    // ever been observed issuing the cycle, and widening their decode
    // without a TNT ladder run to prove it would be a blind change.
    if (tnt_board(cfg)->has_gbus && (offset & 0x1F000u) == OFF_NVDATA)
        return (uint32_t)nvram_read(cfg, offset - OFF_NVDATA) << 24;
    LOG(1, "long read of unwired island offset +$%05X", offset);
    return 0;
}

uint32_t tnt_gc_read32(config_t *cfg, uint32_t offset) {
    return gc_access32(cfg, offset, false);
}

uint32_t tnt_gc_peek32(config_t *cfg, uint32_t offset) {
    return gc_access32(cfg, offset, true);
}

void tnt_gc_write32(config_t *cfg, uint32_t offset, uint32_t value) {
    if (offset >= OFF_INTS && offset < OFF_INTS + 0x10u) {
        int_write(cfg, offset, TNT_LE32(value));
        return;
    }
    if (offset >= OFF_DBDMA && offset < OFF_DBDMA_END) {
        int chan = (int)((offset - OFF_DBDMA) >> 8);
        dbdma_reg_write(tnt_st(cfg)->dbdma, chan, offset & 0xFFu, TNT_LE32(value));
        return;
    }
    if ((offset & 0x1F000u) == OFF_AWACS) {
        tnt_awacs_write32(cfg, offset - OFF_AWACS, TNT_LE32(value));
        return;
    }
    if ((offset & 0x1F000u) == OFF_NVPORT) {
        // POST selects the NVRAM bank with a 32-bit stwbrx of the bank
        // number (the shipping ROM's logging helper does exactly this).
        tnt_st(cfg)->gc.nvram_bank = (uint8_t)TNT_LE32(value);
        return;
    }
    LOG(1, "long write of unwired island offset +$%05X = $%08X", offset, value);
}
