// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// prep_io.c
// A PReP-compatibility I/O window for the TNT boards -- an EXPERIMENT, off
// unless the machine is built with `prep_io=on`.
//
// A PReP machine (IBM Power Series, Motorola PowerStack) puts its ISA/PCI
// I/O space at $80000000 and its PCI configuration space at $80800000, and
// software written for one (the OS/2 Warp PowerPC Edition boot loader and
// microkernel) drives PC-style ISA devices there directly.  No Macintosh has
// any of that.  This window is the "PCI-to-ISA bridge card" such a port
// would need on real hardware, built into the board: when enabled it claims
// $80000000-$80FFFFFF (Bandit 1's PCI memory window then starts at
// $81000000) and decodes:
//
//   $80000000 + port     ISA I/O: port $92 (the endian bit), $850 (the I/O
//                        map), a 16550 COM1 at $3F8 (machine.prep.com1,
//                        with sent()/receive() like the SCC channels),
//                        an MC146818 RTC at $70/$71 reading the board's
//                        clock (machine.rtc), PReP's 8 KB NVRAM behind
//                        $74/$75 (index) and $76/$77 (data), an 8042
//                        keyboard controller at $60/$64 (polled: no keys
//                        yet, but the self-tests and the keyboard's and
//                        mouse's command replies a driver probes with),
//                        the two 8259s at $20/$21 and $A0/$A1 with the
//                        edge/level control registers at $4D0/$4D1;
//                        ports from $1000 up forwarded to Bandit 1's PCI I/O
//                        space ($F2000000 + port), where PCI I/O BARs live;
//                        every other port logged and floating ($FF)
//   $80800000            PCI configuration, PReP direct-mapped: the slot
//                        is a one-hot bit in A11-A18 (1 << slot, Linux's
//                        CFGADDR), the function A8-A10, the register
//                        A0-A7; higher bits are ignored (the OS/2 loader
//                        reads the Sandalfoot's SCSI chip at $80881000,
//                        i.e. slot 12 with A19 set).  The PReP slots are
//                        mapped onto the Network Server's own devices: slot
//                        12 (the Sandalfoot's on-board 53C810) is the first
//                        53C825A (Bandit 1 device $11), slot 13 the second
//                        ($12).  Every other slot is empty.  Slot 12
//                        presents the 53C810's device ID: the platform's
//                        stanzas (RDF.STZ, "11000") bind the NCR driver to
//                        it, and the 53C825A is register-compatible.
//   $BFFFFFF0            interrupt acknowledge: the 8259 pair's vector
//
// Interrupts.  Once the guest has switched the bridge to little-endian PReP
// mode (port $92), the CPU's external interrupt is the 8259 pair's output
// (grand_central.c asks tnt_prep_io_owns_irq), and the board's interrupt
// sources a PReP OS knows are wired to the PIC: the first 53C825A (Grand
// Central source 22) to IRQ 13 -- the Sandalfoot's SCSI line -- the second
// (26) to IRQ 15, the 8042 to IRQ 1 and 12.
//
// Byte lanes.  A 60x in little-endian mode munges a data address (byte ^7,
// halfword ^6, word ^4); a PReP host bridge in little-endian mode -- System
// Control Port A ($92) bit 1 set -- reverses the eight byte lanes between
// CPU and memory AND I/O, which undoes the munge and byte-reverses the
// value: PReP software sees true little-endian memory, and its boot loader
// depends on that (it byte-reverses its own image and then executes it).
// Hammerhead never does this, so a write to port $92 sets the CPU core's
// bridge flag (ppc_set_le_bridge), which models exactly that reversal, and
// this window then sees plain addresses and the bus's usual big-endian
// value convention (the core has already swapped the value).
//
// Not checkpointed: an experiment's state.

#include "tnt.h"

#include "log.h"
#include "memory.h"
#include "object.h"
#include "pci.h"
#include "ppc.h"
#include "rtc.h"
#include "system.h"
#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_USE_CATEGORY_NAME("prep");

#define PREP_IO_BASE   0x80000000u
#define PREP_IO_SIZE   0x01000000u // ISA/PCI I/O (8 MB) + the config window above it
#define PREP_PORT92_LE 0x02u
#define PREP_CFG_BASE  0x00800000u // offset of the config window inside ours
#define PREP_PCI_IO    0x1000u // ports from here up are PCI I/O (Bandit 1)
#define COM_FIFO       4096u

typedef struct prep_uart {
    uint8_t ier, lcr, mcr, scr, dll, dlm, fcr;
    uint8_t rx[COM_FIFO];
    unsigned rx_head, rx_tail;
    unsigned rx_hold; // LSR polls before the next queued byte "arrives" (one character time)
    uint64_t polls_since_tx; // LSR reads since the guest last transmitted: large = it is waiting for input
    uint8_t tx[COM_FIFO];
    unsigned tx_len;
    uint64_t tx_dropped;
} prep_uart_t;

typedef struct prep_rtc {
    uint8_t index;
    uint8_t reg_b; // 24-hour, BCD by default; the guest may select binary (DM)
    uint8_t ram[128]; // $0E-$7F: CMOS RAM
} prep_rtc_t;

typedef struct prep_kbc {
    uint8_t out[64]; // output buffer queue, aux flag in out_aux
    bool out_aux[64];
    unsigned head, tail;
    uint8_t cmd_byte;
    uint8_t pending; // controller command awaiting its data byte ($60, $D4, $D1...), or 0
    uint8_t kbd_pending; // keyboard command awaiting its argument ($ED, $F0, $F3), or 0
    bool last_was_cmd;
} prep_kbc_t;

typedef struct prep_pic {
    uint8_t irr, isr, imr;
    uint8_t lines; // input levels
    uint8_t elcr; // 1 = level-triggered
    uint8_t base; // ICW2
    uint8_t icw_step; // 0 = operational; 2/3/4 = expecting that ICW
    bool icw4, single, auto_eoi, read_isr;
} prep_pic_t;

typedef struct tnt_prep_io {
    config_t *cfg;
    memory_interface_t iface;
    uint8_t port92;
    uint8_t port850;
    // Does the bridge reverse the byte lanes to memory in little-endian mode,
    // as a PReP host bridge does (machine.prep.lane_reversal, default on)?
    // Off models a Macintosh's memory: the CPU's munge is all there is, so
    // the window itself undoes it (munged()) and ISA byte order applies.
    bool lane_reversal;
    prep_uart_t com1;
    prep_rtc_t rtc;
    prep_kbc_t kbc;
    prep_pic_t pic[2]; // master ($20), slave ($A0) cascaded on IRQ 2
    memory_interface_t iack_iface;
    uint16_t nv_index;
    uint8_t nvram[8192];
    uint64_t unknown_reads, unknown_writes;
    struct object *node, *com1_node;
} tnt_prep_io_t;

static tnt_prep_io_t *g_prep; // one machine at a time; the object methods find it here

// --- 16550 ----------------------------------------------------------------

static uint8_t uart_read(prep_uart_t *u, unsigned reg) {
    bool dlab = (u->lcr & 0x80) != 0;
    switch (reg) {
    case 0:
        if (dlab)
            return u->dll;
        if (u->rx_head != u->rx_tail && !u->rx_hold) {
            uint8_t c = u->rx[u->rx_tail];
            u->rx_tail = (u->rx_tail + 1) % COM_FIFO;
            u->rx_hold = 4000;
            return c;
        }
        return 0;
    case 1:
        return dlab ? u->dlm : u->ier;
    case 2: // IIR: no interrupt pending; FIFOs enabled when FCR asked
        return (u->fcr & 1) ? 0xC1 : 0x01;
    case 3:
        return u->lcr;
    case 4:
        return u->mcr;
    case 5: // LSR: transmitter always empty; data ready once the next byte has "arrived"
        u->polls_since_tx++;
        if (u->rx_hold)
            u->rx_hold--;
        return 0x60 | (u->rx_head != u->rx_tail && !u->rx_hold ? 0x01 : 0x00);
    case 6: // MSR: a terminal on the cable -- CTS, DSR, DCD
        return 0xB0;
    default:
        return u->scr;
    }
}

static void uart_write(prep_uart_t *u, unsigned reg, uint8_t v) {
    bool dlab = (u->lcr & 0x80) != 0;
    switch (reg) {
    case 0:
        if (dlab) {
            u->dll = v;
        } else if (u->tx_len < COM_FIFO) {
            u->tx[u->tx_len++] = v;
            u->polls_since_tx = 0;
        } else {
            u->tx_dropped++;
        }
        break;
    case 1:
        if (dlab)
            u->dlm = v;
        else
            u->ier = v;
        break;
    case 2:
        u->fcr = v;
        break;
    case 3:
        u->lcr = v;
        break;
    case 4:
        u->mcr = v;
        break;
    case 7:
        u->scr = v;
        break;
    default:
        break;
    }
}

// --- MC146818 RTC ----------------------------------------------------------
// The time is the board's own clock (rtc_get_seconds: Macintosh seconds since
// 1904, advancing with emulated time), so the guest's view of it is
// deterministic.  Update-in-progress is never set: a read is always coherent.

static uint8_t rtc_enc(const prep_rtc_t *r, int v) {
    return (r->reg_b & 0x04) ? (uint8_t)v : (uint8_t)(((v / 10) << 4) | (v % 10));
}

static uint8_t rtc_read(tnt_prep_io_t *p) {
    prep_rtc_t *r = &p->rtc;
    unsigned i = r->index & 0x7Fu;
    if (i <= 9) {
        uint32_t mac = p->cfg->rtc ? rtc_get_seconds(p->cfg->rtc) : 0;
        time_t t = (time_t)((int64_t)mac - 2082844800LL); // 1904 -> 1970
        struct tm tm;
        gmtime_r(&t, &tm);
        switch (i) {
        case 0:
            return rtc_enc(r, tm.tm_sec);
        case 2:
            return rtc_enc(r, tm.tm_min);
        case 4:
            return rtc_enc(r, tm.tm_hour);
        case 6:
            return rtc_enc(r, tm.tm_wday + 1);
        case 7:
            return rtc_enc(r, tm.tm_mday);
        case 8:
            return rtc_enc(r, tm.tm_mon + 1);
        case 9:
            return rtc_enc(r, tm.tm_year % 100);
        default:
            return 0; // the alarms
        }
    }
    switch (i) {
    case 0x0A:
        return 0x26; // 32.768 kHz time base, 1024 Hz periodic rate, UIP clear
    case 0x0B:
        return r->reg_b;
    case 0x0C:
        return 0x00;
    case 0x0D:
        return 0x80; // VRT: the battery is good
    default:
        return r->ram[i];
    }
}

static void rtc_write(tnt_prep_io_t *p, uint8_t v) {
    prep_rtc_t *r = &p->rtc;
    unsigned i = r->index & 0x7Fu;
    if (i == 0x0B)
        r->reg_b = v;
    else if (i >= 0x0E)
        r->ram[i] = v;
    // Setting the time is accepted and ignored: the board's clock is the time.
}

// --- 8042 keyboard controller ---------------------------------------------------

static void kbc_push(prep_kbc_t *k, uint8_t v, bool aux) {
    unsigned next = (k->head + 1) % 64;
    if (next == k->tail)
        return;
    k->out[k->head] = v;
    k->out_aux[k->head] = aux;
    k->head = next;
}

static uint8_t kbc_status(prep_kbc_t *k) {
    uint8_t st = 0x14; // SYS (self-test passed), keyboard not inhibited
    if (k->head != k->tail)
        st |= 0x01 | (k->out_aux[k->tail] ? 0x20 : 0x00);
    if (k->last_was_cmd)
        st |= 0x08;
    return st;
}

static uint8_t kbc_data_read(prep_kbc_t *k) {
    if (k->head == k->tail)
        return 0x00;
    uint8_t v = k->out[k->tail];
    k->tail = (k->tail + 1) % 64;
    return v;
}

// A byte for the keyboard (aux=false) or the mouse (aux=true): both answer
// like the stock devices -- ACK, and the extra bytes reset and identify return.
static void kbc_device_byte(prep_kbc_t *k, uint8_t v, bool aux) {
    if (!aux && k->kbd_pending) {
        uint8_t c = k->kbd_pending;
        k->kbd_pending = 0;
        kbc_push(k, 0xFA, false);
        if (c == 0xF0 && v == 0)
            kbc_push(k, 0x02, false); // current scan code set: 2
        return;
    }
    switch (v) {
    case 0xFF: // reset: ACK, BAT passed (a mouse adds its ID)
        kbc_push(k, 0xFA, aux);
        kbc_push(k, 0xAA, aux);
        if (aux)
            kbc_push(k, 0x00, aux);
        break;
    case 0xF2: // identify
        kbc_push(k, 0xFA, aux);
        if (aux) {
            kbc_push(k, 0x00, aux);
        } else {
            kbc_push(k, 0xAB, aux);
            kbc_push(k, 0x83, aux);
        }
        break;
    case 0xEE: // echo
        kbc_push(k, 0xEE, aux);
        break;
    case 0xED: // LEDs, typematic rate, scan code set: an argument follows
    case 0xF3:
    case 0xF0:
        if (!aux)
            k->kbd_pending = v;
        kbc_push(k, 0xFA, aux);
        break;
    default:
        kbc_push(k, 0xFA, aux);
        break;
    }
}

static void kbc_data_write(prep_kbc_t *k, uint8_t v) {
    k->last_was_cmd = false;
    uint8_t c = k->pending;
    k->pending = 0;
    switch (c) {
    case 0x60:
        k->cmd_byte = v;
        return;
    case 0xD4:
        kbc_device_byte(k, v, true);
        return;
    case 0xD1: // output port: the A20 / reset lines, nothing here
        return;
    default:
        kbc_device_byte(k, v, false);
        return;
    }
}

static void kbc_command(prep_kbc_t *k, uint8_t v) {
    k->last_was_cmd = true;
    switch (v) {
    case 0x20:
        kbc_push(k, k->cmd_byte, false);
        break;
    case 0x60:
    case 0xD1:
    case 0xD4:
        k->pending = v;
        break;
    case 0xAA: // controller self-test
        kbc_push(k, 0x55, false);
        break;
    case 0xAB: // keyboard interface test
    case 0xA9: // aux interface test
        kbc_push(k, 0x00, false);
        break;
    case 0xC0: // input port
        kbc_push(k, 0xBF, false);
        break;
    case 0xD0: // output port
        kbc_push(k, 0xCF, false);
        break;
    default: // $A7/$A8/$AD/$AE enable/disable, $FE pulse reset...: accepted
        break;
    }
}

// --- the 8259 pair -------------------------------------------------------------
// Fully nested mode, fixed priority (0 highest), the slave on master IRQ 2.

static int pic_highest(uint8_t bits) {
    return bits ? __builtin_ctz(bits) : -1;
}

// The IRQ a PIC would deliver now: requested, unmasked, and of higher
// priority than everything in service.
static int pic_pending(const prep_pic_t *c) {
    int req = pic_highest(c->irr & (uint8_t)~c->imr);
    if (req < 0)
        return -1;
    int svc = pic_highest(c->isr);
    return (svc >= 0 && svc <= req) ? -1 : req;
}

static void pic_update(tnt_prep_io_t *p);

static void pic_input(prep_pic_t *c, int n, bool level) {
    uint8_t bit = (uint8_t)(1u << n);
    bool was = (c->lines & bit) != 0;
    if (level)
        c->lines |= bit;
    else
        c->lines &= (uint8_t)~bit;
    if (c->elcr & bit) {
        if (level)
            c->irr |= bit;
        else
            c->irr &= (uint8_t)~bit;
    } else if (level && !was) {
        c->irr |= bit;
    }
}

// Drive ISA IRQ `irq` (0-15).
static void prep_irq(tnt_prep_io_t *p, int irq, bool level) {
    if (irq < 0 || irq > 15)
        return;
    pic_input(&p->pic[irq >> 3], irq & 7, level);
    pic_update(p);
}

// The slave's output is master input 2; the master's output is the CPU's.
static void pic_update(tnt_prep_io_t *p) {
    pic_input(&p->pic[0], 2, pic_pending(&p->pic[1]) >= 0);
    if (tnt_prep_io_owns_irq(p->cfg))
        tnt_gc_recompute(p->cfg);
}

static void pic_eoi(prep_pic_t *c, int n) {
    if (n < 0)
        n = pic_highest(c->isr);
    if (n >= 0)
        c->isr &= (uint8_t) ~(1u << n);
}

// The acknowledge cycle: the vector of the highest pending IRQ, its ISR bit
// set and (edge-triggered) its IRR bit cleared; a spurious IRQ 7 if none.
static uint8_t pic_ack_one(prep_pic_t *c, int n) {
    uint8_t bit = (uint8_t)(1u << n);
    if (!c->auto_eoi)
        c->isr |= bit;
    if (!(c->elcr & bit))
        c->irr &= (uint8_t)~bit;
    return (uint8_t)(c->base + n);
}

static uint8_t pic_iack(tnt_prep_io_t *p) {
    prep_pic_t *m = &p->pic[0], *sl = &p->pic[1];
    uint8_t v;
    int n = pic_pending(m);
    if (n < 0) {
        v = (uint8_t)(m->base + 7);
    } else if (n == 2 && !m->single) {
        int k = pic_pending(sl);
        pic_ack_one(m, 2);
        v = k >= 0 ? pic_ack_one(sl, k) : (uint8_t)(sl->base + 7);
    } else {
        v = pic_ack_one(m, n);
    }
    LOG(3, "IACK -> vector $%02X", v);
    pic_update(p);
    return v;
}

static uint8_t pic_read(prep_pic_t *c, bool data) {
    if (data)
        return c->imr;
    return c->read_isr ? c->isr : c->irr;
}

static void pic_write(tnt_prep_io_t *p, prep_pic_t *c, bool data, uint8_t v) {
    if (!data && (v & 0x10)) { // ICW1
        c->imr = 0;
        c->isr = 0;
        c->irr = 0;
        c->read_isr = false;
        c->icw4 = (v & 1) != 0;
        c->single = (v & 2) != 0;
        c->icw_step = 2;
    } else if (data && c->icw_step == 2) {
        c->base = v & 0xF8;
        c->icw_step = c->single ? (c->icw4 ? 4 : 0) : 3;
    } else if (data && c->icw_step == 3) {
        c->icw_step = c->icw4 ? 4 : 0;
    } else if (data && c->icw_step == 4) {
        c->auto_eoi = (v & 2) != 0;
        c->icw_step = 0;
    } else if (data) { // OCW1
        c->imr = v;
    } else if ((v & 0x18) == 0x00) { // OCW2
        if ((v & 0xE0) == 0x20)
            pic_eoi(c, -1); // non-specific EOI
        else if ((v & 0xE0) == 0x60)
            pic_eoi(c, v & 7); // specific EOI
    } else if ((v & 0x18) == 0x08) { // OCW3
        if (v & 2)
            c->read_isr = (v & 1) != 0;
    }
    pic_update(p);
}

// The 8042's two lines: data waiting for the keyboard (IRQ 1) or the mouse
// (IRQ 12), each enabled by its command-byte bit.
static void kbc_irq(tnt_prep_io_t *p) {
    prep_kbc_t *k = &p->kbc;
    bool full = k->head != k->tail;
    bool aux = full && k->out_aux[k->tail];
    prep_irq(p, 1, full && !aux && (k->cmd_byte & 1));
    prep_irq(p, 12, full && aux && (k->cmd_byte & 2));
}

// --- the port decoder -------------------------------------------------------

// Window offset -> ISA port.  First undo the 60x little-endian munge when the
// bridge is in little-endian mode; then, when the I/O map register ($850)
// selects the DISCONTIGUOUS map, fold PReP's spread layout back: each
// 32-port group sits on its own 4 KB page, port = (page << 5) | (offset & $1F).
// An offset inside a page but past its 32 ports names no port ($FFFF).
#define PREP_NO_PORT 0xFFFFu
static uint32_t prep_port(tnt_prep_io_t *p, uint32_t offset, unsigned size) {
    (void)size;
    if (p->port850 & 1) {
        if (offset & 0xFE0u)
            return PREP_NO_PORT;
        return ((offset >> 12) << 5) | (offset & 0x1Fu);
    }
    return offset;
}

// The config window: offset (inside it) -> Bandit 1's (device, fn, reg).
static pci_bus_t *prep_bus(tnt_prep_io_t *p) {
    tnt_state_t *st = tnt_st(p->cfg);
    for (int i = 0; i < st->bridge_count; i++)
        if (st->bridge[i].base == TNT_BANDIT1_BASE)
            return st->bridge[i].bus;
    return NULL;
}
static bool prep_cfg_decode(uint32_t off, int *dev, uint32_t *fn, uint32_t *reg) {
    uint32_t sel = (off >> 11) & 0xFFu;
    if (!sel || (sel & (sel - 1)))
        return false; // no slot bit, or more than one: no device answers
    int slot = 11 + __builtin_ctz(sel);
    if (slot == 12)
        *dev = 0x11;
    else if (slot == 13)
        *dev = 0x12;
    else
        return false;
    *fn = (off >> 8) & 7u;
    *reg = off & 0xFFu;
    return true;
}
static uint8_t prep_cfg_in8(tnt_prep_io_t *p, uint32_t off) {
    int dev;
    uint32_t fn, reg;
    pci_bus_t *bus = prep_bus(p);
    if (!bus || !prep_cfg_decode(off, &dev, &fn, &reg)) {
        LOG(2, "cfg read  $%06X: no IDSEL", off);
        return 0xFF;
    }
    uint32_t dw = pci_bus_cfg_read(bus, dev, fn, reg);
    if (dev == 0x11 && (reg & 0xFCu) == 0 && (dw & 0xFFFFu) == 0x1000u)
        dw = 0x00011000u; // present the Sandalfoot's 53C810 (see the header)
    uint8_t v = (uint8_t)(dw >> (8 * (reg & 3)));
    LOG(2, "cfg read  dev %d fn %u reg $%02X -> $%02X", dev, fn, reg, v);
    return v;
}
static void prep_cfg_out8(tnt_prep_io_t *p, uint32_t off, uint8_t v) {
    int dev;
    uint32_t fn, reg;
    pci_bus_t *bus = prep_bus(p);
    if (!bus || !prep_cfg_decode(off, &dev, &fn, &reg))
        return;
    LOG(2, "cfg write dev %d fn %u reg $%02X = $%02X", dev, fn, reg, v);
    pci_bus_cfg_write(bus, dev, fn, reg, reg & 3, v);
}

// Is `port` inside an I/O BAR a Bandit 1 device has enabled?  Only those are
// forwarded: Bandit answers an unclaimed I/O cycle with a machine check,
// where a PReP bus floats, and PReP software probes.  (A BAR's size is not
// recorded here; 256 ports covers the 53C8xx's 128 and every other I/O BAR
// on these boards.)
static bool prep_pci_io_claimed(tnt_prep_io_t *p, uint32_t port) {
    pci_bus_t *bus = prep_bus(p);
    if (!bus || port < PREP_PCI_IO || port >= 0x10000u)
        return false;
    for (int dev = 11; dev < 32; dev++) {
        if (!pci_bus_device(bus, dev))
            continue;
        if (!(pci_bus_cfg_read(bus, dev, 0, 0x04) & 1u)) // I/O space enable
            continue;
        for (uint32_t r = 0x10; r <= 0x24; r += 4) {
            uint32_t bar = pci_bus_cfg_read(bus, dev, 0, r);
            if ((bar & 1u) && port - (bar & ~3u) < 0x100u)
                return true;
        }
    }
    return false;
}

static uint8_t prep_in8(tnt_prep_io_t *p, uint32_t port) {
    if (prep_pci_io_claimed(p, port))
        return memory_read_uint8(TNT_BANDIT1_BASE + port);
    if (port >= 0x3F8 && port <= 0x3FF)
        return uart_read(&p->com1, port - 0x3F8);
    switch (port) {
    case 0x60: {
        uint8_t v = kbc_data_read(&p->kbc);
        kbc_irq(p);
        return v;
    }
    case 0x64:
        return kbc_status(&p->kbc);
    case 0x20:
    case 0x21:
        return pic_read(&p->pic[0], port & 1);
    case 0xA0:
    case 0xA1:
        return pic_read(&p->pic[1], port & 1);
    case 0x4D0:
    case 0x4D1:
        return p->pic[port & 1].elcr;
    case 0x70:
        return p->rtc.index;
    case 0x74:
        return (uint8_t)p->nv_index;
    case 0x75:
        return (uint8_t)(p->nv_index >> 8);
    case 0x76:
    case 0x77:
        return p->nvram[p->nv_index & 0x1FFFu];
    case 0x71:
        return rtc_read(p);
    case 0x92:
        return p->port92;
    case 0x850:
        return p->port850;
    case 0x80C: // planar ID: the Sandalfoot ($FE), which PPC_INFO.STZ's "00FE" stanza describes
        return 0xFE;
    default:
        break;
    }
    p->unknown_reads++;
    LOG(2, "in8  $%04X -> $FF (no device)", port);
    return 0xFF;
}

static void prep_out8(tnt_prep_io_t *p, uint32_t port, uint8_t v) {
    if (prep_pci_io_claimed(p, port)) {
        memory_write_uint8(TNT_BANDIT1_BASE + port, v);
        return;
    }
    if (port >= 0x3F8 && port <= 0x3FF) {
        uart_write(&p->com1, port - 0x3F8, v);
        return;
    }
    switch (port) {
    case 0x60:
        kbc_data_write(&p->kbc, v);
        kbc_irq(p);
        return;
    case 0x64:
        kbc_command(&p->kbc, v);
        kbc_irq(p);
        return;
    case 0x20:
    case 0x21:
        pic_write(p, &p->pic[0], port & 1, v);
        return;
    case 0xA0:
    case 0xA1:
        pic_write(p, &p->pic[1], port & 1, v);
        return;
    case 0x4D0:
    case 0x4D1:
        p->pic[port & 1].elcr = v;
        return;
    case 0x70:
        p->rtc.index = v;
        return;
    case 0x74:
        p->nv_index = (uint16_t)((p->nv_index & 0xFF00u) | v);
        return;
    case 0x75:
        p->nv_index = (uint16_t)((p->nv_index & 0x00FFu) | (v << 8));
        return;
    case 0x76:
    case 0x77:
        p->nvram[p->nv_index & 0x1FFFu] = v;
        return;
    case 0x71:
        rtc_write(p, v);
        return;
    case 0x92:
        if ((v ^ p->port92) & PREP_PORT92_LE) {
            LOG(1, "port $92: %s-endian", (v & PREP_PORT92_LE) ? "little" : "big");
            if (p->lane_reversal)
                ppc_set_le_bridge((v & PREP_PORT92_LE) != 0);
        }
        p->port92 = v;
        tnt_gc_recompute(p->cfg); // who drives the CPU's interrupt line may have changed
        return;
    case 0x850:
        LOG(1, "port $850 = $%02X (I/O map %s)", v, (v & 1) ? "discontiguous" : "contiguous");
        p->port850 = v;
        return;
    default:
        break;
    }
    p->unknown_writes++;
    LOG(2, "out8 $%04X = $%02X (no device)", port, v);
}

// Without memory lane reversal (a Macintosh) the window is a device behind
// Bandit 1 like any other: when the bridge's lanes are reversed for a
// little-endian client, the CPU hands over its munged address (byte ^7,
// halfword ^6, word ^4) with the register's bytes in big-endian order, and
// the window undoes the munge and reverses the bytes, as Bandit does for
// its PCI devices.
static bool munged(tnt_prep_io_t *p) {
    if (p->lane_reversal)
        return false;
    pci_bus_t *bus = prep_bus(p);
    return bus && pci_bus_lane_reverse(bus);
}

// Every access is split into bytes: the ISA side is a byte bus, and PCI
// configuration is byte-addressable.
static uint8_t io_in(tnt_prep_io_t *p, uint32_t offset) {
    if (offset >= PREP_CFG_BASE)
        return prep_cfg_in8(p, offset - PREP_CFG_BASE);
    return prep_in8(p, prep_port(p, offset, 1));
}
static void io_out(tnt_prep_io_t *p, uint32_t offset, uint8_t v) {
    if (offset >= PREP_CFG_BASE)
        prep_cfg_out8(p, offset - PREP_CFG_BASE, v);
    else
        prep_out8(p, prep_port(p, offset, 1), v);
}
static uint8_t io_read8(void *ctx, uint32_t offset) {
    tnt_prep_io_t *p = ctx;
    return io_in(p, munged(p) ? offset ^ 7u : offset);
}
// Otherwise the bus interface's value convention is big-endian: the byte at
// the lowest address is the most significant (behind a lane-reversing
// bridge the core has already swapped an LE-mode access).
static uint16_t io_read16(void *ctx, uint32_t offset) {
    tnt_prep_io_t *p = ctx;
    if (munged(p)) {
        offset ^= 6u;
        return (uint16_t)(io_in(p, offset) | (io_in(p, offset + 1) << 8));
    }
    return (uint16_t)((io_in(p, offset) << 8) | io_in(p, offset + 1));
}
static uint32_t io_read32(void *ctx, uint32_t offset) {
    tnt_prep_io_t *p = ctx;
    if (munged(p)) {
        offset ^= 4u;
        return (uint32_t)io_in(p, offset) | ((uint32_t)io_in(p, offset + 1) << 8) |
               ((uint32_t)io_in(p, offset + 2) << 16) | ((uint32_t)io_in(p, offset + 3) << 24);
    }
    return ((uint32_t)io_in(p, offset) << 24) | ((uint32_t)io_in(p, offset + 1) << 16) |
           ((uint32_t)io_in(p, offset + 2) << 8) | (uint32_t)io_in(p, offset + 3);
}
static void io_write8(void *ctx, uint32_t offset, uint8_t v) {
    tnt_prep_io_t *p = ctx;
    io_out(p, munged(p) ? offset ^ 7u : offset, v);
}
static void io_write16(void *ctx, uint32_t offset, uint16_t v) {
    tnt_prep_io_t *p = ctx;
    if (munged(p)) {
        offset ^= 6u;
        io_out(p, offset, (uint8_t)v);
        io_out(p, offset + 1, (uint8_t)(v >> 8));
        return;
    }
    io_out(p, offset, (uint8_t)(v >> 8));
    io_out(p, offset + 1, (uint8_t)v);
}
static void io_write32(void *ctx, uint32_t offset, uint32_t v) {
    tnt_prep_io_t *p = ctx;
    if (munged(p)) {
        offset ^= 4u;
        for (unsigned i = 0; i < 4; i++)
            io_out(p, offset + i, (uint8_t)(v >> (8 * i)));
        return;
    }
    for (unsigned i = 0; i < 4; i++)
        io_out(p, offset + i, (uint8_t)(v >> (24 - 8 * i)));
}
// Inspection never touches the UART's queues.
static uint8_t io_peek8(void *ctx, uint32_t offset) {
    tnt_prep_io_t *p = ctx;
    uint32_t port = prep_port(p, offset, 1);
    return port == 0x92 ? p->port92 : port == 0x850 ? p->port850 : 0xFF;
}

static uint8_t iack_read8(void *ctx, uint32_t offset) {
    tnt_prep_io_t *p = ctx;
    // $BFFFFFF0, or the byte a munged lbz of it reaches ($...F7).
    return (offset & ~7u) == 0xFF0u ? pic_iack(p) : 0xFF;
}
static uint16_t iack_read16(void *ctx, uint32_t offset) {
    return (uint16_t)((iack_read8(ctx, offset) << 8) | 0xFF);
}
static uint32_t iack_read32(void *ctx, uint32_t offset) {
    return ((uint32_t)iack_read8(ctx, offset) << 24) | 0x00FFFFFFu;
}
static void iack_write8(void *ctx, uint32_t offset, uint8_t v) {
    (void)ctx, (void)offset, (void)v;
}
static void iack_write16(void *ctx, uint32_t offset, uint16_t v) {
    (void)ctx, (void)offset, (void)v;
}
static void iack_write32(void *ctx, uint32_t offset, uint32_t v) {
    (void)ctx, (void)offset, (void)v;
}

bool tnt_prep_io_owns_irq(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    return st && st->prep && (st->prep->port92 & PREP_PORT92_LE);
}

bool tnt_prep_io_irq_line(config_t *cfg) {
    tnt_prep_io_t *p = tnt_st(cfg)->prep;
    return p && pic_pending(&p->pic[0]) >= 0;
}

// A Grand Central source changed: the ones a PReP OS knows reach the PIC.
void tnt_prep_io_gc_source(config_t *cfg, int n, bool level) {
    tnt_state_t *st = tnt_st(cfg);
    if (!st || !st->prep)
        return;
    if (n == ANS_INT_FW0)
        prep_irq(st->prep, 13, level);
    else if (n == ANS_INT_FW1)
        prep_irq(st->prep, 15, level);
}

// --- object model: machine.prep, machine.prep.com1 --------------------------

static DEF_GETTER(prep_attr_port92) {
    (void)self;
    return val_uint(1, g_prep ? g_prep->port92 : 0);
}
static DEF_GETTER(prep_attr_unknown) {
    (void)self;
    return val_uint(8, g_prep ? g_prep->unknown_reads + g_prep->unknown_writes : 0);
}

static DEF_GETTER(prep_attr_lanes) {
    (void)self;
    return val_bool(g_prep ? g_prep->lane_reversal : true);
}
static DEF_SETTER(prep_set_lanes) {
    (void)self;
    if (!g_prep)
        return val_err("prep_io not enabled");
    g_prep->lane_reversal = val_as_bool(&in);
    ppc_set_le_bridge(g_prep->lane_reversal && (g_prep->port92 & PREP_PORT92_LE));
    return val_none();
}

// Put Bandit 1 in little-endian mode -- its byte lanes reversed, as Open
// Firmware's `little-endian? true` leaves them for NT -- by a config cycle
// to its own mode-select register (device 11, $50; BIG_ENDIAN is bit 24).
static DEF_METHOD(prep_method_bandit_le) {
    (void)self;
    if (!g_prep)
        return val_err("prep_io not enabled");
    pci_bus_t *bus = prep_bus(g_prep);
    if (!bus)
        return val_err("no Bandit 1");
    bool le = argc < 1 || val_as_bool(&argv[0]);
    uint8_t b3 = (uint8_t)(pci_bus_cfg_read(bus, 11, 0, 0x50) >> 24);
    b3 = le ? (uint8_t)(b3 & ~1u) : (uint8_t)(b3 | 1u);
    pci_bus_cfg_write(bus, 11, 0, 0x50, 3, b3);
    return val_bool(le);
}
static const arg_decl_t prep_bandit_le_args[] = {
    {.name = "little",
     .kind = VK_BOOL,
     .validation_flags = OBJ_ARG_OPTIONAL,
     .doc = "reverse the lanes (default true)"},
};

static const member_t prep_members[] = {
    {.kind = MK_ATTR,
     .name = "lane_reversal",
     .doc = "Does the bridge reverse the byte lanes to memory in LE mode, as PReP's does (true), or not, as a Mac's "
            "Hammerhead does not (false)?",                                                                   .attr = {.type = VK_BOOL, .get = prep_attr_lanes, .set = prep_set_lanes}},
    {.kind = MK_METHOD,
     .name = "bandit_little_endian",
     .doc = "Set Bandit 1's byte lanes reversed (true) or straight (false) through its mode-select register",
     .method = {.args = prep_bandit_le_args, .nargs = 1, .result = VK_BOOL, .fn = prep_method_bandit_le}                                                                              },
    {.kind = MK_ATTR,
     .name = "port92",
     .doc = "System Control Port A; bit 1 is the bridge's little-endian mode",
     .attr = {.type = VK_UINT, .get = prep_attr_port92}                                                                                                                               },
    {.kind = MK_ATTR,
     .name = "unknown",
     .doc = "Accesses to ports no device answers (logged under `prep` at level 1)",
     .attr = {.type = VK_UINT, .get = prep_attr_unknown}                                                                                                                              },
};
static const class_desc_t prep_class = {
    .name = "prep_io",
    .doc = "The experimental PReP-compatibility I/O window at $80000000 (prep_io=on)",
    .members = prep_members,
    .n_members = sizeof(prep_members) / sizeof(prep_members[0]),
};

static DEF_METHOD(com1_method_sent) {
    (void)self;
    (void)argc;
    (void)argv;
    if (!g_prep)
        return val_err("prep_io not enabled");
    prep_uart_t *u = &g_prep->com1;
    char *text = malloc(u->tx_len * 4 + 1);
    if (!text)
        return val_err("sent: out of memory");
    size_t o = 0;
    for (unsigned i = 0; i < u->tx_len; i++) {
        uint8_t b = u->tx[i];
        if (b == '\\') {
            text[o++] = '\\';
            text[o++] = '\\';
        } else if (b == '\t' || b == '\n' || b == '\r' || (b >= 0x20 && b < 0x7F)) {
            text[o++] = (char)b;
        } else {
            o += (size_t)sprintf(text + o, "\\x%02X", b);
        }
    }
    text[o] = '\0';
    u->tx_len = 0;
    value_t v = val_str(text);
    free(text);
    return v;
}

static DEF_METHOD(com1_method_receive) {
    (void)self;
    if (!g_prep)
        return val_err("prep_io not enabled");
    if (argc != 1 || argv[0].kind != VK_STRING || !argv[0].s)
        return val_err("receive: expected a string");
    prep_uart_t *u = &g_prep->com1;
    unsigned n = 0;
    for (const char *s = argv[0].s; *s; s++) {
        unsigned next = (u->rx_head + 1) % COM_FIFO;
        if (next == u->rx_tail)
            break;
        u->rx[u->rx_head] = (uint8_t)*s;
        u->rx_head = next;
        n++;
    }
    return val_uint(4, n);
}

static DEF_GETTER(com1_attr_waiting) {
    (void)self;
    return val_uint(8, g_prep ? g_prep->com1.polls_since_tx : 0);
}

static const arg_decl_t com1_receive_args[] = {
    {.name = "text", .kind = VK_STRING, .doc = "bytes for the guest to read from RBR"},
};
static const member_t com1_members[] = {
    {.kind = MK_ATTR,
     .name = "waiting",
     .doc = "LSR polls since the guest last transmitted (large: it is waiting for input)",
     .attr = {.type = VK_UINT, .get = com1_attr_waiting}                                            },
    {.kind = MK_METHOD,
     .name = "sent",
     .doc = "Drain and return what the guest has written to COM1",
     .method = {.args = NULL, .nargs = 0, .result = VK_STRING, .fn = com1_method_sent}              },
    {.kind = MK_METHOD,
     .name = "receive",
     .doc = "Queue bytes for the guest to read from COM1",
     .method = {.args = com1_receive_args, .nargs = 1, .result = VK_UINT, .fn = com1_method_receive}},
};
static const class_desc_t com1_class = {
    .name = "prep_uart",
    .doc = "A 16550 at ISA port $3F8",
    .members = com1_members,
    .n_members = sizeof(com1_members) / sizeof(com1_members[0]),
};

// --- lifetime ----------------------------------------------------------------

// The option, or GS_PREP_IO=1 in the environment.  The environment is how a
// checkpoint restore gets the window: a restore rebuilds the machine from its
// model and RAM size alone (system_restore), and the window is structural --
// it decides who owns $80000000 before any state is read.
bool tnt_prep_io_enabled(config_t *cfg) {
    const char *v = machine_build_opts_option(&cfg->build_opts, "prep_io");
    if (v && strcmp(v, "on") == 0)
        return true;
    const char *e = getenv("GS_PREP_IO");
    return e && strcmp(e, "1") == 0;
}

void tnt_prep_io_init(config_t *cfg) {
    tnt_prep_io_t *p = calloc(1, sizeof *p);
    if (!p)
        return;
    p->cfg = cfg;
    p->rtc.reg_b = 0x02; // 24-hour, BCD
    p->lane_reversal = true;
    p->kbc.cmd_byte = 0x65; // translate, both interrupts enabled, SYS
    p->iface.read_uint8 = io_read8;
    p->iface.read_uint16 = io_read16;
    p->iface.read_uint32 = io_read32;
    p->iface.write_uint8 = io_write8;
    p->iface.write_uint16 = io_write16;
    p->iface.write_uint32 = io_write32;
    p->iface.peek_uint8 = io_peek8;
    memory_map_add(cfg->memory_map, PREP_IO_BASE, PREP_IO_SIZE, "PReP I/O window", &p->iface, p);
    p->iack_iface.read_uint8 = iack_read8;
    p->iack_iface.read_uint16 = iack_read16;
    p->iack_iface.read_uint32 = iack_read32;
    p->iack_iface.write_uint8 = iack_write8;
    p->iack_iface.write_uint16 = iack_write16;
    p->iack_iface.write_uint32 = iack_write32;
    memory_map_add(cfg->memory_map, 0xBFFFF000u, MEM_PAGE_SIZE, "PReP interrupt acknowledge", &p->iack_iface, p);
    p->node = object_new(&prep_class, p, "prep");
    if (p->node) {
        object_attach(machine_object(), p->node);
        p->com1_node = object_new(&com1_class, p, "com1");
        if (p->com1_node)
            object_attach(p->node, p->com1_node);
    }
    g_prep = p;
    tnt_st(cfg)->prep = p;
    LOG(0, "PReP I/O window at $%08X (prep_io=on)", PREP_IO_BASE);
}

void tnt_prep_io_teardown(config_t *cfg) {
    tnt_state_t *st = tnt_st(cfg);
    tnt_prep_io_t *p = st ? st->prep : NULL;
    if (!p)
        return;
    struct object *nodes[2] = {p->com1_node, p->node};
    for (int i = 0; i < 2; i++) {
        if (nodes[i]) {
            object_detach(nodes[i]);
            object_delete(nodes[i]);
        }
    }
    if (g_prep == p)
        g_prep = NULL;
    ppc_set_le_bridge(false);
    free(p);
    st->prep = NULL;
}
