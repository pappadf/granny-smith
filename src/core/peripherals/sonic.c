// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sonic.c
// DP83932 SONIC Ethernet controller model — see sonic.h.  Register and
// descriptor semantics follow the DP83932B datasheet (§3 buffer management,
// §4 registers) cross-checked against Apple's ROM self-tests
// (OS/StartMgr/UnivTestEnv/SONIC_BitMarch/CAMDMA/Interrupt/Loopback.c) and
// the System 7.1 SONIC driver (DeclData/DeclNet/Sonic/SonicEnet.a).
// Evidence labels: [D] datasheet, [A] Apple source, [I] inferred.

#include "sonic.h"

#include "log.h"

#include <stdlib.h>
#include <string.h>

LOG_USE_CATEGORY_NAME("sonic");

// === Register indices (RA5..RA0; datasheet Table 4-1 + SonicEqu.a) [D][A] ===
#define R_CR    0x00 // command
#define R_DCR   0x01 // data configuration
#define R_RCR   0x02 // receive control / status
#define R_TCR   0x03 // transmit control / status
#define R_IMR   0x04 // interrupt mask
#define R_ISR   0x05 // interrupt status (write-1-to-clear)
#define R_UTDA  0x06 // upper transmit descriptor address
#define R_CTDA  0x07 // current transmit descriptor address
#define R_TPS   0x08 // transmit packet size (reads inverted [A])
#define R_TFC   0x09 // transmit fragment count
#define R_TSA0  0x0A // transmit start address 0
#define R_TSA1  0x0B // transmit start address 1
#define R_TFS   0x0C // transmit fragment size (stores >> bus width [A])
#define R_URDA  0x0D // upper receive descriptor address
#define R_CRDA  0x0E // current receive descriptor address
#define R_CRBA0 0x0F // current receive buffer address 0
#define R_CRBA1 0x10 // current receive buffer address 1
#define R_RBWC0 0x11 // remaining buffer word count 0
#define R_RBWC1 0x12 // remaining buffer word count 1
#define R_EOBC  0x13 // end of buffer word count (bit 0 = 0)
#define R_URRA  0x14 // upper receive resource address
#define R_RSA   0x15 // resource start address (bit 0 = 0)
#define R_REA   0x16 // resource end address (bit 0 = 0)
#define R_RRP   0x17 // resource read pointer (bit 0 = 0)
#define R_RWP   0x18 // resource write pointer (bit 0 = 0)
#define R_TRBA0 0x19 // temporary receive buffer address 0
#define R_TRBA1 0x1A // temporary receive buffer address 1
#define R_TBWC0 0x1B // temporary buffer word count 0 (bit 0 = 0)
#define R_TBWC1 0x1C // temporary buffer word count 1
#define R_ADDR0 0x1D // address generator 0
#define R_ADDR1 0x1E // address generator 1
#define R_LLFA  0x1F // last link field address
#define R_TTDA  0x20 // temporary transmit descriptor address
#define R_CEP   0x21 // CAM entry pointer (low 4 bits)
#define R_CAP2  0x22 // CAM address port 2 (reads CAM cell)
#define R_CAP1  0x23 // CAM address port 1
#define R_CAP0  0x24 // CAM address port 0
#define R_CE    0x25 // CAM enable
#define R_CDP   0x26 // CAM descriptor pointer
#define R_CDC   0x27 // CAM descriptor count (low 5 bits)
#define R_SR    0x28 // silicon revision
#define R_WT0   0x29 // watchdog timer 0
#define R_WT1   0x2A // watchdog timer 1
#define R_RSC   0x2B // receive sequence counter
#define R_CRCT  0x2C // CRC error tally
#define R_FAET  0x2D // frame alignment error tally
#define R_MPT   0x2E // missed packet tally
#define R_DCR2  0x3F // data configuration 2 (rev > 3)

#define SONIC_REG_COUNT 0x40

// The Quadra SONIC answers the driver's rev-dependent DCR2 setup path
// (SonicEnet.a <H5> "only the Quadra's need this register set") [A][I].
#define SONIC_SILICON_REV 4

// === Command register bits (datasheet 4.3.1) [D] ===
#define CR_LCAM  0x0200 // load CAM
#define CR_RRRA  0x0100 // read RRA
#define CR_RST   0x0080 // software reset
#define CR_ST    0x0020 // start watchdog timer
#define CR_STP   0x0010 // stop watchdog timer
#define CR_RXEN  0x0008 // receiver enable
#define CR_RXDIS 0x0004 // receiver disable
#define CR_TXP   0x0002 // transmit packet(s)
#define CR_HTX   0x0001 // halt transmission

// === Interrupt status/mask bits (datasheet 4.3.5/4.3.6) [D] ===
#define ISR_BR    0x4000 // bus retry
#define ISR_HBL   0x2000 // heartbeat lost
#define ISR_LCD   0x1000 // load CAM done
#define ISR_PINT  0x0800 // programmable interrupt
#define ISR_PKTRX 0x0400 // packet received
#define ISR_TXDN  0x0200 // transmission done
#define ISR_TXER  0x0100 // transmit error
#define ISR_TC    0x0080 // timer complete
#define ISR_RDE   0x0040 // receive descriptors exhausted
#define ISR_RBE   0x0020 // receive buffers exhausted
#define ISR_RBAE  0x0010 // receive buffer area exceeded
#define ISR_ALL   0x7FFF

// === RCR bits (datasheet 4.3.3) [D] ===
#define RCR_LB_MASK 0x0600 // LB1:LB0 — nonzero = a loopback mode is on
#define RCR_CTL     0xFE00 // writable control bits (ERR RNT BRD PRO AMC LB1 LB0)
#define RCR_LPKT    0x0040 // last packet in RBA
#define RCR_LBK     0x0002 // loopback packet received
#define RCR_PRX     0x0001 // packet received OK

// === TCR bits (datasheet 4.3.4) [D] ===
#define TCR_PINTR 0x8000 // programmable interrupt request (from TXpkt.config)
#define TCR_CRCI  0x2000 // CRC inhibit
#define TCR_CTL   0xF000 // writable control bits (PINTR POWC CRCI EXDIS)
#define TCR_NCRS  0x0100 // no carrier sense (always set in MAC loopback)
#define TCR_CRSL  0x0080 // carrier sense lost (always set in MAC loopback)
#define TCR_BCM   0x0002 // byte count mismatch
#define TCR_PTX   0x0001 // packet transmitted OK

// Largest frame the model gathers/loops back (datasheet allows to 64 KB;
// the driver and tests stay below standard Ethernet sizes).
#define SONIC_FRAME_MAX 4096

struct sonic {
    uint16_t reg[SONIC_REG_COUNT];
    uint16_t cam[16][3]; // CAM cells: 3× 16-bit per entry (ap0/ap1/ap2)

    bool in_reset; // CR.RST latched
    bool rx_enabled; // receiver on (CR.RXEN/RXDIS)
    bool timer_on; // watchdog running (CR.ST/STP) — value is static in v1
    bool irq_line; // current INT output level

    uint8_t rba_seq, pkt_seq; // RSC halves (datasheet 3.4.3.2)
    uint8_t byte2_latch; // high byte of an in-flight 16-bit register write

    sonic_irq_cb irq_cb;
    void *irq_ctx;
    sonic_mem_read_fn mem_rd;
    sonic_mem_write_fn mem_wr;
    void *mem_ctx;
};

// ============================================================
// Bus access (guest-physical; hooks or machine bus)
// ============================================================

// The machine installs guest-physical accessors via sonic_set_memory_hooks
// (the chip is a bus master); with none installed, DMA reads as zero and
// writes vanish — logged once so a missing wiring is visible.

static uint32_t bus_read(sonic_t *s, uint32_t phys, unsigned width) {
    if (s->mem_rd)
        return s->mem_rd(s->mem_ctx, phys, width);
    LOG(1, "DMA read $%08X with no memory hooks installed", phys);
    return 0;
}

static void bus_write(sonic_t *s, uint32_t phys, uint32_t value, unsigned width) {
    if (s->mem_wr) {
        s->mem_wr(s->mem_ctx, phys, value, width);
        return;
    }
    (void)value;
    (void)width;
    LOG(1, "DMA write $%08X with no memory hooks installed", phys);
}

// Descriptor-field step: 32-bit bus mode (DCR.DW set) puts each 16-bit
// field in the low half of a longword slot; 16-bit mode packs them [D].
static inline unsigned field_step(const sonic_t *s) {
    return (s->reg[R_DCR] & 0x0020) ? 4 : 2;
}

static uint16_t field_read(sonic_t *s, uint32_t addr) {
    if (field_step(s) == 4)
        return (uint16_t)bus_read(s, addr, 4); // low 16 bits of the BE longword
    return (uint16_t)bus_read(s, addr, 2);
}

static void field_write(sonic_t *s, uint32_t addr, uint16_t value) {
    if (field_step(s) == 4)
        bus_write(s, addr, value, 4); // upper half writes as zero
    else
        bus_write(s, addr, value, 2);
}

// ============================================================
// Interrupt line
// ============================================================

static void update_irq(sonic_t *s) {
    bool line = (s->reg[R_ISR] & s->reg[R_IMR]) != 0;
    if (line == s->irq_line)
        return;
    s->irq_line = line;
    if (s->irq_cb)
        s->irq_cb(s->irq_ctx, line);
}

static void raise_isr(sonic_t *s, uint16_t bits) {
    s->reg[R_ISR] |= bits;
    update_irq(s);
}

// ============================================================
// Commands
// ============================================================

// Load CAM (CR.LCAM): DMA the CAM descriptor area — CDC entries of
// {entry#, ap0, ap1, ap2} followed by the CAM enable word — then set LCD.
// CDP ends past the enable field, CDC reaches zero (datasheet 4.1.1) [D].
static void cmd_load_cam(sonic_t *s) {
    unsigned step = field_step(s);
    uint32_t addr = ((uint32_t)s->reg[R_URRA] << 16) | s->reg[R_CDP];
    unsigned count = s->reg[R_CDC] & 0x1F;
    for (unsigned i = 0; i < count; i++) {
        uint16_t idx = field_read(s, addr) & 0xF;
        s->cam[idx][0] = field_read(s, addr + step);
        s->cam[idx][1] = field_read(s, addr + 2 * step);
        s->cam[idx][2] = field_read(s, addr + 3 * step);
        addr += 4 * step;
        LOG(2, "LCAM entry %u: %04X %04X %04X", idx, s->cam[idx][0], s->cam[idx][1], s->cam[idx][2]);
    }
    s->reg[R_CE] = field_read(s, addr);
    addr += step;
    s->reg[R_CDP] = (uint16_t)addr;
    s->reg[R_CDC] = 0;
    raise_isr(s, ISR_LCD);
}

// Read RRA (CR.RRRA or automatic refill): load CRBA/RBWC from the resource
// at RRP and advance it, wrapping REA -> RSA (datasheet 3.4.1) [D].
// Returns false when no resource is available (RRP has caught up with RWP).
static bool cmd_read_rra(sonic_t *s) {
    unsigned step = field_step(s);
    if (s->reg[R_RRP] == s->reg[R_RWP]) {
        LOG(1, "read RRA with empty resource area (RRP=RWP=$%04X)", s->reg[R_RRP]);
        return false;
    }
    uint32_t addr = ((uint32_t)s->reg[R_URRA] << 16) | s->reg[R_RRP];
    s->reg[R_CRBA0] = field_read(s, addr);
    s->reg[R_CRBA1] = field_read(s, addr + step);
    s->reg[R_RBWC0] = field_read(s, addr + 2 * step);
    s->reg[R_RBWC1] = field_read(s, addr + 3 * step);
    uint16_t rrp = (uint16_t)(s->reg[R_RRP] + 4 * step);
    if (rrp >= s->reg[R_REA])
        rrp = s->reg[R_RSA];
    s->reg[R_RRP] = rrp;
    LOG(2, "read RRA: buffer $%04X%04X words %u", s->reg[R_CRBA1], s->reg[R_CRBA0],
        ((uint32_t)s->reg[R_RBWC1] << 16) | s->reg[R_RBWC0]);
    return true;
}

// Deliver one looped-back frame through the receive buffer management:
// packet bytes into the current RBA, then a filled RDA descriptor
// (datasheet 3.4.3-3.4.6) [D].  `frame` excludes the FCS; four CRC bytes
// are appended unless TCR.CRCI suppressed them on transmit.
static void rx_deliver(sonic_t *s, const uint8_t *frame, uint32_t len) {
    unsigned step = field_step(s);
    uint32_t rx_len = len + ((s->reg[R_TCR] & TCR_CRCI) ? 0 : 4);

    uint32_t rba = ((uint32_t)s->reg[R_CRBA1] << 16) | s->reg[R_CRBA0];
    for (uint32_t i = 0; i < len; i++)
        bus_write(s, rba + i, frame[i], 1);
    for (uint32_t i = len; i < rx_len; i++)
        bus_write(s, rba + i, 0x5A, 1); // fake FCS filler [I]

    // Buffer accounting: word count consumed, LPKT when the remainder
    // drops below EOBC (datasheet 3.4.2).
    uint32_t words = (rx_len + 1) >> 1;
    uint32_t rbwc = ((uint32_t)s->reg[R_RBWC1] << 16) | s->reg[R_RBWC0];
    rbwc = (rbwc > words) ? rbwc - words : 0;
    s->reg[R_RBWC0] = (uint16_t)rbwc;
    s->reg[R_RBWC1] = (uint16_t)(rbwc >> 16);
    bool last_in_rba = rbwc < s->reg[R_EOBC];

    // Receive status: OK + loopback (+ last-packet), mirrored into RCR's
    // status half and the descriptor status field [D].
    uint16_t status = RCR_PRX | RCR_LBK | (last_in_rba ? RCR_LPKT : 0);
    s->reg[R_RCR] = (uint16_t)((s->reg[R_RCR] & RCR_CTL) | status);

    // Fill the current RDA descriptor: status, byte_count, ptr0/1, seq_no;
    // then release it (in_use = 0) and follow the link.
    uint32_t rda = ((uint32_t)s->reg[R_URDA] << 16) | s->reg[R_CRDA];
    field_write(s, rda, status);
    field_write(s, rda + 1 * step, (uint16_t)rx_len);
    field_write(s, rda + 2 * step, (uint16_t)rba);
    field_write(s, rda + 3 * step, (uint16_t)(rba >> 16));
    field_write(s, rda + 4 * step, (uint16_t)(((uint16_t)s->rba_seq << 8) | s->pkt_seq));
    uint16_t link = field_read(s, rda + 5 * step);
    field_write(s, rda + 6 * step, 0); // in_use: SONIC is done with this one
    s->pkt_seq++;
    s->reg[R_RSC] = (uint16_t)(((uint16_t)s->rba_seq << 8) | s->pkt_seq);

    // Advance CRBA past the packet (longword-aligned in 32-bit mode [D]).
    uint32_t next_rba = rba + rx_len;
    next_rba = (next_rba + (step - 1)) & ~(uint32_t)(step - 1);
    s->reg[R_CRBA0] = (uint16_t)next_rba;
    s->reg[R_CRBA1] = (uint16_t)(next_rba >> 16);

    if (link & 1) {
        // End of descriptor list — receive descriptors exhausted.
        raise_isr(s, ISR_RDE);
    } else {
        s->reg[R_CRDA] = link & 0xFFFE;
        // The next descriptor must be available (in_use != 0) [D].
        uint32_t next = ((uint32_t)s->reg[R_URDA] << 16) | s->reg[R_CRDA];
        if (field_read(s, next + 6 * step) == 0)
            raise_isr(s, ISR_RDE);
    }

    // Exhausted RBA: fetch the next resource, RBE when the RRA is dry.
    if (last_in_rba) {
        s->rba_seq++;
        s->pkt_seq = 0;
        if (!cmd_read_rra(s))
            raise_isr(s, ISR_RBE);
    }
    raise_isr(s, ISR_PKTRX);
}

// Transmit (CR.TXP): walk the TDA list — per descriptor gather the
// fragments, post status, follow the link until EOL (datasheet 3.5) [D].
static void cmd_transmit(sonic_t *s) {
    unsigned step = field_step(s);
    uint8_t frame[SONIC_FRAME_MAX];

    for (int guard = 0; guard < 64; guard++) { // bounded against mis-linked lists
        uint32_t tda = ((uint32_t)s->reg[R_UTDA] << 16) | s->reg[R_CTDA];
        uint16_t config = field_read(s, tda + 1 * step);
        uint16_t pkt_size = field_read(s, tda + 2 * step);
        uint16_t frag_count = field_read(s, tda + 3 * step);

        // TXpkt.config bits 15-12 load into the TCR control half [D].
        s->reg[R_TCR] = (uint16_t)((s->reg[R_TCR] & ~TCR_CTL) | (config & TCR_CTL));
        if (config & TCR_PINTR)
            raise_isr(s, ISR_PINT);

        // Gather the fragment list.
        uint32_t total = 0;
        uint32_t off = tda + 4 * step;
        for (unsigned f = 0; f < frag_count; f++) {
            uint16_t p0 = field_read(s, off);
            uint16_t p1 = field_read(s, off + step);
            uint16_t fsize = field_read(s, off + 2 * step);
            uint32_t src = ((uint32_t)p1 << 16) | p0;
            for (uint32_t i = 0; i < fsize && total < sizeof(frame); i++)
                frame[total++] = (uint8_t)bus_read(s, src + i, 1);
            off += 3 * step;
            s->reg[R_TSA0] = p0;
            s->reg[R_TSA1] = p1;
            s->reg[R_TFS] = (uint16_t)(fsize >> ((step == 4) ? 2 : 1));
        }
        uint16_t link = field_read(s, off);
        s->reg[R_LLFA] = (uint16_t)off;
        s->reg[R_TPS] = pkt_size;
        s->reg[R_TFC] = frag_count;

        uint16_t status;
        if (total != pkt_size) {
            // Byte count mismatch aborts the transmission [D].
            status = TCR_BCM;
            s->reg[R_TCR] = (uint16_t)((s->reg[R_TCR] & TCR_CTL) | status);
            field_write(s, tda, status);
            raise_isr(s, ISR_TXER);
            return;
        }

        // Success — in loopback CRS is never seen (NCRS+CRSL, datasheet
        // 4.3.4 notes); same result stands for the wireless void.
        status = TCR_PTX | TCR_NCRS | TCR_CRSL;
        s->reg[R_TCR] = (uint16_t)((s->reg[R_TCR] & TCR_CTL) | status);
        field_write(s, tda, status);
        LOG(2, "TX %u bytes (%u frags)%s", total, frag_count,
            (s->reg[R_RCR] & RCR_LB_MASK) ? " -> loopback" : " -> void");

        // Loop the frame back when a loopback mode is programmed and the
        // receiver is listening.
        if ((s->reg[R_RCR] & RCR_LB_MASK) && s->rx_enabled)
            rx_deliver(s, frame, total);

        if (link & 1) {
            raise_isr(s, ISR_TXDN);
            return;
        }
        s->reg[R_CTDA] = link & 0xFFFE;
    }
    LOG(1, "transmit: descriptor list did not terminate (EOL missing)");
}

// ============================================================
// Register file
// ============================================================

uint16_t sonic_reg_read(sonic_t *s, uint32_t reg) {
    reg &= 0x3F;
    switch (reg) {
    case R_CR:
        return (uint16_t)((s->in_reset ? CR_RST : 0) | (s->timer_on ? CR_ST : CR_STP) |
                          (s->rx_enabled ? CR_RXEN : CR_RXDIS));
    case R_TPS:
        return (uint16_t)~s->reg[R_TPS]; // reads inverted [A] (BitMarch)
    case R_CAP0:
    case R_CAP1:
    case R_CAP2:
        // CAM ports present the cell CEP points at (valid in reset) [D].
        return s->cam[s->reg[R_CEP] & 0xF][R_CAP0 - reg];
    case R_SR:
        return SONIC_SILICON_REV;
    default:
        return s->reg[reg];
    }
}

void sonic_reg_write(sonic_t *s, uint32_t reg, uint16_t value) {
    reg &= 0x3F;
    switch (reg) {
    case R_CR:
        if (value & CR_RST) {
            // Software reset: latch RST with the timer/receiver stop bits;
            // pending commands are dropped [D].
            s->in_reset = true;
            s->rx_enabled = false;
            s->timer_on = false;
            return;
        }
        if (s->in_reset) {
            s->in_reset = false; // leaving reset; fall through to commands
        }
        if (value & CR_RXDIS)
            s->rx_enabled = false;
        else if (value & CR_RXEN)
            s->rx_enabled = true;
        if (value & CR_STP)
            s->timer_on = false;
        else if (value & CR_ST)
            s->timer_on = true;
        if (value & CR_LCAM)
            cmd_load_cam(s);
        if (value & CR_RRRA)
            cmd_read_rra(s);
        if (value & CR_TXP)
            cmd_transmit(s);
        return;
    case R_ISR:
        s->reg[R_ISR] &= (uint16_t)~value; // write-1-to-clear [D]
        update_irq(s);
        return;
    case R_IMR:
        s->reg[R_IMR] = value & ISR_ALL;
        update_irq(s);
        return;
    case R_RCR:
        s->reg[R_RCR] = (uint16_t)((value & RCR_CTL) | (s->reg[R_RCR] & ~RCR_CTL));
        return;
    case R_TCR: {
        // A 0->1 transition of PINTR raises PINT immediately — Apple's
        // interrupt self-test generates its test interrupt this way [A]
        // (SONIC_Loopback.c: clear PINTR, set PINTR, expect PINT).
        bool pintr_rose = !(s->reg[R_TCR] & TCR_PINTR) && (value & TCR_PINTR);
        s->reg[R_TCR] = (uint16_t)((value & TCR_CTL) | (s->reg[R_TCR] & ~TCR_CTL));
        if (pintr_rose)
            raise_isr(s, ISR_PINT);
        return;
    }
    case R_TFS:
        // Stores pre-shifted by the bus width [A] (BitMarch data_shifted).
        s->reg[R_TFS] = (uint16_t)(value >> ((field_step(s) == 4) ? 2 : 1));
        return;
    case R_EOBC:
    case R_RSA:
    case R_REA:
    case R_RRP:
    case R_RWP:
    case R_TBWC0:
        s->reg[reg] = value & 0xFFFE; // word pointers: bit 0 forced 0 [A][D]
        return;
    case R_CEP:
        s->reg[R_CEP] = value & 0x000F;
        return;
    case R_CDC:
        s->reg[R_CDC] = value & 0x001F;
        return;
    case R_SR:
    case R_CAP0:
    case R_CAP1:
    case R_CAP2:
        return; // read-only
    default:
        s->reg[reg] = value;
        return;
    }
}

// ============================================================
// Lifecycle
// ============================================================

void sonic_hard_reset(sonic_t *s) {
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->cam, 0, sizeof(s->cam));
    s->in_reset = true; // comes up in software-reset state [D]
    s->rx_enabled = false;
    s->timer_on = false;
    s->rba_seq = s->pkt_seq = 0;
    // TCR.NCRS and TCR.BCM are set by a hardware reset (datasheet 4.3.4).
    s->reg[R_TCR] = TCR_NCRS | TCR_BCM;
    if (s->irq_line) {
        s->irq_line = false;
        if (s->irq_cb)
            s->irq_cb(s->irq_ctx, false);
    }
}

sonic_t *sonic_init(checkpoint_t *cp) {
    sonic_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    sonic_hard_reset(s);
    if (cp) {
        sonic_t saved;
        system_read_checkpoint_data(cp, &saved, sizeof(saved));
        // Restore value state; callbacks/hooks are rebound by the machine.
        memcpy(s->reg, saved.reg, sizeof(s->reg));
        memcpy(s->cam, saved.cam, sizeof(s->cam));
        s->in_reset = saved.in_reset;
        s->rx_enabled = saved.rx_enabled;
        s->timer_on = saved.timer_on;
        s->irq_line = saved.irq_line;
        s->rba_seq = saved.rba_seq;
        s->pkt_seq = saved.pkt_seq;
    }
    return s;
}

void sonic_delete(sonic_t *s) {
    free(s);
}

void sonic_checkpoint(sonic_t *s, checkpoint_t *cp) {
    system_write_checkpoint_data(cp, s, sizeof(*s));
}

void sonic_set_irq_callback(sonic_t *s, sonic_irq_cb cb, void *context) {
    s->irq_cb = cb;
    s->irq_ctx = context;
    if (cb && s->irq_line)
        cb(context, true); // re-drive the level after (re)binding
}

void sonic_set_memory_hooks(sonic_t *s, sonic_mem_read_fn rd, sonic_mem_write_fn wr, void *context) {
    s->mem_rd = rd;
    s->mem_wr = wr;
    s->mem_ctx = context;
}
